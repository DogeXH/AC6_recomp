// win_probe.exe: a tiny Windows program that proves the Deck/Proton test rig before the game
// exists (netplay/docs/TESTING_WINDOWS.md). Built by win_probe_build.sh with the same
// clang + xwin (x86_64-pc-windows-msvc) toolchain as ac6recomp.exe.
//
//   win_probe.exe <instance-dir> <lobby host> <lobby port> [echo port] [hold seconds]
//
// With hold seconds > 0 it then shows a small window for that long (tests win-stop.sh: the
// launcher closes the window, WM_CLOSE ends the probe, and logs "closed by WM_CLOSE").
//
// Writes <instance-dir>/probe_result.txt (and probe_unixpath.txt through a Unix-style path):
// exe path, working directory, path mapping, Winsock UDP to the lobby and to an echo port
// (reply = UDP reaches the Mac and back), the ICMP-unreachable behaviour of recvfrom
// (WSAECONNRESET with and without SIO_UDP_CONNRESET), and whether Vulkan (winevulkan) and
// D3D12 (vkd3d-proton) create a device.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <unknwn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

static FILE *out;
#define P(...) do { fprintf(out, __VA_ARGS__); fprintf(out, "\n"); fflush(out); \
                    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int udp_roundtrip(const char *host, int port, const char *label, int connreset_off) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) { P("%s: socket failed %d", label, WSAGetLastError()); return 0; }
  if (connreset_off) {
    BOOL f = FALSE; DWORD n = 0;
    int r = WSAIoctl(s, SIO_UDP_CONNRESET, &f, sizeof f, NULL, 0, &n, NULL, NULL);
    P("%s: SIO_UDP_CONNRESET off -> %d (err %d)", label, r, r ? WSAGetLastError() : 0);
  }
  DWORD tmo = 3000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
  struct sockaddr_in to = {0};
  to.sin_family = AF_INET; to.sin_port = htons((u_short)port);
  inet_pton(AF_INET, host, &to.sin_addr);
  char msg[64];
  int len = snprintf(msg, sizeof msg, "ac6np-win-probe %s %lu", label, GetTickCount());
  int sent = sendto(s, msg, len, 0, (struct sockaddr *)&to, sizeof to);
  struct sockaddr_in me = {0}; int ml = sizeof me;
  getsockname(s, (struct sockaddr *)&me, &ml);
  P("%s: sendto %s:%d -> %d (err %d) local port %d", label, host, port, sent,
    sent < 0 ? WSAGetLastError() : 0, ntohs(me.sin_port));
  char buf[512]; struct sockaddr_in from = {0}; int fl = sizeof from;
  int got = recvfrom(s, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
  int ok = 0;
  if (got >= 0) {
    buf[got] = 0;
    char fa[64]; inet_ntop(AF_INET, &from.sin_addr, fa, sizeof fa);
    P("%s: recvfrom %d bytes from %s:%d: %.80s", label, got, fa, ntohs(from.sin_port),
      got && (unsigned char)buf[0] >= 0x20 ? buf : "(binary)");
    ok = 1;
  } else {
    int e = WSAGetLastError();
    P("%s: recvfrom -> err %d%s", label, e,
      e == WSAETIMEDOUT ? " (timeout: no reply)" : e == WSAECONNRESET ? " (WSAECONNRESET: ICMP unreachable)" : "");
  }
  closesocket(s);
  return ok;
}

static void probe_vulkan(void) {
  HMODULE m = LoadLibraryA("vulkan-1.dll");
  if (!m) { P("vulkan: vulkan-1.dll not loadable (err %lu)", GetLastError()); return; }
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(m, "vkGetInstanceProcAddr");
  PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
  PFN_vkEnumerateInstanceVersion ev = (PFN_vkEnumerateInstanceVersion)gipa(NULL, "vkEnumerateInstanceVersion");
  uint32_t iv = 0; if (ev) ev(&iv);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "win_probe"; app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  VkInstance inst = NULL;
  VkResult r = ci(&ici, NULL, &inst);
  P("vulkan: loader %u.%u.%u, vkCreateInstance -> %d", VK_API_VERSION_MAJOR(iv),
    VK_API_VERSION_MINOR(iv), VK_API_VERSION_PATCH(iv), (int)r);
  if (r != VK_SUCCESS) return;
  PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
  PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
  PFN_vkDestroyInstance di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
  uint32_t n = 0; epd(inst, &n, NULL);
  VkPhysicalDevice devs[8]; if (n > 8) n = 8; epd(inst, &n, devs);
  for (uint32_t i = 0; i < n; i++) {
    VkPhysicalDeviceProperties p; gpp(devs[i], &p);
    P("vulkan: device %u: %s api %u.%u.%u driver 0x%x", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
      VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion);
  }
  di(inst, NULL);
}

typedef HRESULT (WINAPI *PFN_D3D12CreateDevice_)(void *, int, const GUID *, void **);
static const GUID IID_ID3D12Device_ = {0x189819f1, 0x1db6, 0x4b57, {0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7}};

static void probe_d3d12(void) {
  HMODULE m = LoadLibraryA("d3d12.dll");
  if (!m) { P("d3d12: d3d12.dll not loadable (err %lu)", GetLastError()); return; }
  char path[MAX_PATH] = {0}; GetModuleFileNameA(m, path, sizeof path);
  PFN_D3D12CreateDevice_ cd = (PFN_D3D12CreateDevice_)GetProcAddress(m, "D3D12CreateDevice");
  if (!cd) { P("d3d12: no D3D12CreateDevice in %s", path); return; }
  const int levels[] = {0xc200 /*12_2*/, 0xc100 /*12_1*/, 0xc000 /*12_0*/, 0xb100 /*11_1*/, 0xb000 /*11_0*/};
  for (int i = 0; i < 5; i++) {
    IUnknown *dev = NULL;
    HRESULT hr = cd(NULL, levels[i], &IID_ID3D12Device_, (void **)&dev);
    if (SUCCEEDED(hr)) {
      P("d3d12: %s D3D12CreateDevice ok at feature level 0x%x", path, levels[i]);
      dev->lpVtbl->Release(dev);
      return;
    }
    if (i == 4) P("d3d12: %s D3D12CreateDevice failed hr=0x%08lx", path, (unsigned long)hr);
  }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_CLOSE) { P("win_probe: closed by WM_CLOSE"); PostQuitMessage(0); return 0; }
  return DefWindowProcA(h, m, w, l);
}

static void hold_window(int seconds) {
  WNDCLASSA wc = {0};
  wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "ac6np_probe";
  wc.hbrBackground = (HBRUSH)GetStockObject(GRAY_BRUSH);
  RegisterClassA(&wc);
  HWND h = CreateWindowA("ac6np_probe", "win_probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 640, 60, 320, 180,
                         NULL, NULL, wc.hInstance, NULL);
  P("win_probe: window %p shown, holding %d s", (void *)h, seconds);
  SetTimer(h, 1, (UINT)seconds * 1000, NULL);
  MSG msg;
  while (GetMessageA(&msg, NULL, 0, 0) > 0) {
    if (msg.message == WM_TIMER) { P("win_probe: hold elapsed"); break; }
    DispatchMessageA(&msg);
  }
}

int main(int argc, char **argv) {
  if (argc < 4) { fprintf(stderr, "usage: win_probe.exe <instance-dir> <lobby host> <lobby port> [echo port]\n"); return 2; }
  const char *dir = argv[1], *host = argv[2];
  int port = atoi(argv[3]), echo = argc > 4 ? atoi(argv[4]) : 0;
  char p[1024];
  snprintf(p, sizeof p, "%s/probe_result.txt", dir);
  out = fopen(p, "w");
  if (!out) { fprintf(stderr, "cannot write %s\n", p); return 3; }
  char exe[MAX_PATH], cwd[MAX_PATH];
  GetModuleFileNameA(NULL, exe, sizeof exe); GetCurrentDirectoryA(sizeof cwd, cwd);
  P("win_probe: exe %s", exe);
  P("win_probe: cwd %s", cwd);
  P("win_probe: result file %s", p);
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  const char *(CDECL *wv)(void) = nt ? (const char *(CDECL *)(void))GetProcAddress(nt, "wine_get_version") : NULL;
  P("win_probe: wine %s", wv ? wv() : "(not wine)");
  // A Unix-style absolute path, as the Mac drivers send in repl commands ("screenshot /run/...").
  char unixp[1024];
  const char *u = getenv("AC6NP_PROBE_UNIX_DIR");
  if (u) {
    snprintf(unixp, sizeof unixp, "%s/probe_unixpath.txt", u);
    FILE *f = fopen(unixp, "w");
    char full[MAX_PATH] = {0}; GetFullPathNameA(unixp, sizeof full, full, NULL);
    if (f) { fprintf(f, "written via %s\n", unixp); fclose(f); }
    P("paths: fopen(\"%s\") -> %s (full path %s)", unixp, f ? "ok" : "FAILED", full);
  }
  // A read-only file the game needs (AC6NP_PROBE_READ, e.g. the ISO on Z:): open and read 2 KiB.
  const char *rd = getenv("AC6NP_PROBE_READ");
  if (rd) {
    HANDLE h = CreateFileA(rd, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz = {0}; char b[2048]; DWORD n = 0;
    if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &sz); ReadFile(h, b, sizeof b, &n, NULL); CloseHandle(h); }
    P("paths: read-only open \"%s\" -> %s, %lld bytes, read %lu", rd,
      h != INVALID_HANDLE_VALUE ? "ok" : "FAILED", (long long)sz.QuadPart, n);
  }
  WSADATA wsa;
  int wr = WSAStartup(MAKEWORD(2, 2), &wsa);
  P("winsock: WSAStartup -> %d (%s)", wr, wsa.szDescription);
  udp_roundtrip(host, port, "lobby", 0);
  if (echo) {
    udp_roundtrip(host, echo, "echo", 0);
    udp_roundtrip(host, echo + 1, "closed-port", 0);
    udp_roundtrip(host, echo + 1, "closed-port-connreset-off", 1);
  }
  WSACleanup();
  probe_vulkan();
  probe_d3d12();
  if (argc > 5 && atoi(argv[5]) > 0) hold_window(atoi(argv[5]));
  P("win_probe: done");
  fclose(out);
  return 0;
}

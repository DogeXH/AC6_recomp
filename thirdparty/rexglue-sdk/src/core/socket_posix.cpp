#include <rex/net/socket.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rex::net {

int socket_close(SocketHandle handle) {
  return close(static_cast<int>(handle));
}

int socket_ioctl(SocketHandle handle, uint32_t cmd, uint8_t* arg) {
  // Xbox sockets use the WinSock ioctl ABI. Linux and macOS use different
  // request numbers, so passing the guest command through leaves the socket
  // blocking even though ioctlsocket(FIONBIO) was requested.
  constexpr uint32_t kXboxFionbio = 0x8004667Eu;
  if (cmd == kXboxFionbio) {
    if (!arg) {
      errno = EFAULT;
      return -1;
    }

    // The argument lives in big-endian guest memory. FIONBIO only needs a
    // boolean, so normalize the four guest bytes into a native int.
    int nonblocking = arg[0] || arg[1] || arg[2] || arg[3];
    return ioctl(static_cast<int>(handle), FIONBIO, &nonblocking);
  }

  return ioctl(static_cast<int>(handle), static_cast<unsigned long>(cmd), arg);
}

}  // namespace rex::net

// Runtime test for src/netplay_input_driver.cpp against the SDK's real InputSystem
// (librexruntime), without the game, a window or a GPU. Built and run by run_driver_test.sh.
//
// Checks: the scripted pad merges into guest user 0 (not user 1), press/hold timing, stick and
// chord output, keystroke events, touch, wait_trace ignoring old trace lines, the progress
// file, screenshot failure without a presenter, the capture.now trigger protocol, and that
// Shutdown() releases everything.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/input/nop/nop_input_driver.h>

#include "netplay_input_driver.h"

// The netplay glue defines netplay_trace; a runtime built without it rejects nothing here and
// this stand-in is used. If the runtime has it, RegisterFlag rejects this duplicate and
// SetFlagByName/Query go to the runtime's own cvar (both reach it by name).
REXCVAR_DEFINE_STRING(netplay_trace, "", "Netplay", "test stand-in for the glue's cvar");

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using rex::X_RESULT;
using rex::X_STATUS;

static int g_failures = 0;
#define CHECK(x)                                                     \
  do {                                                               \
    if (!(x)) {                                                      \
      std::fprintf(stderr, "  FAILED line %d: %s\n", __LINE__, #x);  \
      ++g_failures;                                                  \
    }                                                                \
  } while (0)

static std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static double Ms(Clock::time_point since) {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: driver_runtime_test <empty-work-dir>\n");
    return 2;
  }
  const fs::path dir = fs::absolute(argv[1]);
  fs::create_directories(dir);
  const fs::path script = dir / "script.txt";
  const fs::path trace = dir / "trace.jsonl";
  const fs::path shots = dir / "shots";
  const fs::path flag = dir / "touched.flag";
  {
    std::ofstream(trace) << "{\"t_ms\":1,\"ev\":\"go\"}\n";  // from an "earlier run"
    std::ofstream out(script);
    out << "# runtime test\n"
           "wait 100\n"
           "press A 300\n"
           "wait 200\n"
           "hold LB+LSU 300\n"
           "touch " << flag.string() << "\n"
           "screenshot " << (dir / "shot.ppm").string() << "\n"
           "wait_trace \"ev\":\"go\" 5000\n"
           "log runtime test done\n";
  }
  CHECK(rex::cvar::SetFlagByName("netplay_trace", trace.string()));
  CHECK(rex::cvar::Query<std::string>("netplay_trace") == trace.string());

  rex::input::InputSystem input(nullptr);
  input.AddDriver(std::make_unique<rex::input::nop::NopInputDriver>(nullptr, 1));
  auto driver = ac6::netplay_input::CreateHarnessDriver(script.string(), shots.string());
  CHECK(driver != nullptr);
  CHECK(ac6::netplay_input::CreateHarnessDriver(script.string(), shots.string()) == nullptr);
  input.AddDriver(std::move(driver));
  input.SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
  CHECK(input.Setup() == X_STATUS_SUCCESS);
  CHECK(fs::is_directory(shots));

  const auto t0 = Clock::now();
  input.AttachWindow(nullptr);  // the runtime is ready: the script starts

  double a_down = -1, a_up = -1, lb_down = -1, lb_up = -1;
  bool saw_stick = false, saw_lb_without_stick = false, go_written = false;
  double flag_seen = -1;
  std::vector<uint16_t> keys_down, keys_up;
  while (Ms(t0) < 8000) {
    rex::input::X_INPUT_STATE st{};
    CHECK(input.GetState(0, &st) == X_ERROR_SUCCESS);
    const uint16_t buttons = st.gamepad.buttons;
    const int16_t ly = st.gamepad.thumb_ly;
    const double now = Ms(t0);
    if ((buttons & rex::input::X_INPUT_GAMEPAD_A) && a_down < 0) a_down = now;
    if (!(buttons & rex::input::X_INPUT_GAMEPAD_A) && a_down >= 0 && a_up < 0) a_up = now;
    if ((buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) && lb_down < 0) lb_down = now;
    if (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) {
      if (ly == 32767) saw_stick = true;
      if (ly == 0) saw_lb_without_stick = true;
    }
    if (!(buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) && lb_down >= 0 && lb_up < 0) {
      lb_up = now;
    }
    rex::input::X_INPUT_KEYSTROKE ks{};
    while (input.GetKeystroke(0, 0, &ks) == X_ERROR_SUCCESS) {
      const uint16_t vk = ks.virtual_key;
      const uint16_t flags = ks.flags;
      (flags & rex::input::X_INPUT_KEYSTROKE_KEYDOWN ? keys_down : keys_up).push_back(vk);
    }
    // The "go" line already in the trace must not count: append a new one 300 ms after the
    // flag appears and expect wait_trace to have waited for it.
    if (flag_seen < 0 && fs::exists(flag)) flag_seen = now;
    if (!go_written && flag_seen >= 0 && now - flag_seen >= 300) {
      std::ofstream(trace, std::ios::app) << "{\"t_ms\":2,\"ev\":\"go\"}\n";
      go_written = true;
    }
    if (ReadAll(script.string() + ".progress").find("\"ev\":\"finish\"") != std::string::npos) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  std::printf("A down %.0f ms, up %.0f ms; LB down %.0f ms, up %.0f ms\n", a_down, a_up, lb_down,
              lb_up);
  CHECK(a_down >= 80 && a_down < 400);
  CHECK(a_up - a_down >= 280 && a_up - a_down < 450);
  CHECK(lb_down - a_up >= 280 && lb_down - a_up < 500);  // 100 gap + 200 wait
  CHECK(lb_up - lb_down >= 280 && lb_up - lb_down < 450);
  CHECK(saw_stick && !saw_lb_without_stick);
  CHECK(fs::exists(flag));

  const std::vector<uint16_t> want_down = {0x5800, 0x5805, 0x5820};
  const std::vector<uint16_t> want_up = {0x5800, 0x5805, 0x5820};
  CHECK(keys_down == want_down);
  CHECK(keys_up == want_up);

  rex::input::X_INPUT_STATE other{};
  CHECK(input.GetState(1, &other) == X_ERROR_DEVICE_NOT_CONNECTED);  // synthetic pads: user 0

  const std::string progress = ReadAll(script.string() + ".progress");
  std::printf("progress:\n%s", progress.c_str());
  CHECK(progress.find("\"ev\":\"start\"") != std::string::npos);
  CHECK(progress.find("\"ev\":\"finish\",\"result\":\"ok\"") != std::string::npos);
  CHECK(progress.find("\"result\":\"error\",\"waited_ms\":0,\"detail\":\"no presenter\"") !=
        std::string::npos);
  CHECK(progress.find("\"cmd\":\"wait_trace \\\"ev\\\":\\\"go\\\" 5000\"") != std::string::npos);
  CHECK(!fs::exists(dir / "shot.ppm"));
  // waited_ms of the wait_trace step (line 8) shows the pre-existing "go" line was skipped.
  const size_t end8 = progress.find("\"line\":8,\"ev\":\"end\",\"result\":\"ok\",\"waited_ms\":");
  CHECK(end8 != std::string::npos);
  if (end8 != std::string::npos) {
    const long waited = std::atol(progress.c_str() + progress.find("waited_ms\":", end8) + 11);
    std::printf("wait_trace waited %ld ms\n", waited);
    CHECK(waited >= 250);
  }

  // On-demand capture: without a presenter it reports failure and still clears the trigger.
  std::ofstream(shots / "capture.now") << "";
  const auto t1 = Clock::now();
  while (fs::exists(shots / "capture.now") && Ms(t1) < 3000) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(!fs::exists(shots / "capture.now"));
  const std::string last = ReadAll(shots / "capture.last");
  std::printf("capture.last: %s", last.c_str());
  CHECK(last.find("\"index\":0,\"ok\":false") != std::string::npos);
  CHECK(last.find("\"error\":\"no presenter\"") != std::string::npos);

  ac6::netplay_input::Shutdown();
  ac6::netplay_input::Shutdown();  // idempotent
  rex::input::X_INPUT_STATE after{};
  CHECK(input.GetState(0, &after) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(after.gamepad.buttons) == 0);

  std::printf("%s (%d failed checks)\n", g_failures ? "FAIL" : "ok", g_failures);
  return g_failures ? 1 : 0;
}

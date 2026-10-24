// Netplay test harness: scripted input driver and in-process screenshots (test tooling only).
//
// Off by default. Two app cvars turn it on:
//   netplay_input_script   = path of an input script (netplay_input_script.h describes the format)
//   netplay_screenshot_dir = directory polled for "capture.now" trigger files
// When either is set, Install() adds a ScriptedInputDriver to the default input system. It
// reports one synthetic pad, which the SDK merges with the real controllers of guest user 0, so
// physical controls keep working. Screenshots read the presenter's guest output (no window or
// screen capture is involved). See netplay/docs/TESTING.md.
#pragma once

#include <memory>
#include <string>

namespace rex {
class Runtime;
struct RuntimeConfig;
}
namespace rex::input {
class InputDriver;
}

namespace ac6::netplay_input {

// Call from Ac6recompApp::OnPreSetup. Wraps config.input_factory when a harness cvar is set;
// does nothing otherwise.
void Install(rex::RuntimeConfig& config);

// The driver Install() adds to the input system, with its script and screenshot threads
// started (they wait until the driver sees a window or the first poll). One per process:
// returns nullptr if one exists. Install() uses it; tests call it with their own InputSystem.
std::unique_ptr<rex::input::InputDriver> CreateHarnessDriver(const std::string& script_path,
                                                             const std::string& screenshot_dir);

// Call from Ac6recompApp::OnPostSetup: screenshots read the presenter through this runtime (the
// vendored SDK has no Runtime singleton). Shutdown() clears it.
void SetRuntime(rex::Runtime* runtime);

// Call from Ac6recompApp::OnShutdown, before the graphics system and its presenter go away.
// Stops the script and screenshot threads and releases every scripted input. Idempotent.
void Shutdown();

}  // namespace ac6::netplay_input

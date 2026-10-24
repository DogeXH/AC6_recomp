// Netplay test harness: scripted controller input (parser, runner and file helpers).
//
// This half has no rex dependencies so it can be unit-tested on its own
// (netplay/tools/input_test). netplay_input_driver.{h,cpp} wires it into the runtime.
//
// Script format (one command per line; see netplay/docs/TESTING.md):
//   # comment                      full-line comments only
//   wait <ms>
//   press <inputs> [hold_ms]       hold (default press_ms), release, then pause gap_ms
//   hold <inputs> <ms>             same as press with an explicit hold time
//   wait_file <path> <timeout_ms>
//   wait_trace <substring> <timeout_ms>
//   log <text>
//   touch <path>                   create/update a file (to signal another instance)
//   loop_start <n> / loop_end      n >= 1, nesting allowed
//   screenshot <path.ppm>
//   screenshot_every <ms> <dir>    periodic <dir>/frame_<NNNN>.ppm
//   screenshot_stop
//   set press_ms <ms> | set gap_ms <ms>
//   repl <dir>                     interactive: run <dir>/cmd_NNNN.txt files as they appear
//                                  (N = 1, 2, ...; write them atomically via rename). After each
//                                  file the runner writes <dir>/done_NNNN.txt ("ok" or
//                                  "error: ..."); a file containing just "end" leaves the repl.
// <inputs> is one name or several joined with '+': A B X Y LB RB LT RT START BACK UP DOWN LEFT
// RIGHT LS RS, and stick directions LSU LSD LSL LSR RSU RSD RSL RSR (case-insensitive).
// For wait_file and wait_trace the last token is the timeout and everything between the command
// and the timeout is the path or substring, taken literally (spaces and quotes included).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ac6::netplay_input {

// --- Inputs -------------------------------------------------------------------------------

// Bit set of scripted inputs. The low 16 bits equal the XInput button bits.
enum Input : uint32_t {
  kDpadUp = 0x0001,
  kDpadDown = 0x0002,
  kDpadLeft = 0x0004,
  kDpadRight = 0x0008,
  kStart = 0x0010,
  kBack = 0x0020,
  kLeftThumb = 0x0040,
  kRightThumb = 0x0080,
  kLeftShoulder = 0x0100,
  kRightShoulder = 0x0200,
  kButtonA = 0x1000,
  kButtonB = 0x2000,
  kButtonX = 0x4000,
  kButtonY = 0x8000,
  kLeftTrigger = 1u << 16,
  kRightTrigger = 1u << 17,
  kLeftStickUp = 1u << 18,
  kLeftStickDown = 1u << 19,
  kLeftStickLeft = 1u << 20,
  kLeftStickRight = 1u << 21,
  kRightStickUp = 1u << 22,
  kRightStickDown = 1u << 23,
  kRightStickLeft = 1u << 24,
  kRightStickRight = 1u << 25,
};
inline constexpr uint32_t kButtonMask = 0xF3FF;  // XInput buttons we drive (no guide)
inline constexpr uint32_t kAllInputs = kButtonMask | 0x03FF0000u;

// Host-endian controller state produced from an input set.
struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};

// Full deflection for stick directions; opposite directions cancel out.
PadState PadFromInputs(uint32_t inputs);

// One name ("A", "lsu") or a '+' chord ("LB+RB"). Returns false on an unknown name.
bool ParseInputs(std::string_view text, uint32_t* out);
// Canonical names for logs ("LB+RB"), "none" for 0.
std::string InputsToString(uint32_t inputs);

// XInputGetKeystroke-style events for an input transition (VK_PAD_* codes; diagonals are
// reported as separate directions).
struct KeyEvent {
  uint16_t virtual_key = 0;
  bool down = false;
};
void KeyEventsForTransition(uint32_t before, uint32_t after, std::vector<KeyEvent>* out);

// --- Script -------------------------------------------------------------------------------

enum class Op : uint8_t {
  kWait,
  kPress,
  kWaitFile,
  kWaitTrace,
  kLog,
  kTouch,
  kLoopStart,
  kLoopEnd,
  kScreenshot,
  kScreenshotEvery,
  kScreenshotStop,
  kSet,
  kRepl,
};

struct Step {
  Op op = Op::kWait;
  int line = 0;              // 1-based script line
  std::string source;        // trimmed line text, for logs and progress
  uint32_t ms = 0;           // wait/hold/timeout/interval ms; loop count; set value
  bool default_hold = false;  // kPress without an explicit hold time
  uint32_t inputs = 0;       // kPress
  std::string arg;           // path, substring, text, dir; kSet: setting name
  uint32_t aux = 0;          // kRepl: index of the next command file
};

struct Script {
  std::vector<Step> steps;
};

inline constexpr size_t kMaxScriptBytes = 1 << 20;
inline constexpr size_t kMaxSteps = 10000;
inline constexpr size_t kMaxLineBytes = 4096;
inline constexpr size_t kMaxLoopDepth = 16;
inline constexpr uint32_t kMaxLoopCount = 1000000;
inline constexpr uint32_t kMaxMs = 24u * 60 * 60 * 1000;
inline constexpr uint32_t kMinScreenshotIntervalMs = 100;

// Parses a whole script. On failure returns false and sets *error to "line N: reason".
bool ParseScript(std::string_view text, Script* out, std::string* error);

// --- Runner -------------------------------------------------------------------------------

enum class WaitResult { kReady, kNotYet, kUnavailable };

struct ProgressEvent {
  enum class Kind { kBegin, kEnd, kFinish };
  Kind kind = Kind::kBegin;
  uint64_t seq = 0;        // executed-step counter (loops repeat lines)
  size_t index = 0;        // step index in the script
  int line = 0;
  uint64_t elapsed_ms = 0;  // since the script started
  std::string command;     // kBegin: the step's source text
  std::string result;      // kEnd: "ok", "timeout", "error"; kFinish: "ok", "failed", "stopped"
  std::string detail;      // error or failure text
  uint64_t waited_ms = 0;  // kEnd: time spent in the step
};

// Everything the runner needs from the outside world. Called on the runner's thread.
class ScriptHost {
 public:
  virtual ~ScriptHost() = default;
  virtual void SetInputs(uint32_t inputs) = 0;
  virtual bool FileExists(const std::string& path) = 0;
  virtual bool Touch(const std::string& path, std::string* error) = 0;
  // kReady once a new trace line contains `needle`; kUnavailable when no trace is configured.
  virtual WaitResult PollTrace(std::string_view needle) = 0;
  virtual bool Screenshot(const std::string& path, std::string* error) = 0;
  virtual bool StartScreenshots(uint32_t interval_ms, const std::string& dir,
                                std::string* error) = 0;
  virtual void StopScreenshots() = 0;
  virtual void Log(std::string_view message) = 0;
  virtual void Progress(const ProgressEvent& event) = 0;
};

class ScriptRunner {
 public:
  enum class State { kRunning, kFinished, kFailed, kStopped };

  static constexpr uint32_t kDefaultPressMs = 100;
  static constexpr uint32_t kDefaultGapMs = 100;
  static constexpr uint32_t kWaitPollMs = 50;

  ScriptRunner(Script script, ScriptHost* host);

  // Runs every step that is due at `now_ms` (a monotonic clock). Returns how long the caller
  // may sleep before polling again, or std::nullopt once the script has ended.
  std::optional<uint32_t> Poll(uint64_t now_ms);
  // Releases all inputs and ends the script (no-op once ended).
  void Stop(uint64_t now_ms);

  State state() const { return state_; }
  const std::string& failure() const { return failure_; }

 private:
  enum class Phase { kIdle, kSleeping, kHolding, kGap, kWaiting };
  struct Loop {
    size_t start;
    uint32_t remaining;
  };

  // Starts steps_[pc_]; returns false when the step is still in progress.
  bool BeginStep(uint64_t now_ms);
  // Continues the step in progress; returns false while it still needs time.
  bool ContinueStep(uint64_t now_ms);
  void EndStep(uint64_t now_ms, std::string result, std::string detail = {});
  void Finish(uint64_t now_ms, State state, std::string detail);
  bool PollRepl(uint64_t now_ms);
  uint64_t Elapsed(uint64_t now_ms) const { return now_ms - start_ms_; }

  Script script_;
  ScriptHost* host_;
  State state_ = State::kRunning;
  std::string failure_;
  bool started_ = false;
  uint64_t start_ms_ = 0;
  size_t pc_ = 0;
  Phase phase_ = Phase::kIdle;
  uint64_t phase_until_ms_ = 0;
  uint64_t step_began_ms_ = 0;
  uint64_t seq_ = 0;
  uint32_t press_ms_ = kDefaultPressMs;
  uint32_t gap_ms_ = kDefaultGapMs;
  std::vector<Loop> loops_;
};

// --- File helpers -------------------------------------------------------------------------

// Follows a JSONL file that other code appends to. Lines are only considered once complete.
class TraceTail {
 public:
  explicit TraceTail(std::filesystem::path path);
  // Skips everything currently in the file (content from earlier runs).
  void SkipExisting();
  // Looks for the first complete line after the previous match that contains `needle`; on a
  // match, the cursor moves past that line. A shrinking file is treated as rewritten from 0.
  bool Find(std::string_view needle);
  const std::filesystem::path& path() const { return path_; }

  static constexpr size_t kMaxReadPerPoll = 1 << 20;
  static constexpr size_t kMaxLineBytes = 64 * 1024;

 private:
  std::filesystem::path path_;
  uint64_t offset_ = 0;   // bytes consumed (including `partial_`)
  std::string partial_;   // incomplete last line
};

// Writes a binary PPM (P6) from R8G8B8X8 rows. Writes `<path>.tmp` and renames it, so readers
// never see a partial file.
bool WritePpm(const std::filesystem::path& path, uint32_t width, uint32_t height, size_t stride,
              const std::vector<uint8_t>& rgbx, std::string* error);

// 1 + the largest N among files named <prefix><digits><suffix> in `dir` (0 if none).
uint32_t NextFreeIndex(const std::filesystem::path& dir, std::string_view prefix,
                       std::string_view suffix);
// "<prefix><NNNN><suffix>" with at least four digits.
std::string IndexedName(std::string_view prefix, uint32_t index, std::string_view suffix);

std::string JsonEscape(std::string_view text);

}  // namespace ac6::netplay_input

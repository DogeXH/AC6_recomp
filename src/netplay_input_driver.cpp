// Netplay test harness: scripted input driver and in-process screenshots (test tooling only).
// See netplay_input_driver.h and netplay/docs/TESTING.md.

#include "netplay_input_driver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <native/ui/presenter.h>

#include "netplay_input_script.h"
#include "render_hooks.h"

REXCVAR_DEFINE_STRING(netplay_input_script, "", "Netplay",
                      "Test harness only: path to a scripted input file. Empty = off. Scripted "
                      "input is merged with real controllers, which keep working.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(netplay_screenshot_dir, "", "Netplay",
                      "Test harness only: directory polled for a capture.now trigger file; each "
                      "trigger writes capture_NNNN.ppm there. Empty = off.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXLOG_DEFINE_CATEGORY(netinput)
#define NETINPUT_INFO(...) REXLOG_CAT_INFO(::rex::log::netinput(), __VA_ARGS__)
#define NETINPUT_WARN(...) REXLOG_CAT_WARN(::rex::log::netinput(), __VA_ARGS__)
#define NETINPUT_ERROR(...) REXLOG_CAT_ERROR(::rex::log::netinput(), __VA_ARGS__)

namespace {
// Register the category during static initialisation, before InitLogging, so a
// `[log.levels] netinput = "..."` entry in the config counts as an explicit level.
[[maybe_unused]] const rex::LogCategoryId g_netinput_category = rex::log::netinput();
}  // namespace

namespace ac6::netplay_input {

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;

// The vendored SDK (Linux/Steam Deck, Windows) addresses pads by guest user slot; the scripted
// pad answers for user 0 only and InputSystem::GetState merges it with the real pads there.
constexpr uint32_t kScriptUser = 0;
std::atomic<rex::Runtime*>& RuntimeSlot();
constexpr uint32_t kTriggerPollMs = 250;
constexpr uint32_t kMaxPeriodicFrames = 20000;  // about 11 min at 30 fps
constexpr size_t kMaxQueuedKeystrokes = 64;
constexpr uint64_t kKeystrokeMaxAgeMs = 1000;

uint64_t SteadyMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

uint64_t UnixMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count());
}

bool ReadSmallFile(const std::filesystem::path& path, size_t limit, std::string* out,
                   std::string* error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot open " + path.string();
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) {
    *error = "cannot read " + path.string();
    return false;
  }
  if (out->size() > limit) {
    *error = path.string() + " is larger than " + std::to_string(limit) + " bytes";
    return false;
  }
  return true;
}

// Writes a small text file atomically (tmp + rename).
void WriteFileAtomic(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) std::filesystem::remove(tmp, ec);
}

// Owns the script and screenshot threads. Shared by the input driver (guest-thread reads) and
// the module-level Install()/Shutdown() pair.
class Service final : public ScriptHost {
 public:
  Service(std::filesystem::path script_path, std::filesystem::path screenshot_dir)
      : script_path_(std::move(script_path)), screenshot_dir_(std::move(screenshot_dir)) {}
  ~Service() override { Stop(); }

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;

  void Start() {
    if (!script_path_.empty()) script_thread_ = std::thread([this] { ScriptThreadMain(); });
    capture_thread_ = std::thread([this] { CaptureThreadMain(); });
  }

  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (script_thread_.joinable()) script_thread_.join();
    if (capture_thread_.joinable()) capture_thread_.join();
    // The runner releases its inputs when it stops; make sure nothing stays held regardless.
    const uint64_t s = state_.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(s) != 0) {
      state_.store(((s >> 32) + 1) << 32, std::memory_order_release);
    }
  }

  // The runtime is set up (window attached or the guest polled input): captures can work and
  // the script may begin.
  void MarkReady() {
    if (ready_flag_.load(std::memory_order_acquire)) return;
    {
      std::lock_guard lock(mutex_);
      if (ready_) return;
      ready_ = true;
      ready_flag_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    NETINPUT_INFO("netplay_input: runtime ready");
  }

  // --- Guest-thread side (never blocks for long) ---

  // (packet_number << 32) | inputs
  uint64_t Snapshot() const { return state_.load(std::memory_order_acquire); }

  bool PopKeystroke(KeyEvent* out) {
    const uint64_t now = SteadyMs();
    std::lock_guard lock(key_mutex_);
    while (!keys_.empty() && now - keys_.front().queued_ms > kKeystrokeMaxAgeMs) keys_.pop_front();
    if (keys_.empty()) return false;
    *out = keys_.front().event;
    keys_.pop_front();
    return true;
  }

  // --- ScriptHost (script thread) ---

  void SetInputs(uint32_t inputs) override {
    if (inputs == last_inputs_) return;
    std::vector<KeyEvent> events;
    KeyEventsForTransition(last_inputs_, inputs, &events);
    {
      const uint64_t now = SteadyMs();
      std::lock_guard lock(key_mutex_);
      for (const KeyEvent& e : events) {
        if (keys_.size() >= kMaxQueuedKeystrokes) keys_.pop_front();
        keys_.push_back({e, now});
      }
    }
    const uint64_t packet = (state_.load(std::memory_order_relaxed) >> 32) + 1;
    state_.store((packet << 32) | inputs, std::memory_order_release);
    last_inputs_ = inputs;
  }

  bool FileExists(const std::string& path) override {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(path), ec);
  }

  bool Touch(const std::string& path, std::string* error) override {
    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << UnixMs() << '\n';
    out.close();
    if (!out) {
      *error = "cannot write " + path;
      return false;
    }
    return true;
  }

  WaitResult PollTrace(std::string_view needle) override {
    if (!trace_) return WaitResult::kUnavailable;
    return trace_->Find(needle) ? WaitResult::kReady : WaitResult::kNotYet;
  }

  bool Screenshot(const std::string& path, std::string* error) override {
    return Capture(std::filesystem::path(path), error);
  }

  bool StartScreenshots(uint32_t interval_ms, const std::string& dir, std::string* error) override {
    const std::filesystem::path d(dir);
    std::error_code ec;
    std::filesystem::create_directories(d, ec);
    if (!std::filesystem::is_directory(d, ec)) {
      *error = "cannot create directory " + dir;
      return false;
    }
    const uint32_t first = NextFreeIndex(d, "frame_", ".ppm");
    {
      std::lock_guard lock(mutex_);
      periodic_active_ = true;
      periodic_interval_ms_ = std::max(interval_ms, kMinScreenshotIntervalMs);
      periodic_dir_ = d;
      periodic_next_ms_ = SteadyMs();
      periodic_index_ = first;
      periodic_count_ = 0;
    }
    cv_.notify_all();
    NETINPUT_INFO("netplay_input: capturing every {} ms to {} from frame {}", interval_ms, dir, first);
    return true;
  }

  void StopScreenshots() override {
    std::lock_guard lock(mutex_);
    periodic_active_ = false;
  }

  void Log(std::string_view message) override { NETINPUT_INFO("netplay_input: {}", message); }

  void Progress(const ProgressEvent& e) override {
    std::string json = "{\"unix_ms\":" + std::to_string(UnixMs()) +
                       ",\"elapsed_ms\":" + std::to_string(e.elapsed_ms) +
                       ",\"seq\":" + std::to_string(e.seq) + ",\"step\":" + std::to_string(e.index) +
                       ",\"line\":" + std::to_string(e.line);
    switch (e.kind) {
      case ProgressEvent::Kind::kBegin:
        json += ",\"ev\":\"begin\",\"cmd\":\"" + JsonEscape(e.command) + "\"";
        break;
      case ProgressEvent::Kind::kEnd:
        json += ",\"ev\":\"end\",\"result\":\"" + JsonEscape(e.result) +
                "\",\"waited_ms\":" + std::to_string(e.waited_ms);
        break;
      case ProgressEvent::Kind::kFinish:
        json += ",\"ev\":\"finish\",\"result\":\"" + JsonEscape(e.result) + "\"";
        break;
    }
    if (!e.detail.empty()) json += ",\"detail\":\"" + JsonEscape(e.detail) + "\"";
    json += "}";
    WriteProgressLine(json);
  }

 private:
  struct QueuedKey {
    KeyEvent event;
    uint64_t queued_ms;
  };

  void WriteProgressLine(const std::string& json) {
    if (!progress_.is_open()) return;
    progress_ << json << '\n';
    progress_.flush();
  }

  void ScriptThreadMain() {
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [&] { return stopping_ || ready_; });
      if (stopping_) return;
    }

    std::filesystem::path progress_path = script_path_;
    progress_path += ".progress";
    progress_.open(progress_path, std::ios::binary | std::ios::trunc);
    if (!progress_) NETINPUT_WARN("netplay_input: cannot write {}", progress_path.string());

    std::string text;
    std::string error;
    Script script;
    if (!ReadSmallFile(script_path_, kMaxScriptBytes, &text, &error) ||
        !ParseScript(text, &script, &error)) {
      NETINPUT_ERROR("netplay_input: script {} not run: {}", script_path_.string(), error);
      WriteProgressLine("{\"unix_ms\":" + std::to_string(UnixMs()) +
                        ",\"ev\":\"error\",\"detail\":\"" + JsonEscape(error) + "\"}");
      return;
    }
    WriteProgressLine("{\"unix_ms\":" + std::to_string(UnixMs()) +
                      ",\"ev\":\"start\",\"script\":\"" + JsonEscape(script_path_.string()) +
                      "\",\"steps\":" + std::to_string(script.steps.size()) + "}");

    // The netplay core appends to this file; only lines written after this point count.
    const std::string trace_path = rex::cvar::Query<std::string>("netplay_trace");
    if (!trace_path.empty()) {
      trace_ = std::make_unique<TraceTail>(std::filesystem::path(trace_path));
      trace_->SkipExisting();
    }

    NETINPUT_INFO("netplay_input: running {} ({} steps)", script_path_.string(), script.steps.size());
    ScriptRunner runner(std::move(script), this);
    while (true) {
      const std::optional<uint32_t> delay = runner.Poll(SteadyMs());
      if (!delay) break;
      std::unique_lock lock(mutex_);
      if (cv_.wait_for(lock, std::chrono::milliseconds(std::min<uint32_t>(*delay, 1000)),
                       [&] { return stopping_; })) {
        lock.unlock();
        runner.Stop(SteadyMs());
        break;
      }
    }
  }

  void CaptureThreadMain() {
    std::unique_lock lock(mutex_);
    uint64_t next_trigger_poll_ms = 0;
    uint64_t next_frames_log_ms = 0;
    uint64_t last_frames = 0;
    while (!stopping_) {
      const uint64_t now = SteadyMs();
      // Every 2 s: presented frames since the last line and the current rate, so netplay
      // traces (unix_ms) can be lined up with the game's frame rate on both machines.
      if (ready_ && now >= next_frames_log_ms) {
        const ac6::FrameStats fs = ac6::GetFrameStats();
        if (next_frames_log_ms) {
          NETINPUT_INFO("netplay_input: frames_log unix_ms {} frames {} (+{}) fps {:.1f}",
                        UnixMs(), fs.frame_count, fs.frame_count - last_frames, fs.fps);
        }
        last_frames = fs.frame_count;
        next_frames_log_ms = now + 2000;
      }
      if (ready_ && !screenshot_dir_.empty() && now >= next_trigger_poll_ms) {
        next_trigger_poll_ms = now + kTriggerPollMs;
        lock.unlock();
        HandleTrigger();
        lock.lock();
        continue;
      }
      if (ready_ && periodic_active_ && now >= periodic_next_ms_) {
        const std::filesystem::path path =
            periodic_dir_ / IndexedName("frame_", periodic_index_++, ".ppm");
        periodic_next_ms_ += periodic_interval_ms_;
        if (periodic_next_ms_ <= now) periodic_next_ms_ = now + periodic_interval_ms_;
        if (++periodic_count_ >= kMaxPeriodicFrames) {
          periodic_active_ = false;
          NETINPUT_WARN("netplay_input: periodic capture stopped after {} frames", kMaxPeriodicFrames);
        }
        lock.unlock();
        std::string error;
        if (!Capture(path, &error)) {
          NETINPUT_WARN("netplay_input: capture {} failed: {}", path.string(), error);
        }
        lock.lock();
        continue;
      }
      uint64_t wake = now + 1000;
      if (ready_) wake = std::min(wake, next_frames_log_ms);
      if (ready_ && !screenshot_dir_.empty()) wake = std::min(wake, next_trigger_poll_ms);
      if (ready_ && periodic_active_) wake = std::min(wake, periodic_next_ms_);
      cv_.wait_for(lock, std::chrono::milliseconds(wake > now ? wake - now : 0));
    }
  }

  // capture.now -> capture_NNNN.ppm, then capture.last (status JSON), then the trigger is
  // removed: its disappearance tells the requester that the capture finished.
  void HandleTrigger() {
    const std::filesystem::path trigger = screenshot_dir_ / "capture.now";
    std::error_code ec;
    if (!std::filesystem::exists(trigger, ec)) return;
    if (!trigger_index_known_) {
      trigger_index_ = NextFreeIndex(screenshot_dir_, "capture_", ".ppm");
      trigger_index_known_ = true;
    }
    const uint32_t index = trigger_index_++;
    const std::filesystem::path out = screenshot_dir_ / IndexedName("capture_", index, ".ppm");
    std::string error;
    const bool ok = Capture(out, &error);
    std::string status = "{\"unix_ms\":" + std::to_string(UnixMs()) +
                         ",\"index\":" + std::to_string(index) +
                         ",\"ok\":" + (ok ? "true" : "false") + ",\"path\":\"" +
                         JsonEscape(out.string()) + "\"";
    if (!ok) status += ",\"error\":\"" + JsonEscape(error) + "\"";
    status += "}\n";
    WriteFileAtomic(screenshot_dir_ / "capture.last", status);
    std::filesystem::remove(trigger, ec);
    if (ok) {
      NETINPUT_INFO("netplay_input: wrote {}", out.string());
    } else {
      NETINPUT_WARN("netplay_input: capture {} failed: {}", out.string(), error);
    }
  }

  // Any thread. The presenter's CaptureGuestOutput may be called from any thread, also
  // concurrently (rex/ui/presenter.h); Shutdown() joins our threads before it is destroyed.
  bool Capture(const std::filesystem::path& path, std::string* error) {
    if (!ready_flag_.load(std::memory_order_acquire)) {
      *error = "runtime not ready";
      return false;
    }
    rex::Runtime* runtime = RuntimeSlot().load(std::memory_order_acquire);
    rex::system::IGraphicsSystem* graphics = runtime ? runtime->graphics_system() : nullptr;
    rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
    if (!presenter) {
      *error = "no presenter";
      return false;
    }
    rex::ui::RawImage image;
    if (!presenter->CaptureGuestOutput(image)) {
      *error = "no guest output yet";
      return false;
    }
    if (path.has_parent_path()) {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    return WritePpm(path, image.width, image.height, image.stride, image.data, error);
  }

  const std::filesystem::path script_path_;
  const std::filesystem::path screenshot_dir_;

  std::atomic<uint64_t> state_{0};
  std::atomic<bool> ready_flag_{false};

  std::mutex key_mutex_;
  std::deque<QueuedKey> keys_;

  std::mutex mutex_;  // guards the fields below
  std::condition_variable cv_;
  bool stopping_ = false;
  bool ready_ = false;
  bool periodic_active_ = false;
  uint32_t periodic_interval_ms_ = 0;
  std::filesystem::path periodic_dir_;
  uint64_t periodic_next_ms_ = 0;
  uint32_t periodic_index_ = 0;
  uint32_t periodic_count_ = 0;

  // Script thread only.
  uint32_t last_inputs_ = 0;
  std::unique_ptr<TraceTail> trace_;
  std::ofstream progress_;

  // Capture thread only.
  uint32_t trigger_index_ = 0;
  bool trigger_index_known_ = false;

  std::thread script_thread_;
  std::thread capture_thread_;
};

// One scripted pad for guest user 0. InputSystem::GetState merges it with the physical pads of
// that user (buttons OR, larger stick deflection wins), so normal controls keep working.
class ScriptedInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptedInputDriver(std::shared_ptr<Service> service)
      : InputDriver(nullptr, 0), service_(std::move(service)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override {
    if (user_index != kScriptUser) return X_ERROR_DEVICE_NOT_CONNECTED;
    service_->MarkReady();
    if (out_state) {
      const uint64_t snapshot = service_->Snapshot();
      const PadState pad = PadFromInputs(static_cast<uint32_t>(snapshot));
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = static_cast<uint32_t>(snapshot >> 32);
      out_state->gamepad.buttons = pad.buttons;
      out_state->gamepad.left_trigger = pad.left_trigger;
      out_state->gamepad.right_trigger = pad.right_trigger;
      out_state->gamepad.thumb_lx = pad.thumb_lx;
      out_state->gamepad.thumb_ly = pad.thumb_ly;
      out_state->gamepad.thumb_rx = pad.thumb_rx;
      out_state->gamepad.thumb_ry = pad.thumb_ry;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (user_index != kScriptUser) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
      out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  // Never claims vibration: InputSystem::SetState stops at the first success, and the real pad
  // (added before this driver) must keep its rumble.
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return user_index == kScriptUser ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    if (user_index != kScriptUser) return X_ERROR_DEVICE_NOT_CONNECTED;
    KeyEvent key;
    if (!out_keystroke || !service_->PopKeystroke(&key)) return X_ERROR_EMPTY;
    std::memset(out_keystroke, 0, sizeof(*out_keystroke));
    out_keystroke->virtual_key = key.virtual_key;
    out_keystroke->flags = static_cast<uint16_t>(key.down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN
                                                          : rex::input::X_INPUT_KEYSTROKE_KEYUP);
    return X_ERROR_SUCCESS;
  }

  void OnWindowAvailable(rex::ui::Window* window) override {
    (void)window;
    service_->MarkReady();
  }

 private:
  std::shared_ptr<Service> service_;
};

std::atomic<rex::Runtime*>& RuntimeSlot() {
  static std::atomic<rex::Runtime*> runtime{nullptr};
  return runtime;
}

std::shared_ptr<Service>& InstalledService() {
  static std::shared_ptr<Service> service;
  return service;
}

}  // namespace

std::unique_ptr<rex::input::InputDriver> CreateHarnessDriver(const std::string& script_path,
                                                             const std::string& screenshot_dir) {
  if (InstalledService()) return nullptr;
  if (!screenshot_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(screenshot_dir), ec);
    if (ec) NETINPUT_WARN("netplay_input: cannot create {}: {}", screenshot_dir, ec.message());
  }
  auto service = std::make_shared<Service>(std::filesystem::path(script_path),
                                           std::filesystem::path(screenshot_dir));
  service->Start();
  InstalledService() = service;
  return std::make_unique<ScriptedInputDriver>(std::move(service));
}

void Install(rex::RuntimeConfig& config) {
  const std::string script = REXCVAR_GET(netplay_input_script);
  const std::string shots = REXCVAR_GET(netplay_screenshot_dir);
  if (script.empty() && shots.empty()) return;

  // Test tooling that was asked for: keep its log lines even under ac6_performance_mode.
  rex::SetCategoryLevel(rex::log::netinput(), spdlog::level::info);
  NETINPUT_INFO("netplay_input: harness enabled (script '{}', screenshot dir '{}')", script, shots);

  // The SDK's own default factory is CreateDefaultInputSystem; this app does not install
  // another one, so the scripted pad is simply added on top of the default drivers.
  config.input_factory = [script, shots](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    std::unique_ptr<rex::input::InputSystem> input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (input && !tool_mode) {
      if (auto driver = CreateHarnessDriver(script, shots)) input->AddDriver(std::move(driver));
    }
    return input;
  };
}

void SetRuntime(rex::Runtime* runtime) {
  RuntimeSlot().store(runtime, std::memory_order_release);
}

void Shutdown() {
  std::shared_ptr<Service> service = std::move(InstalledService());
  InstalledService().reset();
  if (service) service->Stop();
  RuntimeSlot().store(nullptr, std::memory_order_release);
}

}  // namespace ac6::netplay_input

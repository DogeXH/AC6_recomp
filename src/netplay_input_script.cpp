// Netplay test harness: scripted controller input (parser, runner and file helpers).
// See netplay_input_script.h for the script format.

#include "netplay_input_script.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace ac6::netplay_input {

namespace {

struct NamedInput {
  std::string_view name;
  uint32_t bits;
  uint16_t virtual_key;  // VK_PAD_* (XInput.h)
};

constexpr std::array<NamedInput, 24> kNamedInputs = {{
    {"A", kButtonA, 0x5800},
    {"B", kButtonB, 0x5801},
    {"X", kButtonX, 0x5802},
    {"Y", kButtonY, 0x5803},
    {"RB", kRightShoulder, 0x5804},
    {"LB", kLeftShoulder, 0x5805},
    {"LT", kLeftTrigger, 0x5806},
    {"RT", kRightTrigger, 0x5807},
    {"UP", kDpadUp, 0x5810},
    {"DOWN", kDpadDown, 0x5811},
    {"LEFT", kDpadLeft, 0x5812},
    {"RIGHT", kDpadRight, 0x5813},
    {"START", kStart, 0x5814},
    {"BACK", kBack, 0x5815},
    {"LS", kLeftThumb, 0x5816},
    {"RS", kRightThumb, 0x5817},
    {"LSU", kLeftStickUp, 0x5820},
    {"LSD", kLeftStickDown, 0x5821},
    {"LSR", kLeftStickRight, 0x5822},
    {"LSL", kLeftStickLeft, 0x5823},
    {"RSU", kRightStickUp, 0x5830},
    {"RSD", kRightStickDown, 0x5831},
    {"RSR", kRightStickRight, 0x5832},
    {"RSL", kRightStickLeft, 0x5833},
}};

constexpr int kMaxInstantStepsPerPoll = 256;

char AsciiUpper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (AsciiUpper(a[i]) != AsciiUpper(b[i])) return false;
  }
  return true;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string_view Trim(std::string_view s) {
  while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
  while (!s.empty() && IsSpace(s.back())) s.remove_suffix(1);
  return s;
}

// Splits off the first whitespace-delimited token; `rest` is trimmed.
std::pair<std::string_view, std::string_view> SplitFirst(std::string_view s) {
  s = Trim(s);
  size_t i = 0;
  while (i < s.size() && !IsSpace(s[i])) ++i;
  return {s.substr(0, i), Trim(s.substr(i))};
}

// Splits off the last whitespace-delimited token; `head` is trimmed.
std::pair<std::string_view, std::string_view> SplitLast(std::string_view s) {
  s = Trim(s);
  size_t i = s.size();
  while (i > 0 && !IsSpace(s[i - 1])) --i;
  return {Trim(s.substr(0, i)), s.substr(i)};
}

std::vector<std::string_view> Tokens(std::string_view s) {
  std::vector<std::string_view> out;
  while (true) {
    auto [token, rest] = SplitFirst(s);
    if (token.empty()) break;
    out.push_back(token);
    s = rest;
  }
  return out;
}

bool ParseMs(std::string_view token, uint32_t* out) {
  if (token.empty() || token.size() > 10) return false;
  for (char c : token) {
    if (c < '0' || c > '9') return false;
  }
  uint64_t value = 0;
  auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (ec != std::errc() || ptr != token.data() + token.size() || value > kMaxMs) return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

int16_t Axis(bool negative, bool positive) {
  if (negative == positive) return 0;
  return positive ? int16_t{32767} : int16_t{-32768};
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

// Length of the well-formed UTF-8 sequence at s[i], or 0 if it is malformed.
size_t Utf8SequenceLength(std::string_view s, size_t i) {
  const auto b0 = static_cast<unsigned char>(s[i]);
  size_t len;
  unsigned char lo = 0x80, hi = 0xBF;  // allowed range of the second byte
  if (b0 < 0x80) return 1;
  if (b0 >= 0xC2 && b0 <= 0xDF) {
    len = 2;
  } else if (b0 >= 0xE0 && b0 <= 0xEF) {
    len = 3;
    if (b0 == 0xE0) lo = 0xA0;
    if (b0 == 0xED) hi = 0x9F;
  } else if (b0 >= 0xF0 && b0 <= 0xF4) {
    len = 4;
    if (b0 == 0xF0) lo = 0x90;
    if (b0 == 0xF4) hi = 0x8F;
  } else {
    return 0;
  }
  if (i + len > s.size()) return 0;
  for (size_t k = 1; k < len; ++k) {
    const auto b = static_cast<unsigned char>(s[i + k]);
    const unsigned char min = (k == 1) ? lo : 0x80;
    const unsigned char max = (k == 1) ? hi : 0xBF;
    if (b < min || b > max) return 0;
  }
  return len;
}

}  // namespace

// --- Inputs -------------------------------------------------------------------------------

PadState PadFromInputs(uint32_t inputs) {
  PadState pad;
  pad.buttons = static_cast<uint16_t>(inputs & kButtonMask);
  pad.left_trigger = (inputs & kLeftTrigger) ? 255 : 0;
  pad.right_trigger = (inputs & kRightTrigger) ? 255 : 0;
  pad.thumb_lx = Axis(inputs & kLeftStickLeft, inputs & kLeftStickRight);
  pad.thumb_ly = Axis(inputs & kLeftStickDown, inputs & kLeftStickUp);
  pad.thumb_rx = Axis(inputs & kRightStickLeft, inputs & kRightStickRight);
  pad.thumb_ry = Axis(inputs & kRightStickDown, inputs & kRightStickUp);
  return pad;
}

bool ParseInputs(std::string_view text, uint32_t* out) {
  uint32_t result = 0;
  while (true) {
    const size_t plus = text.find('+');
    const std::string_view name = text.substr(0, plus);
    if (name.empty()) return false;
    const auto it = std::find_if(kNamedInputs.begin(), kNamedInputs.end(),
                                 [&](const NamedInput& n) { return EqualsIgnoreCase(n.name, name); });
    if (it == kNamedInputs.end()) return false;
    result |= it->bits;
    if (plus == std::string_view::npos) break;
    text.remove_prefix(plus + 1);
  }
  *out = result;
  return true;
}

std::string InputsToString(uint32_t inputs) {
  std::string out;
  for (const auto& n : kNamedInputs) {
    if (!(inputs & n.bits)) continue;
    if (!out.empty()) out += '+';
    out += n.name;
  }
  return out.empty() ? "none" : out;
}

void KeyEventsForTransition(uint32_t before, uint32_t after, std::vector<KeyEvent>* out) {
  // Releases first, so a chord change reads as up-then-down.
  for (const auto& n : kNamedInputs) {
    if ((before & n.bits) && !(after & n.bits)) out->push_back({n.virtual_key, false});
  }
  for (const auto& n : kNamedInputs) {
    if (!(before & n.bits) && (after & n.bits)) out->push_back({n.virtual_key, true});
  }
}

// --- Script -------------------------------------------------------------------------------

bool ParseScript(std::string_view text, Script* out, std::string* error) {
  out->steps.clear();
  auto fail = [&](int line, const std::string& why) {
    *error = "line " + std::to_string(line) + ": " + why;
    out->steps.clear();
    return false;
  };
  if (text.size() > kMaxScriptBytes) return fail(0, "script is larger than 1 MiB");

  std::vector<size_t> open_loops;  // indices of unmatched loop_start steps
  int line_no = 0;
  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t nl = text.find('\n', pos);
    const std::string_view raw =
        text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
    ++line_no;
    if (raw.size() > kMaxLineBytes) return fail(line_no, "line is longer than 4096 bytes");
    const std::string_view line = Trim(raw);
    if (line.empty() || line.front() == '#') continue;
    for (char c : line) {
      const auto u = static_cast<unsigned char>(c);
      if ((u < 0x20 && c != '\t') || u == 0x7F) return fail(line_no, "control character in line");
    }
    if (out->steps.size() >= kMaxSteps) return fail(line_no, "more than 10000 steps");

    auto [cmd_token, rest] = SplitFirst(line);
    std::string cmd(cmd_token);
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), AsciiLower);
    const std::vector<std::string_view> args = Tokens(rest);

    Step step;
    step.line = line_no;
    step.source = std::string(line);

    if (cmd == "wait") {
      step.op = Op::kWait;
      if (args.size() != 1 || !ParseMs(args[0], &step.ms)) return fail(line_no, "usage: wait <ms>");
    } else if (cmd == "press" || cmd == "hold") {
      step.op = Op::kPress;
      const bool is_hold = cmd == "hold";
      const bool arity_ok = is_hold ? args.size() == 2 : (args.size() == 1 || args.size() == 2);
      if (!arity_ok) {
        return fail(line_no, is_hold ? "usage: hold <inputs> <ms>" : "usage: press <inputs> [hold_ms]");
      }
      if (!ParseInputs(args[0], &step.inputs)) {
        return fail(line_no, "unknown input '" + std::string(args[0]) + "'");
      }
      if (args.size() == 2) {
        if (!ParseMs(args[1], &step.ms) || step.ms == 0) {
          return fail(line_no, "hold time must be 1.." + std::to_string(kMaxMs) + " ms");
        }
      } else {
        step.default_hold = true;
      }
    } else if (cmd == "wait_file" || cmd == "wait_trace") {
      step.op = cmd == "wait_file" ? Op::kWaitFile : Op::kWaitTrace;
      auto [head, timeout] = SplitLast(rest);
      if (head.empty() || !ParseMs(timeout, &step.ms)) {
        return fail(line_no, "usage: " + cmd +
                                 (cmd == "wait_file" ? " <path> <timeout_ms>" : " <substring> <timeout_ms>"));
      }
      step.arg = std::string(head);
    } else if (cmd == "log") {
      step.op = Op::kLog;
      step.arg = std::string(rest);
    } else if (cmd == "touch") {
      step.op = Op::kTouch;
      if (rest.empty()) return fail(line_no, "usage: touch <path>");
      step.arg = std::string(rest);
    } else if (cmd == "loop_start") {
      step.op = Op::kLoopStart;
      if (args.size() != 1 || !ParseMs(args[0], &step.ms) || step.ms < 1 || step.ms > kMaxLoopCount) {
        return fail(line_no, "usage: loop_start <n> (1.." + std::to_string(kMaxLoopCount) + ")");
      }
      if (open_loops.size() >= kMaxLoopDepth) return fail(line_no, "loops nested deeper than 16");
      open_loops.push_back(out->steps.size());
    } else if (cmd == "loop_end") {
      step.op = Op::kLoopEnd;
      if (!args.empty()) return fail(line_no, "usage: loop_end");
      if (open_loops.empty()) return fail(line_no, "loop_end without loop_start");
      open_loops.pop_back();
    } else if (cmd == "screenshot") {
      step.op = Op::kScreenshot;
      if (rest.empty()) return fail(line_no, "usage: screenshot <path.ppm>");
      step.arg = std::string(rest);
    } else if (cmd == "screenshot_every") {
      step.op = Op::kScreenshotEvery;
      auto [interval, dir] = SplitFirst(rest);
      if (!ParseMs(interval, &step.ms) || dir.empty()) {
        return fail(line_no, "usage: screenshot_every <ms> <dir>");
      }
      if (step.ms < kMinScreenshotIntervalMs) {
        return fail(line_no, "screenshot interval must be at least " +
                                 std::to_string(kMinScreenshotIntervalMs) + " ms");
      }
      step.arg = std::string(dir);
    } else if (cmd == "repl") {
      step.op = Op::kRepl;
      if (rest.empty()) return fail(line_no, "usage: repl <dir>");
      step.arg = std::string(rest);
      step.aux = 1;
    } else if (cmd == "screenshot_stop") {
      step.op = Op::kScreenshotStop;
      if (!args.empty()) return fail(line_no, "usage: screenshot_stop");
    } else if (cmd == "set") {
      step.op = Op::kSet;
      const bool press = args.size() == 2 && EqualsIgnoreCase(args[0], "press_ms");
      const bool gap = args.size() == 2 && EqualsIgnoreCase(args[0], "gap_ms");
      if (!(press || gap) || !ParseMs(args[1], &step.ms)) {
        return fail(line_no, "usage: set press_ms <ms> | set gap_ms <ms>");
      }
      if (press && step.ms == 0) return fail(line_no, "press_ms must be at least 1");
      step.arg = press ? "press_ms" : "gap_ms";
    } else {
      return fail(line_no, "unknown command '" + std::string(cmd_token) + "'");
    }
    out->steps.push_back(std::move(step));
  }
  if (!open_loops.empty()) {
    return fail(out->steps[open_loops.back()].line, "loop_start without loop_end");
  }
  return true;
}

// --- Runner -------------------------------------------------------------------------------

ScriptRunner::ScriptRunner(Script script, ScriptHost* host)
    : script_(std::move(script)), host_(host) {}

std::optional<uint32_t> ScriptRunner::Poll(uint64_t now_ms) {
  if (state_ != State::kRunning) return std::nullopt;
  if (!started_) {
    started_ = true;
    start_ms_ = now_ms;
    host_->Log("script started, " + std::to_string(script_.steps.size()) + " steps");
  }
  for (int budget = 0; budget < kMaxInstantStepsPerPoll; ++budget) {
    if (phase_ == Phase::kIdle) {
      if (pc_ >= script_.steps.size()) {
        Finish(now_ms, State::kFinished, {});
        return std::nullopt;
      }
      if (BeginStep(now_ms)) {
        if (state_ != State::kRunning) return std::nullopt;
        continue;  // finished instantly
      }
    }
    if (!ContinueStep(now_ms)) break;
    if (state_ != State::kRunning) return std::nullopt;
  }

  switch (phase_) {
    case Phase::kIdle:
      return 0;  // instant-step budget used up; yield, then continue
    case Phase::kWaiting: {
      const uint64_t left = phase_until_ms_ > now_ms ? phase_until_ms_ - now_ms : 0;
      return static_cast<uint32_t>(std::min<uint64_t>(left, kWaitPollMs));
    }
    default: {
      const uint64_t left = phase_until_ms_ > now_ms ? phase_until_ms_ - now_ms : 0;
      return static_cast<uint32_t>(std::min<uint64_t>(left, kMaxMs));
    }
  }
}

void ScriptRunner::Stop(uint64_t now_ms) {
  if (state_ != State::kRunning) return;
  if (!started_) {
    started_ = true;
    start_ms_ = now_ms;
  }
  Finish(now_ms, State::kStopped, "stopped");
}

bool ScriptRunner::BeginStep(uint64_t now_ms) {
  const Step& s = script_.steps[pc_];
  ++seq_;
  step_began_ms_ = now_ms;

  ProgressEvent begin;
  begin.kind = ProgressEvent::Kind::kBegin;
  begin.seq = seq_;
  begin.index = pc_;
  begin.line = s.line;
  begin.elapsed_ms = Elapsed(now_ms);
  begin.command = s.source;
  host_->Progress(begin);
  host_->Log("step " + std::to_string(seq_) + " (line " + std::to_string(s.line) + "): " + s.source);

  std::string error;
  switch (s.op) {
    case Op::kWait:
      phase_ = Phase::kSleeping;
      phase_until_ms_ = now_ms + s.ms;
      return false;
    case Op::kPress:
      host_->SetInputs(s.inputs);
      phase_ = Phase::kHolding;
      phase_until_ms_ = now_ms + (s.default_hold ? press_ms_ : s.ms);
      return false;
    case Op::kWaitFile:
    case Op::kWaitTrace:
      phase_ = Phase::kWaiting;
      phase_until_ms_ = now_ms + s.ms;
      return false;
    case Op::kLog:
      host_->Log("script: " + s.arg);
      EndStep(now_ms, "ok");
      ++pc_;
      return true;
    case Op::kTouch:
      if (!host_->Touch(s.arg, &error)) {
        EndStep(now_ms, "error", error);
        Finish(now_ms, State::kFailed, "line " + std::to_string(s.line) + ": touch failed: " + error);
        return true;
      }
      EndStep(now_ms, "ok");
      ++pc_;
      return true;
    case Op::kLoopStart:
      loops_.push_back({pc_, s.ms});
      EndStep(now_ms, "ok");
      ++pc_;
      return true;
    case Op::kLoopEnd:
      if (loops_.empty()) {  // the parser rejects this; stay safe anyway
        EndStep(now_ms, "error", "loop_end without loop_start");
        Finish(now_ms, State::kFailed, "line " + std::to_string(s.line) + ": loop_end without loop_start");
        return true;
      }
      EndStep(now_ms, "ok");
      if (--loops_.back().remaining > 0) {
        pc_ = loops_.back().start + 1;
      } else {
        loops_.pop_back();
        ++pc_;
      }
      return true;
    case Op::kScreenshot:
      if (host_->Screenshot(s.arg, &error)) {
        EndStep(now_ms, "ok");
      } else {
        host_->Log("screenshot failed: " + error);
        EndStep(now_ms, "error", error);
      }
      ++pc_;
      return true;
    case Op::kScreenshotEvery:
      if (host_->StartScreenshots(s.ms, s.arg, &error)) {
        EndStep(now_ms, "ok");
      } else {
        host_->Log("screenshot_every failed: " + error);
        EndStep(now_ms, "error", error);
      }
      ++pc_;
      return true;
    case Op::kScreenshotStop:
      host_->StopScreenshots();
      EndStep(now_ms, "ok");
      ++pc_;
      return true;
    case Op::kSet:
      (s.arg == "press_ms" ? press_ms_ : gap_ms_) = s.ms;
      EndStep(now_ms, "ok");
      ++pc_;
      return true;
    case Op::kRepl:
      phase_ = Phase::kWaiting;
      phase_until_ms_ = UINT64_MAX;  // waits until a command file arrives
      return false;
  }
  return true;
}

namespace {

bool WriteSmallFile(const std::filesystem::path& path, std::string_view text) {
  const std::filesystem::path tmp = path.string() + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

}  // namespace

// Polls for the repl's next command file. Returns true once the repl step is finished (an "end"
// file) or has spliced the next command's steps in after itself.
bool ScriptRunner::PollRepl(uint64_t now_ms) {
  const std::filesystem::path dir = script_.steps[pc_].arg;
  const uint32_t index = script_.steps[pc_].aux;
  const std::filesystem::path cmd_path = dir / IndexedName("cmd_", index, ".txt");
  const std::filesystem::path done_path = dir / IndexedName("done_", index, ".txt");
  std::error_code ec;
  if (!std::filesystem::exists(cmd_path, ec)) return false;
  std::string text;
  {
    std::ifstream f(cmd_path, std::ios::binary);
    if (!f) return false;
    text.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::filesystem::remove(cmd_path, ec);
  host_->Log("repl: command file " + cmd_path.filename().string());

  const size_t first = text.find_first_not_of(" \t\r\n");
  const size_t last = text.find_last_not_of(" \t\r\n");
  if (first != std::string::npos && text.substr(first, last - first + 1) == "end") {
    WriteSmallFile(done_path, "ok\n");
    phase_ = Phase::kIdle;
    EndStep(now_ms, "ok");
    ++pc_;
    return true;
  }
  Script sub;
  std::string error;
  if (!ParseScript(text, &sub, &error)) {
    WriteSmallFile(done_path, "error: " + error + "\n");
    host_->Log("repl: parse error: " + error);
    script_.steps[pc_].aux = index + 1;
    return false;
  }
  // Splice: <sub steps> <write done_N> <repl again with N+1>.
  Step repl_again = script_.steps[pc_];
  repl_again.aux = index + 1;
  Step done;
  done.op = Op::kTouch;
  done.line = repl_again.line;
  done.source = "touch " + done_path.string();
  done.arg = done_path.string();
  std::vector<Step> insert = std::move(sub.steps);
  insert.push_back(std::move(done));
  insert.push_back(std::move(repl_again));
  script_.steps.insert(script_.steps.begin() + static_cast<std::ptrdiff_t>(pc_ + 1),
                       std::make_move_iterator(insert.begin()),
                       std::make_move_iterator(insert.end()));
  phase_ = Phase::kIdle;
  EndStep(now_ms, "ok");
  ++pc_;
  return true;
}

bool ScriptRunner::ContinueStep(uint64_t now_ms) {
  const Step& s = script_.steps[pc_];
  switch (phase_) {
    case Phase::kIdle:
      return true;
    case Phase::kSleeping:
    case Phase::kGap:
      if (now_ms < phase_until_ms_) return false;
      break;
    case Phase::kHolding:
      if (now_ms < phase_until_ms_) return false;
      host_->SetInputs(0);
      if (gap_ms_ > 0) {
        phase_ = Phase::kGap;
        phase_until_ms_ = now_ms + gap_ms_;
        return false;
      }
      break;
    case Phase::kWaiting: {
      if (s.op == Op::kRepl) return PollRepl(now_ms);
      bool ready = false;
      if (s.op == Op::kWaitFile) {
        ready = host_->FileExists(s.arg);
      } else {
        const WaitResult r = host_->PollTrace(s.arg);
        if (r == WaitResult::kUnavailable) {
          phase_ = Phase::kIdle;
          EndStep(now_ms, "error", "no trace file (netplay_trace is empty)");
          Finish(now_ms, State::kFailed,
                 "line " + std::to_string(s.line) + ": wait_trace needs netplay_trace to be set");
          return true;
        }
        ready = r == WaitResult::kReady;
      }
      if (ready) break;
      if (now_ms < phase_until_ms_) return false;
      phase_ = Phase::kIdle;
      EndStep(now_ms, "timeout");
      Finish(now_ms, State::kFailed,
             "line " + std::to_string(s.line) + ": timed out after " + std::to_string(s.ms) +
                 " ms: " + s.source);
      return true;
    }
  }
  phase_ = Phase::kIdle;
  EndStep(now_ms, "ok");
  ++pc_;
  return true;
}

void ScriptRunner::EndStep(uint64_t now_ms, std::string result, std::string detail) {
  const Step& s = script_.steps[pc_];
  ProgressEvent end;
  end.kind = ProgressEvent::Kind::kEnd;
  end.seq = seq_;
  end.index = pc_;
  end.line = s.line;
  end.elapsed_ms = Elapsed(now_ms);
  end.result = std::move(result);
  end.detail = std::move(detail);
  end.waited_ms = now_ms - step_began_ms_;
  host_->Progress(end);
}

void ScriptRunner::Finish(uint64_t now_ms, State state, std::string detail) {
  state_ = state;
  failure_ = detail;
  phase_ = Phase::kIdle;
  host_->SetInputs(0);
  host_->StopScreenshots();

  ProgressEvent fin;
  fin.kind = ProgressEvent::Kind::kFinish;
  fin.seq = seq_;
  fin.index = pc_;
  fin.line = pc_ < script_.steps.size() ? script_.steps[pc_].line : 0;
  fin.elapsed_ms = Elapsed(now_ms);
  fin.result = state == State::kFinished ? "ok" : state == State::kFailed ? "failed" : "stopped";
  fin.detail = detail;
  host_->Progress(fin);
  host_->Log("script " + fin.result + (detail.empty() ? std::string() : ": " + detail));
}

// --- File helpers -------------------------------------------------------------------------

TraceTail::TraceTail(std::filesystem::path path) : path_(std::move(path)) {}

void TraceTail::SkipExisting() {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path_, ec);
  offset_ = ec ? 0 : size;
  partial_.clear();
}

bool TraceTail::Find(std::string_view needle) {
  std::error_code ec;
  const uint64_t size = std::filesystem::file_size(path_, ec);
  if (ec) return false;  // not created yet
  if (size < offset_) {  // truncated or replaced: start over
    offset_ = 0;
    partial_.clear();
  }
  for (int chunk_count = 0; chunk_count < 8 && offset_ < size; ++chunk_count) {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return false;
    in.seekg(static_cast<std::streamoff>(offset_));
    if (!in) return false;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(size - offset_, kMaxReadPerPoll));
    std::string chunk(want, '\0');
    in.read(chunk.data(), static_cast<std::streamsize>(want));
    const auto got = static_cast<size_t>(in.gcount());
    if (got == 0) return false;
    chunk.resize(got);

    const uint64_t base = offset_;  // file offset of chunk[0]
    size_t start = 0;
    while (true) {
      const size_t nl = chunk.find('\n', start);
      if (nl == std::string::npos) break;
      const std::string_view piece(chunk.data() + start, nl - start);
      bool hit;
      if (partial_.empty()) {
        hit = Contains(piece, needle);
      } else {
        partial_.append(piece.substr(0, kMaxLineBytes - std::min(kMaxLineBytes, partial_.size())));
        hit = Contains(partial_, needle);
        partial_.clear();
      }
      start = nl + 1;
      if (hit) {
        offset_ = base + start;
        return true;
      }
    }
    // Keep the incomplete tail (bounded; an over-long line is matched on its first 64 KiB).
    const std::string_view tail(chunk.data() + start, got - start);
    partial_.append(tail.substr(0, kMaxLineBytes - std::min(kMaxLineBytes, partial_.size())));
    offset_ = base + got;
  }
  return false;
}

bool WritePpm(const std::filesystem::path& path, uint32_t width, uint32_t height, size_t stride,
              const std::vector<uint8_t>& rgbx, std::string* error) {
  constexpr uint32_t kMaxDimension = 16384;
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension) {
    *error = "bad image size " + std::to_string(width) + "x" + std::to_string(height);
    return false;
  }
  const size_t row_bytes = size_t{width} * 4;
  if (stride < row_bytes) {
    *error = "image stride is smaller than a row";
    return false;
  }
  const size_t needed = (size_t{height} - 1) * stride + row_bytes;
  if (rgbx.size() < needed) {
    *error = "image buffer is too small";
    return false;
  }

  std::filesystem::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot create " + tmp.string();
      return false;
    }
    const std::string header =
        "P6\n" + std::to_string(width) + " " + std::to_string(height) + "\n255\n";
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::vector<char> row(size_t{width} * 3);
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* src = rgbx.data() + size_t{y} * stride;
      for (uint32_t x = 0; x < width; ++x) {
        row[size_t{x} * 3 + 0] = static_cast<char>(src[size_t{x} * 4 + 0]);
        row[size_t{x} * 3 + 1] = static_cast<char>(src[size_t{x} * 4 + 1]);
        row[size_t{x} * 3 + 2] = static_cast<char>(src[size_t{x} * 4 + 2]);
      }
      out.write(row.data(), static_cast<std::streamsize>(row.size()));
    }
    out.close();
    if (!out) {
      *error = "write failed: " + tmp.string();
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    *error = "cannot rename to " + path.string() + ": " + ec.message();
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

uint32_t NextFreeIndex(const std::filesystem::path& dir, std::string_view prefix,
                       std::string_view suffix) {
  uint32_t next = 0;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (ec) return 0;
  const std::filesystem::directory_iterator end{};
  for (; !ec && it != end; it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name.size() <= prefix.size() + suffix.size()) continue;
    if (name.compare(0, prefix.size(), prefix) != 0) continue;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    const std::string_view digits =
        std::string_view(name).substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    if (digits.size() > 9) continue;
    uint32_t value = 0;
    auto [ptr, perr] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (perr != std::errc() || ptr != digits.data() + digits.size()) continue;
    next = std::max(next, value + 1);
  }
  return next;
}

std::string IndexedName(std::string_view prefix, uint32_t index, std::string_view suffix) {
  char digits[16];
  std::snprintf(digits, sizeof(digits), "%04u", index);
  std::string out(prefix);
  out += digits;
  out += suffix;
  return out;
}

std::string JsonEscape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
      ++i;
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
      ++i;
    } else if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
    } else if (const size_t len = Utf8SequenceLength(text, i); len > 0) {
      out.append(text.substr(i, len));
      i += len;
    } else {
      out += '?';  // malformed UTF-8 would make the line invalid JSON
      ++i;
    }
  }
  return out;
}

}  // namespace ac6::netplay_input

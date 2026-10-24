// Unit tests for src/netplay_input_script.{h,cpp}: parser, runner timing, trace tailing, PPM
// output and helpers. Run: netplay_input_test [substring-filter]
#include "netplay_input_script.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <unistd.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ac6::netplay_input;

// --- Minimal test harness ----------------------------------------------------------------

namespace {

struct Case {
  const char* name;
  void (*fn)();
};
std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}
struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};
int g_failures = 0;
struct RequireFailed {};

void Fail(const char* file, int line, const char* expr, bool fatal) {
  std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, expr);
  ++g_failures;
  if (fatal) throw RequireFailed{};
}

#define CONCAT2(a, b) a##b
#define CONCAT(a, b) CONCAT2(a, b)
#define TEST_CASE(name)                                                      \
  static void CONCAT(test_fn_, __LINE__)();                                  \
  static Registrar CONCAT(test_reg_, __LINE__)(name, &CONCAT(test_fn_, __LINE__)); \
  static void CONCAT(test_fn_, __LINE__)()
#define CHECK(x) \
  do {           \
    if (!(x)) Fail(__FILE__, __LINE__, #x, false); \
  } while (0)
#define REQUIRE(x) \
  do {             \
    if (!(x)) Fail(__FILE__, __LINE__, #x, true); \
  } while (0)

fs::path TempDir() {
  static fs::path dir = [] {
    const char* base = std::getenv("TMPDIR");
    fs::path p = fs::path(base && *base ? base : "/tmp") /
                 ("netplay_input_test_" + std::to_string(std::random_device{}()));
    fs::create_directories(p);
    return p;
  }();
  return dir;
}

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void Append(const fs::path& p, const std::string& text) {
  std::ofstream out(p, std::ios::binary | std::ios::app);
  out << text;
}

Script MustParse(const std::string& text) {
  Script s;
  std::string err;
  if (!ParseScript(text, &s, &err)) {
    std::fprintf(stderr, "  parse error: %s\n", err.c_str());
    Fail(__FILE__, __LINE__, "MustParse", true);
  }
  return s;
}

std::string ParseError(const std::string& text) {
  Script s;
  std::string err;
  if (ParseScript(text, &s, &err)) return "<parsed>";
  CHECK(s.steps.empty());
  return err;
}

// Records everything the runner asks of the host, with the fake clock's time.
struct FakeHost : ScriptHost {
  uint64_t now = 0;
  uint32_t inputs = 0;
  std::vector<std::pair<uint64_t, uint32_t>> input_log;
  std::set<std::string> files;
  std::unique_ptr<TraceTail> trace;
  std::vector<std::string> logs;
  std::vector<ProgressEvent> progress;
  std::vector<std::string> screenshots;
  bool screenshot_ok = true;
  bool periodic = false;
  uint32_t periodic_ms = 0;
  std::string periodic_dir;

  void SetInputs(uint32_t v) override {
    inputs = v;
    input_log.push_back({now, v});
  }
  bool FileExists(const std::string& path) override { return files.count(path) != 0; }
  bool touch_ok = true;
  bool Touch(const std::string& path, std::string* error) override {
    if (!touch_ok) {
      *error = "read-only";
      return false;
    }
    files.insert(path);
    return true;
  }
  WaitResult PollTrace(std::string_view needle) override {
    if (!trace) return WaitResult::kUnavailable;
    return trace->Find(needle) ? WaitResult::kReady : WaitResult::kNotYet;
  }
  bool Screenshot(const std::string& path, std::string* error) override {
    screenshots.push_back(path);
    if (!screenshot_ok) *error = "no guest output yet";
    return screenshot_ok;
  }
  bool StartScreenshots(uint32_t ms, const std::string& dir, std::string*) override {
    periodic = true;
    periodic_ms = ms;
    periodic_dir = dir;
    return true;
  }
  void StopScreenshots() override { periodic = false; }
  void Log(std::string_view m) override { logs.emplace_back(m); }
  void Progress(const ProgressEvent& e) override { progress.push_back(e); }

  const ProgressEvent* Finish() const {
    for (const auto& e : progress) {
      if (e.kind == ProgressEvent::Kind::kFinish) return &e;
    }
    return nullptr;
  }
};

// Drives the runner like the service thread does: poll, sleep the returned delay.
void RunToEnd(ScriptRunner& runner, FakeHost& host, uint64_t limit_ms = 10'000'000) {
  while (host.now <= limit_ms) {
    const auto delay = runner.Poll(host.now);
    if (!delay) return;
    host.now += *delay;
  }
  Fail(__FILE__, __LINE__, "runner did not finish", true);
}

}  // namespace

// --- Inputs ------------------------------------------------------------------------------

TEST_CASE("inputs: names, case and chords") {
  uint32_t v = 0;
  CHECK(ParseInputs("A", &v) && v == kButtonA);
  CHECK(ParseInputs("start", &v) && v == kStart);
  CHECK(ParseInputs("LB+RB", &v) && v == (kLeftShoulder | kRightShoulder));
  CHECK(ParseInputs("lsu+lsr+a", &v) && v == (kLeftStickUp | kLeftStickRight | kButtonA));
  CHECK(ParseInputs("LT", &v) && v == kLeftTrigger);
  CHECK(ParseInputs("RSD", &v) && v == kRightStickDown);
  const char* all[] = {"A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK", "UP", "DOWN",
                       "LEFT", "RIGHT", "LSU", "LSD", "LSL", "LSR", "LS", "RS", "RSU", "RSD",
                       "RSL", "RSR"};
  uint32_t seen = 0;
  for (const char* name : all) {
    REQUIRE(ParseInputs(name, &v));
    CHECK((seen & v) == 0);
    seen |= v;
  }
  CHECK(seen == kAllInputs);
  CHECK(!ParseInputs("", &v));
  CHECK(!ParseInputs("A+", &v));
  CHECK(!ParseInputs("+A", &v));
  CHECK(!ParseInputs("A++B", &v));
  CHECK(!ParseInputs("GUIDE", &v));
  CHECK(!ParseInputs("AA", &v));
  CHECK(InputsToString(kLeftShoulder | kRightShoulder) == "RB+LB");
  CHECK(InputsToString(0) == "none");
}

TEST_CASE("inputs: pad state") {
  PadState p = PadFromInputs(kButtonA | kDpadUp | kLeftTrigger | kLeftStickUp | kRightStickLeft);
  CHECK(p.buttons == (kButtonA | kDpadUp));
  CHECK(p.left_trigger == 255 && p.right_trigger == 0);
  CHECK(p.thumb_ly == 32767 && p.thumb_lx == 0);
  CHECK(p.thumb_rx == -32768 && p.thumb_ry == 0);
  p = PadFromInputs(kLeftStickLeft | kLeftStickRight | kLeftStickDown);
  CHECK(p.thumb_lx == 0 && p.thumb_ly == -32768);
  p = PadFromInputs(0);
  CHECK(p.buttons == 0 && p.thumb_lx == 0 && p.thumb_ly == 0 && p.left_trigger == 0);
  // Bits outside the XInput button set never leak into buttons.
  CHECK(PadFromInputs(0xFFFFFFFFu).buttons == kButtonMask);
}

TEST_CASE("inputs: keystroke events") {
  std::vector<KeyEvent> ev;
  KeyEventsForTransition(0, kButtonA | kLeftStickUp, &ev);
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].virtual_key == 0x5800 && ev[0].down);
  CHECK(ev[1].virtual_key == 0x5820 && ev[1].down);
  ev.clear();
  KeyEventsForTransition(kButtonA, kButtonB, &ev);
  REQUIRE(ev.size() == 2);
  CHECK(ev[0].virtual_key == 0x5800 && !ev[0].down);  // release first
  CHECK(ev[1].virtual_key == 0x5801 && ev[1].down);
  ev.clear();
  KeyEventsForTransition(kStart, kStart, &ev);
  CHECK(ev.empty());
}

// --- Parser ------------------------------------------------------------------------------

TEST_CASE("parse: every command") {
  const Script s = MustParse(
      "# header comment\r\n"
      "   \n"
      "wait 1500\n"
      "press A\n"
      "press LB+RB 250\n"
      "hold LSU 1200\n"
      "wait_file /tmp/dir with spaces/ready.flag 30000\n"
      "wait_trace \"ev\":\"xam.live\" 60000\n"
      "log hello   world\n"
      "log\n"
      "touch /tmp/flag dir/ready.flag\n"
      "loop_start 3\n"
      "  press DOWN\n"
      "loop_end\n"
      "screenshot /tmp/a b.ppm\n"
      "screenshot_every 500 /tmp/frames dir\n"
      "screenshot_stop\n"
      "set press_ms 80\n"
      "SET GAP_MS 0\n"
      "WAIT 5");
  REQUIRE(s.steps.size() == 18);
  CHECK(s.steps[0].op == Op::kWait && s.steps[0].ms == 1500 && s.steps[0].line == 3);
  CHECK(s.steps[1].op == Op::kPress && s.steps[1].inputs == kButtonA && s.steps[1].default_hold);
  CHECK(s.steps[2].inputs == (kLeftShoulder | kRightShoulder) && s.steps[2].ms == 250 &&
        !s.steps[2].default_hold);
  CHECK(s.steps[3].op == Op::kPress && s.steps[3].inputs == kLeftStickUp && s.steps[3].ms == 1200);
  CHECK(s.steps[4].op == Op::kWaitFile && s.steps[4].arg == "/tmp/dir with spaces/ready.flag" &&
        s.steps[4].ms == 30000);
  CHECK(s.steps[5].op == Op::kWaitTrace && s.steps[5].arg == "\"ev\":\"xam.live\"" &&
        s.steps[5].ms == 60000);
  CHECK(s.steps[6].op == Op::kLog && s.steps[6].arg == "hello   world");
  CHECK(s.steps[7].op == Op::kLog && s.steps[7].arg.empty());
  CHECK(s.steps[8].op == Op::kTouch && s.steps[8].arg == "/tmp/flag dir/ready.flag");
  CHECK(s.steps[9].op == Op::kLoopStart && s.steps[9].ms == 3);
  CHECK(s.steps[11].op == Op::kLoopEnd && s.steps[11].line == 14);
  CHECK(s.steps[12].op == Op::kScreenshot && s.steps[12].arg == "/tmp/a b.ppm");
  CHECK(s.steps[13].op == Op::kScreenshotEvery && s.steps[13].ms == 500 &&
        s.steps[13].arg == "/tmp/frames dir");
  CHECK(s.steps[14].op == Op::kScreenshotStop);
  CHECK(s.steps[15].op == Op::kSet && s.steps[15].arg == "press_ms" && s.steps[15].ms == 80);
  CHECK(s.steps[16].arg == "gap_ms" && s.steps[16].ms == 0);
  CHECK(s.steps[17].op == Op::kWait && s.steps[17].ms == 5 && s.steps[17].line == 20);
  CHECK(s.steps[5].source == "wait_trace \"ev\":\"xam.live\" 60000");
}

TEST_CASE("parse: errors carry line numbers") {
  CHECK(ParseError("wait 1\nbogus 3\n") == "line 2: unknown command 'bogus'");
  CHECK(ParseError("wait\n").rfind("line 1: usage: wait", 0) == 0);
  CHECK(ParseError("wait -5\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("wait 10ms\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("wait 99999999999\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("wait 86400001\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("wait 1 2\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("press\n").rfind("line 1: usage: press", 0) == 0);
  CHECK(ParseError("press Q\n") == "line 1: unknown input 'Q'");
  CHECK(ParseError("press A 0\n").rfind("line 1: hold time", 0) == 0);
  CHECK(ParseError("press A 10 20\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("hold A\n").rfind("line 1: usage: hold", 0) == 0);
  CHECK(ParseError("wait_file 100\n").rfind("line 1: usage: wait_file", 0) == 0);
  CHECK(ParseError("wait_file /x\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("wait_trace xam.live soon\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("loop_start 0\nloop_end\n").rfind("line 1: usage: loop_start", 0) == 0);
  CHECK(ParseError("loop_start 1000001\nloop_end\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("loop_end\n") == "line 1: loop_end without loop_start");
  CHECK(ParseError("wait 1\nloop_start 2\nwait 1\n") == "line 2: loop_start without loop_end");
  CHECK(ParseError("loop_start 2\nloop_end x\n").rfind("line 2:", 0) == 0);
  CHECK(ParseError("screenshot\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("touch\n") == "line 1: usage: touch <path>");
  CHECK(ParseError("screenshot_every 50 /tmp\n").rfind("line 1: screenshot interval", 0) == 0);
  CHECK(ParseError("screenshot_every 500\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("screenshot_stop now\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("set speed 1\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("set press_ms 0\n").rfind("line 1:", 0) == 0);
  CHECK(ParseError("log a\x01" "b\n") == "line 1: control character in line");
  CHECK(ParseError(std::string("log a") + '\0' + "b\n") == "line 1: control character in line");
  CHECK(ParseError("log " + std::string(5000, 'x')) == "line 1: line is longer than 4096 bytes");
}

TEST_CASE("parse: limits") {
  std::string deep;
  for (int i = 0; i < 17; ++i) deep += "loop_start 2\n";
  CHECK(ParseError(deep) == "line 17: loops nested deeper than 16");
  std::string ok_deep;
  for (int i = 0; i < 16; ++i) ok_deep += "loop_start 2\n";
  for (int i = 0; i < 16; ++i) ok_deep += "loop_end\n";
  CHECK(MustParse(ok_deep).steps.size() == 32);

  std::string many;
  for (int i = 0; i < 10001; ++i) many += "wait 0\n";
  CHECK(ParseError(many) == "line 10001: more than 10000 steps");
  CHECK(ParseError(std::string(kMaxScriptBytes + 1, '#')) == "line 0: script is larger than 1 MiB");
  CHECK(MustParse("").steps.empty());
  CHECK(MustParse("# only a comment").steps.empty());
}

TEST_CASE("parse: random input never crashes") {
  std::mt19937 rng(12345);
  const char* words[] = {"wait", "press", "hold", "wait_file", "wait_trace", "log", "touch", "loop_start",
                         "loop_end", "screenshot", "screenshot_every", "screenshot_stop", "set",
                         "A", "LB+RB", "99999", "0", "-1", "#", "\t", " ", "\n", "\r\n", "+",
                         "press_ms", "gap_ms", "/tmp/x"};
  int parsed = 0;
  for (int iter = 0; iter < 3000; ++iter) {
    std::string text;
    const int n = static_cast<int>(rng() % 40);
    for (int i = 0; i < n; ++i) {
      if (rng() % 5 == 0) {
        text += static_cast<char>(rng() % 256);
      } else {
        text += words[rng() % (sizeof(words) / sizeof(words[0]))];
        text += (rng() % 3 == 0) ? "\n" : " ";
      }
    }
    Script s;
    std::string err;
    if (ParseScript(text, &s, &err)) {
      ++parsed;
      // Whatever parses must also run to completion against a host that never satisfies waits.
      // Long loops are cut short with Stop(), which must still release every input.
      FakeHost host;
      ScriptRunner runner(std::move(s), &host);
      for (int polls = 0; polls < 20000; ++polls) {
        const auto delay = runner.Poll(host.now);
        if (!delay) break;
        host.now += *delay;
      }
      runner.Stop(host.now);
      CHECK(runner.state() != ScriptRunner::State::kRunning);
      CHECK(host.inputs == 0);
    } else {
      CHECK(err.rfind("line ", 0) == 0);
    }
  }
  CHECK(parsed > 0);
}

// --- Runner ------------------------------------------------------------------------------

TEST_CASE("runner: press timing, gap and set") {
  FakeHost host;
  host.now = 1000;
  ScriptRunner runner(MustParse("press A\npress B 300\nset press_ms 40\nset gap_ms 10\npress X\n"),
                      &host);
  RunToEnd(runner, host);
  CHECK(runner.state() == ScriptRunner::State::kFinished);
  REQUIRE(host.input_log.size() == 7);  // 3 x (down, up) + final release
  CHECK(host.input_log[0] == std::make_pair(uint64_t{1000}, uint32_t{kButtonA}));
  CHECK(host.input_log[1] == std::make_pair(uint64_t{1100}, uint32_t{0}));
  CHECK(host.input_log[2] == std::make_pair(uint64_t{1200}, uint32_t{kButtonB}));  // after 100 gap
  CHECK(host.input_log[3] == std::make_pair(uint64_t{1500}, uint32_t{0}));
  CHECK(host.input_log[4] == std::make_pair(uint64_t{1600}, uint32_t{kButtonX}));
  CHECK(host.input_log[5] == std::make_pair(uint64_t{1640}, uint32_t{0}));
  CHECK(host.now == 1650);
  const ProgressEvent* fin = host.Finish();
  REQUIRE(fin);
  CHECK(fin->result == "ok" && fin->elapsed_ms == 650);
}

TEST_CASE("runner: wait and loops") {
  FakeHost host;
  ScriptRunner runner(MustParse("loop_start 3\n wait 100\n loop_start 2\n  press A 10\n loop_end\n"
                                "loop_end\nlog done\n"),
                      &host);
  RunToEnd(runner, host);
  CHECK(runner.state() == ScriptRunner::State::kFinished);
  int presses = 0;
  for (const auto& [t, v] : host.input_log) presses += (v == kButtonA);
  CHECK(presses == 6);
  CHECK(host.now == 3 * (100 + 2 * (10 + 100)));
  CHECK(!host.logs.empty() && host.logs[host.logs.size() - 2] == "script: done");
  // seq counts executed steps: 3 x (wait, loop_start, 2 x (press, loop_end), loop_end) + 2 + log.
  uint64_t max_seq = 0;
  for (const auto& e : host.progress) max_seq = std::max(max_seq, e.seq);
  CHECK(max_seq == 1 + 3 * (1 + 1 + 2 * 2 + 1) + 1);
}

TEST_CASE("runner: many instant steps yield between polls") {
  FakeHost host;
  ScriptRunner runner(MustParse("loop_start 1000\nlog x\nloop_end\n"), &host);
  int polls = 0;
  while (true) {
    const auto delay = runner.Poll(host.now);
    ++polls;
    if (!delay) break;
    CHECK(*delay == 0);
    REQUIRE(polls < 100);
  }
  CHECK(polls > 1);
  CHECK(runner.state() == ScriptRunner::State::kFinished);
}

TEST_CASE("runner: wait_file success and timeout") {
  FakeHost host;
  ScriptRunner ok_runner(MustParse("wait_file /tmp/flag 5000\nlog after\n"), &host);
  CHECK(ok_runner.Poll(0) == 50u);
  host.now = 1234;
  CHECK(ok_runner.Poll(host.now).has_value());
  host.files.insert("/tmp/flag");
  host.now = 1300;
  CHECK(!ok_runner.Poll(host.now).has_value());
  CHECK(ok_runner.state() == ScriptRunner::State::kFinished);
  bool saw_wait_end = false;
  for (const auto& e : host.progress) {
    if (e.kind == ProgressEvent::Kind::kEnd && e.line == 1) {
      saw_wait_end = e.result == "ok" && e.waited_ms == 1300;
    }
  }
  CHECK(saw_wait_end);

  FakeHost host2;
  ScriptRunner bad_runner(MustParse("press A 10000\n"), &host2);
  bad_runner.Poll(0);
  CHECK(host2.inputs == kButtonA);
  ScriptRunner to_runner(MustParse("wait_file /nope 300\npress A\n"), &host2);
  RunToEnd(to_runner, host2);
  CHECK(to_runner.state() == ScriptRunner::State::kFailed);
  CHECK(to_runner.failure() == "line 1: timed out after 300 ms: wait_file /nope 300");
  CHECK(host2.now == 300);
  CHECK(host2.inputs == 0);  // failure releases everything
  const ProgressEvent* fin = host2.Finish();
  REQUIRE(fin);
  CHECK(fin->result == "failed");
}

TEST_CASE("runner: wait_trace follows the file in order") {
  const fs::path trace = TempDir() / "runner_trace.jsonl";
  fs::remove(trace);
  Append(trace, "{\"t_ms\":1,\"ev\":\"old.event\"}\n");
  FakeHost host;
  host.trace = std::make_unique<TraceTail>(trace);
  host.trace->SkipExisting();
  ScriptRunner runner(MustParse("wait_trace old.event 400\n"), &host);
  RunToEnd(runner, host);
  CHECK(runner.state() == ScriptRunner::State::kFailed);  // earlier content does not count

  Append(trace, "{\"t_ms\":2,\"ev\":\"xam.live\",\"live\":1}\n{\"t_ms\":3,\"ev\":\"xgi.create\"}\n");
  FakeHost host2;
  host2.trace = std::make_unique<TraceTail>(trace);
  ScriptRunner runner2(MustParse("wait_trace \"ev\":\"xam.live\" 1000\nwait_trace xgi.create 1000\n"
                                 "wait_trace xam.live 200\n"),
                       &host2);
  RunToEnd(runner2, host2);
  CHECK(runner2.state() == ScriptRunner::State::kFailed);  // the third wait needs a new line
  CHECK(runner2.failure().rfind("line 3:", 0) == 0);

  FakeHost host3;  // no trace configured
  ScriptRunner runner3(MustParse("wait_trace x 1000\n"), &host3);
  RunToEnd(runner3, host3);
  CHECK(runner3.state() == ScriptRunner::State::kFailed);
  CHECK(runner3.failure() == "line 1: wait_trace needs netplay_trace to be set");
  CHECK(host3.now == 0);
}

TEST_CASE("runner: screenshots and stop") {
  FakeHost host;
  host.screenshot_ok = false;
  ScriptRunner runner(MustParse("screenshot /tmp/a.ppm\nscreenshot_every 250 /tmp/f\nhold RT 5000\n"),
                      &host);
  CHECK(runner.Poll(0) == 5000u);
  CHECK(host.screenshots.size() == 1);
  CHECK(host.periodic && host.periodic_ms == 250 && host.periodic_dir == "/tmp/f");
  CHECK(host.inputs == kRightTrigger);
  bool saw_error = false;
  for (const auto& e : host.progress) {
    saw_error |= e.kind == ProgressEvent::Kind::kEnd && e.result == "error" &&
                 e.detail == "no guest output yet";
  }
  CHECK(saw_error);  // a failed screenshot is reported but does not stop the script
  host.now = 100;
  runner.Stop(host.now);
  CHECK(runner.state() == ScriptRunner::State::kStopped);
  CHECK(host.inputs == 0);
  CHECK(!host.periodic);
  CHECK(!runner.Poll(200).has_value());
  const ProgressEvent* fin = host.Finish();
  REQUIRE(fin);
  CHECK(fin->result == "stopped");
}

TEST_CASE("runner: touch signals another script, failure aborts") {
  FakeHost host;
  ScriptRunner host_side(MustParse("wait 200\ntouch /run/ready.flag\n"), &host);
  ScriptRunner join_side(MustParse("wait_file /run/ready.flag 1000\nlog joined\n"), &host);
  while (true) {
    const auto a = host_side.Poll(host.now);
    const auto b = join_side.Poll(host.now);
    if (!a && !b) break;
    host.now += 10;
    REQUIRE(host.now < 5000);
  }
  CHECK(host_side.state() == ScriptRunner::State::kFinished);
  CHECK(join_side.state() == ScriptRunner::State::kFinished);
  CHECK(host.now >= 200 && host.now <= 260);

  FakeHost host2;
  host2.touch_ok = false;
  ScriptRunner bad(MustParse("touch /ro/flag\nlog never\n"), &host2);
  RunToEnd(bad, host2);
  CHECK(bad.state() == ScriptRunner::State::kFailed);
  CHECK(bad.failure() == "line 1: touch failed: read-only");
}

// --- File helpers ------------------------------------------------------------------------


TEST_CASE("runner: repl runs command files in order and ends on 'end'") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("np_repl_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  auto write = [&](const char* name, const char* text) {
    std::ofstream f(dir / name, std::ios::binary);
    f << text;
  };
  FakeHost host;
  ScriptRunner runner(MustParse("log before\nrepl " + dir.string() + "\nlog after\n"), &host);
  CHECK(runner.Poll(0).has_value());  // waiting for cmd_0001.txt
  write("cmd_0001.txt", "press A 100\n");
  host.now = 50;
  runner.Poll(host.now);
  CHECK(host.inputs == kButtonA);
  host.now = 400;
  runner.Poll(host.now);  // release; the gap runs until 500
  CHECK(host.inputs == 0);
  host.now = 520;
  runner.Poll(host.now);
  CHECK(host.files.count((dir / "done_0001.txt").string()) == 1);  // via Touch
  CHECK(!fs::exists(dir / "cmd_0001.txt"));
  write("cmd_0002.txt", "bogus command\n");
  host.now = 560;
  runner.Poll(host.now);
  CHECK(fs::exists(dir / "done_0002.txt"));  // parse errors are reported in the done file
  write("cmd_0003.txt", "end\n");
  host.now = 600;
  for (int i = 0; i < 20 && runner.Poll(host.now); ++i) host.now += 50;
  CHECK(runner.state() == ScriptRunner::State::kFinished);
  bool saw_after = false;
  for (const auto& l : host.logs) saw_after |= l.find("script: after") != std::string::npos;
  CHECK(saw_after);
  fs::remove_all(dir);
}

TEST_CASE("trace tail: partial lines, truncation, missing file") {
  const fs::path p = TempDir() / "tail.jsonl";
  fs::remove(p);
  TraceTail tail(p);
  CHECK(!tail.Find("a"));  // missing file
  Append(p, "{\"ev\":\"alpha\"}\n{\"ev\":\"be");
  CHECK(!tail.Find("beta"));  // incomplete line
  Append(p, "ta\"}\n");
  CHECK(tail.Find("beta"));
  CHECK(!tail.Find("alpha"));  // passed over while looking for beta
  Append(p, "x\ny\nalpha-2\n");
  CHECK(tail.Find("alpha"));
  CHECK(!tail.Find("y"));  // y came before alpha-2
  // Rewrite the file shorter: the tail starts over.
  { std::ofstream(p, std::ios::trunc) << "z\n"; }
  CHECK(tail.Find("z"));
  // An over-long line is still matched on its first 64 KiB, and does not grow without bound.
  Append(p, "needle" + std::string(200000, 'q') + "\n");
  CHECK(tail.Find("needle"));
  Append(p, std::string(3 << 20, 'w') + "\nend-marker\n");
  int polls = 0;
  while (!tail.Find("end-marker") && polls < 10) ++polls;
  CHECK(polls < 10);
}

TEST_CASE("ppm: writes P6 with stride and validates input") {
  const fs::path p = TempDir() / "shot.ppm";
  std::vector<uint8_t> px(3 * 12, 0);  // 2x3 image, stride 12 (4 bytes of row padding)
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 2; ++x) {
      uint8_t* q = px.data() + y * 12 + x * 4;
      q[0] = static_cast<uint8_t>(10 * y + x);
      q[1] = 100;
      q[2] = 200;
      q[3] = 0xEE;  // X byte is dropped
    }
  }
  px.resize(2 * 12 + 8);  // the last row need not be padded
  std::string err;
  REQUIRE(WritePpm(p, 2, 3, 12, px, &err));
  const std::string data = ReadAll(p);
  const std::string header = "P6\n2 3\n255\n";
  REQUIRE(data.size() == header.size() + 18);
  CHECK(data.compare(0, header.size(), header) == 0);
  CHECK(static_cast<uint8_t>(data[header.size() + 0]) == 0);
  CHECK(static_cast<uint8_t>(data[header.size() + 3]) == 1);
  CHECK(static_cast<uint8_t>(data[header.size() + 6]) == 10);
  CHECK(static_cast<uint8_t>(data[header.size() + 7]) == 100);
  CHECK(static_cast<uint8_t>(data[header.size() + 17]) == 200);
  CHECK(!fs::exists(TempDir() / "shot.ppm.tmp"));

  CHECK(!WritePpm(p, 0, 3, 12, px, &err));
  CHECK(!WritePpm(p, 2, 3, 4, px, &err));  // stride smaller than a row
  CHECK(!WritePpm(p, 2, 4, 12, px, &err) && err == "image buffer is too small");
  CHECK(!WritePpm(p, 20000, 1, 80000, std::vector<uint8_t>(80000), &err));
  CHECK(!WritePpm(TempDir() / "no_such_dir" / "x.ppm", 2, 3, 12, px, &err));
}

TEST_CASE("helpers: next free index and names") {
  const fs::path d = TempDir() / "frames";
  fs::create_directories(d);
  CHECK(NextFreeIndex(d, "frame_", ".ppm") == 0);
  CHECK(NextFreeIndex(TempDir() / "missing", "frame_", ".ppm") == 0);
  for (const char* name : {"frame_0000.ppm", "frame_0007.ppm", "frame_12.ppm.tmp", "frame_abc.ppm",
                           "capture_0099.ppm", "frame_.ppm", "frame_12345678901.ppm"}) {
    std::ofstream(d / name) << "x";
  }
  CHECK(NextFreeIndex(d, "frame_", ".ppm") == 8);
  CHECK(NextFreeIndex(d, "capture_", ".ppm") == 100);
  CHECK(IndexedName("frame_", 7, ".ppm") == "frame_0007.ppm");
  CHECK(IndexedName("capture_", 123456, ".ppm") == "capture_123456.ppm");
}

TEST_CASE("helpers: json escape") {
  CHECK(JsonEscape("a\"b\\c") == "a\\\"b\\\\c");
  CHECK(JsonEscape("t\tn\n") == "t\\u0009n\\u000a");
  CHECK(JsonEscape("caf\xC3\xA9 \xE2\x9C\x88") == "caf\xC3\xA9 \xE2\x9C\x88");  // valid UTF-8 kept
  CHECK(JsonEscape("bad\xC3") == "bad?");
  CHECK(JsonEscape("\xED\xA0\x80") == "???");  // UTF-16 surrogate encoded in UTF-8
  CHECK(JsonEscape("\xF4\x90\x80\x80") == "????");  // above U+10FFFF
  CHECK(JsonEscape("\xC0\xAF") == "??");  // overlong
}

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int run = 0;
  for (const auto& c : Registry()) {
    if (filter && !std::strstr(c.name, filter)) continue;
    ++run;
    const int before = g_failures;
    try {
      c.fn();
    } catch (const RequireFailed&) {
    }
    std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", c.name);
  }
  std::error_code ec;
  fs::remove_all(TempDir(), ec);
  std::printf("%d test cases, %d failed checks\n", run, g_failures);
  return g_failures == 0 ? 0 : 1;
}

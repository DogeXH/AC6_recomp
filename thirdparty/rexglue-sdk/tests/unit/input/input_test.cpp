/**
 * Focused regressions for multi-driver and MnK XInput keystroke delivery.
 */

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::InputDriver;
using rex::input::InputSystem;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_KEYSTROKE;
using rex::input::X_INPUT_KEYSTROKE_KEYDOWN;
using rex::input::X_INPUT_KEYSTROKE_KEYUP;
using rex::input::X_INPUT_STATE;
using rex::input::X_INPUT_VIBRATION;
using rex::input::mnk::MnkInputDriver;
using rex::ui::VirtualKey;

class FixedKeystrokeDriver final : public InputDriver {
 public:
  FixedKeystrokeDriver(X_RESULT result, uint16_t virtual_key = 0)
      : InputDriver(nullptr, 0), result_(result), virtual_key_(virtual_key) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  X_RESULT GetCapabilities(uint32_t, uint32_t, X_INPUT_CAPABILITIES*) override { return result_; }
  X_RESULT GetState(uint32_t, X_INPUT_STATE*) override { return result_; }
  X_RESULT SetState(uint32_t, X_INPUT_VIBRATION*) override { return result_; }
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t, X_INPUT_KEYSTROKE* out_keystroke) override {
    if (result_ == X_ERROR_SUCCESS && out_keystroke) {
      *out_keystroke = {};
      out_keystroke->virtual_key = virtual_key_;
      out_keystroke->user_index = static_cast<uint8_t>(user_index);
    }
    return result_;
  }

 private:
  X_RESULT result_;
  uint16_t virtual_key_;
};

class ScopedMnkConfig {
 public:
  ScopedMnkConfig() {
    ok_ &= Set("mnk_mode", "true");
    ok_ &= Set("mnk_user_index", "0");
    ok_ &= Set("keybind_start", "Escape");
    ok_ &= Set("keybind_lstick_up", "W");
    ok_ &= Set("keybind_lstick_right", "D");
    ok_ &= Set("keybind_right_trigger", "LMB");
  }

  ~ScopedMnkConfig() {
    for (auto it = previous_.rbegin(); it != previous_.rend(); ++it) {
      rex::cvar::SetFlagByName(it->first, it->second);
    }
  }

  bool ok() const { return ok_; }

 private:
  bool Set(const char* name, const char* value) {
    previous_.emplace_back(name, rex::cvar::GetFlagByName(name));
    return rex::cvar::SetFlagByName(name, value);
  }

  bool ok_ = true;
  std::vector<std::pair<std::string, std::string>> previous_;
};

void SendKey(MnkInputDriver& driver, VirtualKey virtual_key, bool down) {
  rex::ui::KeyEvent event(nullptr, virtual_key, 0, false, false, false, false, false);
  if (down) {
    driver.OnKeyDown(event);
  } else {
    driver.OnKeyUp(event);
  }
}

void SendMouse(MnkInputDriver& driver, rex::ui::MouseEvent::Button button, bool down) {
  rex::ui::MouseEvent event(nullptr, button, 0, 0);
  if (down) {
    driver.OnMouseDown(event);
  } else {
    driver.OnMouseUp(event);
  }
}

X_RESULT ReadKeystroke(MnkInputDriver& driver, X_INPUT_KEYSTROKE& out_keystroke) {
  return driver.GetKeystroke(0, 0, &out_keystroke);
}

void CheckKeystroke(const X_INPUT_KEYSTROKE& keystroke, VirtualKey virtual_key, uint16_t flags,
                    uint8_t user_index = 0) {
  CHECK(keystroke.virtual_key == static_cast<uint16_t>(virtual_key));
  CHECK(keystroke.flags == flags);
  CHECK(keystroke.user_index == user_index);
}

}  // namespace

TEST_CASE("GetKeystroke scans past empty connected drivers", "[input]") {
  constexpr uint16_t kStart = static_cast<uint16_t>(VirtualKey::kXInputPadStart);

  SECTION("a later success wins over an earlier empty result") {
    InputSystem input(nullptr);
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_EMPTY));
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_SUCCESS, kStart));
    X_INPUT_KEYSTROKE keystroke = {};
    CHECK(input.GetKeystroke(0, 0, &keystroke) == X_ERROR_SUCCESS);
    CHECK(keystroke.virtual_key == kStart);
  }

  SECTION("connected empty plus disconnected returns empty") {
    InputSystem input(nullptr);
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_EMPTY));
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_DEVICE_NOT_CONNECTED));
    X_INPUT_KEYSTROKE keystroke = {};
    CHECK(input.GetKeystroke(0, 0, &keystroke) == X_ERROR_EMPTY);
  }

  SECTION("all disconnected remains device not connected") {
    InputSystem input(nullptr);
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_DEVICE_NOT_CONNECTED));
    input.AddDriver(std::make_unique<FixedKeystrokeDriver>(X_ERROR_DEVICE_NOT_CONNECTED));
    X_INPUT_KEYSTROKE keystroke = {};
    CHECK(input.GetKeystroke(0, 0, &keystroke) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
}

TEST_CASE("MnK preserves fast Start edges and suppresses duplicates", "[input]") {
  ScopedMnkConfig config;
  REQUIRE(config.ok());
  MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  SendKey(driver, VirtualKey::kEscape, true);
  SendKey(driver, VirtualKey::kEscape, true);
  SendKey(driver, VirtualKey::kEscape, false);
  SendKey(driver, VirtualKey::kEscape, false);

  X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYDOWN);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYUP);
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);
}

TEST_CASE("MnK left-stick direction releases before diagonal press", "[input]") {
  ScopedMnkConfig config;
  REQUIRE(config.ok());
  MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  SendKey(driver, VirtualKey::kW, true);
  X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadLThumbUp, X_INPUT_KEYSTROKE_KEYDOWN);

  SendKey(driver, VirtualKey::kD, true);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadLThumbUp, X_INPUT_KEYSTROKE_KEYUP);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadLThumbUpRight, X_INPUT_KEYSTROKE_KEYDOWN);
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);
}

TEST_CASE("MnK mouse edges map to right-trigger keystrokes", "[input]") {
  ScopedMnkConfig config;
  REQUIRE(config.ok());
  MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  SendMouse(driver, rex::ui::MouseEvent::Button::kLeft, true);
  SendMouse(driver, rex::ui::MouseEvent::Button::kLeft, false);

  X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadRTrigger, X_INPUT_KEYSTROKE_KEYDOWN);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadRTrigger, X_INPUT_KEYSTROKE_KEYUP);
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);
}

TEST_CASE("MnK focus and active gates cannot replay stale edges", "[input]") {
  ScopedMnkConfig config;
  REQUIRE(config.ok());
  MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  SendKey(driver, VirtualKey::kEscape, true);
  rex::ui::UISetupEvent focus_event;
  driver.OnLostFocus(focus_event);
  X_INPUT_KEYSTROKE keystroke = {};
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);

  driver.OnGotFocus(focus_event);
  SendKey(driver, VirtualKey::kEscape, true);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYDOWN);

  SendKey(driver, VirtualKey::kEscape, false);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYUP);

  bool active = true;
  driver.set_is_active_callback([&active] { return active; });
  SendKey(driver, VirtualKey::kEscape, true);
  active = false;
  // UI capture can change without a host focus event. Polling while inactive
  // must drain the already-queued down edge and held polled state.
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);
  X_INPUT_STATE state = {};
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == 0);

  active = true;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == 0);
  SendKey(driver, VirtualKey::kEscape, false);
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);

  // Once the host produces a new edge, normal delivery resumes.
  SendKey(driver, VirtualKey::kEscape, true);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYDOWN);

  // The P0 policy deliberately drains rather than synthesizing an UP if the
  // DOWN was already consumed. Polled state must still become and remain
  // neutral, and the old held host key must not resurrect on reactivation.
  active = false;
  CHECK(ReadKeystroke(driver, keystroke) == X_ERROR_EMPTY);
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == 0);
  active = true;
  REQUIRE(driver.GetState(0, &state) == X_ERROR_SUCCESS);
  CHECK(static_cast<uint16_t>(state.gamepad.buttons) == 0);
}

TEST_CASE("MnK bindings and user slot are immutable until driver restart", "[input]") {
  ScopedMnkConfig config;
  REQUIRE(config.ok());
  MnkInputDriver driver(nullptr, 0);
  REQUIRE(driver.Setup() == X_STATUS_SUCCESS);

  SendKey(driver, VirtualKey::kEscape, true);
  X_INPUT_KEYSTROKE keystroke = {};
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYDOWN);

  REQUIRE(rex::cvar::SetFlagByName("keybind_start", "Return"));
  REQUIRE(rex::cvar::SetFlagByName("mnk_user_index", "1"));

  // Runtime settings writes are persisted and marked requires-restart, but an
  // active driver keeps its registry-locked startup snapshot.
  SendKey(driver, VirtualKey::kEscape, false);
  REQUIRE(ReadKeystroke(driver, keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYUP);

  SendKey(driver, VirtualKey::kReturn, true);
  CHECK(driver.GetKeystroke(0, 0, &keystroke) == X_ERROR_EMPTY);
  CHECK(driver.GetKeystroke(1, 0, &keystroke) == X_ERROR_DEVICE_NOT_CONNECTED);

  MnkInputDriver restarted_driver(nullptr, 0);
  REQUIRE(restarted_driver.Setup() == X_STATUS_SUCCESS);
  SendKey(restarted_driver, VirtualKey::kReturn, true);
  REQUIRE(restarted_driver.GetKeystroke(1, 0, &keystroke) == X_ERROR_SUCCESS);
  CheckKeystroke(keystroke, VirtualKey::kXInputPadStart, X_INPUT_KEYSTROKE_KEYDOWN, 1);
}

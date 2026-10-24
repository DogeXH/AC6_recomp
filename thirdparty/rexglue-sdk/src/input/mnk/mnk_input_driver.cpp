/**
 * @file        input/mnk/mnk_input_driver.cpp
 * @brief       Keyboard/mouse input driver implementation.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/input/mnk/mnk_input_driver.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include <native/ui/windowed_app_context.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

#if REX_PLATFORM_WIN32
#include <Windows.h>
#include <rex/ui/window_win.h>
#endif

REXCVAR_DEFINE_BOOL(mnk_mode, false, "Input", "Enable keyboard/mouse controller emulation")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(mnk_user_index, 0, "Input", "Controller slot (0-3) for MnK")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(mnk_sensitivity, 1.0, "Input", "Mouse sensitivity for right stick")
    .range(0.01, 10.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(keybind_a, "Space", "Input/Keybinds/Controller", "A button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_b, "Shift", "Input/Keybinds/Controller", "B button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_x, "R", "Input/Keybinds/Controller", "X button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_y, "E", "Input/Keybinds/Controller", "Y button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_left_trigger, "RMB", "Input/Keybinds/Controller", "Left trigger")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_right_trigger, "LMB", "Input/Keybinds/Controller", "Right trigger")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "Q", "Input/Keybinds/Controller", "Left shoulder")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "F", "Input/Keybinds/Controller", "Right shoulder")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_lstick_up, "W", "Input/Keybinds/Controller", "Left stick up")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_lstick_down, "S", "Input/Keybinds/Controller", "Left stick down")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_lstick_left, "A", "Input/Keybinds/Controller", "Left stick left")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_lstick_right, "D", "Input/Keybinds/Controller", "Left stick right")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_lstick_press, "C", "Input/Keybinds/Controller", "Left stick press")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_rstick_press, "MMB", "Input/Keybinds/Controller", "Right stick press")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_dpad_up, "Up", "Input/Keybinds/Controller", "D-pad up")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_dpad_down, "Down", "Input/Keybinds/Controller", "D-pad down")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_dpad_left, "Left", "Input/Keybinds/Controller", "D-pad left")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_dpad_right, "Right", "Input/Keybinds/Controller", "D-pad right")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Input/Keybinds/Controller", "Back button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_start, "Escape", "Input/Keybinds/Controller", "Start button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(keybind_guide, "", "Input/Keybinds/Controller", "Guide button")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::input::mnk {

using rex::ui::VirtualKey;

namespace {

uint16_t QueryKey(std::string_view name) {
  return static_cast<uint16_t>(rex::ui::ParseVirtualKey(rex::cvar::Query<std::string>(name)));
}

}  // namespace

MnkInputDriver::MnkInputDriver(rex::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {}

MnkInputDriver::~MnkInputDriver() {
  // Detach handled by OnClosing; if window outlives the driver, clean up here.
  rex::ui::Window* window = nullptr;
  {
    std::lock_guard lock(state_mutex_);
    window = attached_window_;
    attached_window_ = nullptr;
    mouse_captured_ = false;
  }
  if (window) {
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  }
}

X_STATUS MnkInputDriver::Setup() {
  LoadInputConfig();
  InputConfig config;
  {
    std::lock_guard lock(state_mutex_);
    config = input_config_;
  }
  REXLOG_INFO("MnK input driver initialized: enabled={} user_index={}", config.enabled,
              config.user_index);
  return X_STATUS_SUCCESS;
}

void MnkInputDriver::LoadInputConfig() {
  InputConfig config;
  config.enabled = rex::cvar::Query<bool>("mnk_mode");
  config.user_index = static_cast<uint32_t>(rex::cvar::Query<int32_t>("mnk_user_index"));
  config.sensitivity = rex::cvar::Query<double>("mnk_sensitivity");

  auto set_action = [&config](PadAction action, std::string_view name) {
    config.action_keys[static_cast<size_t>(action)] = QueryKey(name);
  };
  set_action(PadAction::kA, "keybind_a");
  set_action(PadAction::kB, "keybind_b");
  set_action(PadAction::kX, "keybind_x");
  set_action(PadAction::kY, "keybind_y");
  set_action(PadAction::kLeftShoulder, "keybind_left_shoulder");
  set_action(PadAction::kRightShoulder, "keybind_right_shoulder");
  set_action(PadAction::kStart, "keybind_start");
  set_action(PadAction::kBack, "keybind_back");
  set_action(PadAction::kLeftThumbPress, "keybind_lstick_press");
  set_action(PadAction::kRightThumbPress, "keybind_rstick_press");
  set_action(PadAction::kDpadUp, "keybind_dpad_up");
  set_action(PadAction::kDpadDown, "keybind_dpad_down");
  set_action(PadAction::kDpadLeft, "keybind_dpad_left");
  set_action(PadAction::kDpadRight, "keybind_dpad_right");
  set_action(PadAction::kLeftTrigger, "keybind_left_trigger");
  set_action(PadAction::kRightTrigger, "keybind_right_trigger");

  config.guide_key = QueryKey("keybind_guide");
  config.left_stick_up_key = QueryKey("keybind_lstick_up");
  config.left_stick_down_key = QueryKey("keybind_lstick_down");
  config.left_stick_left_key = QueryKey("keybind_lstick_left");
  config.left_stick_right_key = QueryKey("keybind_lstick_right");

  std::lock_guard lock(state_mutex_);
  input_config_ = config;
  ClearInputState();
}

void MnkInputDriver::OnWindowAvailable(rex::ui::Window* window) {
  if (window) {
    {
      std::lock_guard lock(state_mutex_);
      attached_window_ = window;
      has_focus_ = true;
    }
    window->AddInputListener(this, window_z_order());
    window->AddListener(this);
  }
}

void MnkInputDriver::OnClosing(rex::ui::UIEvent&) {
  rex::ui::Window* window = nullptr;
  bool release_mouse = false;
  {
    std::lock_guard lock(state_mutex_);
    window = attached_window_;
    release_mouse = mouse_captured_;
    mouse_captured_ = false;
    attached_window_ = nullptr;
    has_focus_ = false;
    ClearInputState();
  }
  if (window) {
    if (release_mouse) {
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      window->ReleaseMouse();
    }
    window->RemoveInputListener(this);
    window->RemoveListener(this);
  }
}

static bool IsBindPressed(const bool (&key_down)[256], uint16_t virtual_key) {
  return virtual_key && virtual_key < 256 && key_down[virtual_key];
}

X_RESULT MnkInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  if (!input_config_.enabled || user_index != input_config_.user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->flags = 0;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetState(uint32_t user_index, X_INPUT_STATE* out_state) {
  const bool active = is_active();
  UpdateMouseCapture(input_config_.enabled, active);

  std::lock_guard lock(state_mutex_);
  const bool input_available = ReconcileInputContextLocked(active);
  if (!input_config_.enabled) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index != input_config_.user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!input_available) {
    // UI capture is not necessarily accompanied by a host focus event. Drop
    // both polled and queued state at the first inactive observation so a key
    // held before opening a dialog cannot reappear after it closes.
    ClearInputState();
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  uint16_t buttons = 0;
  auto action_pressed = [this](PadAction action) {
    return IsBindPressed(key_down_, input_config_.action_keys[static_cast<size_t>(action)]);
  };
  if (action_pressed(PadAction::kA))
    buttons |= X_INPUT_GAMEPAD_A;
  if (action_pressed(PadAction::kB))
    buttons |= X_INPUT_GAMEPAD_B;
  if (action_pressed(PadAction::kX))
    buttons |= X_INPUT_GAMEPAD_X;
  if (action_pressed(PadAction::kY))
    buttons |= X_INPUT_GAMEPAD_Y;
  if (action_pressed(PadAction::kLeftShoulder))
    buttons |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (action_pressed(PadAction::kRightShoulder))
    buttons |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  if (action_pressed(PadAction::kLeftThumbPress))
    buttons |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (action_pressed(PadAction::kRightThumbPress))
    buttons |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (action_pressed(PadAction::kBack))
    buttons |= X_INPUT_GAMEPAD_BACK;
  if (action_pressed(PadAction::kStart))
    buttons |= X_INPUT_GAMEPAD_START;
  if (IsBindPressed(key_down_, input_config_.guide_key))
    buttons |= X_INPUT_GAMEPAD_GUIDE;
  if (action_pressed(PadAction::kDpadUp))
    buttons |= X_INPUT_GAMEPAD_DPAD_UP;
  if (action_pressed(PadAction::kDpadDown))
    buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (action_pressed(PadAction::kDpadLeft))
    buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (action_pressed(PadAction::kDpadRight))
    buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;

  uint8_t lt = action_pressed(PadAction::kLeftTrigger) ? 0xFF : 0;
  uint8_t rt = action_pressed(PadAction::kRightTrigger) ? 0xFF : 0;

  int32_t lx = 0;
  int32_t ly = 0;
  if (IsBindPressed(key_down_, input_config_.left_stick_left_key))
    lx -= INT16_MAX;
  if (IsBindPressed(key_down_, input_config_.left_stick_right_key))
    lx += INT16_MAX;
  if (IsBindPressed(key_down_, input_config_.left_stick_up_key))
    ly += INT16_MAX;
  if (IsBindPressed(key_down_, input_config_.left_stick_down_key))
    ly -= INT16_MAX;

  constexpr double kBaseScale = 200.0;
  int32_t rx = static_cast<int32_t>(mouse_dx_ * input_config_.sensitivity * kBaseScale);
  int32_t ry = static_cast<int32_t>(-mouse_dy_ * input_config_.sensitivity * kBaseScale);
  mouse_dx_ = 0;
  mouse_dy_ = 0;

  auto clamp16 = [](int32_t v) -> int16_t {
    return static_cast<int16_t>(std::clamp(v, (int32_t)INT16_MIN, (int32_t)INT16_MAX));
  };

  packet_number_++;

  if (out_state) {
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = buttons;
    out_state->gamepad.left_trigger = lt;
    out_state->gamepad.right_trigger = rt;
    out_state->gamepad.thumb_lx = clamp16(lx);
    out_state->gamepad.thumb_ly = clamp16(ly);
    out_state->gamepad.thumb_rx = clamp16(rx);
    out_state->gamepad.thumb_ry = clamp16(ry);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) {
  if (!input_config_.enabled || user_index != input_config_.user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT MnkInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  const bool active = is_active();
  UpdateMouseCapture(input_config_.enabled, active);
  std::lock_guard lock(state_mutex_);
  const bool input_available = ReconcileInputContextLocked(active);
  if (!input_config_.enabled) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (user_index != input_config_.user_index) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!input_available) {
    return X_ERROR_EMPTY;
  }
  if (keystroke_queue_.empty()) {
    return X_ERROR_EMPTY;
  }
  if (out_keystroke) {
    *out_keystroke = keystroke_queue_.front();
  }
  keystroke_queue_.pop();
  return X_ERROR_SUCCESS;
}

void MnkInputDriver::EnqueueKeystroke(uint16_t vk_pad, bool down) {
  X_INPUT_KEYSTROKE ks = {};
  ks.virtual_key = vk_pad;
  ks.unicode = 0;
  ks.flags = down ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
  ks.user_index = static_cast<uint8_t>(input_config_.user_index);
  ks.hid_code = 0;
  keystroke_queue_.push(ks);
}

void MnkInputDriver::HandleEdge(PadAction action, uint16_t vk_pad, bool down) {
  bool& was_down = pad_action_down_[static_cast<size_t>(action)];
  if (was_down == down) {
    return;
  }
  was_down = down;
  EnqueueKeystroke(vk_pad, down);
}

void MnkInputDriver::EmitButtonChange(uint16_t host_vk, bool down) {
  static constexpr std::array<VirtualKey, static_cast<size_t>(PadAction::kCount)>
      kActionVirtualKeys = {
          VirtualKey::kXInputPadA,           VirtualKey::kXInputPadB,
          VirtualKey::kXInputPadX,           VirtualKey::kXInputPadY,
          VirtualKey::kXInputPadLShoulder,   VirtualKey::kXInputPadRShoulder,
          VirtualKey::kXInputPadStart,       VirtualKey::kXInputPadBack,
          VirtualKey::kXInputPadLThumbPress, VirtualKey::kXInputPadRThumbPress,
          VirtualKey::kXInputPadDpadUp,      VirtualKey::kXInputPadDpadDown,
          VirtualKey::kXInputPadDpadLeft,    VirtualKey::kXInputPadDpadRight,
          VirtualKey::kXInputPadLTrigger,    VirtualKey::kXInputPadRTrigger,
      };
  for (size_t i = 0; i < kActionVirtualKeys.size(); ++i) {
    if (input_config_.action_keys[i] == host_vk) {
      HandleEdge(static_cast<PadAction>(i), static_cast<uint16_t>(kActionVirtualKeys[i]), down);
    }
  }
  // Guide has no VK_PAD_GUIDE in the XInput keystroke API.
}

void MnkInputDriver::RecomputeLeftStickDirection() {
  int32_t x = 0;
  int32_t y = 0;
  if (IsBindPressed(key_down_, input_config_.left_stick_left_key))
    --x;
  if (IsBindPressed(key_down_, input_config_.left_stick_right_key))
    ++x;
  if (IsBindPressed(key_down_, input_config_.left_stick_up_key))
    ++y;
  if (IsBindPressed(key_down_, input_config_.left_stick_down_key))
    --y;

  uint16_t new_direction = 0;
  if (y > 0) {
    new_direction = static_cast<uint16_t>(x < 0   ? VirtualKey::kXInputPadLThumbUpLeft
                                          : x > 0 ? VirtualKey::kXInputPadLThumbUpRight
                                                  : VirtualKey::kXInputPadLThumbUp);
  } else if (y < 0) {
    new_direction = static_cast<uint16_t>(x < 0   ? VirtualKey::kXInputPadLThumbDownLeft
                                          : x > 0 ? VirtualKey::kXInputPadLThumbDownRight
                                                  : VirtualKey::kXInputPadLThumbDown);
  } else if (x < 0) {
    new_direction = static_cast<uint16_t>(VirtualKey::kXInputPadLThumbLeft);
  } else if (x > 0) {
    new_direction = static_cast<uint16_t>(VirtualKey::kXInputPadLThumbRight);
  }

  if (new_direction == left_stick_direction_vk_pad_) {
    return;
  }
  if (left_stick_direction_vk_pad_) {
    EnqueueKeystroke(left_stick_direction_vk_pad_, false);
  }
  left_stick_direction_vk_pad_ = new_direction;
  if (left_stick_direction_vk_pad_) {
    EnqueueKeystroke(left_stick_direction_vk_pad_, true);
  }
}

void MnkInputDriver::ClearInputState() {
  std::memset(key_down_, 0, sizeof(key_down_));
  mouse_dx_ = 0;
  mouse_dy_ = 0;
  std::queue<X_INPUT_KEYSTROKE> empty_queue;
  keystroke_queue_.swap(empty_queue);
  pad_action_down_.fill(false);
  left_stick_direction_vk_pad_ = 0;
}

bool MnkInputDriver::ReconcileInputContextLocked(bool active) {
  if (!input_config_.enabled || !has_focus_ || !active) {
    ClearInputState();
    return false;
  }
  return true;
}

void MnkInputDriver::CenterCursor(rex::ui::Window* window) {
  if (!window)
    return;
  {
    std::lock_guard lock(state_mutex_);
    if (attached_window_ != window || !mouse_captured_) {
      return;
    }
  }
  int32_t cx = static_cast<int32_t>(window->GetActualLogicalWidth() / 2);
  int32_t cy = static_cast<int32_t>(window->GetActualLogicalHeight() / 2);
  {
    std::lock_guard lock(state_mutex_);
    if (attached_window_ != window || !mouse_captured_) {
      return;
    }
    prev_mouse_x_ = cx;
    prev_mouse_y_ = cy;
  }
#if REX_PLATFORM_WIN32
  auto* win32_window = dynamic_cast<rex::ui::Win32Window*>(window);
  if (win32_window && win32_window->hwnd()) {
    POINT pt = {static_cast<LONG>(cx), static_cast<LONG>(cy)};
    ClientToScreen(win32_window->hwnd(), &pt);
    SetCursorPos(pt.x, pt.y);
  }
#endif
}

void MnkInputDriver::UpdateMouseCapture(bool enabled, bool active) {
  rex::ui::Window* window = nullptr;
  rex::ui::WindowedAppContext* app_context = nullptr;
  {
    std::lock_guard lock(state_mutex_);
    window = attached_window_;
    if (!window) {
      return;
    }
    const bool should_capture = enabled && has_focus_ && active;
    bool needs_ui_update = should_capture != mouse_captured_;
#if REX_PLATFORM_WIN32
    needs_ui_update = needs_ui_update || mouse_captured_;
#endif
    if (!needs_ui_update) {
      return;
    }
    app_context = &window->app_context();
  }

  // Serialize all Window operations with focus and close callbacks on the UI
  // thread. Synchronous dispatch also guarantees that this lambda can't outlive
  // the driver. Internal state is never locked while a Window call may re-enter
  // an input callback.
  app_context->CallInUIThreadSynchronous([this, window, enabled, active] {
    bool start_capture = false;
    bool stop_capture = false;
    bool center_cursor = false;
    {
      std::lock_guard lock(state_mutex_);
      if (attached_window_ != window) {
        return;
      }
      const bool should_capture = enabled && has_focus_ && active;
      if (should_capture && !mouse_captured_) {
        mouse_captured_ = true;
        start_capture = true;
        mouse_dx_ = 0;
        mouse_dy_ = 0;
      } else if (!should_capture && mouse_captured_) {
        mouse_captured_ = false;
        stop_capture = true;
      }
      center_cursor = mouse_captured_;
    }

    if (start_capture) {
      window->CaptureMouse();
      bool still_captured = false;
      {
        std::lock_guard lock(state_mutex_);
        still_captured = attached_window_ == window && mouse_captured_;
      }
      if (still_captured) {
        window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      }
    } else if (stop_capture) {
      window->ReleaseMouse();
      bool still_attached = false;
      {
        std::lock_guard lock(state_mutex_);
        still_attached = attached_window_ == window;
      }
      if (still_attached) {
        window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
      }
    }
    if (center_cursor) {
      CenterCursor(window);
    }
  });
}

void MnkInputDriver::SetKeyState(uint16_t vk, bool down) {
  if (vk < 256) {
    key_down_[vk] = down;
  }
}

void MnkInputDriver::OnKeyDown(rex::ui::KeyEvent& e) {
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!ReconcileInputContextLocked(active)) {
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (!vk || vk >= 256 || key_down_[vk]) {
    return;
  }
  SetKeyState(vk, true);
  EmitButtonChange(vk, true);
  RecomputeLeftStickDirection();
}

void MnkInputDriver::OnKeyUp(rex::ui::KeyEvent& e) {
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!ReconcileInputContextLocked(active)) {
    return;
  }
  uint16_t vk = static_cast<uint16_t>(e.virtual_key());
  if (!vk || vk >= 256 || !key_down_[vk]) {
    return;
  }
  SetKeyState(vk, false);
  EmitButtonChange(vk, false);
  RecomputeLeftStickDirection();
}

void MnkInputDriver::OnMouseDown(rex::ui::MouseEvent& e) {
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!ReconcileInputContextLocked(active)) {
    return;
  }
  uint16_t vk = 0;
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      vk = static_cast<uint16_t>(VirtualKey::kLButton);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      vk = static_cast<uint16_t>(VirtualKey::kRButton);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      vk = static_cast<uint16_t>(VirtualKey::kMButton);
      break;
    default:
      return;
  }
  if (key_down_[vk]) {
    return;
  }
  SetKeyState(vk, true);
  EmitButtonChange(vk, true);
  RecomputeLeftStickDirection();
}

void MnkInputDriver::OnMouseUp(rex::ui::MouseEvent& e) {
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!ReconcileInputContextLocked(active)) {
    return;
  }
  uint16_t vk = 0;
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      vk = static_cast<uint16_t>(VirtualKey::kLButton);
      break;
    case rex::ui::MouseEvent::Button::kRight:
      vk = static_cast<uint16_t>(VirtualKey::kRButton);
      break;
    case rex::ui::MouseEvent::Button::kMiddle:
      vk = static_cast<uint16_t>(VirtualKey::kMButton);
      break;
    default:
      return;
  }
  if (!key_down_[vk]) {
    return;
  }
  SetKeyState(vk, false);
  EmitButtonChange(vk, false);
  RecomputeLeftStickDirection();
}

void MnkInputDriver::OnMouseMove(rex::ui::MouseEvent& e) {
  const bool active = is_active();
  std::lock_guard lock(state_mutex_);
  if (!ReconcileInputContextLocked(active)) {
    return;
  }
  int32_t x = e.x();
  int32_t y = e.y();
  mouse_dx_ += x - prev_mouse_x_;
  mouse_dy_ += y - prev_mouse_y_;
  prev_mouse_x_ = x;
  prev_mouse_y_ = y;
}

void MnkInputDriver::OnLostFocus(rex::ui::UISetupEvent&) {
  rex::ui::Window* window = nullptr;
  bool release_mouse = false;
  {
    std::lock_guard lock(state_mutex_);
    has_focus_ = false;
    ClearInputState();
    window = attached_window_;
    release_mouse = mouse_captured_;
    mouse_captured_ = false;
  }
  if (release_mouse && window) {
    window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    window->ReleaseMouse();
  }
}

void MnkInputDriver::OnGotFocus(rex::ui::UISetupEvent&) {
  std::lock_guard lock(state_mutex_);
  has_focus_ = true;
}

}  // namespace rex::input::mnk

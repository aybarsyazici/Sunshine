/**
 * @file src/platform/windows/input/winuhid_gamepad.cpp
 * @brief Definitions for WinUHid gamepad input handling.
 */
// Windows headers
#include <windows.h>

// WinUHid headers
#include "WinUHid.h"
#include "WinUHidPS5.h"

// Local headers
#include "winuhid_gamepad.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

#include <memory>
#include <mutex>
#include <cmath>

using namespace std::literals;

namespace platf {
  // Forward declare input_raw_t from input.cpp
  struct input_raw_t {
    void *vigem;
    std::vector<std::shared_ptr<void>> gamepads;
  };

  // Helper function to convert degrees to radians
  inline float deg2rad(float degrees) {
    return degrees * (M_PI / 180.0f);
  }
}

namespace platf::gamepad {

  struct winuhid_joypad_state {
    PWINUHID_PS5_GAMEPAD device = nullptr;
    WINUHID_PS5_INPUT_REPORT report = {};
    std::mutex report_mutex;
    uint8_t client_index = 0;  // Store client-relative index for feedback

    // Track last feedback to avoid duplicate sends
    gamepad_feedback_msg_t last_rumble {};
    gamepad_feedback_msg_t last_rgb_led {};

    // Touch tracking (up to 2 fingers on PS5 touchpad)
    struct TouchState {
      bool active = false;
      uint16_t x = 0;
      uint16_t y = 0;
    };
    TouchState touches[2];

    ~winuhid_joypad_state() {
      if (device) {
        WinUHidPS5Destroy(device);
        device = nullptr;
      }
    }
  };

  // Convert Sunshine button flags to PS5 button state
  static void
  map_buttons(const gamepad_state_t &gamepad_state, PWINUHID_PS5_INPUT_REPORT report) {
    auto flags = gamepad_state.buttonFlags;

    // Face buttons
    report->ButtonCross = (flags & A) ? 1 : 0;
    report->ButtonCircle = (flags & B) ? 1 : 0;
    report->ButtonSquare = (flags & X) ? 1 : 0;
    report->ButtonTriangle = (flags & Y) ? 1 : 0;

    // Shoulder buttons
    report->ButtonL1 = (flags & LEFT_BUTTON) ? 1 : 0;
    report->ButtonR1 = (flags & RIGHT_BUTTON) ? 1 : 0;

    // Stick buttons
    report->ButtonL3 = (flags & LEFT_STICK) ? 1 : 0;
    report->ButtonR3 = (flags & RIGHT_STICK) ? 1 : 0;

    // System buttons
    report->ButtonShare = (flags & BACK) ? 1 : 0;
    report->ButtonOptions = (flags & START) ? 1 : 0;
    report->ButtonHome = (flags & HOME) ? 1 : 0;
    report->ButtonTouchpad = (flags & TOUCHPAD_BUTTON) ? 1 : 0;
    report->ButtonMute = (flags & MISC_BUTTON) ? 1 : 0;

    // D-pad via hat switch
    int hat_x = 0, hat_y = 0;
    if (flags & DPAD_LEFT) hat_x = -1;
    if (flags & DPAD_RIGHT) hat_x = 1;
    if (flags & DPAD_UP) hat_y = -1;
    if (flags & DPAD_DOWN) hat_y = 1;
    WinUHidPS5SetHatState(report, hat_x, hat_y);
  }

  // Convert Sunshine stick coordinates to PS5 format
  // Sunshine: int16_t range [-32768, 32767], centered at 0
  // PS5: uint8_t range [0, 255], centered at 0x80 (128)
  static UCHAR
  convert_stick_axis(std::int16_t axis, bool invert_y = false) {
    // Invert Y-axis if requested (standard gamepad convention: up = negative)
    if (invert_y) {
      axis = -axis;
    }
    // Scale from [-32768, 32767] to [0, 255]
    // Add 32768 to shift to [0, 65535], then divide by 256
    return static_cast<UCHAR>((axis + 32768) / 256);
  }

  // Rumble callback: forward to Moonlight client
  static VOID WINAPI
  rumble_callback(PVOID context, UCHAR left_motor, UCHAR right_motor) {
    auto ctx = static_cast<std::pair<feedback_queue_t, std::shared_ptr<winuhid_joypad_state>>*>(context);
    auto &feedback_queue = ctx->first;
    auto &joypad = ctx->second;

    // Don't resend duplicate rumble data
    if (joypad->last_rumble.type == platf::gamepad_feedback_e::rumble &&
        joypad->last_rumble.data.rumble.lowfreq == left_motor &&
        joypad->last_rumble.data.rumble.highfreq == right_motor) {
      return;
    }

    auto msg = gamepad_feedback_msg_t::make_rumble(joypad->client_index, left_motor, right_motor);
    feedback_queue->raise(msg);
    joypad->last_rumble = msg;
  }

  // LED callback: forward to Moonlight client
  static VOID WINAPI
  lightbar_callback(PVOID context, UCHAR r, UCHAR g, UCHAR b) {
    auto ctx = static_cast<std::pair<feedback_queue_t, std::shared_ptr<winuhid_joypad_state>>*>(context);
    auto &feedback_queue = ctx->first;
    auto &joypad = ctx->second;

    // Don't resend duplicate LED data
    if (joypad->last_rgb_led.type == platf::gamepad_feedback_e::set_rgb_led &&
        joypad->last_rgb_led.data.rgb_led.r == r &&
        joypad->last_rgb_led.data.rgb_led.g == g &&
        joypad->last_rgb_led.data.rgb_led.b == b) {
      return;
    }

    auto msg = gamepad_feedback_msg_t::make_rgb_led(joypad->client_index, r, g, b);
    feedback_queue->raise(msg);
    joypad->last_rgb_led = msg;
  }

  // Player LED callback: not currently used by Sunshine
  static VOID WINAPI
  player_led_callback(PVOID context, UCHAR led_value) {
    // No-op for now
  }

  // Trigger effect callback: forward to Moonlight client
  static VOID WINAPI
  trigger_effect_callback(PVOID context, PCWINUHID_PS5_TRIGGER_EFFECT left, PCWINUHID_PS5_TRIGGER_EFFECT right) {
    auto ctx = static_cast<std::pair<feedback_queue_t, std::shared_ptr<winuhid_joypad_state>>*>(context);
    auto &feedback_queue = ctx->first;

    // Build trigger effect message
    uint8_t event_flags = 0;
    if (left) event_flags |= 0x08;  // Left trigger
    if (right) event_flags |= 0x04;  // Right trigger

    std::array<uint8_t, 10> left_data = {}, right_data = {};
    uint8_t type_left = left ? left->Type : 0;
    uint8_t type_right = right ? right->Type : 0;

    if (left) {
      std::memcpy(left_data.data(), left->Data, 10);
    }
    if (right) {
      std::memcpy(right_data.data(), right->Data, 10);
    }

    auto &joypad = ctx->second;
    feedback_queue->raise(gamepad_feedback_msg_t::make_adaptive_triggers(
      joypad->client_index, event_flags, type_left, type_right, left_data, right_data));
  }

  // Mic LED callback: not currently used by Sunshine
  static VOID WINAPI
  mic_led_callback(PVOID context, UCHAR led_state) {
    // No-op for now
  }

  int
  alloc_winuhid(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue) {
    BOOST_LOG(info) << "Creating WinUHid DualSense 5 controller for gamepad " << id.globalIndex;

    // Create joypad state
    auto joypad = std::make_shared<winuhid_joypad_state>();
    joypad->client_index = id.clientRelativeIndex;

    // Initialize input report
    WinUHidPS5InitializeInputReport(&joypad->report);

    // Set up device info
    WINUHID_PS5_GAMEPAD_INFO device_info = {};

    // Generate MAC address based on gamepad index (similar to inputtino)
    if (!config::input.ds5_inputtino_randomize_mac && id.globalIndex >= 0 && id.globalIndex <= 255) {
      // Private virtual MAC: 02:00:00:00:00:XX where XX is the gamepad index
      device_info.MacAddress[0] = 0x02;
      device_info.MacAddress[1] = 0x00;
      device_info.MacAddress[2] = 0x00;
      device_info.MacAddress[3] = 0x00;
      device_info.MacAddress[4] = 0x00;
      device_info.MacAddress[5] = static_cast<UCHAR>(id.globalIndex);
    } else {
      // Random MAC
      for (int i = 0; i < 6; i++) {
        device_info.MacAddress[i] = static_cast<UCHAR>(rand() % 256);
      }
      device_info.MacAddress[0] = (device_info.MacAddress[0] & 0xFE) | 0x02;  // Set private bit
    }

    // Create callback context (freed when joypad is destroyed)
    auto callback_context = new std::pair<feedback_queue_t, std::shared_ptr<winuhid_joypad_state>>(feedback_queue, joypad);

    // Create WinUHid PS5 device
    joypad->device = WinUHidPS5Create(
      &device_info,
      rumble_callback,
      lightbar_callback,
      player_led_callback,
      trigger_effect_callback,
      mic_led_callback,
      callback_context
    );

    if (!joypad->device) {
      DWORD last_error = GetLastError();
      BOOST_LOG(error) << "Failed to create WinUHid PS5 gamepad: error " << last_error;
      delete callback_context;
      return -1;
    }

    BOOST_LOG(info) << "WinUHid DualSense 5 controller created successfully for gamepad " << id.globalIndex;

    // Activate motion sensors by sending motion event state feedback
    feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_ACCEL, 100));
    feedback_queue->raise(gamepad_feedback_msg_t::make_motion_event_state(id.clientRelativeIndex, LI_MOTION_TYPE_GYRO, 100));

    // Store in raw input array
    raw->gamepads[id.globalIndex] = joypad;

    return 0;
  }

  void
  free_winuhid(input_raw_t *raw, int nr) {
    if (auto joypad = std::static_pointer_cast<winuhid_joypad_state>(raw->gamepads[nr])) {
      BOOST_LOG(info) << "Destroying WinUHid gamepad " << nr;
      raw->gamepads[nr].reset();
    }
  }

  void
  update_winuhid(input_raw_t *raw, int nr, const gamepad_state_t &gamepad_state) {
    auto joypad = std::static_pointer_cast<winuhid_joypad_state>(raw->gamepads[nr]);
    if (!joypad || !joypad->device) {
      return;
    }

    std::lock_guard<std::mutex> lock(joypad->report_mutex);

    // Map buttons
    map_buttons(gamepad_state, &joypad->report);

    // Map analog sticks (invert Y-axis: up = negative in gamepad convention)
    joypad->report.LeftStickX = convert_stick_axis(gamepad_state.lsX, false);
    joypad->report.LeftStickY = convert_stick_axis(gamepad_state.lsY, true);
    joypad->report.RightStickX = convert_stick_axis(gamepad_state.rsX, false);
    joypad->report.RightStickY = convert_stick_axis(gamepad_state.rsY, true);

    // Map triggers
    joypad->report.LeftTrigger = gamepad_state.lt;
    joypad->report.RightTrigger = gamepad_state.rt;

    // Also set L2/R2 button flags based on trigger threshold
    joypad->report.ButtonL2 = (gamepad_state.lt > 128) ? 1 : 0;
    joypad->report.ButtonR2 = (gamepad_state.rt > 128) ? 1 : 0;

    // Submit input report
    if (!WinUHidPS5ReportInput(joypad->device, &joypad->report)) {
      BOOST_LOG(error) << "Failed to submit WinUHid PS5 input report: error " << GetLastError();
    }
  }

  void
  touch_winuhid(input_raw_t *raw, const gamepad_touch_t &touch) {
    BOOST_LOG(debug) << "touch_winuhid called: gamepad=" << touch.id.globalIndex
                     << " pointer=" << touch.pointerId
                     << " x=" << touch.x << " y=" << touch.y
                     << " pressure=" << touch.pressure;

    auto joypad = std::static_pointer_cast<winuhid_joypad_state>(raw->gamepads[touch.id.globalIndex]);
    if (!joypad || !joypad->device) {
      BOOST_LOG(warning) << "touch_winuhid: joypad not found for gamepad " << touch.id.globalIndex;
      return;
    }

    std::lock_guard<std::mutex> lock(joypad->report_mutex);

    // PS5 touchpad is 1920x1080
    constexpr int touchpad_width = 1920;
    constexpr int touchpad_height = 1080;

    // Map pointer ID to touch index (0 or 1)
    UCHAR touch_index = touch.pointerId % 2;

    if (touch.pressure > 0.5f) {
      // Touch down
      USHORT x = static_cast<USHORT>(touch.x * touchpad_width);
      USHORT y = static_cast<USHORT>(touch.y * touchpad_height);

      BOOST_LOG(debug) << "Touch down: finger=" << (int)touch_index << " pos=(" << x << "," << y << ")";

      joypad->touches[touch_index].active = true;
      joypad->touches[touch_index].x = x;
      joypad->touches[touch_index].y = y;

      WinUHidPS5SetTouchState(&joypad->report, touch_index, TRUE, x, y);
    } else {
      // Touch up
      BOOST_LOG(debug) << "Touch up: finger=" << (int)touch_index;
      joypad->touches[touch_index].active = false;
      WinUHidPS5SetTouchState(&joypad->report, touch_index, FALSE, 0, 0);
    }
  }

  void
  motion_winuhid(input_raw_t *raw, const gamepad_motion_t &motion) {
    auto joypad = std::static_pointer_cast<winuhid_joypad_state>(raw->gamepads[motion.id.globalIndex]);
    if (!joypad || !joypad->device) {
      return;
    }

    std::lock_guard<std::mutex> lock(joypad->report_mutex);

    switch (motion.motionType) {
      case LI_MOTION_TYPE_ACCEL:
        // Accelerometer: already in m/s^2, pass directly
        WinUHidPS5SetAccelState(&joypad->report, motion.x, motion.y, motion.z);
        break;

      case LI_MOTION_TYPE_GYRO:
        // Gyroscope: Sunshine provides deg/s, WinUHid expects rad/s
        WinUHidPS5SetGyroState(&joypad->report,
          platf::deg2rad(motion.x),
          platf::deg2rad(motion.y),
          platf::deg2rad(motion.z));
        break;
    }
  }

  void
  battery_winuhid(input_raw_t *raw, const gamepad_battery_t &battery) {
    auto joypad = std::static_pointer_cast<winuhid_joypad_state>(raw->gamepads[battery.id.globalIndex]);
    if (!joypad || !joypad->device) {
      return;
    }

    std::lock_guard<std::mutex> lock(joypad->report_mutex);

    // Determine if wired or wireless based on battery state
    BOOL wired = (battery.state == LI_BATTERY_STATE_CHARGING || battery.state == LI_BATTERY_STATE_FULL);
    UCHAR percentage = static_cast<UCHAR>(battery.percentage);

    WinUHidPS5SetBatteryState(&joypad->report, wired, percentage);
  }

}  // namespace platf::gamepad

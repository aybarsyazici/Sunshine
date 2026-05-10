/**
 * @file src/platform/windows/input/winuhid_gamepad.h
 * @brief Declarations for WinUHid gamepad input handling.
 */
#pragma once

#include "src/platform/common.h"

namespace platf::gamepad {
  /**
   * Allocate a new gamepad using WinUHid backend.
   */
  int
  alloc_winuhid(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);

  /**
   * Free a WinUHid gamepad.
   */
  void
  free_winuhid(input_raw_t *raw, int nr);

  /**
   * Update gamepad state (buttons, sticks, triggers).
   */
  void
  update_winuhid(input_raw_t *raw, int nr, const gamepad_state_t &gamepad_state);

  /**
   * Update touchpad state.
   */
  void
  touch_winuhid(input_raw_t *raw, const gamepad_touch_t &touch);

  /**
   * Update motion sensor state (accelerometer, gyroscope).
   */
  void
  motion_winuhid(input_raw_t *raw, const gamepad_motion_t &motion);

  /**
   * Update battery state.
   */
  void
  battery_winuhid(input_raw_t *raw, const gamepad_battery_t &battery);

}  // namespace platf::gamepad

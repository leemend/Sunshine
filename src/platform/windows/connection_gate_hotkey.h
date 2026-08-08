/**
 * @file src/platform/windows/connection_gate_hotkey.h
 * @brief Global hotkey (Ctrl+Alt+Shift+O) that toggles the connection gate open/closed,
 * even while a fullscreen exclusive game has input focus - global hotkeys are OS-level and
 * bypass whatever has capture, on Windows.
 */
#pragma once

namespace connection_gate_hotkey {

  /**
   * @brief Registers the global hotkey and starts listening for it, on a dedicated background
   * thread with its own hidden message-only window. Call once, at startup.
   */
  void init();

}  // namespace connection_gate_hotkey

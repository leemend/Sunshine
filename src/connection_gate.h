/**
 * @file src/connection_gate.h
 * @brief Runtime connection gate: controls whether paired clients can actually start a new
 * streaming session, independent of pairing/cert trust (which remains permanent, as normal).
 */
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace connection_gate {

  enum class mode_e {
    open,  ///< Always accept connections from paired clients.
    closed,  ///< Reject all new session launches.
    auto_close,  ///< Open now; once the last client disconnects (session count reaches 0),
                 ///< starts a configurable delay (connection_gate_auto_close_seconds); if it's
                 ///< still 0 when that elapses, closes. A delay of 0 makes this close
                 ///< immediately - there's no separate "immediate" mode anymore. Re-arms fully
                 ///< every time a session becomes active again, so a reconnect within the
                 ///< delay window naturally finds the gate still open - no separate grace
                 ///< period is needed on top of this.
  };

  /**
   * @brief Parses a config string ("open"/"closed"/"auto_close") into mode_e.
   * @details Falls back to mode_e::closed for any unrecognized value, since failing closed
   * is the safe default for a feature whose whole purpose is restricting access. Also accepts
   * the old "auto_inactivity" string for backward compatibility with existing sunshine.conf
   * files from before these two modes were merged - treated the same as "auto_close".
   */
  mode_e mode_from_string(const std::string &value);

  std::string mode_to_string(mode_e mode);

  /**
   * @brief Initializes the live gate state from config at startup. Call once, at startup.
   */
  void init();

  /**
   * @brief Returns the currently active mode. This is live, runtime state - it starts from
   * the configured default but can change afterward (systray, hotkey, web UI), independent of
   * what's written in sunshine.conf.
   */
  mode_e current_mode();

  /**
   * @brief Changes the live mode. Does not touch sunshine.conf - this is a runtime toggle,
   * not a persistent config change. Invokes the on-mode-changed callback (if one is
   * registered) whenever this actually changes the mode.
   */
  void set_mode(mode_e mode);

  /**
   * @brief Registers a callback invoked whenever set_mode() actually changes the mode,
   * regardless of what called it (systray, hotkey, web UI). Used by system_tray.cpp to keep
   * its own checkmarks in sync even when the mode changes from somewhere else - without
   * connection_gate needing to know system_tray exists at all. Only one callback can be
   * registered at a time; a later call replaces the previous one.
   */
  void set_on_mode_changed(std::function<void()> callback);

  /**
   * @brief Whether a new session launch should currently be allowed.
   * @details True for open. False for closed. For auto_close, true until the configured delay
   * elapses with no active session, at which point it becomes false - without changing which
   * mode is selected. Becomes true again as soon as a session becomes active, re-arming the
   * delay for the next time everyone disconnects.
   */
  bool is_open();

  /**
   * @brief Seconds remaining before auto_close mode closes the gate, if applicable.
   * @details Returns std::nullopt when not currently counting down - wrong mode, a session
   * is currently active, or the gate has already closed. Purely for display purposes (e.g. a
   * countdown in the web UI); nothing here should be relied on for actually gating access.
   */
  std::optional<int> seconds_until_auto_close();

}  // namespace connection_gate

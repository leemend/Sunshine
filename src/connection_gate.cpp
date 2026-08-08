/**
 * @file src/connection_gate.cpp
 */
#include "connection_gate.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

#include "config.h"
#include "logging.h"
#include "rtsp.h"

using namespace std::literals;

namespace connection_gate {

  static mode_e g_mode = mode_e::closed;

  // For auto_close: the mode itself (g_mode) never changes on its own - only this separate
  // flag does, tracking whether the auto_close cycle currently happens to be open or closed.
  // Re-arms to true every time a session becomes active (not just when the mode is first
  // selected) - this is what makes a reconnect within the delay window "just work" without a
  // separate grace-period mechanism: is_open() is still true at that point, same as any other
  // open gate, because the cycle never actually finished closing.
  static std::atomic<bool> g_auto_state_open {true};

  // When session_count() most recently dropped to 0, for the auto_close delay timer. Reset
  // whenever a session becomes active again.
  static std::mutex g_inactivity_mutex;
  static std::optional<std::chrono::steady_clock::time_point> g_became_inactive_at;

  static std::function<void()> g_on_mode_changed;

  mode_e mode_from_string(const std::string &value) {
    if (value == "open") {
      return mode_e::open;
    } else if (value == "auto_close" || value == "auto_inactivity") {
      // "auto_inactivity" accepted for backward compatibility with configs written before
      // these two modes were merged into one.
      return mode_e::auto_close;
    } else if (value == "closed") {
      return mode_e::closed;
    }

    BOOST_LOG(warning) << "connection_gate: unrecognized mode \"" << value << "\", defaulting to closed"sv;
    return mode_e::closed;
  }

  std::string mode_to_string(mode_e mode) {
    switch (mode) {
      case mode_e::open:
        return "open";
      case mode_e::auto_close:
        return "auto_close";
      case mode_e::closed:
      default:
        return "closed";
    }
  }

  void init() {
    g_mode = mode_from_string(config::sunshine.connection_gate_mode);
    if (g_mode == mode_e::auto_close) {
      g_auto_state_open = true;
    }
    BOOST_LOG(info) << "connection_gate: starting in mode \""sv << mode_to_string(g_mode) << "\""sv;

    std::thread([]() {
      // Runs for the process lifetime, same as the rest of Sunshine's background threads -
      // nothing here holds a resource that needs explicit shutdown handling, it only reads
      // already-public, thread-safe state (rtsp_stream::session_count()) and this module's own
      // atomics/mutex-protected state.
      int previous_session_count = -1;  // -1 so the very first check always counts as "changed"

      while (true) {
        std::this_thread::sleep_for(1s);

        int current_session_count = rtsp_stream::session_count();
        bool just_became_inactive = previous_session_count != 0 && current_session_count == 0;
        bool just_became_active = current_session_count > 0 && previous_session_count == 0;
        previous_session_count = current_session_count;

        if (just_became_active) {
          std::lock_guard lock(g_inactivity_mutex);
          g_became_inactive_at.reset();

          // Re-arm: a session becoming active again means the auto_close cycle should be
          // treated as freshly open, ready to start a new countdown next time everyone
          // leaves. Without this, once the gate closed once it would never detect - or log -
          // any later disconnect, since the countdown check below is itself gated on
          // g_auto_state_open already being true.
          if (g_mode == mode_e::auto_close && !g_auto_state_open) {
            BOOST_LOG(info) << "connection_gate: session active again, auto_close mode re-armed"sv;
            g_auto_state_open = true;
          }
        }

        if (just_became_inactive && g_mode == mode_e::auto_close) {
          std::lock_guard lock(g_inactivity_mutex);
          g_became_inactive_at = std::chrono::steady_clock::now();
        }

        if (g_mode == mode_e::auto_close && g_auto_state_open && current_session_count == 0) {
          std::optional<std::chrono::steady_clock::time_point> became_inactive_at;
          {
            std::lock_guard lock(g_inactivity_mutex);
            became_inactive_at = g_became_inactive_at;
          }

          if (became_inactive_at) {
            auto elapsed = std::chrono::steady_clock::now() - *became_inactive_at;
            auto threshold = std::chrono::seconds(config::sunshine.connection_gate_auto_close_seconds);

            if (elapsed >= threshold) {
              BOOST_LOG(info) << "connection_gate: "sv << config::sunshine.connection_gate_auto_close_seconds
                               << " second(s) with no active session reached, auto_close mode closing the gate"sv;
              g_auto_state_open = false;
              if (g_on_mode_changed) {
                g_on_mode_changed();
              }
            }
          }
        }
      }
    }).detach();
  }

  mode_e current_mode() {
    return g_mode;
  }

  void set_on_mode_changed(std::function<void()> callback) {
    g_on_mode_changed = std::move(callback);
  }

  void set_mode(mode_e mode) {
    if (g_mode == mode) {
      return;
    }

    BOOST_LOG(info) << "connection_gate: mode changed from \""sv << mode_to_string(g_mode) << "\" to \""sv << mode_to_string(mode) << "\""sv;
    g_mode = mode;

    if (mode == mode_e::auto_close) {
      g_auto_state_open = true;
      std::lock_guard lock(g_inactivity_mutex);
      g_became_inactive_at.reset();
    }

    if (g_on_mode_changed) {
      g_on_mode_changed();
    }
  }

  bool is_open() {
    switch (g_mode) {
      case mode_e::open:
        return true;
      case mode_e::closed:
        return false;
      case mode_e::auto_close:
        return g_auto_state_open;
      default:
        return false;
    }
  }

  std::optional<int> seconds_until_auto_close() {
    if (g_mode != mode_e::auto_close || !g_auto_state_open) {
      return std::nullopt;
    }

    if (rtsp_stream::session_count() != 0) {
      return std::nullopt;
    }

    std::optional<std::chrono::steady_clock::time_point> became_inactive_at;
    {
      std::lock_guard lock(g_inactivity_mutex);
      became_inactive_at = g_became_inactive_at;
    }

    if (!became_inactive_at) {
      return std::nullopt;
    }

    auto elapsed = std::chrono::steady_clock::now() - *became_inactive_at;
    auto threshold = std::chrono::seconds(config::sunshine.connection_gate_auto_close_seconds);
    auto remaining = threshold - std::chrono::duration_cast<std::chrono::seconds>(elapsed);

    return std::max(0, static_cast<int>(remaining.count()));
  }

}  // namespace connection_gate

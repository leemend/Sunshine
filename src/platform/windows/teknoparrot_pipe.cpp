/**
 * @file src/platform/windows/teknoparrot_pipe.cpp
 * @brief Implementation of the TeknoParrot named-pipe identity bridge. See the header for the
 * wire protocol.
 */
#include "teknoparrot_pipe.h"

// platform includes
#include <windows.h>

// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace teknoparrot_pipe {

  void debug_log(const std::string &msg) {
    OutputDebugStringA(("[SunshineDebug][teknoparrot_pipe] " + msg + "\n").c_str());
  }

  namespace {

    constexpr auto *PIPE_NAME = L"\\\\.\\pipe\\SunshineTeknoParrotInput";
    constexpr size_t MAX_QUEUE_SIZE = 256;  ///< Oldest events are dropped if this is exceeded.

    enum : uint8_t {
      MSG_ROSTER = 0x01,
      MSG_KEY = 0x02,
      MSG_MOUSE_MOVE = 0x03,
      MSG_MOUSE_BUTTON = 0x04,
      MSG_MOUSE_WHEEL = 0x05,
      MSG_ABS_POSITION = 0x06,
      MSG_GAMEPAD_SLOT = 0x07,
      MSG_CLIENT_IDENTITY = 0x08
    };

    std::atomic<bool> g_running {false};
    std::atomic<bool> g_client_connected {false};
    std::thread g_server_thread;

    std::mutex g_queue_mutex;
    std::condition_variable g_queue_cv;
    std::deque<std::vector<uint8_t>> g_queue;

    std::mutex g_roster_mutex;
    std::set<int> g_connected_players;  ///< Players currently known to be connected, so a newly
                                         ///< attached TeknoParrotUI reader can be caught up.
    std::map<int, std::size_t> g_player_session_counts;  ///< Live sessions sharing each player.
    std::map<std::string, int> g_client_slots;  ///< Sticky client-identity -> player-slot map, so
                                                 ///< a client keeps its number across session
                                                 ///< restarts (settings screens, app switches)
                                                 ///< instead of drifting on every reconnect.
    std::map<int, std::string> g_pending_slot_owners;  ///< Atomic reservations between slot
                                                        ///< assignment and roster publication.
    std::map<int, std::string> g_client_uuids;  ///< player -> persistent paired Sunshine client UUID.
    std::map<int, int> g_gamepad_slots;  ///< player -> real Windows XInput user index, guarded by
                                          ///< g_roster_mutex like g_connected_players. Unlike
                                          ///< Roster, a GamepadSlot message is only ever sent
                                          ///< once, right when ViGEmBus first allocates the
                                          ///< controller (see send_gamepad_slot()) - so without
                                          ///< tracking it here too, a TeknoParrotUI reader that
                                          ///< connects (or reconnects) *after* that one-shot
                                          ///< message went out would never learn which XInput
                                          ///< index belongs to which player, and every streamed
                                          ///< player's gamepad would silently misattribute as the
                                          ///< host's own local controller. Replayed alongside the
                                          ///< roster in server_loop() for the same reason.

    /**
     * @brief Builds a serialized gamepad-slot message. Shared by send_gamepad_slot() and the
     * connection-time replay in server_loop().
     */
    std::vector<uint8_t> make_gamepad_slot_message(int player, int xinput_index) {
      std::vector<uint8_t> buf;
      buf.reserve(3);
      buf.push_back(MSG_GAMEPAD_SLOT);
      buf.push_back(static_cast<uint8_t>(player));
      buf.push_back(static_cast<uint8_t>(xinput_index));
      return buf;
    }

    /**
     * @brief Builds a serialized roster message. Shared by send_roster() and the connection-time
     * roster replay in server_loop().
     */
    std::vector<uint8_t> make_roster_message(int player, bool connected) {
      std::vector<uint8_t> buf;
      buf.reserve(3);
      buf.push_back(MSG_ROSTER);
      buf.push_back(static_cast<uint8_t>(player));
      buf.push_back(connected ? 1 : 0);
      return buf;
    }


    /**
     * @brief Appends an integral value to a byte buffer in little-endian order.
     */
    template<class T>
    void append_le(std::vector<uint8_t> &buf, T value) {
      static_assert(std::is_integral<T>::value, "append_le() only supports integral types");
      using unsigned_t = typename std::make_unsigned<T>::type;
      auto uvalue = static_cast<unsigned_t>(value);
      for (size_t i = 0; i < sizeof(T); ++i) {
        buf.push_back(static_cast<uint8_t>((uvalue >> (8 * i)) & 0xFF));
      }
    }

    std::vector<uint8_t> make_client_identity_message(int player, const std::string &client_uuid) {
      constexpr size_t MAX_UUID_BYTES = 0xFFFF;

      const size_t uuid_size = (std::min)(client_uuid.size(), MAX_UUID_BYTES);

      std::vector<uint8_t> buf;
      buf.reserve(4 + uuid_size);
      buf.push_back(MSG_CLIENT_IDENTITY);
      buf.push_back(static_cast<uint8_t>(player));
      append_le<uint16_t>(buf, static_cast<uint16_t>(uuid_size));
      buf.insert(
        buf.end(),
        client_uuid.begin(),
        client_uuid.begin() + static_cast<std::string::difference_type>(uuid_size)
      );
      return buf;
    }
    /**
     * @brief Queues a fully-serialized message for the writer loop. Drops the event outright
     * (rather than blocking or growing unbounded) if no client is currently connected, since
     * these are live input events with no value once stale.
     */
    void push_message(std::vector<uint8_t> &&msg) {
      if (!g_running.load(std::memory_order_relaxed)) {
        debug_log("push_message: DROPPED (g_running=false, pipe server thread not running)");
        return;
      }
      if (!g_client_connected.load(std::memory_order_relaxed)) {
        debug_log("push_message: dropped, no TeknoParrotUI reader connected (message type " + std::to_string((int) msg[0]) + ")");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (g_queue.size() >= MAX_QUEUE_SIZE) {
          g_queue.pop_front();
        }
        g_queue.push_back(std::move(msg));
      }
      g_queue_cv.notify_one();
    }

    /**
     * @brief Repeatedly accepts a single TeknoParrotUI connection and serves queued messages to
     * it until the pipe breaks, then waits for the next connection. Runs until deinit() is
     * called.
     */
    void server_loop() {
      while (g_running.load(std::memory_order_relaxed)) {
        HANDLE pipe = CreateNamedPipeW(
          PIPE_NAME,
          PIPE_ACCESS_OUTBOUND,
          PIPE_TYPE_BYTE | PIPE_WAIT,
          1,  // max instances: one TeknoParrotUI reader at a time
          4096,  // out buffer size
          0,  // in buffer size (unused; one-way pipe)
          0,  // default timeout
          nullptr
        );

        if (pipe == INVALID_HANDLE_VALUE) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
          CloseHandle(pipe);
          continue;
        }

        if (!g_running.load(std::memory_order_relaxed)) {
          CloseHandle(pipe);
          break;
        }

        g_client_connected.store(true, std::memory_order_relaxed);

        // A newly-connected reader may have missed the connect edges for players that joined
        // before it attached (e.g. TeknoParrotUI opened after a Moonlight client was already
        // streaming). Replay the current roster so it catches up immediately.
        {
          std::lock_guard<std::mutex> roster_lock(g_roster_mutex);
          if (!g_connected_players.empty()) {
            std::lock_guard<std::mutex> queue_lock(g_queue_mutex);
            for (int player : g_connected_players) {
              g_queue.push_back(make_roster_message(player, true));
            }
          }

          // Replay the persistent paired-client UUID for each currently-connected streamed
          // player too. This lets TeknoParrotUI attach after streaming has already begun and
          // still resolve P2-P4 to the correct saved remote-player profile.
          if (!g_client_uuids.empty()) {
            std::lock_guard<std::mutex> queue_lock(g_queue_mutex);
            for (const auto &entry : g_client_uuids) {
              if (g_connected_players.count(entry.first) && !entry.second.empty()) {
                g_queue.push_back(make_client_identity_message(entry.first, entry.second));
              }
            }
          }

          // Same problem as the roster, but for gamepad-slot assignments: GamepadSlot is only
          // ever sent once, at the moment ViGEmBus allocates the controller. A reader that
          // wasn't connected yet at that moment (or reconnected after) would otherwise never
          // learn the mapping - see g_gamepad_slots above.
          if (!g_gamepad_slots.empty()) {
            std::lock_guard<std::mutex> queue_lock(g_queue_mutex);
            for (const auto &entry : g_gamepad_slots) {
              g_queue.push_back(make_gamepad_slot_message(entry.first, entry.second));
            }
          }
        }
        g_queue_cv.notify_one();

        while (g_running.load(std::memory_order_relaxed)) {
          std::vector<uint8_t> msg;
          {
            std::unique_lock<std::mutex> lock(g_queue_mutex);
            g_queue_cv.wait_for(lock, std::chrono::milliseconds(200), [] {
              return !g_queue.empty() || !g_running.load(std::memory_order_relaxed);
            });

            if (!g_running.load(std::memory_order_relaxed)) {
              break;
            }
            if (g_queue.empty()) {
              continue;
            }

            msg = std::move(g_queue.front());
            g_queue.pop_front();
          }

          DWORD written = 0;
          if (!WriteFile(pipe, msg.data(), (DWORD) msg.size(), &written, nullptr)) {
            // TeknoParrotUI disconnected or the pipe otherwise broke; go back to accepting.
            break;
          }
        }

        g_client_connected.store(false, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(g_queue_mutex);
          g_queue.clear();
        }

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
      }
    }

  }  // namespace

  void init() {
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
      return;  // already running
    }
    debug_log("=== TEKNOPARROT BRIDGE BUILD MARKER: touch-investigation-round-2 ===");
    g_server_thread = std::thread(server_loop);
  }

  void deinit() {
    if (!g_running.exchange(false)) {
      return;
    }
    g_queue_cv.notify_all();

    // Unblock a pending ConnectNamedPipe()/accept wait, if any, by connecting and immediately
    // closing a throwaway client handle to our own pipe.
    HANDLE unblock = CreateFileW(PIPE_NAME, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (unblock != INVALID_HANDLE_VALUE) {
      CloseHandle(unblock);
    }

    if (g_server_thread.joinable()) {
      g_server_thread.join();
    }
  }

  void send_roster(int player, bool connected) {
    if (player < 2 || player > 4) {
      return;
    }

    bool roster_changed = false;
    {
      std::lock_guard<std::mutex> roster_lock(g_roster_mutex);
      if (connected) {
        g_pending_slot_owners.erase(player);
        auto &session_count = g_player_session_counts[player];
        ++session_count;
        if (session_count == 1) {
          g_connected_players.insert(player);
          roster_changed = true;
        }
      } else {
        auto count_it = g_player_session_counts.find(player);
        if (count_it == g_player_session_counts.end()) {
          return;
        }

        if (--count_it->second != 0) {
          return;
        }

        g_player_session_counts.erase(count_it);
        g_connected_players.erase(player);
        roster_changed = true;

        // The controller (if any) that was allocated for this player's previous session is
        // gone too - don't let a stale index get replayed to the next reader as if it still
        // applied. If the player reconnects and gets a new controller, a fresh GamepadSlot
        // message (and the tracking below) will repopulate this.
        g_gamepad_slots.erase(player);
        g_client_uuids.erase(player);
      }
    }

    if (roster_changed) {
      push_message(make_roster_message(player, connected));
    }
  }

  void send_client_identity(int player, const std::string &client_uuid) {
    if (player < 2 || player > 4 || client_uuid.empty()) {
      return;
    }

    {
      std::lock_guard<std::mutex> roster_lock(g_roster_mutex);
      g_client_uuids[player] = client_uuid;
    }

    debug_log(
      "send_client_identity: player " +
      std::to_string(player) +
      " -> paired UUID [" +
      client_uuid +
      "]"
    );

    push_message(make_client_identity_message(player, client_uuid));
  }

  void send_mouse_move(int player, int32_t delta_x, int32_t delta_y) {
    std::vector<uint8_t> buf;
    buf.reserve(10);
    buf.push_back(MSG_MOUSE_MOVE);
    buf.push_back(static_cast<uint8_t>(player));
    append_le(buf, delta_x);
    append_le(buf, delta_y);
    push_message(std::move(buf));
  }

  void send_mouse_button(int player, int button, bool down) {
    std::vector<uint8_t> buf;
    buf.reserve(4);
    buf.push_back(MSG_MOUSE_BUTTON);
    buf.push_back(static_cast<uint8_t>(player));
    buf.push_back(down ? 1 : 0);
    buf.push_back(static_cast<uint8_t>(button));
    push_message(std::move(buf));
  }

  void send_gamepad_slot(int player, int xinput_index) {
    {
      std::lock_guard<std::mutex> roster_lock(g_roster_mutex);
      g_gamepad_slots[player] = xinput_index;
    }

    push_message(make_gamepad_slot_message(player, xinput_index));
  }

  void send_mouse_wheel(int player, int32_t delta) {
    std::vector<uint8_t> buf;
    buf.reserve(6);
    buf.push_back(MSG_MOUSE_WHEEL);
    buf.push_back(static_cast<uint8_t>(player));
    append_le(buf, delta);
    push_message(std::move(buf));
  }

  void send_abs_position(int player, int32_t x, int32_t y) {
    std::vector<uint8_t> buf;
    buf.reserve(10);
    buf.push_back(MSG_ABS_POSITION);
    buf.push_back(static_cast<uint8_t>(player));
    append_le(buf, x);
    append_le(buf, y);
    push_message(std::move(buf));
  }

  void send_key(int player, uint16_t vk_code, bool down) {
    std::vector<uint8_t> buf;
    buf.reserve(5);
    buf.push_back(MSG_KEY);
    buf.push_back(static_cast<uint8_t>(player));
    buf.push_back(down ? 1 : 0);
    append_le(buf, vk_code);
    push_message(std::move(buf));
  }

  int assign_player_slot(const std::string &client_address) {
    std::lock_guard<std::mutex> lock(g_roster_mutex);

    // This client has connected before (possibly in an earlier session that got torn down and
    // recreated without it ever truly disconnecting) - give it the same slot back.
    if (!client_address.empty()) {
      auto it = g_client_slots.find(client_address);
      if (it != g_client_slots.end()) {
        const int existing_slot = it->second;
        const auto pending_it = g_pending_slot_owners.find(existing_slot);
        if (pending_it == g_pending_slot_owners.end() || pending_it->second == client_address) {
          if (!g_connected_players.count(existing_slot)) {
            g_pending_slot_owners[existing_slot] = client_address;
          }
          debug_log("assign_player_slot(\"" + client_address + "\"): REUSING existing slot " + std::to_string(existing_slot));
          return existing_slot;
        }

        // A different new client reserved this formerly-idle slot between the capacity check
        // and allocation. Drop this stale mapping and search for another slot.
        g_client_slots.erase(it);
      }
    }

    // Otherwise, claim the lowest-numbered slot that isn't currently live under a *different*
    // client's active session.
    for (int candidate = 2; candidate <= 4; ++candidate) {
      const bool taken = g_connected_players.count(candidate) != 0;
      if (!taken && !g_pending_slot_owners.count(candidate)) {
        // Reusing a free slot invalidates any old sticky reservation for that slot. Without
        // this cleanup, a disconnected client could later reclaim a slot now occupied by a
        // different live client and both would be routed as the same player.
        for (auto it = g_client_slots.begin(); it != g_client_slots.end();) {
          if (it->second == candidate) {
            it = g_client_slots.erase(it);
          } else {
            ++it;
          }
        }

        if (!client_address.empty()) {
          g_client_slots[client_address] = candidate;
        }
        g_pending_slot_owners[candidate] = client_address;
        debug_log("assign_player_slot(\"" + client_address + "\"): NEW assignment, slot " + std::to_string(candidate));
        return candidate;
      }
    }

    // The launch/resume gate should reject this before input allocation. If two requests race,
    // fail closed here rather than assigning a duplicate slot and allowing control bleed.
    debug_log("assign_player_slot(\"" + client_address + "\"): NO FREE SLOT");
    return 0;
  }

  bool has_free_player_slot(const std::string &client_address) {
    std::lock_guard<std::mutex> lock(g_roster_mutex);

    // Already holds a sticky slot from an earlier session - not a new occupant, so it always
    // has "a slot" regardless of how full the other three are.
    if (!client_address.empty()) {
      auto client_it = g_client_slots.find(client_address);
      if (client_it != g_client_slots.end()) {
        auto pending_it = g_pending_slot_owners.find(client_it->second);
        if (pending_it == g_pending_slot_owners.end() || pending_it->second == client_address) {
          return true;
        }
      }
    }

    for (int candidate = 2; candidate <= 4; ++candidate) {
      const bool taken = g_connected_players.count(candidate) != 0;
      if (!taken && !g_pending_slot_owners.count(candidate)) {
        return true;
      }
    }

    return false;
  }

}  // namespace teknoparrot_pipe

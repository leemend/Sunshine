/**
 * @file src/platform/windows/teknoparrot_pipe.h
 * @brief Bridges Sunshine's per-client player identity out to TeknoParrotUI over a local named
 * pipe, since SendInput()-based keyboard/mouse injection carries no device identity that
 * Windows RawInput can expose.
 *
 * Wire protocol (all multi-byte fields little-endian):
 *   Every message starts with a 1-byte type tag, followed by a fixed number of bytes for that
 *   type. There is no length prefix; the reader determines length from the type tag alone.
 *
 *   0x01 Roster        [type][player][connected]                     3 bytes
 *   0x02 Key            [type][player][down][vk_lo][vk_hi]            5 bytes
 *   0x03 MouseMove       [type][player][dx0..dx3][dy0..dy3]            10 bytes
 *   0x04 MouseButton    [type][player][down][button]                 4 bytes
 *   0x05 MouseWheel      [type][player][delta0..delta3]                6 bytes
 *   0x06 AbsPosition    [type][player][x0..x3][y0..y3]                10 bytes
 *   0x07 GamepadSlot     [type][player][xinput_index]                  3 bytes
 *
 *   player:  2-4 (slot 1 is reserved for the host's own local input)
 *   button:  0=Left, 1=Right, 2=Middle, 3=Button4(X1), 4=Button5(X2)
 *   dx/dy/delta: signed 32-bit, little-endian
 *   x/y: signed 32-bit, little-endian. Normalized 0-65535 across the client's full touch/cursor
 *        surface (the same convention a real absolute HID pointer device uses), for lightgun-
 *        style point-and-shoot games where the client reports an absolute touch or cursor
 *        position instead of relative deltas. Not meaningful for trackball-type games - a
 *        trackball has no absolute position, only velocity.
 *   vk: unsigned 16-bit Windows virtual-key code, little-endian
 *   xinput_index: the real Windows XInput user index (0-3) ViGEmBus assigned this player's
 *        virtual controller - the same index XInputGetState() and DirectInput controller
 *        enumeration would report, letting TeknoParrotUI tell which physical/virtual slot
 *        belongs to which streamed player.
 *
 * The pipe is named \\.\pipe\SunshineTeknoParrotInput and is written to by Sunshine only
 * (PIPE_ACCESS_OUTBOUND); TeknoParrotUI connects as a reader. If no client is connected,
 * events are silently dropped rather than queued or blocked, so normal streaming is never
 * affected by whether TeknoParrotUI happens to be running.
 */
#pragma once

#include <cstdint>
#include <string>

namespace teknoparrot_pipe {

  /**
   * @brief Starts the background pipe server thread. Safe to call once during input subsystem
   * init; subsequent calls are no-ops.
   */
  void init();

  /**
   * @brief Stops the background pipe server thread and closes any open pipe handle.
   */
  void deinit();

  /**
   * @brief Announces that a client session has been assigned (or has released) a player slot,
   * so TeknoParrotUI can add/remove the corresponding synthetic device entry live.
   * @param player Player slot, 2-4 (see next_player_slot() in input.cpp).
   * @param connected True if the client just connected, false if it disconnected.
   */
  void send_roster(int player, bool connected);

  /**
   * @brief Sends a relative mouse movement delta for a player.
   */
  void send_mouse_move(int player, int32_t delta_x, int32_t delta_y);

  /**
   * @brief Sends a mouse button state change for a player.
   * @param button 0=Left, 1=Right, 2=Middle, 3=Button4, 4=Button5.
   */
  void send_mouse_button(int player, int button, bool down);

  /**
   * @brief Sends a mouse wheel delta for a player, in the same units Sunshine uses internally
   * (WHEEL_DELTA-based ticks).
   */
  void send_mouse_wheel(int player, int32_t delta);

  /**
   * @brief Sends an absolute pointer position for a player - lightgun-style point-and-shoot
   * input, where the client reports where the finger/cursor is rather than how far it moved.
   * @param x Normalized 0-65535 across the client's full touch/cursor surface, same convention
   * as a real absolute HID pointer device.
   * @param y Same convention as x.
   */
  void send_abs_position(int player, int32_t x, int32_t y);

  /**
   * @brief Forwards a lightgun-style absolute pointer position for a player. Expects x/y already
   * normalized 0-65535 across the client's own touch/cursor surface - the same convention
   * TeknoParrotUI's moveAbsolute lightgun path expects, and the same convention a real absolute
   * HID pointer device uses. Deliberately does NOT route through Sunshine's internal
   * client_to_touchport() conversion - that's scaled for inputtino's (a different, Linux-side
   * consumer) coordinate expectations, not this one, and doing so was found to crush values down
   * to near-zero rather than the intended full range.
   * @param player Player slot, or 0 to skip (no-op).
   */
  void forward_touch_position(int player, float x, float y);

  /**
   * @brief Sends a keyboard key state change for a player.
   * @param vk_code Windows virtual-key code.
   */
  void send_key(int player, uint16_t vk_code, bool down);

  /**
   * @brief Announces which real Windows XInput user index (0-3) ViGEmBus assigned a player's
   * virtual controller, so TeknoParrotUI can tell one streamed player's gamepad apart from
   * another's during binding capture - unlike mouse/keyboard, gamepad slots aren't inherently
   * tied to a player; Windows just hands out the next free XInput index in allocation order.
   * Sent once, right when the controller is first allocated (PSS_CONTROLLER_ARRIVAL_PACKET).
   * @param player Player slot, 2-4.
   * @param xinput_index The real XInput user index (0-3) this player's controller was assigned.
   */
  void send_gamepad_slot(int player, int xinput_index);

  /**
   * @brief Temporary diagnostic logger (OutputDebugStringA, visible in DebugView++). Remove once
   * the Native Touch / absolute-position issue is diagnosed.
   */
  void debug_log(const std::string &msg);

  /**
   * @brief Assigns a player slot (2-4) to a client, keyed by a stable identifier for that
   * client (its address). The same client reliably gets the same slot back across session
   * restarts - Sunshine tears down and recreates its streaming session (and, before this,
   * silently reassigned a new player number) whenever a client navigates a settings screen or
   * the app renegotiates the stream, even though the client itself never actually disconnects.
   * Falls back to round-robin if more than three distinct clients are simultaneously active.
   * @param client_address A stable per-client identifier (the connecting client's address).
   * @return The assigned player slot, 2-4.
   */
  int assign_player_slot(const std::string &client_address);

  /**
   * @brief Whether a genuinely new client (not one already holding a sticky slot) could
   * currently get a real, non-colliding player slot - i.e. whether assign_player_slot() would
   * hand back a real slot rather than falling through to its collision-risk round-robin
   * overflow. Read-only: does not reserve or change anything, safe to call speculatively
   * before a session even starts (e.g. from nvhttp.cpp's launch()/resume() handlers) to reject
   * a connection cleanly - a normal "couldn't connect" on the client's end - rather than
   * letting them stream in and silently fight another player for the same slot.
   * @param client_address A stable per-client identifier (the connecting client's address).
   * Reconnecting clients that already hold a sticky slot always return true here, since they
   * aren't creating a new occupant - just resuming their own.
   */
  bool has_free_player_slot(const std::string &client_address);

}  // namespace teknoparrot_pipe

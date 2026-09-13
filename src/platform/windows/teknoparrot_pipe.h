/**
 * @file src/platform/windows/teknoparrot_pipe.h
 * @brief Bridges Sunshine's per-client player identity out to TeknoParrotUI over a local named
 * pipe, since SendInput()-based keyboard/mouse injection carries no device identity that
 * Windows RawInput can expose.
 *
 * Wire protocol (all multi-byte fields little-endian):
 *   Every message starts with a 1-byte type tag. Fixed-size messages have no length prefix.
 *   Variable-size messages include their own type-specific length field.
 *
 *   0x01 Roster          [type][player][connected]                     3 bytes
 *   0x02 Key             [type][player][down][vk_lo][vk_hi]            5 bytes
 *   0x03 MouseMove       [type][player][dx0..dx3][dy0..dy3]           10 bytes
 *   0x04 MouseButton     [type][player][down][button]                  4 bytes
 *   0x05 MouseWheel      [type][player][delta0..delta3]                6 bytes
 *   0x06 AbsPosition     [type][player][x0..x3][y0..y3]               10 bytes
 *   0x07 GamepadSlot     [type][player][xinput_index]                  3 bytes
 *   0x08 ClientIdentity  [type][player][uuid_len_lo][uuid_len_hi][uuid UTF-8...]
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
 *        virtual controller.
 *   uuid_len: unsigned 16-bit UTF-8 byte count for the paired Sunshine client UUID.
 *
 * The pipe is named \\.\pipe\SunshineTeknoParrotInput and is written to by Sunshine only
 * (PIPE_ACCESS_OUTBOUND); TeknoParrotUI connects as a reader. If no client is connected,
 * live events are silently dropped rather than queued or blocked. Persistent state such as
 * roster, client identity, and gamepad-slot assignment is replayed when a reader connects.
 */
#pragma once

#include <cstdint>
#include <string>

namespace teknoparrot_pipe {

  void init();
  void deinit();

  /**
   * @brief Announces that a client session has been assigned or has released a player slot.
   */
  void send_roster(int player, bool connected);

  /**
   * @brief Associates a live P2-P4 slot with Sunshine's persistent paired-client UUID.
   *
   * This UUID is intentionally distinct from the Moonlight client_unique_id used internally
   * for sticky slot assignment. The paired-client UUID is the durable identity used by
   * Sunshine's client-management/unpair APIs and is therefore suitable for persistent
   * per-client TeknoParrot settings.
   */
  void send_client_identity(int player, const std::string &client_uuid);

  void send_mouse_move(int player, int32_t delta_x, int32_t delta_y);
  void send_mouse_button(int player, int button, bool down);
  void send_mouse_wheel(int player, int32_t delta);
  void send_abs_position(int player, int32_t x, int32_t y);

  /**
   * @brief Forwards a normalized lightgun-style absolute pointer position.
   */
  void forward_touch_position(int player, float x, float y);

  void send_key(int player, uint16_t vk_code, bool down);

  /**
   * @brief Announces which real Windows XInput user index ViGEmBus assigned a streamed player.
   */
  void send_gamepad_slot(int player, int xinput_index);

  void debug_log(const std::string &msg);

  /**
   * @brief Assigns P2-P4 using the stable authenticated client identity supplied by stream.cpp.
   * Returns 0 instead of colliding with a live assignment if no slot is available.
   */
  int assign_player_slot(const std::string &client_identity);

  /**
   * @brief Returns whether a new client can obtain a non-colliding P2-P4 slot.
   */
  bool has_free_player_slot(const std::string &client_identity);

}  // namespace teknoparrot_pipe

/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>
#include <string>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  /**
   * @brief Allocate and initialize platform input state for a stream.
   *
   * @param mail Mailbox used to exchange messages with worker threads.
   * @param client_address The connecting client's address, used as a stable identifier so the
   * TeknoParrot identity bridge (Windows only) can give the same client the same player number
   * across session restarts - navigating a client-side settings screen, or the app renegotiating
   * the stream, tears down and recreates this session (and would otherwise silently reassign a
   * new player number) without the client ever actually disconnecting.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(safe::mail_t mail, const std::string &client_address = {});

  struct touch_port_t: public platf::touch_port_t {
    int env_width;
    int env_height;

    // Offset x and y coordinates of the client
    float client_offsetX;
    float client_offsetY;

    float scalar_inv;
    float scalar_tpcoords;

    int env_logical_width;
    int env_logical_height;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

  /**
   * @brief Records which client session (2-4, or 0 if unassigned) is the source of the mouse/keyboard event
   * about to be forwarded to the platform backend.
   * @details SendInput()-based injection on Windows carries no per-client identity, so this
   * lets src/input.cpp tag the currently-processing session immediately before calling into
   * platf::*, and src/platform/windows/input.cpp read it back out when forwarding events to
   * the TeknoParrot bridge pipe. 0 means "unassigned / not a tracked player".
   * @param player_index The player slot (2-4) assigned to the client session, or 0 if unassigned.
   */
  void set_active_player(int player_index);

  /**
   * @brief Retrieves the player slot set by the most recent set_active_player() call on this
   * thread.
   * @return The player slot (2-4), or 0 if unassigned.
   */
  int get_active_player();
}  // namespace input

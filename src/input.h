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
   * @param client_identity Stable Moonlight client identity used by the TeknoParrot bridge
   * for sticky P2-P4 player-slot assignment across stream/session recreation.
   * @param client_uuid Sunshine's persistent paired-client UUID. This follows the saved
   * Sunshine pairing and is forwarded to TeknoParrotUI so per-client settings can persist
   * independently of whichever P2-P4 slot the client receives on a particular session.
   * @return Shared input state bound to the stream mailbox.
   */
  std::shared_ptr<input_t> alloc(
    safe::mail_t mail,
    const std::string &client_identity = {},
    const std::string &client_uuid = {}
  );

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
  std::pair<float, float> scale_client_contact_area(
    const std::pair<float, float> &val,
    uint16_t rotation,
    const std::pair<float, float> &scalar
  );

  /**
   * @brief Records which client session (2-4, or 0 if unassigned) is the source of the
   * mouse/keyboard event about to be forwarded to the platform backend.
   */
  void set_active_player(int player_index);

  /**
   * @brief Retrieves the player slot set by the most recent set_active_player() call.
   */
  int get_active_player();
}  // namespace input

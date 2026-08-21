/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <vector>

// local includes
#include "crypto.h"
#include "thread_safe.h"

namespace stream {
  struct session_info_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    bool host_audio;
    std::string unique_id;
    std::string client_name;
    std::string client_uuid;
    int width;
    int height;
    int fps;
    int gcmap;
    int appid;
    int surround_info;
    std::string surround_params;
    bool continuous_audio;
    bool enable_hdr;
    bool enable_sops;

    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;
  };

  void launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();

  /**
   * @brief Get information about currently active streaming sessions.
   * @return A snapshot of the active sessions.
   */
  std::vector<stream::session_info_t> active_sessions();

  /**
   * @brief Terminates a single running streaming session.
   * @param session_id Launch session ID of the session to terminate.
   * @return True if a matching session was found and stopped.
   */
  /**
   * @brief Arms the existing RTSP listener to recognize one connectivity-test callback.
   *
   * A real pending Moonlight launch session always takes priority over this probe.
   */
  void arm_connectivity_probe();

  /**
   * @brief Returns true after the armed connectivity callback reaches TCP 48010.
   */
  bool connectivity_probe_received();

  /**
   * @brief Cancels an armed TCP 48010 connectivity probe.
   */
  void cancel_connectivity_probe();

  bool terminate_session(std::uint32_t session_id);

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream

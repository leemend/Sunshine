/**
 * @file src/system_tray.cpp
 * @brief Definitions for the system tray icon and notification system.
 */
// macros
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1

  #if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #include <accctrl.h>
    #include <aclapi.h>
    #include <windows.h>
    #define TRAY_ICON WEB_DIR "images/sunshine.ico"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing.ico"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing.ico"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked.ico"
  #elif defined(__linux__) || defined(linux) || defined(__linux) || defined(__FreeBSD__)
    #define TRAY_ICON SUNSHINE_TRAY_PREFIX "-tray"
    #define TRAY_ICON_PLAYING SUNSHINE_TRAY_PREFIX "-playing"
    #define TRAY_ICON_PAUSING SUNSHINE_TRAY_PREFIX "-pausing"
    #define TRAY_ICON_LOCKED SUNSHINE_TRAY_PREFIX "-locked"
  #elif defined(__APPLE__) || defined(__MACH__)
    #define TRAY_ICON WEB_DIR "images/logo-sunshine-16.png"
    #define TRAY_ICON_PLAYING WEB_DIR "images/sunshine-playing-16.png"
    #define TRAY_ICON_PAUSING WEB_DIR "images/sunshine-pausing-16.png"
    #define TRAY_ICON_LOCKED WEB_DIR "images/sunshine-locked-16.png"
    #include <CoreFoundation/CoreFoundation.h>
    #include <dispatch/dispatch.h>
    #include <unordered_map>
  #endif

  // standard includes
  #include <algorithm>
  #include <atomic>
  #include <chrono>
  #include <csignal>
  #include <cstdint>
  #include <cstring>
  #include <format>
  #include <string>
  #include <thread>
  #include <vector>

  // lib includes
  #include <boost/filesystem.hpp>
  #include <boost/process/v1/environment.hpp>
  #include <tray/src/tray.h>

  // local includes
  #include "confighttp.h"
  #include "connection_gate.h"
  #include "display_device.h"
  #include "logging.h"
  #include "nvhttp.h"
  #include "platform/common.h"
  #include "process.h"
  #include "rtsp.h"
  #include "src/entry_handler.h"
  #include "stream.h"
  #include "upnp.h"

using namespace std::literals;

// system_tray namespace
namespace system_tray {
  static std::atomic tray_initialized = false;

  static std::vector<std::string> connection_menu_labels;
  static std::vector<tray_menu> connection_menu_items;
  static std::vector<tray_menu> disconnect_menu_items;
  static std::vector<std::string> connectivity_menu_labels;
  static std::vector<tray_menu> connectivity_menu_items;
  static std::string connectivity_public_ip;
  static std::vector<std::string> connectivity_advanced_labels;
  static std::vector<tray_menu> connectivity_advanced_items;
  static std::atomic_bool manual_connectivity_test_pending = false;
  static std::atomic_bool tray_notification_armed = false;

  struct disconnect_and_unpair_context_t {
    std::uint32_t session_id;
    std::string client_uuid;
  };

  static std::vector<disconnect_and_unpair_context_t> disconnect_and_unpair_contexts;

  void tray_disconnect_session_cb(struct tray_menu *item) {
    auto session_id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(item->context));

    if (rtsp_stream::terminate_session(session_id)) {
      BOOST_LOG(info) << "Disconnected streaming session " << session_id << " from system tray";
    } else {
      BOOST_LOG(warning) << "Unable to disconnect streaming session " << session_id << " from system tray";
    }
  }

  void tray_disconnect_and_unpair_session_cb(struct tray_menu *item) {
    auto *context = static_cast<disconnect_and_unpair_context_t *>(item->context);

    if (!context) {
      BOOST_LOG(warning) << "Unable to disconnect and unpair session: missing tray context";
      return;
    }

    const auto session_id = context->session_id;
    const auto client_uuid = context->client_uuid;

    if (rtsp_stream::terminate_session(session_id)) {
      BOOST_LOG(info) << "Disconnected streaming session " << session_id << " from system tray";
    } else {
      BOOST_LOG(warning) << "Unable to disconnect streaming session " << session_id << " from system tray";
    }

    if (!client_uuid.empty()) {
      if (nvhttp::unpair_client(client_uuid)) {
        BOOST_LOG(info) << "Unpaired client " << client_uuid << " from system tray";
      } else {
        BOOST_LOG(warning) << "Unable to unpair client " << client_uuid << " from system tray";
      }
    } else {
      BOOST_LOG(warning) << "Unable to unpair client: no paired client UUID associated with session";
    }
  }

  void rebuild_current_connections_menu() {
    auto sessions = rtsp_stream::active_sessions();

    std::vector<std::string> new_labels;
    std::vector<tray_menu> new_connection_items;
    std::vector<tray_menu> new_disconnect_items;
    std::vector<disconnect_and_unpair_context_t> new_disconnect_and_unpair_contexts;

    if (sessions.empty()) {
      new_labels.emplace_back("No active connections");

      new_connection_items.push_back({
        .text = new_labels.back().c_str(),
        .disabled = 1,
      });

      new_connection_items.push_back({.text = nullptr});
    } else {
      new_labels.reserve(sessions.size());
      new_connection_items.reserve(sessions.size() + 1);
      new_disconnect_items.reserve((sessions.size() * 3) + 1);
      new_disconnect_and_unpair_contexts.reserve(sessions.size());

      for (const auto &session : sessions) {
        auto label = session.address;

        if (!session.client_name.empty()) {
          label = std::format("{} ({})", session.client_name, session.address);
        } else if (!session.client_unique_id.empty() &&
                  session.client_unique_id != "unknown" &&
                  session.client_unique_id != "0123456789ABCDEF") {
          label = std::format("{} ({})", session.client_unique_id, session.address);
        }

        new_labels.emplace_back(std::move(label));

        const auto disconnect_index = new_disconnect_items.size();

        new_disconnect_and_unpair_contexts.push_back({
          .session_id = session.id,
          .client_uuid = session.client_uuid,
        });

        new_disconnect_items.push_back({
          .text = "Disconnect",
          .cb = tray_disconnect_session_cb,
          .context = reinterpret_cast<void *>(static_cast<std::uintptr_t>(session.id)),
        });

        new_disconnect_items.push_back({
          .text = "Disconnect & Unpair",
          .disabled = session.client_uuid.empty() ? 1 : 0,
          .cb = tray_disconnect_and_unpair_session_cb,
          .context = &new_disconnect_and_unpair_contexts.back(),
        });

        new_disconnect_items.push_back({.text = nullptr});

        new_connection_items.push_back({
          .text = new_labels.back().c_str(),
          .submenu = &new_disconnect_items[disconnect_index],
        });
      }

      new_connection_items.push_back({.text = nullptr});
    }

    connection_menu_labels.swap(new_labels);
    connection_menu_items.swap(new_connection_items);
    disconnect_menu_items.swap(new_disconnect_items);
    disconnect_and_unpair_contexts.swap(new_disconnect_and_unpair_contexts);
  }

  void tray_refresh_connectivity_cb(struct tray_menu *item);
  void tray_copy_public_ip_cb(struct tray_menu *item);

  void rebuild_connectivity_diagnostics_menu() {
    const auto diagnostics = upnp::get_diagnostics();
    const bool active_stream = !rtsp_stream::active_sessions().empty();

    connectivity_public_ip = diagnostics.external_address;

    const bool all_mappings_ok =
      !diagnostics.mappings.empty() &&
      std::all_of(
        diagnostics.mappings.begin(),
        diagnostics.mappings.end(),
        [](const upnp::mapping_status_t &mapping) {
          return mapping.mapping_known && mapping.mapped;
        }
      );

    std::vector<std::string> new_labels;
    std::vector<std::string> new_advanced_labels;

    const auto internet_access_label = [&diagnostics, active_stream]() -> std::string {
      if (active_stream) {
        return "Internet Access: Streaming";
      }

      switch (diagnostics.internet_access) {
        case upnp::internet_access_e::testing:
          return "Internet Access: Testing...";
        case upnp::internet_access_e::working:
          return "Internet Access: Working";
        case upnp::internet_access_e::blocked:
          return "Internet Access: Ensure Port Forwarding";
        case upnp::internet_access_e::unavailable:
          return "Internet Access: Test Unavailable";
        case upnp::internet_access_e::not_tested:
        default:
          return "Internet Access: Not Tested";
      }
    }();

    if (!diagnostics.enabled) {
      new_labels.emplace_back("Automatic Port Setup: Off");

      if (!diagnostics.lan_address.empty()) {
        new_labels.emplace_back(std::format("Local IP: {}", diagnostics.lan_address));
      }

      if (!diagnostics.external_address.empty()) {
        new_labels.emplace_back(std::format("Public IP: {}", diagnostics.external_address));
      }

      new_labels.emplace_back("Router Setup: Manual Port Forwarding May Be Required");
      new_labels.emplace_back(internet_access_label);
    } else if (!diagnostics.igd_found) {
      new_labels.emplace_back("Automatic Port Setup: On");
      new_labels.emplace_back("Router: UPnP Unavailable");

      if (!diagnostics.lan_address.empty()) {
        new_labels.emplace_back(std::format("Local IP: {}", diagnostics.lan_address));
      }

      if (!diagnostics.external_address.empty()) {
        new_labels.emplace_back(std::format("Public IP: {}", diagnostics.external_address));
      }

      new_labels.emplace_back("Router Setup: Manual Port Forwarding May Be Required");
      new_labels.emplace_back(internet_access_label);
    } else {
      new_labels.emplace_back("Automatic Port Setup: On");
      new_labels.emplace_back(
        diagnostics.igd_connected ?
          "Router: Connected" :
          "Router: UPnP Not Connected"
      );

      if (!diagnostics.lan_address.empty()) {
        new_labels.emplace_back(std::format("Local IP: {}", diagnostics.lan_address));
      }

      if (!diagnostics.external_address.empty()) {
        new_labels.emplace_back(std::format("Public IP: {}", diagnostics.external_address));
      }

      new_labels.emplace_back(
        diagnostics.igd_connected && all_mappings_ok ?
          "Router Setup: Good" :
          "Router Setup: Needs Attention"
      );

      new_labels.emplace_back(internet_access_label);
    }

    if (!diagnostics.mappings.empty()) {
      new_advanced_labels.reserve(diagnostics.mappings.size());
      auto sorted_mappings = diagnostics.mappings;

      std::sort(
        sorted_mappings.begin(),
        sorted_mappings.end(),
        [](const upnp::mapping_status_t &a, const upnp::mapping_status_t &b) {
          const auto a_port = std::stoi(a.wan_port);
          const auto b_port = std::stoi(b.wan_port);

          if (a_port != b_port) {
            return a_port < b_port;
          }

          return a.protocol < b.protocol;
        }
      );

      for (const auto &mapping : sorted_mappings) {
        const auto router_status = [&mapping]() -> std::string_view {
          if (!mapping.mapping_known) {
            return "Manual/Unknown";
          }

          return mapping.mapped ? "UPnP Mapped" : "UPnP Failed";
        }();

        const auto internet_status = [&mapping, active_stream]() -> std::string_view {
          if (active_stream) {
            return "In Use";
          }

          switch (mapping.internet_reachability) {
            case upnp::port_reachability_e::testing:
              return "Testing...";
            case upnp::port_reachability_e::reachable:
              return "Reachable";
            case upnp::port_reachability_e::blocked:
              return "Probe Failed";
            case upnp::port_reachability_e::unavailable:
              return "Test Unavailable";
            case upnp::port_reachability_e::not_tested:
            default:
              return "Not Tested";
          }
        }();

        new_advanced_labels.emplace_back(
          std::format(
            "{} {} | Router: {} | Internet: {}",
            mapping.protocol,
            mapping.wan_port,
            router_status,
            internet_status
          )
        );
      }
    }

    std::vector<tray_menu> new_advanced_items;
    new_advanced_items.reserve(new_advanced_labels.size() + 1);

    for (const auto &label : new_advanced_labels) {
      new_advanced_items.push_back({
        .text = label.c_str(),
        .disabled = 1,
      });
    }

    new_advanced_items.push_back({.text = nullptr});

    std::vector<tray_menu> new_items;
    new_items.reserve(new_labels.size() + 4);

    for (const auto &label : new_labels) {
      new_items.push_back({
        .text = label.c_str(),
        .disabled = 1,
      });
    }

    if (!connectivity_public_ip.empty()) {
      new_items.push_back({
        .text = "Copy Public IP",
        .cb = tray_copy_public_ip_cb,
      });
    }

    new_items.push_back({.text = "-"});
    new_items.push_back({
      .text = "Refresh Connection Status",
      .cb = tray_refresh_connectivity_cb,
    });

    if (!new_advanced_labels.empty()) {
      new_items.push_back({
        .text = "TCP/UDP Port Status",
        .submenu = new_advanced_items.data(),
      });
    }

    new_items.push_back({.text = nullptr});

    connectivity_menu_labels.swap(new_labels);
    connectivity_menu_items.swap(new_items);
    connectivity_advanced_labels.swap(new_advanced_labels);
    connectivity_advanced_items.swap(new_advanced_items);
  }

  void tray_copy_public_ip_cb([[maybe_unused]] struct tray_menu *item) {
#if defined(_WIN32)
    if (connectivity_public_ip.empty()) {
      BOOST_LOG(warning) << "Unable to copy public IP: no public IP is currently available"sv;
      return;
    }

    if (!OpenClipboard(nullptr)) {
      BOOST_LOG(warning) << "Unable to open Windows clipboard for public IP copy"sv;
      return;
    }

    if (!EmptyClipboard()) {
      BOOST_LOG(warning) << "Unable to clear Windows clipboard for public IP copy"sv;
      CloseClipboard();
      return;
    }

    const auto byte_count = connectivity_public_ip.size() + 1;

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byte_count);
    if (!memory) {
      BOOST_LOG(warning) << "Unable to allocate clipboard memory for public IP"sv;
      CloseClipboard();
      return;
    }

    void *destination = GlobalLock(memory);
    if (!destination) {
      BOOST_LOG(warning) << "Unable to lock clipboard memory for public IP"sv;
      GlobalFree(memory);
      CloseClipboard();
      return;
    }

    std::memcpy(
      destination,
      connectivity_public_ip.c_str(),
      byte_count
    );

    GlobalUnlock(memory);

    if (!SetClipboardData(CF_TEXT, memory)) {
      BOOST_LOG(warning) << "Unable to place public IP onto Windows clipboard"sv;
      GlobalFree(memory);
      CloseClipboard();
      return;
    }

    // Windows owns the memory after a successful SetClipboardData().
    CloseClipboard();

    BOOST_LOG(info) << "Copied public IP to clipboard: " << connectivity_public_ip;
#else
    BOOST_LOG(warning) << "Copy Public IP is only supported on Windows"sv;
#endif
  }

  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Opening UI from system tray"sv;
    launch_ui();
  }

  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://github.com/sponsors/LizardByte");
  }

  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.patreon.com/LizardByte");
  }

  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item) {
    platf::open_url("https://www.paypal.com/paypalme/ReenigneArcher");
  }

  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Resetting display device config from system tray"sv;

    std::ignore = display_device::reset_persistence();
  }

  void tray_restart_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Restarting from system tray"sv;

    platf::restart();
  }

  void tray_quit_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Quitting from system tray"sv;

  #ifdef _WIN32
    // If we're running in a service, return a special status to
    // tell it to terminate too, otherwise it will just respawn us.
    if (GetConsoleWindow() == nullptr) {
      lifetime::exit_sunshine(ERROR_SHUTDOWN_IN_PROGRESS, true);
      return;
    }
  #endif

    lifetime::exit_sunshine(0, true);
  }

  /**
   * @brief Reflects connection_gate's current selected mode onto the tray's checkmarks and
   * refreshes the tray so the change is visible immediately. Called after any menu-driven
   * mode change, and once from init_tray() to reflect the mode Sunshine actually started in.
   */
  void sync_connection_gate_menu();

  void tray_connection_gate_open_cb([[maybe_unused]] struct tray_menu *item) {
    connection_gate::set_mode(connection_gate::mode_e::open);
    sync_connection_gate_menu();
  }

  void tray_connection_gate_closed_cb([[maybe_unused]] struct tray_menu *item) {
    connection_gate::set_mode(connection_gate::mode_e::closed);
    sync_connection_gate_menu();
  }

  void tray_connection_gate_auto_close_cb([[maybe_unused]] struct tray_menu *item) {
    connection_gate::set_mode(connection_gate::mode_e::auto_close);
    sync_connection_gate_menu();
  }

  // Named (not inline-anonymous like the rest of the static tray struct below) so
  // sync_connection_gate_menu() can index into it directly to update .checked state.
  static struct tray_menu connection_gate_submenu[] = {
    {.text = "Open", .checkbox = 1, .cb = tray_connection_gate_open_cb},
    {.text = "Closed", .checkbox = 1, .cb = tray_connection_gate_closed_cb},
    {.text = "Auto-Close (closes after configurable delay once everyone's gone)", .checkbox = 1, .cb = tray_connection_gate_auto_close_cb},
    {.text = nullptr}
  };

  #ifdef _WIN32
    static struct tray_menu display_device_reset_confirm_submenu[] = {
      {.text = "This clears Sunshine's saved display configuration", .disabled = 1},
      {.text = "Confirm Reset", .cb = tray_reset_display_device_config_cb},
      {.text = nullptr}
    };
  #endif

  // Tray menu
  static struct tray tray = {
    .icon = TRAY_ICON,
    .tooltip = PROJECT_NAME,
    .menu =
      (struct tray_menu[]) {
        // todo - use boost/locale to translate menu strings
        {.text = "Open Sunshine Web UI", .cb = tray_open_ui_cb},
        {.text = "-"},
        {.text = "Donate",
         .submenu =
           (struct tray_menu[]) {
             {.text = "GitHub Sponsors", .cb = tray_donate_github_cb},
             {.text = "Patreon", .cb = tray_donate_patreon_cb},
             {.text = "PayPal", .cb = tray_donate_paypal_cb},
             {.text = nullptr}
           }},
        {.text = "Sunshine Host Settings", .submenu = connection_gate_submenu},
        {.text = "Host Connection Details"},
        {.text = "Client Connections"},
        {.text = "-"},
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
        {.text = "Reset Saved Display Config", .submenu = display_device_reset_confirm_submenu},
  #endif
        {.text = "Restart Sunshine", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  static void clear_armed_tray_notification() {
    if (!tray_notification_armed.exchange(false)) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
  }

  void tray_refresh_connectivity_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Refreshing Internet connectivity diagnostics from system tray"sv;

    manual_connectivity_test_pending = true;

    // This synchronously publishes the Testing state to the tray, then launches
    // the actual WAN test on a worker thread.
    upnp::refresh_internet_access();

    if (tray_initialized && !config::sunshine.managed_mode) {
      clear_armed_tray_notification();

      tray.notification_title = nullptr;
      tray.notification_text = nullptr;
      tray.notification_cb = nullptr;
      tray.notification_icon = nullptr;
      tray_update(&tray);

      tray.notification_title = "Connectivity Test Started";
      tray.notification_text = "Checking Internet access...";
      tray.notification_cb = nullptr;
      tray.notification_icon = TRAY_ICON;
      tray_update(&tray);

      // Keep the notification data valid until the next tray update, but mark
      // it so that unrelated tray refreshes clear it before calling tray_update().
      tray_notification_armed = true;
    }
  }

  void refresh_current_connections() {
    // Keep the previous menu storage alive until tray_update() has replaced
    // the native menu, since it may still reference these strings/items/contexts.
    auto old_labels = std::move(connection_menu_labels);
    auto old_connection_items = std::move(connection_menu_items);
    auto old_disconnect_items = std::move(disconnect_menu_items);
    auto old_disconnect_and_unpair_contexts = std::move(disconnect_and_unpair_contexts);

    rebuild_current_connections_menu();
    tray.menu[5].submenu = connection_menu_items.data();

    if (tray_initialized) {
      clear_armed_tray_notification();
      tray_update(&tray);
    }
  }

  void refresh_connectivity_diagnostics() {
    const auto diagnostics = upnp::get_diagnostics();

    const bool manual_test_completed =
      diagnostics.internet_access != upnp::internet_access_e::testing &&
      manual_connectivity_test_pending.exchange(false);

    // Keep the previous menu storage alive until tray_update() has replaced
    // the native menu, since it may still reference these strings/items.
    auto old_labels = std::move(connectivity_menu_labels);
    auto old_items = std::move(connectivity_menu_items);
    auto old_advanced_labels = std::move(connectivity_advanced_labels);
    auto old_advanced_items = std::move(connectivity_advanced_items);

    rebuild_connectivity_diagnostics_menu();

    tray.menu[4].submenu = connectivity_menu_items.data();

    if (tray_initialized) {
      // Any previously displayed notification must be cleared before this
      // otherwise-normal tray refresh so Windows doesn't replay it.
      clear_armed_tray_notification();

      tray_update(&tray);

      if (manual_test_completed && !config::sunshine.managed_mode) {
        static std::string notification_text;

        if (!rtsp_stream::active_sessions().empty()) {
          notification_text = "Moonlight is actively streaming.";
        } else switch (diagnostics.internet_access) {
          case upnp::internet_access_e::working:
            notification_text = "Internet access is working.";
            break;

          case upnp::internet_access_e::blocked:
            notification_text = "Ensure port forwarding is configured.";
            break;

          case upnp::internet_access_e::unavailable:
            notification_text = "The Internet connectivity test is unavailable.";
            break;

          case upnp::internet_access_e::not_tested:
          default:
            notification_text = "Internet access could not be tested.";
            break;
        }

        tray.notification_title = "Connectivity Test Complete";
        tray.notification_text = notification_text.c_str();
        tray.notification_cb = nullptr;
        tray.notification_icon = TRAY_ICON;
        tray_update(&tray);

        tray_notification_armed = true;
      }
    }
  }

  void sync_connection_gate_menu() {
    auto mode = connection_gate::current_mode();

    connection_gate_submenu[0].checked =
      (mode == connection_gate::mode_e::open);

    connection_gate_submenu[1].checked =
      (mode == connection_gate::mode_e::closed);

    connection_gate_submenu[2].checked =
      (mode == connection_gate::mode_e::auto_close);

    if (tray_initialized) {
      clear_armed_tray_notification();
      tray_update(&tray);
    }
  }

  const char *GetResourcePath(const char *relativePath) {
  #ifdef __APPLE__
    if (!relativePath || !*relativePath) {
      return nullptr;
    }

    // Simple cache ensures our string pointers live forever
    static std::unordered_map<std::string, std::string> g_cache;
    auto search = g_cache.find(relativePath);
    if (search != g_cache.end()) {
      return search->second.c_str();
    }

    // If we're running from an .app bundle, get the internal Resources dir
    CFBundleRef bundle = CFBundleGetMainBundle();
    if (!bundle) {
      return relativePath;
    }

    CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(bundle);
    if (!resourcesURL) {
      return relativePath;
    }

    char resourcesPath[PATH_MAX];
    bool ok = CFURLGetFileSystemRepresentation(
      resourcesURL,
      true,
      reinterpret_cast<UInt8 *>(resourcesPath),
      sizeof(resourcesPath)
    );
    CFRelease(resourcesURL);
    if (!ok) {
      return relativePath;
    }

    std::string full;
    if (relativePath && relativePath[0] == '/') {
      full = relativePath;
    } else {
      full = std::string(resourcesPath) + "/" + relativePath;
    }

    BOOST_LOG(debug) << "System Tray: using " << full << " for icon path";

    auto [it, inserted] = g_cache.emplace(relativePath, std::move(full));
    return it->second.c_str();
  #else
    return relativePath;
  #endif
  }

  int init_tray() {
  #ifdef _WIN32
    // If we're running as SYSTEM, Explorer.exe will not have permission to open our thread handle
    // to monitor for thread termination. If Explorer fails to open our thread, our tray icon
    // will persist forever if we terminate unexpectedly. To avoid this, we will modify our thread
    // DACL to add an ACE that allows SYNCHRONIZE access to Everyone.
    {
      PACL old_dacl;
      PSECURITY_DESCRIPTOR sd;
      auto error = GetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old_dacl, nullptr, &sd);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "GetSecurityInfo() failed: "sv << error;
        return 1;
      }

      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      SID_IDENTIFIER_AUTHORITY sid_authority = SECURITY_WORLD_SID_AUTHORITY;
      PSID world_sid;
      if (!AllocateAndInitializeSid(&sid_authority, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world_sid)) {
        error = GetLastError();
        BOOST_LOG(warning) << "AllocateAndInitializeSid() failed: "sv << error;
        return 1;
      }

      auto free_sid = util::fail_guard([world_sid]() {
        FreeSid(world_sid);
      });

      EXPLICIT_ACCESS ea {};
      ea.grfAccessPermissions = SYNCHRONIZE;
      ea.grfAccessMode = GRANT_ACCESS;
      ea.grfInheritance = NO_INHERITANCE;
      ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      ea.Trustee.ptstrName = (LPSTR) world_sid;

      PACL new_dacl;
      error = SetEntriesInAcl(1, &ea, old_dacl, &new_dacl);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetEntriesInAcl() failed: "sv << error;
        return 1;
      }

      auto free_new_dacl = util::fail_guard([new_dacl]() {
        LocalFree(new_dacl);
      });

      error = SetSecurityInfo(GetCurrentThread(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
      if (error != ERROR_SUCCESS) {
        BOOST_LOG(warning) << "SetSecurityInfo() failed: "sv << error;
        return 1;
      }
    }

    // Wait for the shell to be initialized before registering the tray icon.
    // This ensures the tray icon works reliably after a logoff/logon cycle.
    while (GetShellWindow() == nullptr) {
      Sleep(1000);
    }
  #endif

  #ifdef __APPLE__
    // if these icon paths are relative, resolve to internal .app Resources path
    tray.allIconPaths[0] = GetResourcePath(TRAY_ICON);
    tray.allIconPaths[1] = GetResourcePath(TRAY_ICON_LOCKED);
    tray.allIconPaths[2] = GetResourcePath(TRAY_ICON_PLAYING);
    tray.allIconPaths[3] = GetResourcePath(TRAY_ICON_PAUSING);

    tray.icon = tray.allIconPaths[0];
  #endif

    refresh_current_connections();
    refresh_connectivity_diagnostics();

    if (tray_init(&tray) < 0) {
      BOOST_LOG(warning) << "Failed to create system tray"sv;
      return 1;
    }

    BOOST_LOG(info) << "System tray created"sv;
    tray_initialized = true;
    connection_gate::set_on_mode_changed(sync_connection_gate_menu);
    sync_connection_gate_menu();
    return 0;
  }

  int process_tray_events() {
    if (!tray_initialized) {
      BOOST_LOG(error) << "System tray is not initialized"sv;
      return 1;
    }

    // Block until an event is processed or tray_quit() is called
    return tray_loop(1);
  }

  int end_tray() {
    if (tray_initialized) {
      tray_initialized = false;
      tray_exit();
    }
    return 0;
  }

  void update_tray_playing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    clear_armed_tray_notification();

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;

    static std::string msg;
    msg = std::format("Streaming started for {}", app_name);

    tray.icon = TRAY_ICON_PLAYING;
    tray.tooltip = msg.c_str();
    tray_update(&tray);

    if (config::sunshine.managed_mode) {
      return;
    }

    tray.notification_title = "Stream Started";
    tray.notification_text = msg.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray_update(&tray);

    tray_notification_armed = true;
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    clear_armed_tray_notification();

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;

    static std::string msg;
    msg = std::format("Streaming paused for {}", app_name);

    tray.icon = TRAY_ICON_PAUSING;
    tray.tooltip = msg.c_str();
    tray_update(&tray);

    if (config::sunshine.managed_mode) {
      return;
    }

    tray.notification_title = "Stream Paused";
    tray.notification_text = msg.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray_update(&tray);

    tray_notification_armed = true;
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    clear_armed_tray_notification();

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);

    if (config::sunshine.managed_mode) {
      return;
    }

    static std::string msg;
    msg = std::format("Application {} successfully stopped", app_name);

    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg.c_str();
    tray_update(&tray);

    tray_notification_armed = true;
  }

  #ifdef _WIN32
  namespace {
    constexpr wchar_t PAIRING_DIALOG_CLASS_NAME[] = L"SunshinePairingPinDialog";
    constexpr int PAIRING_PIN_EDIT_ID = 1001;
    constexpr int PAIRING_BUTTON_ID = 1002;
    constexpr int PAIRING_NAME_EDIT_ID = 1003;
    constexpr int PAIRING_GATE_CHECKBOX_ID = 1004;

    constexpr COLORREF PAIRING_BG = RGB(249, 249, 249);
    constexpr COLORREF PAIRING_CARD = RGB(255, 255, 255);
    constexpr COLORREF PAIRING_TEXT = RGB(32, 32, 32);
    constexpr COLORREF PAIRING_MUTED = RGB(96, 96, 96);
    constexpr COLORREF PAIRING_BORDER = RGB(218, 218, 218);
    constexpr COLORREF PAIRING_ACCENT = RGB(255, 145, 0);

    struct pairing_dialog_state_t {
      HWND pin_edit = nullptr;
      HWND name_edit = nullptr;
      HWND pair_button = nullptr;
      HWND gate_checkbox = nullptr;
      bool host_gate_closed = false;
      HFONT title_font = nullptr;
      HFONT body_font = nullptr;
      HFONT label_font = nullptr;
      HFONT button_font = nullptr;
      HBRUSH background_brush = nullptr;
      HBRUSH edit_brush = nullptr;
      bool paired = false;
    };

    std::string wide_to_utf8(const std::wstring &value) {
      if (value.empty()) {
        return {};
      }

      const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
      );

      if (required <= 0) {
        return {};
      }

      std::string result(static_cast<std::size_t>(required), '\0');
      WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        required,
        nullptr,
        nullptr
      );
      return result;
    }

    HWND create_pairing_static(
      HWND parent,
      const wchar_t *text,
      int x,
      int y,
      int width,
      int height,
      HFONT font
    ) {
      HWND control = CreateWindowExW(
        0,
        L"STATIC",
        text,
        WS_CHILD | WS_VISIBLE,
        x, y, width, height,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
      );

      if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
      }
      return control;
    }

    void apply_windows_11_window_style(HWND hwnd) {
      // Resolve DwmSetWindowAttribute dynamically so we don't add a new link dependency.
      HMODULE dwmapi = LoadLibraryW(L"dwmapi.dll");
      if (!dwmapi) {
        return;
      }

      using dwm_set_window_attribute_t = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
      auto set_attribute = reinterpret_cast<dwm_set_window_attribute_t>(
        GetProcAddress(dwmapi, "DwmSetWindowAttribute")
      );

      if (set_attribute) {
        // DWMWA_WINDOW_CORNER_PREFERENCE / DWMWCP_ROUND
        constexpr DWORD window_corner_preference = 33;
        constexpr DWORD round_corners = 2;
        set_attribute(hwnd, window_corner_preference, &round_corners, sizeof(round_corners));

        // Keep the caption visually integrated with the light Windows 11 surface.
        constexpr DWORD caption_color_attribute = 35;
        constexpr DWORD text_color_attribute = 36;
        const COLORREF caption_color = PAIRING_BG;
        const COLORREF caption_text_color = PAIRING_TEXT;
        set_attribute(hwnd, caption_color_attribute, &caption_color, sizeof(caption_color));
        set_attribute(hwnd, text_color_attribute, &caption_text_color, sizeof(caption_text_color));
      }

      FreeLibrary(dwmapi);
    }

    void draw_pairing_button(const DRAWITEMSTRUCT *draw_item) {
      if (!draw_item) {
        return;
      }

      const bool primary = draw_item->CtlID == PAIRING_BUTTON_ID;
      const bool pressed = (draw_item->itemState & ODS_SELECTED) != 0;
      const bool disabled = (draw_item->itemState & ODS_DISABLED) != 0;
      RECT rect = draw_item->rcItem;

      COLORREF fill;
      COLORREF border;
      COLORREF text;

      if (disabled) {
        fill = RGB(232, 232, 232);
        border = RGB(220, 220, 220);
        text = PAIRING_MUTED;
      } else if (primary) {
        fill = pressed ? RGB(232, 126, 0) : PAIRING_ACCENT;
        border = fill;
        text = RGB(255, 255, 255);
      } else {
        fill = pressed ? RGB(238, 238, 238) : PAIRING_CARD;
        border = PAIRING_BORDER;
        text = PAIRING_TEXT;
      }

      HBRUSH brush = CreateSolidBrush(fill);
      HPEN pen = CreatePen(PS_SOLID, 1, border);
      auto old_brush = SelectObject(draw_item->hDC, brush);
      auto old_pen = SelectObject(draw_item->hDC, pen);
      RoundRect(draw_item->hDC, rect.left, rect.top, rect.right, rect.bottom, 8, 8);
      SelectObject(draw_item->hDC, old_pen);
      SelectObject(draw_item->hDC, old_brush);
      DeleteObject(pen);
      DeleteObject(brush);

      wchar_t text_buffer[64] {};
      GetWindowTextW(draw_item->hwndItem, text_buffer, 64);
      SetBkMode(draw_item->hDC, TRANSPARENT);
      SetTextColor(draw_item->hDC, text);

      HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw_item->hwndItem, WM_GETFONT, 0, 0));
      auto old_font = font ? SelectObject(draw_item->hDC, font) : nullptr;
      DrawTextW(draw_item->hDC, text_buffer, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (old_font) {
        SelectObject(draw_item->hDC, old_font);
      }

      if ((draw_item->itemState & ODS_FOCUS) != 0) {
        RECT focus = rect;
        InflateRect(&focus, -4, -4);
        DrawFocusRect(draw_item->hDC, &focus);
      }
    }

    LRESULT CALLBACK pairing_dialog_wnd_proc(HWND hwnd, UINT message, WPARAM w_param, LPARAM l_param) {
      auto *state = reinterpret_cast<pairing_dialog_state_t *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

      if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(l_param);
        state = static_cast<pairing_dialog_state_t *>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      }

      switch (message) {
        case WM_CREATE: {
          if (!state) {
            return -1;
          }

          apply_windows_11_window_style(hwnd);

          state->background_brush = CreateSolidBrush(PAIRING_BG);
          state->edit_brush = CreateSolidBrush(PAIRING_CARD);
          state->title_font = CreateFontW(
            -24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
          );
          state->body_font = CreateFontW(
            -16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
          );
          state->label_font = CreateFontW(
            -15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
          );
          state->button_font = CreateFontW(
            -15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
          );

          // Header: orange Sunshine accent + clean Windows 11 typography.
          create_pairing_static(hwnd, L"Pair a Moonlight client", 58, 20, 390, 38, state->title_font);
          create_pairing_static(
            hwnd,
            L"Enter the PIN shown by Moonlight to approve this connection.",
            58, 61, 390, 24, state->body_font
          );

          create_pairing_static(hwnd, L"Pairing PIN", 28, 108, 190, 22, state->label_font);
          state->pin_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL,
            28, 134, 196, 38,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(PAIRING_PIN_EDIT_ID)),
            GetModuleHandleW(nullptr),
            nullptr
          );
          SendMessageW(state->pin_edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
          SendMessageW(state->pin_edit, EM_SETLIMITTEXT, 4, 0);

          create_pairing_static(hwnd, L"Connection Name  (optional)", 244, 108, 224, 22, state->label_font);
          state->name_edit = CreateWindowExW(
            WS_EX_CLIENTEDGE,
            L"EDIT",
            L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_AUTOHSCROLL,
            244, 134, 224, 38,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(PAIRING_NAME_EDIT_ID)),
            GetModuleHandleW(nullptr),
            nullptr
          );
          SendMessageW(state->name_edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->body_font), TRUE);
          SendMessageW(state->name_edit, EM_SETLIMITTEXT, 64, 0);

          int divider_y = 214;
          int button_y = 232;

          if (state->host_gate_closed) {
            create_pairing_static(
              hwnd,
              L"Host connection is currently closed.",
              28, 190, 440, 22, state->label_font
            );

            state->gate_checkbox = CreateWindowExW(
              0,
              L"BUTTON",
              L"Open host connection for pairing",
              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
              28, 218, 440, 26,
              hwnd,
              reinterpret_cast<HMENU>(static_cast<INT_PTR>(PAIRING_GATE_CHECKBOX_ID)),
              GetModuleHandleW(nullptr),
              nullptr
            );
            SendMessageW(
              state->gate_checkbox,
              WM_SETFONT,
              reinterpret_cast<WPARAM>(state->body_font),
              TRUE
            );

            divider_y = 264;
            button_y = 282;
          }

          HWND cancel_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            149, button_y, 92, 36,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            GetModuleHandleW(nullptr),
            nullptr
          );
          SendMessageW(cancel_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);

          state->pair_button = CreateWindowExW(
            0,
            L"BUTTON",
            L"Pair",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            253, button_y, 92, 36,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(PAIRING_BUTTON_ID)),
            GetModuleHandleW(nullptr),
            nullptr
          );
          SendMessageW(state->pair_button, WM_SETFONT, reinterpret_cast<WPARAM>(state->button_font), TRUE);

          if (state->host_gate_closed) {
            EnableWindow(state->pair_button, FALSE);
          }

          SetPropW(hwnd, L"SunshinePairingDividerY", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(divider_y)));

          SetFocus(state->pin_edit);
          return 0;
        }

        case WM_PAINT: {
          PAINTSTRUCT ps {};
          HDC dc = BeginPaint(hwnd, &ps);
          FillRect(dc, &ps.rcPaint, state && state->background_brush ? state->background_brush : GetSysColorBrush(COLOR_WINDOW));

          // Small Sunshine-colored mark like the tray/notification icon accent.
          HBRUSH accent_brush = CreateSolidBrush(PAIRING_ACCENT);
          HPEN accent_pen = CreatePen(PS_SOLID, 1, PAIRING_ACCENT);
          auto old_brush = SelectObject(dc, accent_brush);
          auto old_pen = SelectObject(dc, accent_pen);
          Ellipse(dc, 28, 29, 46, 47);
          SelectObject(dc, old_pen);
          SelectObject(dc, old_brush);
          DeleteObject(accent_pen);
          DeleteObject(accent_brush);

          // Soft divider above the action buttons.
          const auto divider_prop = GetPropW(hwnd, L"SunshinePairingDividerY");
          const int divider_y = divider_prop ?
            static_cast<int>(reinterpret_cast<INT_PTR>(divider_prop)) :
            214;

          HPEN divider_pen = CreatePen(PS_SOLID, 1, RGB(232, 232, 232));
          old_pen = SelectObject(dc, divider_pen);
          MoveToEx(dc, 28, divider_y, nullptr);
          LineTo(dc, 468, divider_y);
          SelectObject(dc, old_pen);
          DeleteObject(divider_pen);

          EndPaint(hwnd, &ps);
          return 0;
        }

        case WM_CTLCOLORSTATIC: {
          HDC dc = reinterpret_cast<HDC>(w_param);
          SetBkMode(dc, TRANSPARENT);
          SetTextColor(dc, PAIRING_TEXT);
          return reinterpret_cast<LRESULT>(state && state->background_brush ? state->background_brush : GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_CTLCOLOREDIT: {
          HDC dc = reinterpret_cast<HDC>(w_param);
          SetBkColor(dc, PAIRING_CARD);
          SetTextColor(dc, PAIRING_TEXT);
          return reinterpret_cast<LRESULT>(state && state->edit_brush ? state->edit_brush : GetSysColorBrush(COLOR_WINDOW));
        }

        case WM_DRAWITEM:
          draw_pairing_button(reinterpret_cast<const DRAWITEMSTRUCT *>(l_param));
          return TRUE;

        case WM_COMMAND: {
          if (!state) {
            break;
          }

          const int command = LOWORD(w_param);
          if (command == IDCANCEL) {
            DestroyWindow(hwnd);
            return 0;
          }

          if (command == PAIRING_GATE_CHECKBOX_ID && HIWORD(w_param) == BN_CLICKED) {
            const bool checked =
              SendMessageW(state->gate_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            EnableWindow(state->pair_button, checked ? TRUE : FALSE);
            InvalidateRect(state->pair_button, nullptr, TRUE);
            return 0;
          }

          if (command == PAIRING_BUTTON_ID) {
            wchar_t pin_wide[5] {};
            const int pin_length = GetWindowTextW(state->pin_edit, pin_wide, 5);

            if (pin_length != 4 ||
                pin_wide[0] < L'0' || pin_wide[0] > L'9' ||
                pin_wide[1] < L'0' || pin_wide[1] > L'9' ||
                pin_wide[2] < L'0' || pin_wide[2] > L'9' ||
                pin_wide[3] < L'0' || pin_wide[3] > L'9') {
              MessageBoxW(
                hwnd,
                L"Enter the 4-digit PIN shown on the Moonlight client.",
                L"Invalid PIN",
                MB_OK | MB_ICONWARNING
              );
              SetFocus(state->pin_edit);
              SendMessageW(state->pin_edit, EM_SETSEL, 0, -1);
              return 0;
            }

            std::string pin;
            pin.reserve(4);
            for (int i = 0; i < 4; ++i) {
              pin.push_back(static_cast<char>(pin_wide[i]));
            }

            wchar_t name_wide[65] {};
            const int name_length = GetWindowTextW(state->name_edit, name_wide, 65);
            const std::string name = name_length > 0 ?
              wide_to_utf8(std::wstring(name_wide, static_cast<std::size_t>(name_length))) :
              std::string {};

            bool opened_gate_for_pairing = false;

            if (connection_gate::current_mode() == connection_gate::mode_e::closed) {
              const bool authorized_to_open =
                state->gate_checkbox &&
                SendMessageW(state->gate_checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;

              if (!authorized_to_open) {
                MessageBoxW(
                  hwnd,
                  L"Host connection is currently closed. Check the box to open it before pairing.",
                  L"Host Connection Closed",
                  MB_OK | MB_ICONWARNING
                );
                return 0;
              }

              connection_gate::set_mode(connection_gate::mode_e::open);
              sync_connection_gate_menu();
              opened_gate_for_pairing = true;
            }

            if (!nvhttp::pin(pin, name)) {
              if (opened_gate_for_pairing) {
                connection_gate::set_mode(connection_gate::mode_e::closed);
                sync_connection_gate_menu();
              }

              MessageBoxW(
                hwnd,
                L"Sunshine could not find a pending pairing request. Start pairing again from Moonlight and retry.",
                L"Pairing Failed",
                MB_OK | MB_ICONERROR
              );
              return 0;
            }

            state->paired = true;
            DestroyWindow(hwnd);
            return 0;
          }

          break;
        }

        case WM_CLOSE:
          DestroyWindow(hwnd);
          return 0;

        case WM_DESTROY:
          RemovePropW(hwnd, L"SunshinePairingDividerY");
          if (state) {
            if (state->title_font) {
              DeleteObject(state->title_font);
            }
            if (state->body_font) {
              DeleteObject(state->body_font);
            }
            if (state->label_font) {
              DeleteObject(state->label_font);
            }
            if (state->button_font) {
              DeleteObject(state->button_font);
            }
            if (state->background_brush) {
              DeleteObject(state->background_brush);
            }
            if (state->edit_brush) {
              DeleteObject(state->edit_brush);
            }
          }
          return 0;
      }

      return DefWindowProcW(hwnd, message, w_param, l_param);
    }

    bool show_pairing_pin_dialog() {
      const HINSTANCE instance = GetModuleHandleW(nullptr);

      static ATOM window_class = 0;
      if (!window_class) {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = pairing_dialog_wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = PAIRING_DIALOG_CLASS_NAME;

        window_class = RegisterClassExW(&wc);
        if (!window_class) {
          BOOST_LOG(error) << "Unable to register native pairing dialog window class: " << GetLastError();
          return false;
        }
      }

      pairing_dialog_state_t state;
      state.host_gate_closed =
        connection_gate::current_mode() == connection_gate::mode_e::closed;

      constexpr int width = 510;
      const int height = state.host_gate_closed ? 390 : 340;
      const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
      const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

      HWND hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        PAIRING_DIALOG_CLASS_NAME,
        L"Sunshine",
        WS_CAPTION | WS_SYSMENU,
        x, y, width, height,
        nullptr,
        nullptr,
        instance,
        &state
      );

      if (!hwnd) {
        BOOST_LOG(error) << "Unable to create native pairing dialog: " << GetLastError();
        return false;
      }

      ShowWindow(hwnd, SW_SHOW);
      UpdateWindow(hwnd);
      SetForegroundWindow(hwnd);

      MSG message {};
      while (IsWindow(hwnd)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
          break;
        }

        if (!IsDialogMessageW(hwnd, &message)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }

      return state.paired;
    }
  }  // namespace
  #endif

  void update_tray_require_pin() {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);
    if (config::sunshine.managed_mode) {
      BOOST_LOG(info) << "Pairing request received while running in managed mode"sv;
      return;
    }

    tray.icon = TRAY_ICON;
    tray.notification_title = "Incoming Pairing Request";
  #ifdef _WIN32
    tray.notification_text = "Click here to enter the Moonlight pairing PIN";
  #else
    tray.notification_text = "Click here to complete the pairing process";
  #endif
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
  #ifdef _WIN32
      if (show_pairing_pin_dialog()) {
        BOOST_LOG(info) << "Moonlight client paired from native system tray PIN dialog"sv;

        // Clear the pairing notification state after a successful submission so
        // unrelated tray refreshes cannot replay the old request notification.
        tray.notification_title = nullptr;
        tray.notification_text = nullptr;
        tray.notification_cb = nullptr;
        tray.notification_icon = nullptr;
        tray.icon = TRAY_ICON;
        tray.tooltip = PROJECT_NAME;
        tray_update(&tray);
      }
  #else
      launch_ui("/pin");
  #endif
    };
    tray_update(&tray);
  }

  // Threading functions available on all platforms
  static void tray_thread_worker() {
    platf::set_thread_name("system_tray");
    BOOST_LOG(info) << "System tray thread started"sv;

    // Initialize the tray in this thread
    if (init_tray() != 0) {
      BOOST_LOG(error) << "Failed to initialize tray in thread"sv;
      return;
    }

    // Main tray event loop
    while (process_tray_events() == 0);

    BOOST_LOG(info) << "System tray thread ended"sv;
  }

  int init_tray_threaded() {
    try {
      auto tray_thread = std::thread(tray_thread_worker);

      // The tray thread doesn't require strong lifetime management.
      // It will exit asynchronously when tray_exit() is called.
      tray_thread.detach();

      BOOST_LOG(info) << "System tray thread initialized successfully"sv;
      return 0;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "Failed to create tray thread: " << e.what();
      return 1;
    }
  }

}  // namespace system_tray
#endif
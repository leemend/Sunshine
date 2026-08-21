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
  static std::vector<std::string> connectivity_advanced_labels;
  static std::vector<tray_menu> connectivity_advanced_items;
  static std::atomic_bool manual_connectivity_test_pending = false;
  static std::atomic_bool connectivity_notification_armed = false;

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

  void rebuild_connectivity_diagnostics_menu() {
    const auto diagnostics = upnp::get_diagnostics();

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

    const auto internet_access_label = [&diagnostics]() -> std::string {
      switch (diagnostics.internet_access) {
        case upnp::internet_access_e::testing:
          return "Internet Access: Testing...";
        case upnp::internet_access_e::working:
          return "Internet Access: Working";
        case upnp::internet_access_e::blocked:
          return "Internet Access: Blocked";
        case upnp::internet_access_e::unavailable:
          return "Internet Access: Test Unavailable";
        case upnp::internet_access_e::not_tested:
        default:
          return "Internet Access: Not Tested";
      }
    }();

    if (!diagnostics.enabled) {
      new_labels.emplace_back("Automatic Port Setup: Off");
      new_labels.emplace_back("Router Setup: Manual Setup Required");
      new_labels.emplace_back(internet_access_label);
    } else if (!diagnostics.igd_found) {
      new_labels.emplace_back("Automatic Port Setup: On");
      new_labels.emplace_back("Router: Not Found");
      new_labels.emplace_back("Router Setup: Needs Attention");
      new_labels.emplace_back(internet_access_label);
    } else {
      new_labels.emplace_back("Automatic Port Setup: On");
      new_labels.emplace_back(
        diagnostics.igd_connected ? "Router: Connected" : "Router: Not Connected"
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

        const auto internet_status = [&mapping]() -> std::string_view {
          switch (mapping.internet_reachability) {
            case upnp::port_reachability_e::testing:
              return "Testing...";
            case upnp::port_reachability_e::reachable:
              return "Reachable";
            case upnp::port_reachability_e::blocked:
              return "Blocked";
            case upnp::port_reachability_e::unavailable:
              return "Test Unavailable";
            case upnp::port_reachability_e::not_tested:
            default:
              return mapping.protocol == "UDP" ?
                "Requires Active Stream" :
                "Not Tested";
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

    if (new_advanced_labels.empty() && !diagnostics.mappings.empty()) {
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
        const auto router_status = mapping.mapping_known ?
          (mapping.mapped ? "UPnP Mapped"sv : "UPnP Failed"sv) :
          "Manual/Unknown"sv;

        const auto internet_status = [&mapping]() -> std::string_view {
          switch (mapping.internet_reachability) {
            case upnp::port_reachability_e::testing:
              return "Testing...";
            case upnp::port_reachability_e::reachable:
              return "Reachable";
            case upnp::port_reachability_e::blocked:
              return "Blocked";
            case upnp::port_reachability_e::unavailable:
              return "Test Unavailable";
            case upnp::port_reachability_e::not_tested:
            default:
              return mapping.protocol == "UDP" ?
                "Requires Active Stream" :
                "Not Tested";
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
    new_items.reserve(new_labels.size() + 3);

    for (const auto &label : new_labels) {
      new_items.push_back({
        .text = label.c_str(),
        .disabled = 1,
      });
    }

    new_items.push_back({.text = "-"});
    new_items.push_back({
      .text = "Refresh Connectivity",
      .cb = tray_refresh_connectivity_cb,
    });

    if (!new_advanced_labels.empty()) {
      new_items.push_back({
        .text = "Advanced Details",
        .submenu = new_advanced_items.data(),
      });
    }

    new_items.push_back({.text = nullptr});

    connectivity_menu_labels.swap(new_labels);
    connectivity_menu_items.swap(new_items);
    connectivity_advanced_labels.swap(new_advanced_labels);
    connectivity_advanced_items.swap(new_advanced_items);
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

  static struct tray_menu display_device_recovery_submenu[] = {
    {.text = "Reset Saved Display Config", .submenu = display_device_reset_confirm_submenu},
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
        {.text = "Open Sunshine", .cb = tray_open_ui_cb},
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
        {.text = "Host Connectivity"},
        {.text = "Client Connections"},
        {.text = "-"},
  // Currently display device settings are only supported on Windows
  #ifdef _WIN32
        {.text = "Display Device Recovery", .submenu = display_device_recovery_submenu},
  #endif
        {.text = "Restart", .cb = tray_restart_cb},
        {.text = "Quit", .cb = tray_quit_cb},
        {.text = nullptr}
      },
    .iconPathCount = 4,
    .allIconPaths = {TRAY_ICON, TRAY_ICON_LOCKED, TRAY_ICON_PLAYING, TRAY_ICON_PAUSING},
  };

  void tray_refresh_connectivity_cb([[maybe_unused]] struct tray_menu *item) {
    BOOST_LOG(info) << "Refreshing Internet connectivity diagnostics from system tray"sv;

    manual_connectivity_test_pending = true;

    // This synchronously publishes the Testing state to the tray, then launches
    // the actual WAN test on a worker thread.
    upnp::refresh_internet_access();

    if (tray_initialized) {
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

      // Leave the fields alive until the next connectivity update. Clearing them
      // immediately can prevent the native Windows notification from appearing.
      connectivity_notification_armed = true;
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
      // If a prior connectivity notification is still armed, clear it before
      // this tray update so automatic refreshes cannot replay stale popups.
      if (connectivity_notification_armed.exchange(false)) {
        tray.notification_title = nullptr;
        tray.notification_text = nullptr;
        tray.notification_cb = nullptr;
        tray.notification_icon = nullptr;
      }

      tray_update(&tray);

      if (manual_test_completed) {
        static std::string notification_text;

        switch (diagnostics.internet_access) {
          case upnp::internet_access_e::working:
            notification_text = "Internet access is working.";
            break;
          case upnp::internet_access_e::blocked:
            notification_text = "Internet access appears to be blocked.";
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

        // Keep this notification valid until the next tray refresh, where it
        // will be cleared before tray_update(). That prevents periodic replay.
        connectivity_notification_armed = true;
      }
    }
  }

  void sync_connection_gate_menu() {
    auto mode = connection_gate::current_mode();
    connection_gate_submenu[0].checked = (mode == connection_gate::mode_e::open);
    connection_gate_submenu[1].checked = (mode == connection_gate::mode_e::closed);
    connection_gate_submenu[2].checked = (mode == connection_gate::mode_e::auto_close);

    if (tray_initialized) {
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

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
    tray.icon = TRAY_ICON_PLAYING;
    tray.notification_title = "Stream Started";

    static std::string msg = std::format("Streaming started for {}", app_name);
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PLAYING;
    tray_update(&tray);
  }

  void update_tray_pausing(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON_PAUSING;
    tray_update(&tray);

    static std::string msg = std::format("Streaming paused for {}", app_name);
    tray.icon = TRAY_ICON_PAUSING;
    tray.notification_title = "Stream Paused";
    tray.notification_text = msg.c_str();
    tray.tooltip = msg.c_str();
    tray.notification_icon = TRAY_ICON_PAUSING;
    tray_update(&tray);
  }

  void update_tray_stopped(std::string app_name) {
    if (!tray_initialized) {
      return;
    }

    tray.notification_title = nullptr;
    tray.notification_text = nullptr;
    tray.notification_cb = nullptr;
    tray.notification_icon = nullptr;
    tray.icon = TRAY_ICON;
    tray_update(&tray);

    static std::string msg = std::format("Application {} successfully stopped", app_name);
    tray.icon = TRAY_ICON;
    tray.notification_icon = TRAY_ICON;
    tray.notification_title = "Application Stopped";
    tray.notification_text = msg.c_str();
    tray.tooltip = PROJECT_NAME;
    tray_update(&tray);
  }

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
    tray.icon = TRAY_ICON;
    tray.notification_title = "Incoming Pairing Request";
    tray.notification_text = "Click here to complete the pairing process";
    tray.notification_icon = TRAY_ICON_LOCKED;
    tray.tooltip = PROJECT_NAME;
    tray.notification_cb = []() {
      launch_ui("/pin");
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
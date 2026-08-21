/**
 * @file src/upnp.cpp
 * @brief Definitions for UPnP port mapping.
 */
// standard includes
#include <stddef.h>  // workaround for type_t error in miniupnpc 2.3.3, see https://github.com/miniupnp/miniupnp/commit/e263ab6f56c382e10fed31347ec68095d691a0e8
#include <atomic>
#include <mutex>
#include <thread>

// lib includes
#include <curl/curl.h>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>

// local includes
#include "config.h"
#include "confighttp.h"
#include "globals.h"
#include "logging.h"
#include "network.h"
#include "nvhttp.h"
#include "rtsp.h"
#include "stream.h"
#include "upnp.h"
#include "utility.h"

#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
  #include "system_tray.h"
#endif


using namespace std::literals;

namespace upnp {

  static std::mutex diagnostics_mutex;
  static diagnostics_t diagnostics;
  static std::atomic_bool internet_test_running = false;

  diagnostics_t get_diagnostics() {
    std::lock_guard lock(diagnostics_mutex);
    return diagnostics;
  }

  static void refresh_tray_diagnostics() {
#if defined SUNSHINE_TRAY && SUNSHINE_TRAY >= 1
    system_tray::refresh_connectivity_diagnostics();
#endif
  }

  static std::size_t discard_http_response(char *, std::size_t size, std::size_t nmemb, void *) {
    return size * nmemb;
  }

  struct internet_test_result_t {
    internet_access_e overall = internet_access_e::not_tested;
    port_reachability_e tcp_47984 = port_reachability_e::not_tested;
    port_reachability_e tcp_47989 = port_reachability_e::not_tested;
    port_reachability_e tcp_48010 = port_reachability_e::not_tested;
  };

  static std::vector<mapping_status_t> default_mapping_statuses() {
    const auto rtsp = std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT));
    const auto video = std::to_string(net::map_port(stream::VIDEO_STREAM_PORT));
    const auto audio = std::to_string(net::map_port(stream::AUDIO_STREAM_PORT));
    const auto control = std::to_string(net::map_port(stream::CONTROL_PORT));
    const auto gs_http = std::to_string(net::map_port(nvhttp::PORT_HTTP));
    const auto gs_https = std::to_string(net::map_port(nvhttp::PORT_HTTPS));
    const auto wm_http = std::to_string(net::map_port(confighttp::PORT_HTTPS));

    std::vector<mapping_status_t> statuses {
      {.protocol = "TCP", .lan_port = rtsp, .wan_port = rtsp, .description = "Sunshine - RTSP"},
      {.protocol = "UDP", .lan_port = video, .wan_port = video, .description = "Sunshine - Video"},
      {.protocol = "UDP", .lan_port = audio, .wan_port = audio, .description = "Sunshine - Audio"},
      {.protocol = "UDP", .lan_port = control, .wan_port = control, .description = "Sunshine - Control"},
      {.protocol = "TCP", .lan_port = gs_http, .wan_port = gs_http, .description = "Sunshine - Client HTTP"},
      {.protocol = "TCP", .lan_port = gs_https, .wan_port = gs_https, .description = "Sunshine - Client HTTPS"},
    };

    if (net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN) {
      statuses.push_back({
        .protocol = "TCP",
        .lan_port = wm_http,
        .wan_port = wm_http,
        .description = "Sunshine - Web UI",
      });
    }

    return statuses;
  }

  static void set_port_reachability(
    std::vector<mapping_status_t> &mappings,
    std::string_view protocol,
    std::string_view wan_port,
    port_reachability_e status
  ) {
    for (auto &mapping : mappings) {
      if (mapping.protocol == protocol && mapping.wan_port == wan_port) {
        mapping.internet_reachability = status;
        return;
      }
    }
  }

  static void apply_internet_test_result(std::vector<mapping_status_t> &mappings, const internet_test_result_t &result) {
    for (auto &mapping : mappings) {
      mapping.internet_reachability = port_reachability_e::not_tested;
    }

    set_port_reachability(mappings, "TCP", "47984", result.tcp_47984);
    set_port_reachability(mappings, "TCP", "47989", result.tcp_47989);
    set_port_reachability(mappings, "TCP", "48010", result.tcp_48010);
  }

  static port_reachability_e test_loopback_http_port(const char *url, bool https) {
    CURL *curl = curl_easy_init();
    if (!curl) {
      return port_reachability_e::unavailable;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_http_response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Sunshine Connectivity Diagnostics");

    if (https) {
      // This is Sunshine's own certificate coming back through the Moonlight
      // loopback relay, so normal public-CA validation is not applicable.
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode result = curl_easy_perform(curl);

    curl_off_t connect_time_us = 0;
    curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME_T, &connect_time_us);
    curl_easy_cleanup(curl);

    if (result == CURLE_OK) {
      return port_reachability_e::reachable;
    }

    // TCP 47984 is HTTPS and Sunshine may reject the probe because it doesn't
    // present a paired-client certificate. Any non-timeout result after reaching
    // the relay still proves the inbound callback reached Sunshine.
    if (https && connect_time_us > 0 && result != CURLE_OPERATION_TIMEDOUT) {
      return port_reachability_e::reachable;
    }

    // We reached the Moonlight relay, but the relay could not complete the
    // callback to Sunshine before our timeout.
    if (connect_time_us > 0) {
      return port_reachability_e::blocked;
    }

    return port_reachability_e::unavailable;
  }

  static port_reachability_e test_loopback_rtsp_port(const char *url) {
    rtsp_stream::arm_connectivity_probe();

    CURL *curl = curl_easy_init();
    if (!curl) {
      rtsp_stream::cancel_connectivity_probe();
      return port_reachability_e::unavailable;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Sunshine Connectivity Diagnostics");

    const CURLcode relay_connect_result = curl_easy_perform(curl);

    if (relay_connect_result != CURLE_OK) {
      curl_easy_cleanup(curl);
      rtsp_stream::cancel_connectivity_probe();
      return port_reachability_e::unavailable;
    }

    // Moonlight's loopback relay accepts our outbound connection on 38010 and
    // then opens a separate inbound TCP connection to the host's public 48010.
    // Sunshine's existing RTSP listener records that callback for us.
    constexpr auto callback_timeout = 3s;
    constexpr auto poll_interval = 25ms;
    const auto deadline = std::chrono::steady_clock::now() + callback_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
      if (rtsp_stream::connectivity_probe_received()) {
        curl_easy_cleanup(curl);
        rtsp_stream::cancel_connectivity_probe();
        return port_reachability_e::reachable;
      }

      std::this_thread::sleep_for(poll_interval);
    }

    curl_easy_cleanup(curl);
    rtsp_stream::cancel_connectivity_probe();

    return port_reachability_e::blocked;
  }

  static internet_test_result_t test_internet_access() {
    internet_test_result_t result;

    // Moonlight's loopback relay uses the standard GameStream ports with a
    // -10000 relay offset. Avoid false results for custom Sunshine base ports.
    if (config::sunshine.port != 47989) {
      return result;
    }

    result.tcp_47984 = test_loopback_http_port(
      "https://loopback-v2.moonlight-stream.org:37984/",
      true
    );

    result.tcp_47989 = test_loopback_http_port(
      "http://loopback-v2.moonlight-stream.org:37989/",
      false
    );

    result.tcp_48010 = test_loopback_rtsp_port(
      "http://loopback-v2.moonlight-stream.org:38010/"
    );

    // The HTTPS relay can fail the initial connection when its callback to
    // Sunshine TCP 47984 is blocked. If either companion relay test proves
    // the Moonlight relay service itself is available, classify this as a
    // blocked host port rather than an unavailable test service.
    if (result.tcp_47984 == port_reachability_e::unavailable &&
        (result.tcp_47989 != port_reachability_e::unavailable ||
         result.tcp_48010 != port_reachability_e::unavailable)) {
      result.tcp_47984 = port_reachability_e::blocked;
    }

    const std::array tested_ports {
      result.tcp_47984,
      result.tcp_47989,
      result.tcp_48010,
    };

    if (std::any_of(
          tested_ports.begin(),
          tested_ports.end(),
          [](port_reachability_e status) {
            return status == port_reachability_e::blocked;
          })) {
      result.overall = internet_access_e::blocked;
    } else if (std::all_of(
                 tested_ports.begin(),
                 tested_ports.end(),
                 [](port_reachability_e status) {
                   return status == port_reachability_e::reachable;
                 })) {
      result.overall = internet_access_e::working;
    } else if (std::any_of(
                 tested_ports.begin(),
                 tested_ports.end(),
                 [](port_reachability_e status) {
                   return status == port_reachability_e::unavailable;
                 })) {
      result.overall = internet_access_e::unavailable;
    } else {
      result.overall = internet_access_e::not_tested;
    }

    return result;
  }

  void refresh_internet_access() {
    bool expected = false;
    if (!internet_test_running.compare_exchange_strong(expected, true)) {
      return;
    }

    {
      std::lock_guard lock(diagnostics_mutex);
      diagnostics.internet_access = internet_access_e::testing;
      set_port_reachability(diagnostics.mappings, "TCP", "47984", port_reachability_e::testing);
      set_port_reachability(diagnostics.mappings, "TCP", "47989", port_reachability_e::testing);
      set_port_reachability(diagnostics.mappings, "TCP", "48010", port_reachability_e::testing);
      diagnostics.last_updated = std::chrono::system_clock::now();
    }

    refresh_tray_diagnostics();

    std::thread([]() {
      const auto internet_test = test_internet_access();

      {
        std::lock_guard lock(diagnostics_mutex);
        diagnostics.internet_access = internet_test.overall;
        apply_internet_test_result(diagnostics.mappings, internet_test);
        diagnostics.last_updated = std::chrono::system_clock::now();
      }

      internet_test_running = false;
      refresh_tray_diagnostics();
    }).detach();
  }

  struct mapping_t {
    struct {
      std::string wan;
      std::string lan;
      std::string proto;
    } port;

    std::string description;
  };

  static std::string_view status_string(int status) {
    switch (status) {
      case 0:
        return "No IGD device found"sv;
      case 1:
        return "Valid IGD device found"sv;
      case 2:
        return "Valid IGD device found,  but it isn't connected"sv;
      case 3:
        return "A UPnP device has been found,  but it wasn't recognized as an IGD"sv;
    }

    return "Unknown status"sv;
  }

  /**
   * This function is a wrapper around UPNP_GetValidIGD() that returns the status code. There is a pre-processor
   * check to determine which version of the function to call based on the version of the MiniUPnPc library.
   */
  int UPNP_GetValidIGDStatus(device_t &device, urls_t *urls, IGDdatas *data, std::array<char, INET6_ADDRESS_STRLEN> &lan_addr) {
#if (MINIUPNPC_API_VERSION >= 18)
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size(), nullptr, 0);
#else
    return UPNP_GetValidIGD(device.get(), &urls->el, data, lan_addr.data(), (int) lan_addr.size());
#endif
  }

  class deinit_t: public platf::deinit_t {
  public:
    deinit_t() {
      auto rtsp = std::to_string(net::map_port(rtsp_stream::RTSP_SETUP_PORT));
      auto video = std::to_string(net::map_port(stream::VIDEO_STREAM_PORT));
      auto audio = std::to_string(net::map_port(stream::AUDIO_STREAM_PORT));
      auto control = std::to_string(net::map_port(stream::CONTROL_PORT));
      auto gs_http = std::to_string(net::map_port(nvhttp::PORT_HTTP));
      auto gs_https = std::to_string(net::map_port(nvhttp::PORT_HTTPS));
      auto wm_http = std::to_string(net::map_port(confighttp::PORT_HTTPS));

      mappings.assign({
        {{rtsp, rtsp, "TCP"s}, "Sunshine - RTSP"s},
        {{video, video, "UDP"s}, "Sunshine - Video"s},
        {{audio, audio, "UDP"s}, "Sunshine - Audio"s},
        {{control, control, "UDP"s}, "Sunshine - Control"s},
        {{gs_http, gs_http, "TCP"s}, "Sunshine - Client HTTP"s},
        {{gs_https, gs_https, "TCP"s}, "Sunshine - Client HTTPS"s},
      });

      // Only map port for the Web Manager if it is configured to accept connection from WAN
      if (net::from_enum_string(config::nvhttp.origin_web_ui_allowed) > net::LAN) {
        mappings.emplace_back(mapping_t {{wm_http, wm_http, "TCP"s}, "Sunshine - Web UI"s});
      }

      // Start the mapping thread
      upnp_thread = std::thread {&deinit_t::upnp_thread_proc, this};
    }

    ~deinit_t() {
      upnp_thread.join();
    }

    /**
     * @brief Opens pinholes for IPv6 traffic if the IGD is capable.
     * @details Not many IGDs support this feature, so we perform error logging with debug level.
     * @return `true` if the pinholes were opened successfully.
     */
    bool create_ipv6_pinholes() {
      int err;
      device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv6, 2, &err)};
      if (!device || err) {
        BOOST_LOG(debug) << "Couldn't discover any IPv6 UPNP devices"sv;
        return false;
      }

      IGDdatas data;
      urls_t urls;
      std::array<char, INET6_ADDRESS_STRLEN> lan_addr;
      auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
      if (status != 1 && status != 2) {
        BOOST_LOG(debug) << "No valid IPv6 IGD: "sv << status_string(status);
        return false;
      }

      if (data.IPv6FC.controlurl[0] != 0) {
        int firewallEnabled;
        int pinholeAllowed;

        // Check if this firewall supports IPv6 pinholes
        err = UPNP_GetFirewallStatus(urls->controlURL_6FC, data.IPv6FC.servicetype, &firewallEnabled, &pinholeAllowed);
        if (err == UPNPCOMMAND_SUCCESS) {
          BOOST_LOG(debug) << "UPnP IPv6 firewall control available. Firewall is "sv
                           << (firewallEnabled ? "enabled"sv : "disabled"sv)
                           << ", pinhole is "sv
                           << (pinholeAllowed ? "allowed"sv : "disallowed"sv);

          if (pinholeAllowed) {
            // Create pinholes for each port
            auto mapping_period = std::to_string(PORT_MAPPING_LIFETIME.count());
            auto shutdown_event = mail::man->event<bool>(mail::shutdown);

            for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
              auto mapping = *it;
              char uniqueId[8];

              // Open a pinhole for the LAN port, since there will be no WAN->LAN port mapping on IPv6
              err = UPNP_AddPinhole(urls->controlURL_6FC, data.IPv6FC.servicetype, "", "0", lan_addr.data(), mapping.port.lan.c_str(), mapping.port.proto.c_str(), mapping_period.c_str(), uniqueId);
              if (err == UPNPCOMMAND_SUCCESS) {
                BOOST_LOG(debug) << "Successfully created pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan;
              } else {
                BOOST_LOG(debug) << "Failed to create pinhole for "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
              }
            }

            return err == 0;
          } else {
            BOOST_LOG(debug) << "IPv6 pinholes are not allowed by the IGD"sv;
            return false;
          }
        } else {
          BOOST_LOG(debug) << "Failed to get IPv6 firewall status: "sv << err;
          return false;
        }
      } else {
        BOOST_LOG(debug) << "IPv6 Firewall Control is not supported by the IGD"sv;
        return false;
      }
    }

    /**
     * @brief Maps a port via UPnP.
     * @param data IGDdatas from UPNP_GetValidIGD()
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param lan_addr Local IP address to map to
     * @param mapping Information about port to map
     * @return `true` on success.
     */
    bool map_upnp_port(const IGDdatas &data, const urls_t &urls, const std::string &lan_addr, const mapping_t &mapping) {
      char intClient[16];
      char intPort[6];
      char desc[80];
      char enabled[4];
      char leaseDuration[16];
      bool indefinite = false;

      // First check if this port is already mapped successfully
      BOOST_LOG(debug) << "Checking for existing UPnP port mapping for "sv << mapping.port.wan;
      auto err = UPNP_GetSpecificPortMappingEntry(
        urls->controlURL,
        data.first.servicetype,
        // In params
        mapping.port.wan.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        // Out params
        intClient,
        intPort,
        desc,
        enabled,
        leaseDuration
      );
      if (err == 714) {  // NoSuchEntryInArray
        BOOST_LOG(debug) << "Mapping entry not found for "sv << mapping.port.wan;
      } else if (err == UPNPCOMMAND_SUCCESS) {
        // Some routers change the description, so we can't check that here
        if (!std::strcmp(intClient, lan_addr.c_str())) {
          if (std::atoi(leaseDuration) == 0) {
            BOOST_LOG(debug) << "Static mapping entry found for "sv << mapping.port.wan;

            // It's a static mapping, so we're done here
            return true;
          } else {
            BOOST_LOG(debug) << "Mapping entry found for "sv << mapping.port.wan << " ("sv << leaseDuration << " seconds remaining)"sv;
          }
        } else {
          BOOST_LOG(warning) << "UPnP conflict detected with: "sv << intClient;

          // Some UPnP IGDs won't let unauthenticated clients delete other conflicting port mappings
          // for security reasons, but we will give it a try anyway.
          err = UPNP_DeletePortMapping(
            urls->controlURL,
            data.first.servicetype,
            mapping.port.wan.c_str(),
            mapping.port.proto.c_str(),
            nullptr
          );
          if (err) {
            BOOST_LOG(error) << "Unable to delete conflicting UPnP port mapping: "sv << err;
            return false;
          }
        }
      } else {
        BOOST_LOG(error) << "UPNP_GetSpecificPortMappingEntry() failed: "sv << err;

        // If we get a strange error from the router, we'll assume it's some old broken IGDv1
        // device and only use indefinite lease durations to hopefully avoid confusing it.
        if (err != 606) {  // Unauthorized
          indefinite = true;
        }
      }

      // Add/update the port mapping
      auto mapping_period = std::to_string(indefinite ? 0 : PORT_MAPPING_LIFETIME.count());
      err = UPNP_AddPortMapping(
        urls->controlURL,
        data.first.servicetype,
        mapping.port.wan.c_str(),
        mapping.port.lan.c_str(),
        lan_addr.data(),
        mapping.description.c_str(),
        mapping.port.proto.c_str(),
        nullptr,
        mapping_period.c_str()
      );

      if (err != UPNPCOMMAND_SUCCESS && !indefinite) {
        // This may be an old/broken IGD that doesn't like non-static mappings.
        BOOST_LOG(debug) << "Trying static mapping after failure: "sv << err;
        err = UPNP_AddPortMapping(
          urls->controlURL,
          data.first.servicetype,
          mapping.port.wan.c_str(),
          mapping.port.lan.c_str(),
          lan_addr.data(),
          mapping.description.c_str(),
          mapping.port.proto.c_str(),
          nullptr,
          "0"
        );
      }

      if (err) {
        BOOST_LOG(error) << "Failed to map "sv << mapping.port.proto << ' ' << mapping.port.lan << ": "sv << err;
        return false;
      }

      BOOST_LOG(debug) << "Successfully mapped "sv << mapping.port.proto << ' ' << mapping.port.lan;
      return true;
    }

    /**
     * @brief Unmaps all ports.
     * @param urls urls_t from UPNP_GetValidIGD()
     * @param data IGDdatas from UPNP_GetValidIGD()
     */
    void unmap_all_upnp_ports(const urls_t &urls, const IGDdatas &data) {
      for (auto it = std::begin(mappings); it != std::end(mappings); ++it) {
        auto status = UPNP_DeletePortMapping(
          urls->controlURL,
          data.first.servicetype,
          it->port.wan.c_str(),
          it->port.proto.c_str(),
          nullptr
        );

        if (status && status != 714) {  // NoSuchEntryInArray
          BOOST_LOG(warning) << "Failed to unmap "sv << it->port.proto << ' ' << it->port.lan << ": "sv << status;
        } else {
          BOOST_LOG(debug) << "Successfully unmapped "sv << it->port.proto << ' ' << it->port.lan;
        }
      }
    }

    /**
     * @brief Maintains UPnP port forwarding rules
     */
    void upnp_thread_proc() {
      platf::set_thread_name("upnp");
      auto shutdown_event = mail::man->event<bool>(mail::shutdown);
      bool mapped = false;
      IGDdatas data;
      urls_t mapped_urls;
      auto address_family = net::af_from_enum_string(config::sunshine.address_family);

      // Refresh UPnP rules every few minutes. They can be lost if the router reboots,
      // WAN IP address changes, or various other conditions.
      do {
        int err = 0;
        device_t device {upnpDiscover(2000, nullptr, nullptr, 0, IPv4, 2, &err)};   
        if (!device || err) {
          BOOST_LOG(warning) << "Couldn't discover any IPv4 UPNP devices"sv;

          {
            std::lock_guard lock(diagnostics_mutex);
            diagnostics.igd_found = false;
            diagnostics.igd_connected = false;
            diagnostics.lan_address.clear();
            diagnostics.external_address.clear();
            diagnostics.igd_url.clear();
            diagnostics.mappings = default_mapping_statuses();
            diagnostics.internet_access = internet_access_e::not_tested;
            diagnostics.last_updated = std::chrono::system_clock::now();
          }

          refresh_tray_diagnostics();

          mapped = false;
          continue;
        }

        for (auto dev = device.get(); dev != nullptr; dev = dev->pNext) {
          BOOST_LOG(debug) << "Found device: "sv << dev->descURL;
        }

        std::array<char, INET6_ADDRESS_STRLEN> lan_addr;

        urls_t urls;
        auto status = upnp::UPNP_GetValidIGDStatus(device, &urls, &data, lan_addr);
        if (status != 1 && status != 2) {
          BOOST_LOG(error) << status_string(status);

          {
            std::lock_guard lock(diagnostics_mutex);
            diagnostics.igd_found = status != 0;
            diagnostics.igd_connected = false;
            diagnostics.lan_address.clear();
            diagnostics.external_address.clear();
            diagnostics.igd_url.clear();
            diagnostics.mappings = default_mapping_statuses();
            diagnostics.internet_access = internet_access_e::not_tested;
            diagnostics.last_updated = std::chrono::system_clock::now();
          }

          refresh_tray_diagnostics();

          mapped = false;
          continue;
        }

        std::string lan_addr_str {lan_addr.data()};

        std::array<char, 64> external_addr {};
        const auto external_addr_status = UPNP_GetExternalIPAddress(
          urls->controlURL,
          data.first.servicetype,
          external_addr.data()
        );

        std::string external_addr_str;
        if (external_addr_status == UPNPCOMMAND_SUCCESS && external_addr[0] != '\0') {
          external_addr_str = external_addr.data();
          BOOST_LOG(debug) << "Router external IPv4 address: "sv << external_addr_str;
        } else {
          BOOST_LOG(debug) << "Unable to query router external IPv4 address: "sv << external_addr_status;
        }

        std::vector<mapping_status_t> mapping_results;
        mapping_results.reserve(mappings.size());

        BOOST_LOG(debug) << "Found valid IGD device: "sv << urls->rootdescURL;

        for (auto it = std::begin(mappings); it != std::end(mappings) && !shutdown_event->peek(); ++it) {
          const bool mapping_succeeded = map_upnp_port(data, urls, lan_addr_str, *it);

          mapping_results.push_back({
            .protocol = it->port.proto,
            .lan_port = it->port.lan,
            .wan_port = it->port.wan,
            .description = it->description,
            .mapping_known = true,
            .mapped = mapping_succeeded,
          });
        }

        {
          std::lock_guard lock(diagnostics_mutex);

          diagnostics.igd_found = true;
          diagnostics.igd_connected = status == 1;
          diagnostics.lan_address = lan_addr_str;
          diagnostics.external_address = external_addr_str;
          diagnostics.igd_url = urls->rootdescURL ? urls->rootdescURL : "";
          diagnostics.mappings = std::move(mapping_results);
          diagnostics.last_updated = std::chrono::system_clock::now();
        }

        refresh_tray_diagnostics();

        // Sunshine's HTTP server starts immediately after the UPnP worker during startup.
        // Give it a brief moment on the first pass so the loopback test doesn't race startup.
        if (!mapped) {
          if (shutdown_event->view(2s)) {
            break;
          }
        }

        const auto internet_test = test_internet_access();

        {
          std::lock_guard lock(diagnostics_mutex);
          diagnostics.internet_access = internet_test.overall;
          apply_internet_test_result(diagnostics.mappings, internet_test);
          diagnostics.last_updated = std::chrono::system_clock::now();
        }

        refresh_tray_diagnostics();

        if (!mapped) {
          BOOST_LOG(info) << "Completed UPnP port mappings to "sv << lan_addr_str << " via "sv << urls->rootdescURL;
        }

        // If we are listening on IPv6 and the IGD has an IPv6 firewall enabled, try to create IPv6 firewall pinholes
        if (address_family == net::af_e::BOTH) {
          if (create_ipv6_pinholes() && !mapped) {
            // Only log the first time through
            BOOST_LOG(info) << "Successfully opened IPv6 pinholes on the IGD"sv;
          }
        }

        mapped = true;
        mapped_urls = std::move(urls);
      } while (!shutdown_event->view(REFRESH_INTERVAL));

      if (mapped) {
        // Unmap ports upon termination
        BOOST_LOG(info) << "Unmapping UPNP ports..."sv;
        unmap_all_upnp_ports(mapped_urls, data);
      }
    }

    std::vector<mapping_t> mappings;
    std::thread upnp_thread;
  };

  std::unique_ptr<platf::deinit_t> start() {
    {
      std::lock_guard lock(diagnostics_mutex);
      diagnostics = {};
      diagnostics.enabled = config::sunshine.flags[config::flag::UPNP];
      diagnostics.mappings = default_mapping_statuses();
      diagnostics.last_updated = std::chrono::system_clock::now();
    }

    refresh_tray_diagnostics();

    if (!config::sunshine.flags[config::flag::UPNP]) {
      return nullptr;
    }

    return std::make_unique<deinit_t>();
  }
}  // namespace upnp
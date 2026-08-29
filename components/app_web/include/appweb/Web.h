/*
   HTTP server: REST API, WebSocket bus monitor, embedded UI.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/Frame.h"

#include <cstdint>
#include <functional>
#include <string>

namespace appweb {

/* Everything the status endpoint reports, supplied by the application so this
   component stays free of bus and network internals. */
struct Status {
  uint32_t frames_ok = 0;
  uint32_t crc_errors = 0;
  uint32_t resyncs = 0;
  uint32_t acks_seen = 0;
  uint32_t naks_seen = 0;
  uint32_t dropped = 0;
  uint32_t bytes = 0;
  uint32_t baud = 0;
  std::string net_state;
  std::string ip;
  std::string ssid;
  int8_t rssi = 0;
};

/* The appliance layer is application-level, so it is handed in rather than
   depended on -- this component stays about HTTP. */
struct ApplianceApi {
  std::function<bool()> has_profile;
  std::function<std::string()> profile_json;    /* id, model, appliance, online */
  std::function<std::string()> readings_json;
  std::function<std::string()> layout_json;
  std::function<std::string()> active_source;
  std::function<std::string(const std::string &)> source_of;
  /* The user's own profiles, already serialised -- this component has no
     business knowing what a profile is made of. */
  std::function<std::string()> stored_json;
  std::function<std::string()> storage_json;
  std::function<bool(const std::string &, const std::string &, bool, std::string *)> save_custom;
  std::function<bool(const std::string &, std::string *)> erase_custom;
  std::function<std::string()> candidates_json;
  std::function<std::string()> catalogue_json;
  std::function<bool(const std::string &id, std::string *error)> activate;
  std::function<void()> clear;
};

/* Health and post-mortem, supplied by the application for the same reason as
   the appliance API: this component stays about HTTP. */
struct DiagnosticsApi {
  std::function<std::string()> health_json;
  std::function<size_t()> coredump_size;
  std::function<bool(size_t offset, void *buf, size_t len)> read_coredump;
  std::function<bool()> erase_coredump;
};

using StatusProvider = std::function<Status()>;

/* Called with true when a firmware upload starts and false if it fails, so the
   application can signal it however it likes without this component knowing
   about LEDs. */
using OtaHook = std::function<void(bool active)>;

bool begin(StatusProvider provider, OtaHook ota_hook = {}, ApplianceApi appliance = {},
           DiagnosticsApi diagnostics = {});

/* Queue a frame for any connected monitor. Never blocks: if a client cannot
   keep up its oldest messages are dropped and the drop is reported to it, so a
   busy bus can never stall the bus task. */
void publish_frame(const dbus2::Frame &frame, int64_t rx_time_us);

/* The byte a frame was answered with, carrying that frame's receive time so the
   monitor can put the two together. Sent as its own message rather than folded
   into the frame, because the frame has already gone out by then. */
void publish_ack(uint8_t ack, int64_t frame_rx_time_us);

/* True once the server has answered anything at all. The firmware update's
   trial period uses it as proof of life: a device serving its setup page is
   reachable, even though it has no station connection to show for it. */
bool has_served();

}  // namespace appweb

/*
   bsh-dbus-adapter -- standalone ESP-IDF firmware for the B/S/H/ D-Bus.

   P1: receive path and bus monitoring. The firmware is strictly read-only --
   it never transmits.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
*/

#include "board.h"
#include "indicator.h"
#include "appliance.h"
#include "appstore/Store.h"
#include "appmqtt/Mqtt.h"
#include "diagnostics.h"
#include "recovery.h"

#include "appcfg/Config.h"
#include "appnet/Net.h"
#include "appweb/Web.h"
#include "dbus2/Crc.h"
#include "dbus2/Manager.h"

#include <esp_app_desc.h>

#include <vector>
#include <esp_ota_ops.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>

static const char *const TAG = "bsh";

static dbus2::Manager g_bus;

static void log_banner() {
  const esp_app_desc_t *app = esp_app_get_description();

  esp_chip_info_t chip;
  esp_chip_info(&chip);

  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);

  ESP_LOGI(TAG, "bsh-dbus-adapter %s (%s %s)", app->version, app->date, app->time);
  ESP_LOGI(TAG, "ESP-IDF %s, %d core(s), rev v%d.%d, %" PRIu32 " MB flash", IDF_VER, chip.cores, chip.revision / 100,
           chip.revision % 100, flash_size / (1024 * 1024));
  ESP_LOGI(TAG, "read-only observer -- this firmware never transmits");
}

/* One line per frame, in the notation the upstream project uses in its docs:
   ll | dd.cc-cc | payload   so captures can be compared against its logs.

   Lowercase hex throughout, length included -- upstream prints the length like
   every other byte, so a 16-byte frame reads 10 there. This used to print it in
   decimal, which agreed with upstream only while frames stayed shorter than ten
   bytes and silently disagreed after that. */
static void log_frame(const dbus2::Frame &f, int64_t) {
  /* At DEBUG, which the shipped log level compiles out entirely: behind an
     appliance there is no USB host to read this, and the monitor in the web
     interface is where frames are actually watched. It stays for the build
     somebody makes with CONFIG_LOG_MAXIMUM_LEVEL raised, where the runtime
     check below keeps it from formatting a line nobody asked for. */
  if (esp_log_level_get(TAG) < ESP_LOG_DEBUG)
    return;

  char hex[3 * 64 + 4];
  size_t pos = 0;
  auto payload = f.payload();
  const size_t shown = std::min<size_t>(payload.size(), 64);
  for (size_t i = 0; i < shown; i++)
    pos += snprintf(hex + pos, sizeof(hex) - pos, i ? " %02x" : "%02x", payload[i]);
  if (shown < payload.size())
    snprintf(hex + pos, sizeof(hex) - pos, " ...");

  if (f.has_command())
    ESP_LOGD(TAG, "%02x | %02x.%02x-%02x | %s", f.length, f.dest, f.data[0], f.data[1], hex);
  else
    ESP_LOGD(TAG, "%02x | %02x       | (short frame, %u bytes)", f.length, f.dest,
             static_cast<unsigned>(f.data.size()));
}

static appnet::State g_net = appnet::State::Idle;
static bool g_seen_frames = false;

/* The green LED has to answer two questions at once -- am I reachable, and am I
   alive -- so network state and bus state are folded into one pattern here.
   Nominal (online plus frames flowing) is the only fully calm signal. */
static void refresh_system_led() {
  using S = indicator::System;
  switch (g_net) {
    case appnet::State::ApMode:
      indicator::set_system(S::ApMode);
      break;
    case appnet::State::Connected:
      indicator::set_system(g_seen_frames ? S::Nominal : S::Connected);
      break;
    case appnet::State::Connecting:
    case appnet::State::Idle:
      indicator::set_system(S::Disconnected);
      break;
  }
}

/* Classifies the bus from the change in counters over a window, so the blue LED
   can tell "nothing arrives" apart from "bytes arrive but are garbage" -- the
   usual symptom of a wrong baud rate or miswiring. */
static void update_bus_health() {
  static dbus2::Stats prev;
  const dbus2::Stats now = g_bus.stats();

  const uint32_t d_frames = now.frames_ok - prev.frames_ok;
  const uint32_t d_errors = (now.crc_errors - prev.crc_errors) + (now.resyncs - prev.resyncs);
  prev = now;

  if (d_frames > 0)
    indicator::set_bus(indicator::Bus::Healthy);
  else if (d_errors > 0)
    indicator::set_bus(indicator::Bus::Garbage);
  else
    indicator::set_bus(indicator::Bus::Silent);
}

appmqtt::Reading to_mqtt(const appliance::Reading &r) {
  appmqtt::Reading m;
  m.id = r.id;
  m.name = r.name;
  m.kind = r.kind;
  m.value = r.value;
  m.unit = r.unit;
  m.device_class = r.device_class;
  m.state_class = r.state_class;
  m.entity_category = r.entity_category;
  m.icon = r.icon;
  m.available = r.available;
  return m;
}

/* The set of readings the broker should know about, refreshed whenever the
   profile changes so a topic never outlives the entity behind it. */
void announce_entities() {
  std::vector<appmqtt::Reading> list;
  for (const auto &r : appliance::readings())
    list.push_back(to_mqtt(r));

  appmqtt::Description d;
  d.device_id = appcfg::default_device_name();
  d.device_name = appcfg::device().name;
  /* The appliance, not the adapter: what Home Assistant shows is the machine
     being watched, and the profile is the only thing that knows which one it
     is. */
  d.model = appliance::profile_model();
  d.manufacturer = "B/S/H/";

  const esp_app_desc_t *app = esp_app_get_description();
  d.firmware = app ? app->version : "";

  appmqtt::announce(d, list.data(), list.size());
}

extern "C" void app_main() {
  log_banner();

  diagnostics::begin();
  indicator::begin();
  indicator::set_system(indicator::System::Booting);
  /* Drive it once right away: the baud scan below blocks for several seconds
     before the main loop starts ticking, and a dark board looks like a dead
     one. Booting is a solid light, so a single tick is enough. */
  indicator::tick(esp_timer_get_time());

  recovery::begin();

  if (!appcfg::begin())
    ESP_LOGE(TAG, "Config storage unavailable -- settings will not persist");

  /* Before appliance::begin(), which may want to load a profile from here. */
  appstore::begin();

  appmqtt::begin();

  /* Publishing needs somewhere to publish to, so the client follows the station
     rather than starting hopefully at boot. */
  appnet::begin([](appnet::State s) {
    g_net = s;
    refresh_system_led();
    appmqtt::on_network(s == appnet::State::Connected);
  });

  /* Every change the decoder makes goes straight out. */
  appliance::set_sink([](const appliance::Reading &r) {
    appmqtt::publish(to_mqtt(r));
  });

  /* A different profile means a different set of entities, so whatever was
     announced before has to be replaced -- and the ones that went away have to
     be withdrawn, or they haunt the dashboard as permanently unavailable. */
  appliance::set_profile_hook(announce_entities);

  dbus2::Manager::Config cfg;
  cfg.rx_pin = board::PIN_RX;
  cfg.tx_pin = board::PIN_TX;
  cfg.baud = board::DEFAULT_BAUD;  // 0 = probe the candidate rates

  if (!g_bus.begin(cfg)) {
    ESP_LOGE(TAG, "Bus init failed -- halting");
    indicator::set_system(indicator::System::Fatal);
    for (;;) {
      indicator::tick(esp_timer_get_time());
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }

  appliance::begin();
  announce_entities();
  g_bus.add_observer(appliance::on_frame);
  g_bus.add_observer(log_frame);
  g_bus.add_observer([](const dbus2::Frame &, int64_t) { indicator::note_frame(); });

  appweb::begin(
      []() {
    dbus2::Stats s = g_bus.stats();
    appweb::Status st;
    st.frames_ok = s.frames_ok;
    st.crc_errors = s.crc_errors;
    st.resyncs = s.resyncs;
    st.acks_seen = s.acks_seen;
    st.naks_seen = s.naks_seen;
    st.dropped = s.dropped;
    st.bytes = s.bytes;
    st.baud = g_bus.baud();
    st.net_state = appnet::to_string(g_net);
    st.ip = appnet::ip();
    st.ssid = appnet::ssid();
    st.rssi = appnet::rssi();
        return st;
      },
      [](bool active) { indicator::set_system(active ? indicator::System::Ota : indicator::System::Booting); },
      appweb::ApplianceApi{
          .has_profile = appliance::has_profile,
          .profile_json = appliance::profile_json,
          .readings_json = appliance::readings_json,
          .layout_json = appliance::layout_json,
          .active_source = appliance::active_source,
          .source_of = appliance::source_of,
          .stored_json = appliance::stored_json,
          .storage_json = appliance::storage_json,
          .save_custom = appliance::save_custom,
          .erase_custom = appliance::erase_custom,
          .candidates_json = appliance::candidates_json,
          .catalogue_json = appliance::catalogue_json,
          .activate = appliance::activate,
          .clear = appliance::clear_profile,
      },
      appweb::DiagnosticsApi{
          .health_json = diagnostics::health_json,
          .coredump_size = diagnostics::coredump_size,
          .read_coredump = diagnostics::read_coredump,
          .erase_coredump = diagnostics::erase_coredump,
      });
  g_bus.add_observer(appweb::publish_frame);
  /* Straight past the decoder: whether a profile explains a frame has nothing to
     do with whether somebody wants to record it. Costs nothing while the raw
     setting is off, which is how it ships.

     Rebuilt as it was on the wire -- LL DS CC CC payload RR RR. The checksum is
     recomputed rather than kept, which changes nothing: the parser only hands
     over frames whose checksum it has already verified, so the two are the same
     bytes. */
  g_bus.add_observer([](const dbus2::Frame &f, int64_t) {
    std::string wire;
    wire.reserve(f.data.size() + 4);
    wire.push_back(static_cast<char>(f.length));
    wire.push_back(static_cast<char>(f.dest));
    wire.append(reinterpret_cast<const char *>(f.data.data()), f.data.size());
    const uint16_t crc = dbus2::crc16(reinterpret_cast<const uint8_t *>(wire.data()), wire.size());
    wire.push_back(static_cast<char>(crc >> 8));
    wire.push_back(static_cast<char>(crc & 0xFF));

    std::string hex;
    hex.reserve(wire.size() * 2);
    char b[3];
    for (unsigned char c : wire) {
      snprintf(b, sizeof(b), "%02x", c);
      hex += b;
    }
    appmqtt::publish_raw_frame(hex);
  });
  g_bus.add_ack_observer(appweb::publish_ack);

  /* A freshly flashed image boots as PENDING_VERIFY. If it never marks itself
     valid -- because it panics, hangs, or cannot get on the network -- the
     bootloader falls back to the previous slot on the next restart. Confirming
     only after the network is actually up is the point: a firmware that builds
     and boots but cannot be reached is exactly as useless as one that crashes,
     and once the board is behind an appliance there is no cable to fix it. */
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &ota_state);
  bool awaiting_verify = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
  const int64_t verify_deadline = esp_timer_get_time() + 60000000;
  if (awaiting_verify)
    ESP_LOGW(TAG, "Running a new image on trial; confirming once the network is up");

  refresh_system_led();

  int64_t next_health = 0;
  int64_t next_stats = 0;

  for (;;) {
    const int64_t now = esp_timer_get_time();

    diagnostics::feed();
    g_bus.process();
    appliance::tick(now);
    recovery::tick(now);
    indicator::tick(now);

    /* Either counts as proof that this image works. Joining a network is the
       usual one; being asked for a page is the one that matters when the
       device came up in setup mode, where there is no station to join and a
       person is standing in front of it typing credentials. That case used to
       expire: sixty seconds into the trial the device rolled back and
       restarted, taking the setup network away mid-sentence and discarding the
       firmware that had just been installed. */
    if (awaiting_verify && (g_net == appnet::State::Connected || appweb::has_served())) {
      awaiting_verify = false;
      esp_ota_mark_app_valid_cancel_rollback();
      ESP_LOGW(TAG, "New image confirmed, rollback cancelled");
    }
    if (awaiting_verify && now > verify_deadline) {
      ESP_LOGE(TAG, "New image was never reached at all -- rebooting to roll back");
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }

    if (now > next_health) {
      next_health = now + 2000000;
      update_bus_health();
      /* Publishes only on a change, so this costs nothing while nothing
         happens -- which, on an appliance in standby, is most of the time. */
      appmqtt::set_appliance_online(appliance::online());

      if (!g_seen_frames && g_bus.stats().frames_ok > 0) {
        g_seen_frames = true;
        refresh_system_led();
      }
    }

    if (now > next_stats) {
      next_stats = now + 30000000;
      dbus2::Stats s = g_bus.stats();
      ESP_LOGI(TAG,
               "stats: %" PRIu32 " frames, %" PRIu32 " crc err, %" PRIu32 " resync, %" PRIu32 " acks, %" PRIu32
               " dropped, %" PRIu32 " bytes @ %" PRIu32 " baud",
               s.frames_ok, s.crc_errors, s.resyncs, s.acks_seen, s.dropped, s.bytes, g_bus.baud());
      ESP_LOGI(TAG, "net: %s%s%s", appnet::to_string(g_net), appnet::ip().empty() ? "" : " ",
               appnet::ip().c_str());
      ESP_LOGI(TAG, "health: heap %" PRIu32 " (min %" PRIu32 "), boot: %s",
               esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
               diagnostics::boot_reason());
    }

    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

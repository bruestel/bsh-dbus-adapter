/*
   HTTP server: REST API, WebSocket bus monitor, embedded UI.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appweb/Web.h"
#include "appcfg/Config.h"
#include "appnet/Net.h"
#include "appmqtt/Mqtt.h"

#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <psa/crypto.h>

#include <cJSON.h>

#include <lwip/sockets.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* Generated from www/index.html at configure time, see CMakeLists.txt. */
extern "C" {
extern const unsigned char ui_index_html_gz[];
extern const unsigned int ui_index_html_gz_len;
}

namespace appweb {
namespace {

const char *const TAG = "appweb";

httpd_handle_t g_server = nullptr;
StatusProvider g_status;
OtaHook g_ota_hook;
ApplianceApi g_appliance;
DiagnosticsApi g_diag;

/* Lets the application drive the LEDs during an update without this component
   knowing anything about them. */
void indicator_hook(bool active) {
  if (g_ota_hook)
    g_ota_hook(active);
}

/* Enough to read back the server's socket table; must match max_open_sockets
   below. Monitors are no longer counted separately -- see the websocket
   section for why the local client table is gone. */
constexpr size_t kMaxSockets = 7;
constexpr int kQueueLen = 64;

/* One serialised frame line, ready to send. Fixed size so a burst cannot
   fragment the heap. Sized for the longest line it can hold: the JSON envelope
   (now including a sequence number) plus a millisecond timestamp plus kHexBytes
   rendered as hex. */
constexpr size_t kHexBytes = 24;
constexpr size_t kMsgText = 96 + kHexBytes * 2;
struct Msg {
  char text[kMsgText];
};

QueueHandle_t g_queue = nullptr;
/* Incremented by the application task when a message will not fit the queue and
   read-and-cleared by the pump task that reports it. A lost increment costs a
   drop notice, not correctness -- but an atomic costs nothing here. */
std::atomic<uint32_t> g_dropped{0};

/* Set as soon as any request reaches the server. A new firmware image is on
   trial until something proves it works, and being asked for a page proves it
   as well as joining a network does -- better, in the case that matters, which
   is a device serving its own setup network with somebody standing in front of
   it. */
std::atomic<bool> g_served{false};

/* Monotonic id stamped onto every frame/ack message as "s". Lets a monitor say
   what it last saw so a reconnect can be filled from the ring below instead of
   resuming blind. Only ever incremented, from the app task in publish_*. */
uint32_t g_seq = 0;

/* A ring of the most recent serialised messages, kept so a monitor that
   connects or reconnects can be handed what it missed before the live stream
   resumes. The WebSocket stays the only delivery path; this is just its memory.
   Written from the app task (publish_*) and read from the httpd task (replay in
   ws_handler), so every touch is under g_ring_mux. */
constexpr size_t kRingCap = 512;
struct RingItem {
  uint32_t seq;
  char text[kMsgText];
};
/* Allocated the first time somebody asks to see the traffic, not at startup.
   At 512 entries this is 74 KiB, and it was the largest thing this firmware
   owned -- reserved for good on a device that spends its life behind an
   appliance with nobody watching the monitor. The C3 has less to spare than
   the C6, and neither has any to waste on a window that is usually closed. */
RingItem *g_ring = nullptr;
size_t g_ring_head = 0;   // where the next append goes
size_t g_ring_count = 0;  // valid entries, <= kRingCap
SemaphoreHandle_t g_ring_mux = nullptr;

/* Append one already-serialised message to the ring. Cheap: a copy under the
   mutex, no allocation. */
/* Called when a monitor appears, from the HTTP task. Failure is not fatal: the
   monitor then shows what arrives from now on, without the replay of what it
   missed, which is a smaller loss than refusing to open. */
bool ring_reserve() {
  if (!g_ring_mux)
    return false;
  xSemaphoreTake(g_ring_mux, portMAX_DELAY);
  if (!g_ring) {
    g_ring = static_cast<RingItem *>(calloc(kRingCap, sizeof(RingItem)));
    if (g_ring)
      ESP_LOGI(TAG, "Monitor ring: %u bytes",
               static_cast<unsigned>(kRingCap * sizeof(RingItem)));
    else
      ESP_LOGW(TAG, "No room for the monitor ring; replay unavailable");
  }
  const bool ok = g_ring != nullptr;
  xSemaphoreGive(g_ring_mux);
  return ok;
}

void ring_append(uint32_t seq, const char *text) {
  if (!g_ring_mux)
    return;
  xSemaphoreTake(g_ring_mux, portMAX_DELAY);
  if (!g_ring) {
    /* Nobody has ever opened the monitor, so there is nothing to replay to. */
    xSemaphoreGive(g_ring_mux);
    return;
  }
  RingItem &slot = g_ring[g_ring_head];
  slot.seq = seq;
  std::snprintf(slot.text, sizeof(slot.text), "%s", text);
  g_ring_head = (g_ring_head + 1) % kRingCap;
  if (g_ring_count < kRingCap)
    g_ring_count++;
  xSemaphoreGive(g_ring_mux);
}

// ---------------------------------------------------------------- auth ----

/* Protection applies everywhere, including the fallback access point.

   It was exempt there at first, reasoning that a password would lock the user
   out of a device they cannot reach any other way. That reasoning does not
   survive scrutiny: jamming the WiFi is enough to force the device into
   access-point mode, and an open setup network then hands over every setting to
   whoever is nearby.

   The lockout it was meant to prevent is covered properly now -- holding the
   BOOT button for five seconds erases the WiFi credentials *and* the password,
   which needs physical access and nothing else. */
bool auth_required() { return appcfg::auth().enabled; }

/* Verifying a password costs 20 000 rounds of PBKDF2, which is the right price
   for a login and the wrong one for a request. Basic authentication sends the
   header on every single request, and the interface polls several times a
   minute: without this the device would spend most of its httpd task deriving
   the same key over and over, and anyone could make it do that by sending
   requests.

   So a header that has already been verified is remembered by its hash for a
   few minutes. Only credentials that passed can ever be in here, so the cache
   cannot admit anyone; it only postpones noticing a password change, which is
   why changing one clears it. */
constexpr int64_t kAuthCacheUs = 300 * 1000000LL;
uint8_t g_auth_hash[32] = {};
bool g_auth_valid = false;
int64_t g_auth_until = 0;

/* A wrong password is cheap for the sender and expensive for us, so after a
   handful of failures the key derivation is skipped entirely for a while. The
   window is short enough to be invisible to somebody mistyping a password and
   long enough to make guessing pointless. */
constexpr int kMaxAuthFailures = 5;
constexpr int64_t kAuthBackoffUs = 10 * 1000000LL;
int g_auth_failures = 0;
int64_t g_auth_blocked_until = 0;

/* Same hash primitive the password derivation uses, through the same API, so
   this file does not pull in a second crypto interface. A failure leaves the
   digest zeroed, which simply misses the cache and costs a derivation. */
void hash_header(const std::string &header, uint8_t out[32]) {
  std::memset(out, 0, 32);
  size_t written = 0;
  psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(header.data()),
                   header.size(), out, 32, &written);
}

void forget_auth_cache() {
  g_auth_valid = false;
  g_auth_failures = 0;
  g_auth_blocked_until = 0;
}

bool check_auth(httpd_req_t *req) {
  if (!auth_required())
    return true;

  size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
  if (len == 0 || len > 256)
    return false;

  std::string header(len + 1, '\0');
  if (httpd_req_get_hdr_value_str(req, "Authorization", header.data(), len + 1) != ESP_OK)
    return false;
  header.resize(len);

  const int64_t now = esp_timer_get_time();
  uint8_t digest[32];
  hash_header(header, digest);
  if (g_auth_valid && now < g_auth_until && std::memcmp(digest, g_auth_hash, sizeof(digest)) == 0)
    return true;

  if (g_auth_failures >= kMaxAuthFailures && now < g_auth_blocked_until)
    return false;

  const std::string prefix = "Basic ";
  if (header.rfind(prefix, 0) != 0)
    return false;

  const std::string b64 = header.substr(prefix.size());
  unsigned char decoded[256];
  size_t decoded_len = 0;
  if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                            reinterpret_cast<const unsigned char *>(b64.data()), b64.size()) != 0)
    return false;
  decoded[decoded_len] = '\0';

  std::string creds(reinterpret_cast<char *>(decoded), decoded_len);
  const size_t colon = creds.find(':');
  if (colon == std::string::npos)
    return false;

  if (!appcfg::verify_password(creds.substr(0, colon), creds.substr(colon + 1))) {
    if (++g_auth_failures >= kMaxAuthFailures)
      g_auth_blocked_until = now + kAuthBackoffUs;
    return false;
  }

  std::memcpy(g_auth_hash, digest, sizeof(g_auth_hash));
  g_auth_valid = true;
  g_auth_until = now + kAuthCacheUs;
  g_auth_failures = 0;
  return true;
}

esp_err_t deny(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"bsh-dbus\"");
  httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
  return ESP_OK;
}

esp_err_t send_json(httpd_req_t *req, cJSON *root) {
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!text) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, text);
  cJSON_free(text);
  return ESP_OK;
}

/* Reads a bounded request body. Anything larger is a mistake or an attack, not
   a configuration. */
bool read_body(httpd_req_t *req, std::string &out, size_t limit = 1024) {
  if (req->content_len == 0 || req->content_len > limit)
    return false;
  out.resize(req->content_len);
  size_t got = 0;
  while (got < req->content_len) {
    int r = httpd_req_recv(req, out.data() + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (r <= 0)
      return false;
    got += r;
  }
  return true;
}

/* One query parameter by name, bounded. Absent or oversized reads as empty,
   which every caller treats as "not given". */
std::string query_param(httpd_req_t *req, const char *key) {
  const size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0 || qlen >= 160)
    return {};
  std::string query(qlen + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), qlen + 1) != ESP_OK)
    return {};
  char value[80] = "";
  if (httpd_query_key_value(query.c_str(), key, value, sizeof(value)) != ESP_OK)
    return {};

  /* httpd_query_key_value hands back the raw slice, percent escapes and all.
     A browser encodes the colon in "custom:profile", so without this the id
     arrives as "custom%3Aprofile" and matches nothing -- which is exactly how
     editing a stored profile came to fail silently. */
  std::string out;
  for (size_t i = 0; value[i]; i++) {
    if (value[i] == '+') {
      out += ' ';
    } else if (value[i] == '%' && value[i + 1] && value[i + 2]) {
      const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = nib(value[i + 1]), lo = nib(value[i + 2]);
      if (hi < 0 || lo < 0) {
        out += value[i];  /* Not an escape after all; take it literally. */
        continue;
      }
      out += static_cast<char>(hi * 16 + lo);
      i += 2;
    } else {
      out += value[i];
    }
  }
  return out;
}

// --------------------------------------------------------------- OTA ----

/* Broadcast so the browser can show progress from the flash side, not just its
   own upload side -- the two diverge once the socket buffer is full. */
void report_ota(size_t written, size_t total) {
  if (!g_queue)
    return;
  Msg msg;
  snprintf(msg.text, sizeof(msg.text), "{\"t\":\"ota\",\"w\":%u,\"n\":%u}",
           static_cast<unsigned>(written), static_cast<unsigned>(total));
  xQueueSend(g_queue, &msg, 0);
}

esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t handle, const char *reason) {
  if (handle)
    esp_ota_abort(handle);
  ESP_LOGE(TAG, "OTA rejected: %s", reason);
  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", reason);
  return send_json(req, r);
}

esp_err_t post_ota(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
  if (!target)
    return ota_fail(req, 0, "no free OTA slot");

  const size_t total = req->content_len;
  if (total < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
    return ota_fail(req, 0, "not a firmware image (too small)");
  if (total > target->size)
    return ota_fail(req, 0, "image larger than the OTA slot");

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, total, &handle);
  if (err != ESP_OK)
    return ota_fail(req, 0, esp_err_to_name(err));

  ESP_LOGW(TAG, "OTA started: %u bytes into %s", static_cast<unsigned>(total), target->label);
  indicator_hook(true);

  char buf[2048];
  size_t written = 0;
  bool checked = false;

  /* The image header, the first segment header and the application descriptor,
     which is as far as we have to read to know whose firmware this is. */
  constexpr size_t kDescAt = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  constexpr size_t kHeaderNeeded = kDescAt + sizeof(esp_app_desc_t);
  std::vector<uint8_t> head;
  head.reserve(kHeaderNeeded);

  while (written < total) {
    int got = httpd_req_recv(req, buf, std::min(sizeof(buf), total - written));
    if (got == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (got <= 0) {
      indicator_hook(false);
      return ota_fail(req, handle, "upload interrupted");
    }

    const char *data = buf;
    size_t len = static_cast<size_t>(got);

    /* Nothing reaches the flash before this passes. The magic byte catches
       anything that is not an ESP image at all; the project name catches
       firmware for a different device, which would otherwise brick this one in
       a way only a cable can undo.

       The header is collected across however many reads it takes rather than
       expected in the first one. A slow uplink delivers a short first chunk,
       and this used to take that as permission to skip the project name and
       write the rest unchecked. */
    if (!checked) {
      const size_t take = std::min(kHeaderNeeded - head.size(), len);
      head.insert(head.end(), buf, buf + take);
      data += take;
      len -= take;

      if (!head.empty() && head[0] != ESP_IMAGE_HEADER_MAGIC) {
        indicator_hook(false);
        return ota_fail(req, handle, "not an ESP firmware image");
      }
      if (head.size() < kHeaderNeeded)
        continue;  // keep reading; still nothing written

      const auto *desc = reinterpret_cast<const esp_app_desc_t *>(head.data() + kDescAt);
      const esp_app_desc_t *self = esp_app_get_description();
      if (std::strncmp(desc->project_name, self->project_name, sizeof(desc->project_name)) != 0) {
        indicator_hook(false);
        return ota_fail(req, handle, "firmware is for a different project");
      }
      ESP_LOGI(TAG, "Incoming version: %s", desc->version);
      checked = true;

      err = esp_ota_write(handle, head.data(), head.size());
      if (err != ESP_OK) {
        indicator_hook(false);
        return ota_fail(req, handle, esp_err_to_name(err));
      }
      written += head.size();
      report_ota(written, total);
      if (len == 0)
        continue;
    }

    err = esp_ota_write(handle, data, len);
    if (err != ESP_OK) {
      indicator_hook(false);
      return ota_fail(req, handle, esp_err_to_name(err));
    }
    written += len;
    report_ota(written, total);
  }

  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation" : esp_err_to_name(err));
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    indicator_hook(false);
    return ota_fail(req, 0, esp_err_to_name(err));
  }

  ESP_LOGW(TAG, "OTA complete, rebooting into %s", target->label);
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddNumberToObject(r, "written", written);
  send_json(req, r);

  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(700));
        esp_restart();
      },
      "ota_reboot", 2048, nullptr, 5, nullptr);
  return ESP_OK;
}

esp_err_t get_ota_status(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  const esp_app_desc_t *self = esp_app_get_description();

  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "partition", running->label);
  cJSON_AddStringToObject(r, "version", self->version);
  cJSON_AddStringToObject(r, "built", self->date);
  cJSON_AddBoolToObject(r, "pending_verify", state == ESP_OTA_IMG_PENDING_VERIFY);
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  cJSON_AddNumberToObject(r, "slot_size", next ? next->size : 0);
  return send_json(req, r);
}

// ------------------------------------------------------------ handlers ----

esp_err_t get_index(httpd_req_t *req) {
  /* The page itself carries no data -- everything comes from /api/v1/info,
     which is protected. Gating it anyway means the browser asks for credentials
     when the page is opened, so every later request already carries them. Left
     open, the page would load and then fail with 401 in the background, which
     browsers do not turn into a login prompt. */
  if (!check_auth(req))
    return deny(req);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, reinterpret_cast<const char *>(ui_index_html_gz), ui_index_html_gz_len);
}

esp_err_t get_info(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  Status s = g_status ? g_status() : Status{};
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "device", appcfg::device().name.c_str());
  cJSON_AddNumberToObject(root, "uptime_s", esp_timer_get_time() / 1000000);
  cJSON_AddNumberToObject(root, "heap", esp_get_free_heap_size());
  cJSON_AddBoolToObject(root, "readonly", true);

  cJSON *bus = cJSON_AddObjectToObject(root, "bus");
  cJSON_AddNumberToObject(bus, "baud", s.baud);
  cJSON_AddNumberToObject(bus, "frames", s.frames_ok);
  cJSON_AddNumberToObject(bus, "crc_errors", s.crc_errors);
  cJSON_AddNumberToObject(bus, "resyncs", s.resyncs);
  cJSON_AddNumberToObject(bus, "acks", s.acks_seen);
  cJSON_AddNumberToObject(bus, "naks", s.naks_seen);
  cJSON_AddNumberToObject(bus, "dropped", s.dropped);
  cJSON_AddNumberToObject(bus, "bytes", s.bytes);

  /* What this firmware is running on, which the interface needs to write an
     ESPHome configuration somebody else can use, and which turns a class of
     radio problem from a morning's detective work into a request. */
  cJSON *hw = cJSON_AddObjectToObject(root, "hardware");
  cJSON_AddStringToObject(hw, "board", s.board.c_str());
  cJSON_AddStringToObject(hw, "chip", CONFIG_IDF_TARGET);
  cJSON_AddNumberToObject(hw, "rx_pin", s.pin_rx);
  cJSON_AddNumberToObject(hw, "tx_pin", s.pin_tx);
  cJSON_AddNumberToObject(hw, "led_pin", s.pin_led);
  cJSON_AddNumberToObject(hw, "activity_pin", s.pin_activity);
  cJSON_AddBoolToObject(hw, "led_inverted", s.led_inverted);

  cJSON *net = cJSON_AddObjectToObject(root, "net");
  cJSON_AddStringToObject(net, "state", s.net_state.c_str());
  cJSON_AddNumberToObject(net, "channel", s.channel);
  cJSON_AddStringToObject(net, "ip", s.ip.c_str());
  cJSON_AddStringToObject(net, "ssid", s.ssid.c_str());
  cJSON_AddNumberToObject(net, "rssi", s.rssi);
  cJSON_AddBoolToObject(net, "provisioning", appnet::provisioning());

  /* boot_epoch_ms is the useful one: frames carry the uptime clock, so adding
     it to that stamp gives the absolute time a frame arrived -- without a
     timestamp having to travel with every single frame. */
  const appnet::Time t = appnet::time_info();
  cJSON *clock = cJSON_AddObjectToObject(root, "time");
  cJSON_AddBoolToObject(clock, "synced", t.synced);
  if (t.synced) {
    cJSON_AddNumberToObject(clock, "epoch_ms", static_cast<double>(t.epoch_ms));
    cJSON_AddNumberToObject(clock, "boot_epoch_ms", static_cast<double>(t.boot_epoch_ms));
  }

  cJSON *mq = cJSON_AddObjectToObject(root, "mqtt");
  cJSON_AddBoolToObject(mq, "enabled", appmqtt::enabled());
  cJSON_AddBoolToObject(mq, "connected", appmqtt::connected());

  cJSON *auth = cJSON_AddObjectToObject(root, "auth");
  appcfg::Auth a = appcfg::auth();
  cJSON_AddBoolToObject(auth, "enabled", a.enabled);
  cJSON_AddBoolToObject(auth, "has_password", a.has_password());
  cJSON_AddStringToObject(auth, "user", a.user.c_str());

  return send_json(req, root);
}

esp_err_t post_wifi(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *ssid = root ? cJSON_GetObjectItem(root, "ssid") : nullptr;
  cJSON *pass = root ? cJSON_GetObjectItem(root, "password") : nullptr;
  if (!cJSON_IsString(ssid)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"ssid required\"}");
    return ESP_OK;
  }

  const bool ok = appcfg::set_wifi(ssid->valuestring, cJSON_IsString(pass) ? pass->valuestring : "");
  cJSON_Delete(root);

  if (!ok) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");

  /* New credentials are applied by restarting, not by reconnecting in place.
     Switching from the setup network to a station while both are up puts the
     one radio in an impossible position: the access point is on a fixed
     channel and the station has to follow the router's, which on a house with
     two access points on different channels is a different one. The device
     then reports itself connected -- correctly -- while barely passing a
     packet, and the phone that was reading the address loses the setup network
     from under it, because that moved channel too.

     A restart costs about five seconds and removes the whole situation: on the
     way up there is no access point to accommodate, and the station connects
     to whatever channel it likes. Measured stable over 150 consecutive probes
     after a restart, against an unreachable device after an in-place switch.

     After the response has gone out, or the client never sees it. */
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      },
      "wifi_apply", 2048, nullptr, 4, nullptr);
  return ESP_OK;
}

esp_err_t post_auth(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    return ESP_OK;
  }

  cJSON *pw = cJSON_GetObjectItem(root, "password");
  cJSON *user = cJSON_GetObjectItem(root, "user");
  cJSON *enabled = cJSON_GetObjectItem(root, "enabled");

  const char *error = nullptr;
  if (cJSON_IsString(pw))
    if (!appcfg::set_password(cJSON_IsString(user) ? user->valuestring : "admin", pw->valuestring))
      error = "could not store password";

  if (!error && cJSON_IsBool(enabled))
    if (!appcfg::set_auth_enabled(cJSON_IsTrue(enabled)))
      error = "cannot enable protection without a password";

  cJSON_Delete(root);

  if (error) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", error);
    return send_json(req, r);
  }

  /* The old credentials must stop working now, not in five minutes. */
  forget_auth_cache();

  appcfg::Auth a = appcfg::auth();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "enabled", a.enabled);
  cJSON_AddBoolToObject(r, "has_password", a.has_password());
  return send_json(req, r);
}

/* The name doubles as the DHCP hostname, the mDNS name and the access-point
   SSID, so it has to be a valid DNS label -- not merely non-empty. Rejecting
   here beats letting the router or a phone quietly mangle it later. */
bool valid_device_name(const std::string &n) {
  if (n.empty() || n.size() > 32)
    return false;
  if (n.front() == '-' || n.back() == '-')
    return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  return true;
}

esp_err_t post_device(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *name = root ? cJSON_GetObjectItem(root, "name") : nullptr;
  if (!cJSON_IsString(name)) {
    cJSON_Delete(root);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"name required\"}");
    return ESP_OK;
  }

  /* Lowercase rather than reject: hostnames are case-insensitive anyway, and
     someone typing a capital should not be met with an error. */
  std::string n = name->valuestring;
  for (char &c : n)
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  cJSON_Delete(root);

  /* An empty name reverts to the MAC-derived default. Without this there is no
     way back to it short of a factory reset. */
  if (n.empty()) {
    appcfg::set_device_name("");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "name", appcfg::default_device_name().c_str());
    cJSON_AddBoolToObject(r, "restart_required", true);
    return send_json(req, r);
  }

  if (!valid_device_name(n)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req,
                       "{\"error\":\"1-32 characters, a-z 0-9 and dashes, not starting or ending with a dash\"}");
    return ESP_OK;
  }

  if (!appcfg::set_device_name(n)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddStringToObject(r, "name", n.c_str());
  /* Hostname and mDNS are bound at interface setup, so the new name only takes
     effect on the next boot. Say so rather than let the user wonder. */
  cJSON_AddBoolToObject(r, "restart_required", true);
  return send_json(req, r);
}

esp_err_t post_reset(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Erases every setting and reboots, so the device comes back as if freshly
     flashed. Unlike the BOOT-button hold -- which only clears what can lock you
     out -- this is a deliberate, confirmable action from a browser, so it can
     afford to be complete. */
  const bool ok = appcfg::factory_reset();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "could not erase all settings");
  send_json(req, r);

  if (ok) {
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(500));
          esp_restart();
        },
        "reset", 2048, nullptr, 4, nullptr);
  }
  return ESP_OK;
}

/* The appliance layer already renders these as JSON; passing the text straight
   through avoids rebuilding the same structures twice in two components. */
esp_err_t send_raw_json(httpd_req_t *req, const std::string &body) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body.c_str());
  return ESP_OK;
}

esp_err_t get_appliance(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.profile_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_appliance.profile_json());
}

esp_err_t get_readings(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.readings_json)
    return send_raw_json(req, "[]");
  return send_raw_json(req, g_appliance.readings_json());
}

esp_err_t get_layout(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.layout_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_appliance.layout_json());
}

/* The user's own profile: read it, replace it, remove it.

   The body limit is far above what any profile needs, because the failure it
   guards against is a runaway upload, not a large appliance. Saving validates
   before storing, so a rejected profile changes nothing at all. */
/* Without ?id= this lists the user's profiles; with one it serves that
   profile's JSON, which is also what the download button fetches. */
esp_err_t get_storage(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.storage_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_appliance.storage_json().c_str());
}

esp_err_t get_custom(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const std::string id = query_param(req, "id");
  if (id.empty()) {
    if (!g_appliance.stored_json)
      return send_raw_json(req, "[]");
    return send_raw_json(req, g_appliance.stored_json().c_str());
  }

  if (!g_appliance.source_of)
    return send_raw_json(req, "{}");
  const std::string json = g_appliance.source_of(id);
  if (json.empty()) {
    httpd_resp_set_status(req, "404 Not Found");
    return send_raw_json(req, "{\"error\":\"no such profile\"}");
  }
  /* Offered as a download when asked for that way, so the browser saves a file
     named after the profile instead of showing JSON in a tab. */
  if (query_param(req, "download") == "1") {
    httpd_resp_set_type(req, "application/octet-stream");
    /* The name is built from a query parameter and goes into a response
       header, so it is filtered here rather than trusted: a quote would end
       the filename early and a CR/LF would end the header and start whatever
       the caller wrote next. Only the characters a slug may contain survive. */
    std::string name;
    for (char c : id.substr(id.find(':') + 1)) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
          c == '-' || c == '_')
        name += c;
      if (name.size() >= 48)
        break;
    }
    if (name.empty())
      name = "profile";
    const std::string disp = "attachment; filename=\"" + name + ".json\"";
    httpd_resp_set_hdr(req, "Content-Disposition", disp.c_str());
    httpd_resp_send(req, json.data(), json.size());
    return ESP_OK;
  }
  return send_raw_json(req, json.c_str());
}

/* ?id=<profile> asks for a specific one; without it, the active profile. That
   keeps the editor's "show me what this says" and "start from what is running"
   on one route. */
esp_err_t get_source(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const std::string id = query_param(req, "id");
  if (!g_appliance.source_of)
    return send_raw_json(req, "{}");
  const std::string json = g_appliance.source_of(id);
  if (json.empty()) {
    httpd_resp_set_status(req, "404 Not Found");
    return send_raw_json(req, "{\"error\":\"no such profile\"}");
  }
  return send_raw_json(req, json.c_str());
}

esp_err_t put_custom(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body, 33 * 1024)) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", "missing or oversized body");
    return send_json(req, r);
  }

  const std::string id = query_param(req, "id");
  if (id.empty()) {
    httpd_resp_set_status(req, "400 Bad Request");
    return send_raw_json(req, "{\"error\":\"an id is required\"}");
  }
  const bool activate_it = query_param(req, "activate") == "1";

  std::string err;
  const bool ok = g_appliance.save_custom &&
                  g_appliance.save_custom(id, body, activate_it, &err);
  if (!ok)
    httpd_resp_set_status(req, "400 Bad Request");

  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", err.empty() ? "could not save" : err.c_str());
  return send_json(req, r);
}

esp_err_t delete_custom(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const std::string id = query_param(req, "id");
  std::string err;
  const bool ok = !id.empty() && g_appliance.erase_custom &&
                  g_appliance.erase_custom(id, &err);
  if (id.empty())
    err = "an id is required";
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", err.empty() ? "nothing to erase" : err.c_str());
  return send_json(req, r);
}

esp_err_t get_candidates(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.candidates_json)
    return send_raw_json(req, "[]");
  return send_raw_json(req, g_appliance.candidates_json());
}

esp_err_t get_catalogue(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_appliance.catalogue_json)
    return send_raw_json(req, "[]");
  return send_raw_json(req, g_appliance.catalogue_json());
}

/* The same ring the WebSocket replays from, but pull rather than push, so a
   script (or a second monitor) can fetch recent bus messages over plain HTTP.
   GET /api/v1/frames?since=N returns every message newer than N, oldest first;
   omit `since` (or pass -1) for the whole ring. "latest" is the newest sequence
   held, to pass back as `since` next time; "gap" is true when the ring no longer
   reaches all the way back to since+1. Each item is the exact JSON the monitor
   receives live, sequence number and all. */
esp_err_t get_frames(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Asking for the recent traffic is asking for the ring, so this is where it
     comes into existence if nobody has wanted it before. */
  ring_reserve();

  const std::string s = query_param(req, "since");
  const long long since = s.empty() ? -1 : std::strtoll(s.c_str(), nullptr, 10);

  std::vector<std::string> out;
  bool have = false, gap = false;
  uint32_t oldest = 0, newest = 0;
  if (g_ring_mux) {
    xSemaphoreTake(g_ring_mux, portMAX_DELAY);
    size_t idx = (g_ring_head + kRingCap - g_ring_count) % kRingCap;
    for (size_t i = 0; i < g_ring_count; i++) {
      const RingItem &it = g_ring[idx];
      if (i == 0)
        oldest = it.seq;
      newest = it.seq;
      have = true;
      if (static_cast<long long>(it.seq) > since)
        out.emplace_back(it.text);
      idx = (idx + 1) % kRingCap;
    }
    if (g_ring_count > 0 && since >= 0 && static_cast<long long>(oldest) > since + 1)
      gap = true;
    xSemaphoreGive(g_ring_mux);
  }

  char head[96];
  std::snprintf(head, sizeof(head), "{\"latest\":%lld,\"oldest\":%lld,\"gap\":%s,\"items\":[",
                have ? static_cast<long long>(newest) : -1, have ? static_cast<long long>(oldest) : -1,
                gap ? "true" : "false");
  std::string body = head;
  for (size_t i = 0; i < out.size(); i++) {
    if (i)
      body += ',';
    body += out[i];
  }
  body += "]}";
  return send_raw_json(req, body);
}

esp_err_t post_appliance(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  std::string body;
  if (!read_body(req, body)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  cJSON *id = root ? cJSON_GetObjectItem(root, "id") : nullptr;
  const std::string want = cJSON_IsString(id) ? id->valuestring : "";
  cJSON_Delete(root);

  if (want.empty()) {
    /* An empty id means "forget what you were told and listen again". */
    if (g_appliance.clear)
      g_appliance.clear();
    return send_raw_json(req, "{\"ok\":true,\"cleared\":true}");
  }

  std::string error = "unavailable";
  if (g_appliance.activate && g_appliance.activate(want, &error))
    return send_raw_json(req, "{\"ok\":true}");

  httpd_resp_set_status(req, "400 Bad Request");
  cJSON *r = cJSON_CreateObject();
  cJSON_AddStringToObject(r, "error", error.c_str());
  return send_json(req, r);
}

esp_err_t get_health(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  if (!g_diag.health_json)
    return send_raw_json(req, "{}");
  return send_raw_json(req, g_diag.health_json());
}

/* Streamed in chunks: a core dump is tens of kilobytes and holding it in memory
   to send it would risk the very condition it was recorded for. */
esp_err_t get_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  const size_t total = g_diag.coredump_size ? g_diag.coredump_size() : 0;
  if (!total) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_sendstr(req, "{\"error\":\"no core dump stored\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.bin\"");

  char buf[1024];
  size_t sent = 0;
  while (sent < total) {
    const size_t chunk = std::min(sizeof(buf), total - sent);
    if (!g_diag.read_coredump(sent, buf, chunk)) {
      httpd_resp_send_chunk(req, nullptr, 0);
      return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, buf, chunk) != ESP_OK)
      return ESP_FAIL;
    sent += chunk;
  }
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t delete_coredump(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  const bool ok = g_diag.erase_coredump && g_diag.erase_coredump();
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(r, "error", "nothing to erase, or erase failed");
  return send_json(req, r);
}

esp_err_t get_mqtt(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  return send_raw_json(req, appmqtt::status_json().c_str());
}

esp_err_t post_mqtt(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);

  /* Roomy enough for a CA certificate: a PEM chain runs to a couple of
     kilobytes before JSON escaping, and refusing it here would look like the
     certificate was wrong rather than merely too long for the reader. */
  std::string body;
  if (!read_body(req, body, 8192)) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"bad body\"}");
    return ESP_OK;
  }

  cJSON *root = cJSON_Parse(body.c_str());
  if (!root) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "{\"error\":\"invalid JSON\"}");
    return ESP_OK;
  }

  const auto str = [&](const char *k, const std::string &fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsString(v) ? std::string(v->valuestring) : fallback;
  };
  const auto flag = [&](const char *k, bool fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
  };

  const auto num = [&](const char *k, long fallback) {
    cJSON *v = cJSON_GetObjectItem(root, k);
    return cJSON_IsNumber(v) ? static_cast<long>(v->valuedouble) : fallback;
  };

  appcfg::Mqtt m = appcfg::mqtt();
  m.tls = flag("tls", m.tls);
  m.host = str("host", m.host);
  const long port = num("port", m.port);
  m.port = (port > 0 && port <= 65535)
               ? static_cast<uint16_t>(port)
               : (m.tls ? appcfg::Mqtt::kPortTls : appcfg::Mqtt::kPortPlain);
  m.client_id = str("client_id", m.client_id);
  m.auth = flag("auth", m.auth);
  m.user = str("user", m.user);
  /* Absent means unchanged, so the settings page can show that a password is
     stored without ever having to hold it. Empty string clears it. */
  cJSON *pw = cJSON_GetObjectItem(root, "password");
  if (cJSON_IsString(pw))
    m.password = pw->valuestring;
  m.tls_insecure = flag("tls_insecure", m.tls_insecure);
  m.ca_cert = str("ca_cert", m.ca_cert);
  m.base = str("base", m.base);
  m.raw_frames = flag("raw_frames", m.raw_frames);
  m.discovery = flag("discovery", m.discovery);
  m.discovery_prefix = str("discovery_prefix", m.discovery_prefix);
  m.enabled = flag("enabled", m.enabled);
  cJSON_Delete(root);

  const auto reject = [&](const char *why) {
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", why);
    send_json(req, r);
  };

  if (m.enabled && !m.configured()) {
    reject("a broker host is required");
    return ESP_OK;
  }
  /* Checked whether or not the client is switched on: saving credentials that
     cannot work, to be discovered later when something is turned on, is the
     kind of delay that makes a setting feel broken. */
  if (m.auth && m.user.empty()) {
    reject("a user name is required when authentication is on");
    return ESP_OK;
  }
  if (m.auth && m.password.empty()) {
    reject("a password is required when authentication is on");
    return ESP_OK;
  }
  if (m.tls && !m.tls_insecure && m.ca_cert.empty()) {
    reject("either provide a CA certificate or say the server is not checked");
    return ESP_OK;
  }
  /* A certificate that is not PEM would be refused deep inside the TLS stack at
     connect time, as a failed handshake with nothing to point at. */
  if (m.tls && !m.tls_insecure && m.ca_cert.find("-----BEGIN CERTIFICATE-----") == std::string::npos) {
    reject("the CA certificate must be PEM, starting with -----BEGIN CERTIFICATE-----");
    return ESP_OK;
  }

  if (!appcfg::set_mqtt(m)) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_sendstr(req, "{\"error\":\"store failed\"}");
    return ESP_OK;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true}");

  /* Reconnect after the response, or the client watches its own connection die
     while waiting for an answer that is already written. */
  static TaskHandle_t apply_task = nullptr;
  if (!apply_task) {
    xTaskCreate(
        [](void *) {
          vTaskDelay(pdMS_TO_TICKS(500));
          appmqtt::reconfigure();
          apply_task = nullptr;
          vTaskDelete(nullptr);
        },
        "mqtt_apply", 4096, nullptr, 4, &apply_task);
  }
  return ESP_OK;
}

esp_err_t post_restart(httpd_req_t *req) {
  if (!check_auth(req))
    return deny(req);
  httpd_resp_sendstr(req, "{\"ok\":true}");
  xTaskCreate(
      [](void *) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
      },
      "restart", 2048, nullptr, 4, nullptr);
  return ESP_OK;
}

/* Captive-portal probes: anything we do not serve gets redirected to the setup
   page, which is what makes phones pop up the "sign in to network" sheet.

   Except under /api/, where a redirect to an HTML page is a confusing answer to
   a mistyped endpoint. Something expecting JSON should be told it was wrong, in
   JSON. */
esp_err_t not_found(httpd_req_t *req, httpd_err_code_t) {
  if (std::strncmp(req->uri, "/api/", 5) == 0) {
    httpd_resp_set_status(req, "404 Not Found");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"no such endpoint\"}");
    return ESP_OK;
  }
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_sendstr(req, "");
  return ESP_OK;
}

// --------------------------------------------------------------- websocket ----

/* There is deliberately no local table of connected monitors.

   ESP-IDF 6 does not call the URI handler once a WebSocket handshake has
   succeeded -- httpd_uri.c returns early with "If the request is websocket
   handshake, then do not call the uri->handler". The widely copied idiom of
   registering the socket inside an `if (req->method == HTTP_GET)` branch (still
   what the ws_echo_server example shows) therefore never runs on IDF 6, and the
   monitor delivered nothing at all while looking perfectly connected: the
   server completes the handshake and answers 101 regardless.

   Asking the server who is attached avoids the whole class of problem. There
   are no stale descriptors when a browser tab dies, no slots to leak, and
   nothing that has to stay in step with a handler-invocation rule that has
   already changed under this code once. */
size_t ws_clients(int *out, size_t max) {
  if (!g_server)
    return 0;

  int fds[kMaxSockets];
  size_t n = kMaxSockets;
  if (httpd_get_client_list(g_server, &n, fds) != ESP_OK)
    return 0;

  size_t found = 0;
  for (size_t i = 0; i < n && found < max; i++)
    if (httpd_ws_get_fd_info(g_server, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET)
      out[found++] = fds[i];
  return found;
}

/* Runs before the 101 is sent, which makes it the only place an upgrade can
   still be refused. Rejecting after the handshake would leave the client
   holding a socket that silently never delivers -- precisely the failure this
   section exists to prevent. Authenticating here also closes a real hole: the
   check used to sit in the dead HTTP_GET branch above, so with a password set
   the monitor was still reachable without one. */
esp_err_t ws_pre_handshake(httpd_req_t *req) {
  if (check_auth(req))
    return ESP_OK;
  deny(req);
  return ESP_FAIL;
}

/* Send one text message to a single socket, synchronously. Safe from here: the
   server runs one HTTP task, so while this handler is on it no queued broadcast
   can be writing to the same socket in parallel. */
void ws_send_text(httpd_req_t *req, const char *text) {
  httpd_ws_frame_t frame = {};
  frame.final = true;
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
  frame.len = std::strlen(text);
  httpd_ws_send_frame(req, &frame);
}

/* A monitor announces the last sequence number it holds as {"since":N} (N = -1
   means it holds nothing). Everything newer still in the ring is sent straight
   back to that one socket, oldest first, so a reconnect resumes exactly where it
   left off before the live stream carries on. If the gap reaches further back
   than the ring keeps, a {"t":"gap"} marker says so rather than letting the
   history look complete when it is not. */
void replay_since(httpd_req_t *req, const char *msg) {
  const char *p = std::strstr(msg, "\"since\"");
  if (!p)
    return;
  ring_reserve();
  p += 7;
  while (*p == ':' || *p == ' ')
    p++;
  const long long since = std::strtoll(p, nullptr, 10);

  std::vector<std::string> out;
  bool gap = false;
  if (g_ring_mux) {
    xSemaphoreTake(g_ring_mux, portMAX_DELAY);
    size_t idx = (g_ring_head + kRingCap - g_ring_count) % kRingCap;
    uint32_t oldest = 0;
    for (size_t i = 0; i < g_ring_count; i++) {
      const RingItem &it = g_ring[idx];
      if (i == 0)
        oldest = it.seq;
      if (static_cast<long long>(it.seq) > since)
        out.emplace_back(it.text);
      idx = (idx + 1) % kRingCap;
    }
    if (g_ring_count > 0 && since >= 0 && static_cast<long long>(oldest) > since + 1)
      gap = true;
    xSemaphoreGive(g_ring_mux);
  }

  if (gap)
    ws_send_text(req, "{\"t\":\"gap\"}");
  for (const std::string &s : out)
    ws_send_text(req, s.c_str());
}

esp_err_t ws_handler(httpd_req_t *req) {

  /* Clients say one thing only: {"since":N}, sent on connect to be handed what
     they missed from the ring (see replay_since). Anything else is ignored, but
     the frame still has to be read -- leaving the payload in the socket buffer
     makes the server call straight back in and spin at full speed, and a browser
     closing its tab was once enough to hang the device. */
  httpd_ws_frame_t frame = {};
  esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
  if (err != ESP_OK)
    return err;

  /* Whatever arrives has to leave the socket buffer, including what we have no
     use for. Reading only the frames small enough to be a message left anything
     larger sitting there, and the server would call straight back in and spin
     -- exactly the hang described above, reachable by sending 512 bytes.

     Beyond a few kilobytes the connection is dropped rather than drained: no
     legitimate message is that size, and allocating whatever a caller claims
     to have sent is the other way to be knocked over. */
  constexpr size_t kWsMessageMax = 512;   // longest thing a client legitimately says
  constexpr size_t kWsDrainMax = 4096;    // beyond this, close instead of read

  if (frame.len > kWsDrainMax) {
    ESP_LOGW(TAG, "WebSocket frame of %u bytes; closing the connection",
             static_cast<unsigned>(frame.len));
    return ESP_FAIL;
  }

  if (frame.len > 0) {
    std::vector<uint8_t> buf(frame.len + 1);
    frame.payload = buf.data();
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK)
      return err;
    buf[frame.len] = 0;
    if (frame.len < kWsMessageMax)
      replay_since(req, reinterpret_cast<const char *>(buf.data()));
  }

  /* A CLOSE needs no bookkeeping: the server drops the socket, and the next
     broadcast simply does not find it any more. */
  return ESP_OK;
}

void broadcast(const char *text) {
  int fds[kMaxSockets];
  const size_t n = ws_clients(fds, kMaxSockets);
  if (n == 0)
    return;

  httpd_ws_frame_t frame = {};
  frame.final = true;
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t *>(const_cast<char *>(text));
  frame.len = std::strlen(text);

  for (size_t i = 0; i < n; i++)
    httpd_ws_send_frame_async(g_server, fds[i], &frame);
}

bool any_client() {
  int fds[kMaxSockets];
  return ws_clients(fds, kMaxSockets) > 0;
}

/* Owns all sockets. Producers only ever push into the queue, so the bus task
   can never block on a slow or vanished HTTP client. */
void ws_pump(void *) {
  Msg msg;
  for (;;) {
    if (xQueueReceive(g_queue, &msg, pdMS_TO_TICKS(200)) == pdPASS) {
      if (any_client())
        broadcast(msg.text);
    }
    if (g_dropped.load() && any_client()) {
      char note[64];
      snprintf(note, sizeof(note), "{\"t\":\"drop\",\"n\":%" PRIu32 "}",
               g_dropped.exchange(0));
      broadcast(note);
    }
  }
}

}  // namespace

void publish_frame(const dbus2::Frame &f, int64_t rx_time_us) {
  if (!g_queue)
    return;

  Msg msg;
  char hex[kHexBytes * 2 + 1] = "";
  size_t pos = 0;
  const size_t shown = f.data.size() < kHexBytes ? f.data.size() : kHexBytes;
  for (size_t i = 0; i < shown; i++)
    pos += snprintf(hex + pos, sizeof(hex) - pos, "%02X", f.data[i]);

  /* g_seq is only ever touched here and in publish_ack, both on the app task,
     so the increment needs no lock; the ring it feeds has its own. */
  const uint32_t seq = g_seq++;
  snprintf(msg.text, sizeof(msg.text), "{\"t\":\"f\",\"s\":%u,\"ts\":%lld,\"l\":%u,\"d\":\"%02X\",\"x\":\"%s\"}",
           static_cast<unsigned>(seq), static_cast<long long>(rx_time_us / 1000), f.length, f.dest, hex);
  ring_append(seq, msg.text);

  /* Never block: a monitor that cannot keep up loses frames, the bus task does
     not lose time. It can still recover them from the ring on reconnect. */
  if (xQueueSend(g_queue, &msg, 0) != pdPASS)
    g_dropped++;
}

void publish_ack(uint8_t ack, int64_t frame_rx_time_us) {
  if (!g_queue)
    return;
  Msg msg;
  const uint32_t seq = g_seq++;
  snprintf(msg.text, sizeof(msg.text), "{\"t\":\"a\",\"s\":%u,\"ts\":%lld,\"v\":%u}",
           static_cast<unsigned>(seq), static_cast<long long>(frame_rx_time_us / 1000), static_cast<unsigned>(ack));
  ring_append(seq, msg.text);
  if (xQueueSend(g_queue, &msg, 0) != pdPASS)
    g_dropped++;
}

bool begin(StatusProvider provider, OtaHook ota_hook, ApplianceApi appliance,
           DiagnosticsApi diagnostics) {
  g_status = std::move(provider);
  g_ota_hook = std::move(ota_hook);
  g_appliance = std::move(appliance);
  g_diag = std::move(diagnostics);

  g_queue = xQueueCreate(kQueueLen, sizeof(Msg));
  if (!g_queue) {
    ESP_LOGE(TAG, "queue alloc failed");
    return false;
  }

  g_ring_mux = xSemaphoreCreateMutex();
  if (!g_ring_mux) {
    ESP_LOGE(TAG, "ring mutex alloc failed");
    return false;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  static_assert(kMaxSockets == 7, "ws_clients() reads back this many sockets");
  /* Every request passes through the matcher, which makes it the one place
     that sees them all. It records that somebody reached us, which is what
     lets a freshly flashed image prove itself while it is still serving its
     setup network -- see appweb::has_served(). */
  cfg.uri_match_fn = [](const char *tpl, const char *uri, size_t len) -> bool {
    g_served = true;
    /* Exactly what the server does with no matcher of its own -- this hook is
       here to observe, not to change which handler runs. */
    return std::strlen(tpl) == len && std::strncmp(tpl, uri, len) == 0;
  };
  cfg.max_open_sockets = kMaxSockets;
  cfg.max_uri_handlers = 32;  /* 27 in use; the editor adds more */
  cfg.lru_purge_enable = true;
  cfg.stack_size = 6144;
  /* No close_fn: with the client list queried from the server there is nothing
     left to clean up, and setting it would make closing the socket our job. */
  /* Firmware uploads arrive in one long request; the default receive timeout is
     too short for a slow uplink. */
  cfg.recv_wait_timeout = 20;
  cfg.send_wait_timeout = 20;

  if (httpd_start(&g_server, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return false;
  }

  /* Designated initialisers on purpose: httpd_uri_t has grown fields across
     IDF versions, and positional ones break silently when it does. */
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = get_index},
      {.uri = "/api/v1/info", .method = HTTP_GET, .handler = get_info},
      {.uri = "/api/v1/config/wifi", .method = HTTP_POST, .handler = post_wifi},
      {.uri = "/api/v1/config/auth", .method = HTTP_POST, .handler = post_auth},
      {.uri = "/api/v1/config/device", .method = HTTP_POST, .handler = post_device},
      {.uri = "/api/v1/config/mqtt", .method = HTTP_GET, .handler = get_mqtt},
      {.uri = "/api/v1/config/mqtt", .method = HTTP_POST, .handler = post_mqtt},
      {.uri = "/api/v1/restart", .method = HTTP_POST, .handler = post_restart},
      {.uri = "/api/v1/reset", .method = HTTP_POST, .handler = post_reset},
      {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = post_ota},
      {.uri = "/api/v1/ota/status", .method = HTTP_GET, .handler = get_ota_status},
      {.uri = "/api/v1/appliance", .method = HTTP_GET, .handler = get_appliance},
      {.uri = "/api/v1/appliance", .method = HTTP_POST, .handler = post_appliance},
      {.uri = "/api/v1/readings", .method = HTTP_GET, .handler = get_readings},
      {.uri = "/api/v1/layout", .method = HTTP_GET, .handler = get_layout},
      {.uri = "/api/v1/custom", .method = HTTP_GET, .handler = get_custom},
      {.uri = "/api/v1/storage", .method = HTTP_GET, .handler = get_storage},
      {.uri = "/api/v1/profile/source", .method = HTTP_GET, .handler = get_source},
      {.uri = "/api/v1/custom", .method = HTTP_PUT, .handler = put_custom},
      {.uri = "/api/v1/custom", .method = HTTP_DELETE, .handler = delete_custom},
      {.uri = "/api/v1/candidates", .method = HTTP_GET, .handler = get_candidates},
      {.uri = "/api/v1/profiles", .method = HTTP_GET, .handler = get_catalogue},
      {.uri = "/api/v1/frames", .method = HTTP_GET, .handler = get_frames},
      {.uri = "/api/v1/health", .method = HTTP_GET, .handler = get_health},
      {.uri = "/api/v1/coredump", .method = HTTP_GET, .handler = get_coredump},
      {.uri = "/api/v1/coredump", .method = HTTP_DELETE, .handler = delete_coredump},
      {.uri = "/api/v1/ws",
       .method = HTTP_GET,
       .handler = ws_handler,
       .is_websocket = true,
       .ws_pre_handshake_cb = ws_pre_handshake},
  };
  for (const auto &r : routes)
    httpd_register_uri_handler(g_server, &r);

  httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, not_found);

  if (xTaskCreate(ws_pump, "ws_pump", 4096, nullptr, 4, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "ws_pump task failed");
    return false;
  }

  ESP_LOGI(TAG, "HTTP server up, auth %s", auth_required() ? "on" : "off");
  return true;
}

bool has_served() { return g_served.load(); }

}  // namespace appweb

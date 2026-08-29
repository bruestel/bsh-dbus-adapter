/*
   WiFi with an access-point fallback, plus a captive-portal DNS responder.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appnet/Net.h"
#include "appcfg/Config.h"

#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mdns.h>

#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace appnet {
namespace {

const char *const TAG = "appnet";

/* Six attempts five seconds apart, so roughly half a minute of patience before
   the setup network appears. Retrying without a pause -- as this did at first --
   burns all attempts in a couple of seconds and rides out nothing at all. */
constexpr int kMaxStaRetries = 6;
constexpr int64_t kStaRetryUs = 5 * 1000000LL;
constexpr uint16_t kDnsPort = 53;
/* The access point's fixed address: what the DNS server binds to, what it
   answers with, and what it reports as the device's address while in setup
   mode. One constant so the three cannot disagree. */
constexpr const char *kApAddress = "192.168.4.1";

/* After a successful join the setup network lingers briefly. Whoever just
   entered the credentials is still connected to it and needs to see that it
   worked -- and to read the new address -- before it disappears under them. */
constexpr int64_t kApLingerUs = 120 * 1000000LL;

/* While the setup network is up the station side still tries to get back on the
   real one, just slowly. Without this the device would sit in access-point mode
   forever after a router reboot, and someone would have to go and fetch it. */
constexpr int64_t kApRetryUs = 30 * 1000000LL;

State g_state = State::Idle;
StateCallback g_cb;
int g_retries = 0;
esp_netif_t *g_sta_netif = nullptr;
esp_netif_t *g_ap_netif = nullptr;
char g_ip[16] = "";
TaskHandle_t g_dns_task = nullptr;
volatile bool g_dns_stop = false;
bool g_ap_active = false;
esp_timer_handle_t g_ap_linger_timer = nullptr;
esp_timer_handle_t g_ap_retry_timer = nullptr;
esp_timer_handle_t g_sta_retry_timer = nullptr;

bool g_time_synced = false;

void on_time_sync(struct timeval *tv) {
  char buf[32] = "";
  const time_t secs = tv ? tv->tv_sec : 0;
  struct tm utc;
  gmtime_r(&secs, &utc);
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &utc);
  ESP_LOGI(TAG, "Clock %s from NTP: %s UTC", g_time_synced ? "corrected" : "set", buf);
  g_time_synced = true;
}

/* Started once the station has an address, never in AP mode -- there is no
   route to a time server on our own network, and the attempts would just be
   noise in the log.

   The router is asked first via DHCP option 42, which is both more likely to
   work on a network without internet access and kinder than reaching for a
   public pool. pool.ntp.org sits behind it as a fallback for routers that do
   not offer one. Servers are renewed on every new address, because a DHCP-
   supplied server replaces the preconfigured list when it arrives. */
void start_sntp() {
  static bool started = false;
  if (started)
    return;

  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  cfg.server_from_dhcp = true;
  cfg.renew_servers_after_new_IP = true;
  cfg.index_of_first_server = 1;
  cfg.sync_cb = on_time_sync;
  /* Nothing here may block: this runs on the event loop, and the bus task must
     keep draining the UART whether or not any time server ever answers. */
  cfg.wait_for_sync = false;

  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "SNTP init failed: %s -- timestamps stay relative to boot",
             esp_err_to_name(err));
    return;
  }
  started = true;
  ESP_LOGI(TAG, "SNTP started, asking DHCP first then pool.ntp.org");
}

void set_state(State s) {
  if (s == g_state)
    return;
  g_state = s;
  ESP_LOGI(TAG, "state: %s", to_string(s));
  if (g_cb)
    g_cb(s);
}

void dns_task(void *);

/* Only while the setup network is up. The task exits within half a second of
   the flag being set, which is its receive timeout. */
void stop_dns() {
  if (!g_dns_task)
    return;
  g_dns_stop = true;
  g_dns_task = nullptr;
}

void start_dns() {
  if (g_dns_task)
    return;
  g_dns_stop = false;
  if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 3, &g_dns_task) != pdPASS) {
    g_dns_task = nullptr;
    ESP_LOGW(TAG, "Captive portal DNS could not start");
  }
}

void stop_ap() {
  if (!g_ap_active)
    return;
  g_ap_active = false;
  stop_dns();
  if (g_ap_retry_timer)
    esp_timer_stop(g_ap_retry_timer);
  ESP_LOGI(TAG, "Setup network closed; reachable at %s", g_ip);
  esp_wifi_set_mode(WIFI_MODE_STA);
}

void start_ap() {
  appcfg::Device dev = appcfg::device();

  if (g_ap_linger_timer)
    esp_timer_stop(g_ap_linger_timer);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  g_ap_active = true;
  start_dns();

  wifi_config_t ap = {};
  const std::string ssid = dev.name;
  std::memcpy(ap.ap.ssid, ssid.data(), std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.ssid_len = static_cast<uint8_t>(std::min(ssid.size(), sizeof(ap.ap.ssid)));
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  /* Open on purpose: this network exists only so the user can get back in, and
     a password they cannot look up anywhere would defeat that. It grants access
     to setup, not to the appliance -- the firmware cannot write to the bus. */
  ap.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_LOGI(TAG, "Access point \"%s\" open at %s", ssid.c_str(), kApAddress);
  std::strncpy(g_ip, kApAddress, sizeof(g_ip) - 1);
  set_state(State::ApMode);

  /* Only worth retrying if there is something to retry with. */
  if (appcfg::wifi().configured() && g_ap_retry_timer) {
    esp_timer_stop(g_ap_retry_timer);
    esp_timer_start_periodic(g_ap_retry_timer, kApRetryUs);
    ESP_LOGI(TAG, "Will keep trying \"%s\" every %llds", appcfg::wifi().ssid.c_str(), kApRetryUs / 1000000);
  }
}

void try_connect() {
  appcfg::Wifi w = appcfg::wifi();
  if (!w.configured()) {
    ESP_LOGI(TAG, "No WiFi credentials stored");
    start_ap();
    return;
  }

  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, w.ssid.data(), std::min(w.ssid.size(), sizeof(sta.sta.ssid)));
  std::memcpy(sta.sta.password, w.password.data(), std::min(w.password.size(), sizeof(sta.sta.password)));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
  g_retries = 0;
  set_state(State::Connecting);
  esp_wifi_connect();
}

void on_wifi_event(void *, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    return;
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    /* In access-point mode the retry timer drives reconnection attempts, so
       reacting to every failure here would only produce a tight loop. */
    if (g_state == State::ApMode)
      return;
    const auto *ev = static_cast<wifi_event_sta_disconnected_t *>(data);
    if (++g_retries <= kMaxStaRetries) {
      ESP_LOGW(TAG, "Disconnected (reason %d), retry %d/%d in %llds", ev ? ev->reason : 0, g_retries,
               kMaxStaRetries, kStaRetryUs / 1000000);
      set_state(State::Connecting);
      if (g_sta_retry_timer) {
        esp_timer_stop(g_sta_retry_timer);
        esp_timer_start_once(g_sta_retry_timer, kStaRetryUs);
      } else {
        esp_wifi_connect();
      }
    } else {
      ESP_LOGW(TAG, "Could not join \"%s\", opening setup network", appcfg::wifi().ssid.c_str());
      start_ap();
    }
    return;
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(data);
    std::snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&event->ip_info.ip));
    const char *announced = nullptr;
    esp_netif_get_hostname(g_sta_netif, &announced);
    ESP_LOGI(TAG, "Connected, IP %s, hostname \"%s\"", g_ip, announced ? announced : "(unset)");

    /* Whether HE actually got negotiated depends on the access point too, so
       report what came out rather than what we asked for. */
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      ESP_LOGI(TAG, "Link: %s, rssi %d, channel %d", ap.phy_11ax ? "WiFi 6 (11ax)"
                                                     : ap.phy_11n ? "WiFi 4 (11n)"
                                                                  : "legacy",
               ap.rssi, ap.primary);
      if (!ap.phy_11ax)
        ESP_LOGI(TAG, "Access point did not offer 802.11ax on 2.4 GHz");
    }
    g_retries = 0;
    if (g_ap_retry_timer)
      esp_timer_stop(g_ap_retry_timer);
    if (g_sta_retry_timer)
      esp_timer_stop(g_sta_retry_timer);
    set_state(State::Connected);
    start_sntp();

    /* Advertise only once we are on the real network -- announcing a name on
       our own access point would just point back at ourselves. */
    static bool mdns_up = false;
    if (!mdns_up) {
      const std::string host = appcfg::device().name;
      if (mdns_init() == ESP_OK) {
        mdns_hostname_set(host.c_str());
        mdns_instance_name_set("B/S/H/ D-Bus monitor");
        mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
        ESP_LOGI(TAG, "Also reachable at http://%s.local/", host.c_str());
        mdns_up = true;
      } else {
        ESP_LOGW(TAG, "mDNS unavailable; use the IP address");
      }
    }

    if (g_ap_active && g_ap_linger_timer) {
      ESP_LOGI(TAG, "Closing the setup network in %llds", kApLingerUs / 1000000);
      esp_timer_stop(g_ap_linger_timer);
      esp_timer_start_once(g_ap_linger_timer, kApLingerUs);
    }
  }
}

/* Answers every A query with our own address so any browser request lands on
   the setup page. Deliberately minimal: it only needs to fool a captive-portal
   probe, not to be a DNS server. */
void dns_task(void *) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed");
    vTaskDelete(nullptr);
    return;
  }

  /* The access point's own address, not INADDR_ANY. This server answers every
     query with 192.168.4.1, which is the point of a captive portal and a lie
     everywhere else: bound to all interfaces it would also answer on the home
     network, where anything that happened to ask it -- or a router probing for
     DNS rebinding -- would be told that every name resolves to the setup
     page. */
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr(kApAddress);
  addr.sin_port = htons(kDnsPort);
  if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  /* So the loop notices it has been asked to stop; without a timeout it would
     sit in recvfrom until the next query, which on a closed access point never
     comes. */
  timeval rcv_timeout = {.tv_sec = 0, .tv_usec = 500000};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

  uint8_t buf[512];
  while (!g_dns_stop) {
    sockaddr_in from = {};
    socklen_t from_len = sizeof(from);
    int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
    if (len < 12)
      continue;

    /* Only answer standard queries with exactly one question. */
    const uint16_t qdcount = static_cast<uint16_t>((buf[4] << 8) | buf[5]);
    if ((buf[2] & 0x80) || qdcount != 1)
      continue;

    /* Walk the QNAME labels to find where the question ends. */
    int pos = 12;
    while (pos < len && buf[pos] != 0) {
      if ((buf[pos] & 0xC0) == 0xC0) { pos += 2; break; }  // compression pointer
      pos += buf[pos] + 1;
    }
    if (pos >= len)
      continue;
    pos += 1 + 4;  // terminating zero + QTYPE + QCLASS
    if (pos > len || pos + 16 > static_cast<int>(sizeof(buf)))
      continue;

    buf[2] = 0x81;  // response, recursion desired
    buf[3] = 0x80;  // recursion available, no error
    buf[6] = 0; buf[7] = 1;  // one answer
    buf[8] = 0; buf[9] = 0;  // no authority
    buf[10] = 0; buf[11] = 0;  // no additional

    uint8_t *a = buf + pos;
    a[0] = 0xC0; a[1] = 0x0C;      // name: pointer to the question
    a[2] = 0x00; a[3] = 0x01;      // type A
    a[4] = 0x00; a[5] = 0x01;      // class IN
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 30;  // TTL 30 s
    a[10] = 0x00; a[11] = 0x04;    // rdlength
    const uint32_t ap = inet_addr(kApAddress);  // already network byte order
    std::memcpy(a + 12, &ap, 4);

    sendto(sock, buf, pos + 16, 0, reinterpret_cast<sockaddr *>(&from), from_len);
  }

  close(sock);
  vTaskDelete(nullptr);
}

}  // namespace

const char *to_string(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Connecting: return "connecting";
    case State::Connected: return "connected";
    case State::ApMode: return "ap-mode";
  }
  return "?";
}

bool begin(StateCallback cb) {
  g_cb = std::move(cb);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  g_sta_netif = esp_netif_create_default_wifi_sta();
  g_ap_netif = esp_netif_create_default_wifi_ap();

  /* Set once, right after the interface exists and long before DHCP runs, so
     the name is in the very first DHCP request rather than a later renewal. */
  const std::string host = appcfg::device().name;
  esp_err_t hn = esp_netif_set_hostname(g_sta_netif, host.c_str());
  if (hn != ESP_OK)
    ESP_LOGW(TAG, "Could not set hostname: %s", esp_err_to_name(hn));

  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr, nullptr));
  ESP_ERROR_CHECK(
      esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr, nullptr));

  /* Station only to begin with. The access point is not a permanent feature --
     it is opened when there is no other way in, and closed again once there is.
     Leaving it up would put a second, open network on the air for no reason.
     While it is up the mode is APSTA, so the station side keeps retrying in the
     background and the device rejoins by itself when the router returns. */
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  const esp_timer_create_args_t linger = {
      .callback = [](void *) { stop_ap(); },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_linger",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&linger, &g_ap_linger_timer);

  const esp_timer_create_args_t retry = {
      .callback =
          [](void *) {
            if (g_ap_active) {
              ESP_LOGI(TAG, "Retrying \"%s\"", appcfg::wifi().ssid.c_str());
              esp_wifi_connect();
            }
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "ap_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&retry, &g_ap_retry_timer);

  const esp_timer_create_args_t sta_retry = {
      .callback = [](void *) { esp_wifi_connect(); },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "sta_retry",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&sta_retry, &g_sta_retry_timer);

  /* Full power unless something has been learned. Upstream configurations carry
     a reduced setting as a workaround for appliance supplies that cannot deliver
     the transmit peak, but applying it pre-emptively costs range through the
     metal of an appliance to avoid a fault nobody here has observed -- and it
     would hide whether this supply is affected at all. A brownout is detected
     by name and steps this down; see diagnostics. */
  const int want = appcfg::tx_power_dbm();
  if (want > 0) {
    esp_wifi_set_max_tx_power(static_cast<int8_t>(want * 4));  // API takes quarter dBm
    ESP_LOGI(TAG, "Transmit power limited to %d dBm", want);
  }
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  ESP_ERROR_CHECK(esp_wifi_start());

  /* The C6 speaks 802.11ax, but the driver logs
       "11ax/11ac mode can not work under phy bw 40M, phymode changed to 11N"
     and silently drops to WiFi 4, because 40 MHz is the default on 2.4 GHz and
     HE needs 20. Pinning the bandwidth gets WiFi 6 back. Throughput is
     irrelevant here -- a few hundred bytes a second -- but HE is markedly more
     airtime-efficient, which matters on a busy home network. */
  esp_err_t bw = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
  if (bw != ESP_OK)
    ESP_LOGW(TAG, "Could not pin 20 MHz bandwidth: %s", esp_err_to_name(bw));

  esp_err_t pr = esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
                                                        WIFI_PROTOCOL_11AX);
  if (pr != ESP_OK) {
    ESP_LOGW(TAG, "Could not enable 802.11ax: %s", esp_err_to_name(pr));
  } else {
    uint8_t active = 0;
    esp_wifi_get_protocol(WIFI_IF_STA, &active);
    ESP_LOGI(TAG, "Station protocols: 0x%02x (11ax %s)", active,
             (active & WIFI_PROTOCOL_11AX) ? "enabled" : "NOT enabled");
  }

  try_connect();
  return true;
}

State state() { return g_state; }
std::string ip() { return g_ip; }

std::string ssid() {
  if (g_state == State::ApMode)
    return appcfg::device().name;
  return appcfg::wifi().ssid;
}

int8_t rssi() {
  wifi_ap_record_t ap;
  if (g_state == State::Connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
    return ap.rssi;
  return 0;
}

bool provisioning() { return g_state == State::ApMode; }

Time time_info() {
  Time t;
  t.synced = g_time_synced;
  if (!t.synced)
    return t;

  struct timeval tv = {};
  gettimeofday(&tv, nullptr);
  t.epoch_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
  /* Both clocks are read as close together as they can be, so the difference is
     the offset between them and not a measurement of how long this took. */
  t.boot_epoch_ms = t.epoch_ms - esp_timer_get_time() / 1000;
  return t;
}

}  // namespace appnet

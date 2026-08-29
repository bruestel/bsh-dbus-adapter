/*
   Publishing decoded appliance state to an MQTT broker.

   Values, and enough description for Home Assistant to build the device by
   itself: every reading is announced under a discovery topic, and withdrawn
   again when a profile change takes it away.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appmqtt/Mqtt.h"
#include "appcfg/Config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <mqtt_client.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cJSON.h>

#include <algorithm>
#include <vector>

namespace appmqtt {
namespace {

const char *const TAG = "appmqtt";

esp_mqtt_client_handle_t g_client = nullptr;
bool g_connected = false;
bool g_have_network = false;
std::string g_base;
std::vector<Reading> g_last;
Description g_device;
bool g_appliance_online = false;

/* Every discovery topic currently announced. Kept so the next announcement can
   clear the ones it no longer needs -- Home Assistant treats a retained
   discovery message as permanent, so an entity that vanishes from the profile
   would otherwise sit in the dashboard forever, unavailable and unexplained. */
std::vector<std::string> g_announced;

/* The published set is written by the application when a profile changes and
   read by the MQTT event task when a connection comes up or Home Assistant
   announces a restart. Those two moments coincide often enough to matter: a
   broker reconnect during a profile switch would walk a vector being replaced.

   Deliberately narrow. It covers the shared data and nothing else -- in
   particular it is never held across esp_mqtt_client_stop(), which waits for
   the very task that would be blocked on it. */
SemaphoreHandle_t data_lock() {
  static SemaphoreHandle_t h = xSemaphoreCreateMutex();
  return h;
}

struct Guard {
  Guard() { xSemaphoreTake(data_lock(), portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(data_lock()); }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
};

std::string topic_status() { return g_base + "/status"; }

/* One state, not two. "The adapter is gone" and "the appliance is unplugged"
   used to be separate topics, but MQTT allows a single last will per connection
   -- so only the first could be corrected when the device died, and on this
   board the two happen together anyway: it draws its power from the bus. A
   subscriber wants one answer regardless: is what I am reading current? */
std::string status_payload() { return g_appliance_online ? "online" : "offline"; }

/* Republished on a slow cycle even when nothing changed, so a consumer can tell
   a quiet appliance from a vanished adapter, and a broker that came back without
   its retained set is corrected within the minute rather than at the next time
   the dryer happens to do something. */
constexpr int64_t kStatusRefreshUs = 60 * 1000000;
int64_t g_status_at = 0;

/* Read once when the client starts rather than per frame: the setting lives in
   NVS, and this is consulted for every frame on the wire. */
bool g_raw_frames = false;

std::string topic_frame() { return g_base + "/frame"; }
std::string topic_state(const std::string &id) { return g_base + "/state/" + id; }

/* Both publishers read g_client and g_base, which the caller must therefore
   have locked. Taking the lock in here instead would deadlock: publish_everything
   already holds it and calls pub() for every reading. */
void pub_stream(const std::string &topic, const std::string &payload) {
  if (!g_client || !g_connected)
    return;
  esp_mqtt_client_publish(g_client, topic.c_str(), payload.data(),
                          static_cast<int>(payload.size()), 0, 0);
}

void pub(const std::string &topic, const std::string &payload, bool retain) {
  if (!g_client || !g_connected)
    return;
  esp_mqtt_client_publish(g_client, topic.c_str(), payload.data(),
                          static_cast<int>(payload.size()), 1, retain ? 1 : 0);
}

/* Home Assistant's component name for one of our entity kinds. Anything
   unrecognised becomes a plain sensor, which displays a value rather than
   refusing to appear. */
const char *component_for(const std::string &kind) {
  if (kind == "binary_sensor")
    return "binary_sensor";
  return "sensor";
}

std::string discovery_topic(const std::string &prefix, const std::string &kind,
                            const std::string &entity_id) {
  return prefix + "/" + component_for(kind) + "/" + g_device.device_id + "/" + entity_id +
         "/config";
}

void publish_discovery(const Reading &r, const std::string &prefix) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "name", r.name.c_str());
  cJSON_AddStringToObject(o, "uniq_id", (g_device.device_id + "_" + r.id).c_str());
  cJSON_AddStringToObject(o, "stat_t", topic_state(r.id).c_str());

  if (!r.unit.empty())
    cJSON_AddStringToObject(o, "unit_of_meas", r.unit.c_str());
  if (!r.device_class.empty())
    cJSON_AddStringToObject(o, "dev_cla", r.device_class.c_str());
  /* Without this Home Assistant records no history for a number -- it shows the
     current value and forgets it. */
  if (!r.state_class.empty())
    cJSON_AddStringToObject(o, "stat_cla", r.state_class.c_str());
  if (!r.entity_category.empty())
    cJSON_AddStringToObject(o, "ent_cat", r.entity_category.c_str());
  if (!r.icon.empty())
    cJSON_AddStringToObject(o, "ic", r.icon.c_str());

  /* A binary sensor has to be told which payloads mean what, and told the
     truth: these are the words to_string() actually writes for a boolean. The
     earlier version of this file declared "true" and "false" while the decoder
     published ON and OFF, so every binary sensor stayed off whatever the
     appliance said. Two places, one fact -- so if the rendering ever moves,
     it moves here too. */
  if (r.kind == "binary_sensor") {
    cJSON_AddStringToObject(o, "pl_on", "On");
    cJSON_AddStringToObject(o, "pl_off", "Off");
  }

  /* One availability topic, carrying whether the appliance is talking. An
     unplugged machine and a dead adapter are the same answer to the only
     question a dashboard is asking: is this current? */
  cJSON_AddStringToObject(o, "avty_t", topic_status().c_str());

  cJSON *dev = cJSON_AddObjectToObject(o, "dev");
  cJSON *ids = cJSON_AddArrayToObject(dev, "ids");
  cJSON_AddItemToArray(ids, cJSON_CreateString(g_device.device_id.c_str()));
  cJSON_AddStringToObject(dev, "name", g_device.device_name.c_str());
  if (!g_device.model.empty())
    cJSON_AddStringToObject(dev, "mdl", g_device.model.c_str());
  if (!g_device.manufacturer.empty())
    cJSON_AddStringToObject(dev, "mf", g_device.manufacturer.c_str());
  if (!g_device.firmware.empty())
    cJSON_AddStringToObject(dev, "sw", g_device.firmware.c_str());

  char *text = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  if (!text)
    return;

  pub(discovery_topic(prefix, r.kind, r.id), text, true);
  cJSON_free(text);
}

/* Everything that has to happen the moment a connection exists, and again
   whenever Home Assistant says it has restarted. The caller holds the lock. */
void publish_everything() {
  const appcfg::Mqtt cfg = appcfg::mqtt();

  pub(topic_status(), status_payload(), true);
  g_status_at = esp_timer_get_time();

  if (cfg.discovery && !g_last.empty()) {
    std::vector<std::string> wanted;
    wanted.reserve(g_last.size());
    for (const Reading &r : g_last) {
      publish_discovery(r, cfg.discovery_prefix);
      wanted.push_back(discovery_topic(cfg.discovery_prefix, r.kind, r.id));
    }

    /* Clear what used to be announced and no longer is. An empty retained
       payload on a discovery topic is how Home Assistant is told to forget an
       entity; without this, changing profile leaves the old one behind. */
    for (const std::string &old : g_announced)
      if (std::find(wanted.begin(), wanted.end(), old) == wanted.end())
        pub(old, "", true);

    g_announced = std::move(wanted);
  }

  for (const Reading &r : g_last)
    pub(topic_state(r.id), r.available ? r.value : "", true);
}

void on_event(void *, esp_event_base_t, int32_t id, void *data) {
  auto *ev = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED: {
      g_connected = true;
      ESP_LOGI(TAG, "Connected to the broker");
      const appcfg::Mqtt cfg = appcfg::mqtt();
      if (cfg.discovery) {
        /* Home Assistant announces its own restarts here, and forgets every
           discovered entity when it does. Re-announcing on that message is what
           keeps the appliance from disappearing after a restart. */
        const std::string t = cfg.discovery_prefix + "/status";
        esp_mqtt_client_subscribe(g_client, t.c_str(), 0);
      }
      {
        Guard g;
        publish_everything();
      }
      break;
    }
    case MQTT_EVENT_DATA: {
      const std::string payload(ev->data, ev->data_len);
      if (payload == "online") {
        ESP_LOGI(TAG, "Home Assistant restarted; re-announcing");
        Guard g;
        publish_everything();
      }
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      g_connected = false;
      ESP_LOGW(TAG, "Disconnected from the broker");
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGW(TAG, "Broker error");
      break;
    default:
      break;
  }
}

void stop() {
  esp_mqtt_client_handle_t client = nullptr;
  {
    /* The handle is cleared under the lock and destroyed outside it. A
       publisher racing this now finds a null handle and returns, instead of
       passing a pointer that is being freed underneath it -- the window
       between checking g_client and using it was real, because this runs on
       the network event task while publish() runs on the main loop.

       The destroy must not happen under the lock: it waits for the MQTT task,
       which may itself be blocked here on a connect notice. */
    Guard g;
    if (!g_client)
      return;
    /* The last will covers a crash; a deliberate stop can say goodbye properly. */
    if (g_connected)
      pub(topic_status(), "offline", true);
    client = g_client;
    g_client = nullptr;
    g_connected = false;
  }
  esp_mqtt_client_stop(client);
  esp_mqtt_client_destroy(client);
}

bool start() {
  const appcfg::Mqtt cfg = appcfg::mqtt();
  if (!cfg.enabled || !cfg.configured() || !g_have_network)
    return false;

  {
    /* Read by the publishers on other tasks, so assigned under the lock rather
       than while somebody is building a topic out of them. */
    Guard g;
    g_base = appcfg::mqtt_base();
    g_raw_frames = cfg.raw_frames;
  }

  /* Held by name for as long as the config struct: esp-mqtt copies these on
     init, but only while the pointers are still good. */
  const std::string uri = cfg.uri();
  const std::string client_id = appcfg::mqtt_client_id();

  esp_mqtt_client_config_t c = {};
  c.broker.address.uri = uri.c_str();
  if (cfg.auth) {
    c.credentials.username = cfg.user.c_str();
    c.credentials.authentication.password = cfg.password.c_str();
  }
  c.credentials.client_id = client_id.empty() ? nullptr : client_id.c_str();

  /* With a certificate the server is checked against it; without one esp-tls
     verifies nothing at all, which is exactly what the insecure setting asks
     for -- and why the two are a single choice in the interface rather than two
     switches that can contradict each other. */
  if (cfg.tls && !cfg.tls_insecure && !cfg.ca_cert.empty()) {
    c.broker.verification.certificate = cfg.ca_cert.c_str();
    c.broker.verification.certificate_len = cfg.ca_cert.size() + 1;  // counted with its terminator
  }

  /* The will is the only thing that speaks for a device that has stopped
     speaking. Retained, so it is still there for whoever asks next. */
  const std::string will = topic_status();
  c.session.last_will.topic = will.c_str();
  c.session.last_will.msg = "offline";
  c.session.last_will.msg_len = 7;
  c.session.last_will.qos = 1;
  c.session.last_will.retain = 1;

  /* Built and wired up before it is published to the other tasks: until the
     assignment below, a publisher sees no client and does nothing, which is
     the right answer while one is still being constructed. */
  esp_mqtt_client_handle_t client = esp_mqtt_client_init(&c);
  if (!client) {
    ESP_LOGE(TAG, "Could not create the client");
    return false;
  }
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, on_event, nullptr);
  {
    Guard g;
    g_client = client;
  }
  if (esp_mqtt_client_start(client) != ESP_OK) {
    ESP_LOGE(TAG, "Could not start the client");
    {
      Guard g;
      g_client = nullptr;
    }
    esp_mqtt_client_destroy(client);
    return false;
  }
  ESP_LOGI(TAG, "Connecting to %s as %s, topics under %s", uri.c_str(), client_id.c_str(),
           g_base.c_str());
  return true;
}

}  // namespace

bool begin() {
  Guard g;
  g_base = appcfg::mqtt_base();
  return true;
}

void on_network(bool connected) {
  g_have_network = connected;
  if (connected)
    start();
  else
    stop();
}

bool connected() { return g_connected; }
bool enabled() { return appcfg::mqtt().enabled; }

void reconfigure() {
  stop();
  start();
}

void announce(const Description &device, const Reading *readings, size_t count) {
  Guard g;
  g_device = device;
  g_last.assign(readings, readings + count);
  if (g_connected)
    publish_everything();
}

void publish(const Reading &r) {
  Guard g;
  /* Kept even while disconnected, so a reconnect republishes the current state
     rather than an empty device that fills in only as the appliance happens to
     change. */
  auto it = std::find_if(g_last.begin(), g_last.end(),
                         [&](const Reading &x) { return x.id == r.id; });
  if (it != g_last.end())
    *it = r;

  /* Inside the lock, not after it: the topic is built from g_base and the
     handle is read by pub(). */
  pub(topic_state(r.id), r.available ? r.value : "", true);
}

void publish_raw_frame(const std::string &hex) {
  Guard g;
  if (!g_raw_frames || !g_connected)
    return;
  pub_stream(topic_frame(), hex);
}

void set_appliance_online(bool online) {
  Guard g;
  const bool changed = (online != g_appliance_online);
  g_appliance_online = online;
  if (!g_connected)
    return;
  const int64_t now = esp_timer_get_time();
  if (changed || (now - g_status_at) >= kStatusRefreshUs) {
    g_status_at = now;
    pub(topic_status(), status_payload(), true);
  }
}

std::string status_json() {
  const appcfg::Mqtt cfg = appcfg::mqtt();
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "enabled", cfg.enabled);
  cJSON_AddBoolToObject(o, "connected", g_connected);
  cJSON_AddStringToObject(o, "uri", cfg.uri().c_str());
  cJSON_AddBoolToObject(o, "tls", cfg.tls);
  cJSON_AddStringToObject(o, "host", cfg.host.c_str());
  cJSON_AddNumberToObject(o, "port", cfg.port);
  cJSON_AddStringToObject(o, "client_id", cfg.client_id.c_str());
  /* What the broker will actually see, so the placeholder can show the default
     without the page having to work out what it would be. */
  cJSON_AddStringToObject(o, "client_id_default", appcfg::mqtt_client_id().c_str());
  cJSON_AddBoolToObject(o, "auth", cfg.auth);
  cJSON_AddStringToObject(o, "user", cfg.user.c_str());
  cJSON_AddBoolToObject(o, "has_password", !cfg.password.empty());
  cJSON_AddBoolToObject(o, "tls_insecure", cfg.tls_insecure);
  /* A certificate authority is public by nature -- it is sent back so the page
     can show and edit what is stored rather than make it type it again. */
  cJSON_AddStringToObject(o, "ca_cert", cfg.ca_cert.c_str());
  cJSON_AddStringToObject(o, "base", appcfg::mqtt_base().c_str());
  cJSON_AddBoolToObject(o, "raw_frames", cfg.raw_frames);
  cJSON_AddBoolToObject(o, "discovery", cfg.discovery);
  cJSON_AddStringToObject(o, "discovery_prefix", cfg.discovery_prefix.c_str());

  char *text = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  std::string out = text ? text : "{}";
  if (text)
    cJSON_free(text);
  return out;
}

}  // namespace appmqtt

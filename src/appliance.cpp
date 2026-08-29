/*
   The appliance layer.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appliance.h"

#include "appcfg/Config.h"
#include "appprofiles/Profiles.h"
#include "appstore/Store.h"
#include "dbus2/decoder/ProfileParser.h"

#include <esp_log.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <unordered_map>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace appliance {
namespace {

const char *const TAG = "appliance";

/* Everything below is reached from two tasks: the main loop feeds frames in and
   ticks the decoder, while the HTTP server answers requests and, on a profile
   change, replaces the decoder's entities and state wholesale. Without this
   lock a profile switch during bus traffic would be reallocating the vectors
   another task is walking.

   Recursive on purpose. Activating a profile calls the profile hook, which asks
   for the readings of the profile just loaded, so the lock is legitimately
   taken twice on one path. A plain mutex would deadlock there.

   It lives here rather than in the decoder because the decoder is deliberately
   free of ESP-IDF, which is what makes it testable on a host.

   One ordering rule keeps this deadlock-free: appliance may call into MQTT --
   the reading sink and the profile hook both do -- and MQTT never calls back
   here. So the two locks are only ever taken in that direction. */
SemaphoreHandle_t state_lock() {
  static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
  return h;
}

struct Guard {
  Guard() { xSemaphoreTakeRecursive(state_lock(), portMAX_DELAY); }
  ~Guard() { xSemaphoreGiveRecursive(state_lock()); }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
};

using namespace dbus2::decoder;

Engine g_engine;
ReadingSink g_sink;
std::function<void()> g_profile_hook;
const bsh_profile_t *g_active = nullptr;

/* The user's own profile, if one is stored.

   It is presented as an ordinary bsh_profile_t so everything downstream --
   activation, the readings, the layout the monitor draws from -- treats it
   exactly like a built-in one. The strings must outlive the record, hence the
   separate members rather than pointers into a temporary. */
/* Stored profiles are addressed as "custom:<slug>". The prefix keeps them from
   ever colliding with a built-in id, and makes it obvious at a glance -- in a
   log line, in NVS, in a URL -- which of the two kinds is meant. */
const char *const kCustomPrefix = "custom:";

bool is_custom(const std::string &id) { return id.rfind(kCustomPrefix, 0) == 0; }
std::string slug_of(const std::string &id) { return id.substr(std::string(kCustomPrefix).size()); }

std::string g_custom_id, g_custom_json, g_custom_model, g_custom_product, g_custom_appliance;
bsh_profile_t g_custom_rec{};

/* Every distinct dest/cmd seen since boot, kept even while a profile is loaded
   so a bad guess can be spotted and corrected later. */
std::vector<uint32_t> g_observed;

void note_pair(uint8_t dest, uint16_t cmd) {
  const uint32_t k = (static_cast<uint32_t>(dest) << 16) | cmd;
  if (std::find(g_observed.begin(), g_observed.end(), k) == g_observed.end()) {
    /* A machine speaking an unrecognised dialect must not be able to grow this
       without bound while nobody is looking. */
    if (g_observed.size() < 128)
      g_observed.push_back(k);
  }
}

bool load(const bsh_profile_t *p, std::string *error) {
  auto res = parse_profile(p->json);
  if (!res.ok) {
    if (error) {
      *error = res.errors.empty() ? "profile could not be parsed" : res.errors.front();
      for (const auto &e : res.errors)
        ESP_LOGE(TAG, "%s: %s", p->id, e.c_str());
    }
    return false;
  }
  g_engine.load(std::move(res.profile));
  g_active = p;
  ESP_LOGI(TAG, "Profile \"%s\" active: %u entities", p->id, p->entities);
  if (g_profile_hook)
    g_profile_hook();
  return true;
}

/* Reads, validates and activates the stored profile. Validation happens before
   anything is replaced, so a broken profile leaves the running one alone
   instead of taking the appliance down with it. */
bool load_custom(const std::string &id, std::string *error) {
  std::string json = appstore::load(slug_of(id));
  if (json.empty()) {
    if (error)
      *error = "no such profile is stored on this device";
    return false;
  }

  auto res = parse_profile(json.c_str());
  if (!res.ok) {
    if (error)
      *error = res.errors.empty() ? "profile could not be parsed" : res.errors.front();
    for (const auto &e : res.errors)
      ESP_LOGE(TAG, "%s: %s", id.c_str(), e.c_str());
    return false;
  }

  /* Read the metadata before the profile is moved into the engine. */
  g_custom_model = res.profile.meta.model.empty() ? slug_of(id) : res.profile.meta.model;
  g_custom_product = res.profile.meta.product;
  g_custom_appliance = res.profile.meta.appliance;
  g_custom_json = std::move(json);
  g_custom_id = id;

  g_custom_rec = {};
  g_custom_rec.id = g_custom_id.c_str();
  g_custom_rec.model = g_custom_model.c_str();
  g_custom_rec.product = g_custom_product.c_str();
  g_custom_rec.appliance = g_custom_appliance.c_str();
  g_custom_rec.entities = static_cast<unsigned>(res.profile.entities.size());
  g_custom_rec.json = g_custom_json.c_str();
  g_custom_rec.json_len = static_cast<unsigned>(g_custom_json.size());

  ESP_LOGI(TAG, "Profile \"%s\" active: %u entities", id.c_str(), g_custom_rec.entities);
  g_engine.load(std::move(res.profile));
  g_active = &g_custom_rec;
  if (g_profile_hook)
    g_profile_hook();
  return true;
}

}  // namespace

void set_sink(ReadingSink sink) {
  Guard g;
  g_sink = std::move(sink);
  /* Translated here so nothing downstream has to know about decoder types. */
  g_engine.set_sink([](const Entity &e, const Value &v, bool available) {
    if (!g_sink)
      return;
    Reading r;
    r.id = e.id;
    r.name = e.name;
    r.kind = to_string(e.kind);
    r.available = available;
    r.value = available ? to_string(v, e.ha.decimals) : "";
    r.unit = e.ha.unit;
    r.device_class = e.ha.device_class;
    r.state_class = e.ha.state_class;
    r.entity_category = e.ha.entity_category;
    r.icon = e.ha.icon;
    g_sink(r);
  });
}

void set_profile_hook(std::function<void()> hook) {
  Guard g;
  g_profile_hook = std::move(hook);
}

void begin() {
  Guard g;
  const std::string want = appcfg::active_profile();
  if (want.empty()) {
    ESP_LOGI(TAG, "No profile chosen; listening to identify the appliance");
    return;
  }
  /* Before there were several profiles there was one, stored as profile.json
     and selected as plain "custom". That file's name is a valid slug, so it
     lists and loads unchanged -- only the stored selection needs pointing at
     its new id, or a device that upgrades would come up with no profile and no
     explanation. */
  if (want == "custom") {
    const std::string migrated = std::string(kCustomPrefix) + "profile";
    if (appstore::exists("profile")) {
      ESP_LOGI(TAG, "Migrating the stored selection to \"%s\"", migrated.c_str());
      appcfg::set_active_profile(migrated);
      std::string err;
      if (!load_custom(migrated, &err))
        ESP_LOGE(TAG, "Could not load the migrated profile: %s", err.c_str());
      return;
    }
  }

  if (is_custom(want)) {
    std::string err;
    if (!load_custom(want, &err))
      ESP_LOGE(TAG, "Could not load the stored profile: %s", err.c_str());
    return;
  }

  const bsh_profile_t *p = appprofiles::find(want);
  if (!p) {
    ESP_LOGW(TAG, "Stored profile \"%s\" is not in this firmware", want.c_str());
    return;
  }
  std::string err;
  if (!load(p, &err))
    ESP_LOGE(TAG, "Could not load \"%s\": %s", want.c_str(), err.c_str());
}

void on_frame(const dbus2::Frame &f, int64_t rx_time_us) {
  Guard g;
  if (!f.has_command())
    return;  /* Frames without a command carry nothing a profile can address. */
  note_pair(f.dest, f.command());
  g_engine.on_frame(f.dest, f.command(), f.payload(), rx_time_us);
}

void tick(int64_t now_us) {
  Guard g;
  g_engine.tick(now_us);
}

bool has_profile() {
  Guard g;
  return g_active != nullptr;
}
std::string profile_id() {
  Guard g;
  return g_active ? g_active->id : "";
}
std::string profile_model() {
  Guard g;
  return g_active ? g_active->model : "";
}
std::string profile_product() {
  Guard g;
  return (g_active && g_active->product) ? g_active->product : "";
}
std::string profile_appliance() {
  Guard g;
  return g_active ? g_active->appliance : "";
}
bool online() {
  Guard g;
  return g_engine.appliance_online(esp_timer_get_time());
}
size_t observed_pairs() {
  Guard g;
  return g_observed.size();
}

std::vector<Reading> readings() {
  Guard g;
  std::vector<Reading> out;
  if (!g_engine.loaded())
    return out;

  const int64_t now = esp_timer_get_time();
  const auto &prof = g_engine.profile();
  const auto &states = g_engine.states();

  out.reserve(prof.entities.size());
  for (size_t i = 0; i < prof.entities.size(); i++) {
    const Entity &e = prof.entities[i];
    /* A hidden marker decodes nothing -- it exists to keep a frame out of the
       monitor. Listing it here would put a reading
       that can never have a value on the page, and announce it to Home
       Assistant as an entity that stays forever unavailable. */
    if (e.kind == Kind::Hidden)
      continue;
    const EntityState &st = states[i];
    Reading r;
    r.id = e.id;
    r.name = e.name;
    r.kind = to_string(e.kind);
    r.available = st.available;
    r.value = st.available ? to_string(st.value, e.ha.decimals) : "";
    r.unit = e.ha.unit;
    r.device_class = e.ha.device_class;
    r.state_class = e.ha.state_class;
    r.entity_category = e.ha.entity_category;
    r.icon = e.ha.icon;
    r.age_s = st.last_seen_us ? (now - st.last_seen_us) / 1000000 : -1;
    r.estimated = st.estimated;
    out.push_back(std::move(r));
  }
  return out;
}

std::vector<Candidate> candidates() {
  Guard g;
  std::vector<Candidate> out;
  for (const auto &m : appprofiles::rank(g_observed)) {
    Candidate c;
    c.id = m.profile->id;
    c.model = m.profile->model;
    c.product = m.profile->product ? m.profile->product : "";
    c.appliance = m.profile->appliance;
    c.hits = m.hits;
    c.expected = m.expected;
    c.entities = m.profile->entities;
    out.push_back(std::move(c));
    if (out.size() >= 8)
      break;  /* Nobody reads past the first handful. */
  }
  return out;
}

bool activate(const std::string &id, std::string *error) {
  Guard g;
  if (is_custom(id)) {
    if (!load_custom(id, error))
      return false;
    appcfg::set_active_profile(id);
    return true;
  }

  const bsh_profile_t *p = appprofiles::find(id);
  if (!p) {
    if (error)
      *error = "no such profile";
    return false;
  }
  if (!load(p, error))
    return false;
  appcfg::set_active_profile(id);
  return true;
}



/* The active profile exactly as the parser sees it.

   The editor needs something to start from, and starting from the profile
   already running beats starting from a blank document: the frames, the byte
   offsets and the value tables that do work are kept, and the user edits the
   one rule that does not. */
std::string active_source() {
  Guard g;
  if (!g_active || !g_active->json)
    return {};
  return std::string(g_active->json, g_active->json_len);
}

/* Any profile the device holds, by id -- built into the firmware or written by
   the user -- so the editor can show what one actually says instead of only its
   name and entity count. */
std::string source_of(const std::string &id) {
  Guard g;
  if (id.empty())
    return active_source();
  if (is_custom(id))
    return appstore::load(slug_of(id));
  const bsh_profile_t *p = appprofiles::find(id);
  if (!p || !p->json)
    return {};
  return std::string(p->json, p->json_len);
}

/* The user's own profiles, newest metadata read straight from the files. There
   are at most a handful and this is only asked for when somebody is looking at
   the list, so parsing on demand beats keeping an index in step with the flash. */
/* Deliberately not under the lock, and this is worth reading before adding one
   back. Listing the stored profiles reads and parses every file in the store,
   several kilobytes each, and it is called from the HTTP task. Held under the
   appliance lock, that stalled on_frame and tick in the main loop for the whole
   time -- long enough, with a full store, to overrun the sixteen-entry bus
   queue and lose frames to answer a page request.

   It is safe without: nothing here touches the decoder, the active profile or
   the observed addresses. It reads the profile store, which is reached from the
   HTTP task and, once at startup, before that task exists. */
std::vector<Stored> stored() {
  std::vector<Stored> out;
  for (const std::string &slug : appstore::list()) {
    Stored s;
    s.id = std::string(kCustomPrefix) + slug;
    s.slug = slug;
    const std::string json = appstore::load(slug);
    auto res = parse_profile(json.c_str());
    s.valid = res.ok;
    if (res.ok) {
      s.model = res.profile.meta.model.empty() ? slug : res.profile.meta.model;
      s.product = res.profile.meta.product;
      s.appliance = res.profile.meta.appliance;
      s.entities = res.profile.entities.size();
    } else {
      /* Listed anyway: a profile that no longer parses is exactly the one the
         user needs to find in order to fix or delete it. */
      s.model = slug;
      s.appliance = "unknown";
    }
    out.push_back(std::move(s));
  }
  return out;
}

/* Validated, then stored, then activated -- in that order.

   Parsing first means a profile that cannot be read never reaches the flash. If
   it did, the next boot would find a stored profile it cannot load, report
   nothing, and give the user no way to tell a broken file from a silent bus. */
bool save_custom(const std::string &id, const std::string &json, bool activate_it,
                 std::string *error) {
  Guard g;
  auto res = parse_profile(json.c_str());
  if (!res.ok) {
    if (error)
      *error = res.errors.empty() ? "profile could not be parsed" : res.errors.front();
    return false;
  }
  if (res.profile.entities.empty()) {
    if (error)
      *error = "profile defines no entities";
    return false;
  }

  const std::string slug = is_custom(id) ? slug_of(id) : id;
  if (!appstore::save(slug, json, error))
    return false;

  /* Saving a profile that is not the running one must not disturb it: editing a
     spare copy while the machine is being watched is the normal case. */
  const std::string full = std::string(kCustomPrefix) + slug;
  if (!activate_it && g_custom_id != full)
    return true;

  if (!load_custom(full, error))
    return false;
  appcfg::set_active_profile(full);
  return true;
}

bool erase_custom(const std::string &id, std::string *error) {
  Guard g;
  const std::string slug = is_custom(id) ? slug_of(id) : id;
  const bool was_active = (g_active == &g_custom_rec) &&
                          g_custom_id == std::string(kCustomPrefix) + slug;
  if (!appstore::erase(slug)) {
    if (error)
      *error = "nothing stored to erase";
    return false;
  }
  /* Leaving a deleted profile running would be the worst of both: the readings
     would keep updating from a file that no longer exists. */
  if (was_active)
    clear_profile();
  return true;
}

void clear_profile() {
  Guard g;
  g_active = nullptr;
  g_engine.load({});
  appcfg::set_active_profile("");
  ESP_LOGI(TAG, "Profile cleared");
  if (g_profile_hook)
    g_profile_hook();
}

namespace {

/* Small hand-rolled JSON writer. cJSON is available, but these payloads are
   flat and fixed-shape, and building trees only to flatten them again would cost
   more heap than the strings themselves during a burst of polling. */
void esc(std::string &out, const std::string &s) {
  for (char c : s) {
    if (c == '"' || c == '\\')
      out += '\\';
    if (static_cast<unsigned char>(c) < 0x20)
      continue;
    out += c;
  }
}

void field(std::string &out, const char *key, const std::string &value, bool last = false) {
  out += '"';
  out += key;
  out += "\":\"";
  esc(out, value);
  out += last ? "\"" : "\",";
}

void number(std::string &out, const char *key, long long value, bool last = false) {
  out += '"';
  out += key;
  out += "\":";
  out += std::to_string(value);
  if (!last)
    out += ',';
}

}  // namespace

std::string profile_json() {
  Guard g;
  std::string o = "{";
  field(o, "id", profile_id());
  field(o, "model", profile_model());
  field(o, "product", profile_product());
  field(o, "appliance", profile_appliance());
  o += std::string("\"loaded\":") + (has_profile() ? "true" : "false") + ",";
  o += std::string("\"online\":") + (online() ? "true" : "false") + ",";
  number(o, "observed_pairs", static_cast<long long>(observed_pairs()), true);
  o += "}";
  return o;
}

/* How many payload bytes an operation consumes, so the monitor can mark a
   two-byte reading against both of its bytes instead of only the first. */
uint8_t extract_width(const Extract &e) {
  switch (e.op) {
    case Op::U16BE: case Op::U16LE: case Op::I16BE: case Op::I16LE: return 2;
    case Op::U24BE: return 3;
    case Op::U32BE: return 4;
    case Op::Bytes: case Op::Ascii: return e.len;
    case Op::Const: case Op::Len: return 0;
    default: return 1;
  }
}

/* What the active profile knows about each frame, keyed "DD.CCCC" in hex.

   The monitor uses this to write meaning beside the bytes: which entity reads a
   given frame, and which byte of the payload it reads. That mapping already
   exists in the profile -- it is exactly what makes decoding possible -- it was
   simply never visible from outside. Sending it once and letting the browser
   index it keeps it off the per-frame path, which has to stay cheap. */
std::string layout_json() {
  Guard g;
  std::string o = "{";
  if (!g_engine.loaded())
    return o + "}";

  /* Grouped by frame rather than listed flat, because that is the question the
     monitor asks: this frame arrived, who has something to say about it. */
  std::unordered_map<uint32_t, std::vector<const Entity *>> by_frame;
  for (const auto &e : g_engine.profile().entities)
    by_frame[(static_cast<uint32_t>(e.dest) << 16) | e.cmd].push_back(&e);

  bool first_frame = true;
  for (const auto &[key, list] : by_frame) {
    char label[16];
    std::snprintf(label, sizeof(label), "%02X.%04X",
                  static_cast<unsigned>(key >> 16), static_cast<unsigned>(key & 0xFFFF));
    if (!first_frame)
      o += ',';
    first_frame = false;
    o += '"';
    o += label;
    o += "\":[";

    bool first = true;
    for (const Entity *e : list) {
      if (!first)
        o += ',';
      first = false;
      o += "{";
      field(o, "id", e->id);
      field(o, "name", e->name);
      field(o, "kind", to_string(e->kind));
      field(o, "op", to_string(e->extract.op));
      number(o, "byte", e->extract.at);
      /* The monitor needs the payload matcher to fold away a hidden frame by its
         data bytes, not merely its address. width is the last field unless a
         payload follows it. */
      number(o, "width", extract_width(e->extract), e->match_payload.empty());
      if (!e->match_payload.empty()) {
        std::string ph;
        char b2[3];
        for (uint8_t b : e->match_payload) {
          std::snprintf(b2, sizeof(b2), "%02X", b);
          ph += b2;
        }
        field(o, "payload", ph, true);
      }
      o += "}";
    }
    o += "]";
  }
  return o + "}";
}

std::string readings_json() {
  Guard g;
  std::string o = "[";
  bool first = true;
  for (const auto &r : readings()) {
    if (!first)
      o += ',';
    first = false;
    o += "{";
    field(o, "id", r.id);
    field(o, "name", r.name);
    field(o, "kind", r.kind);
    field(o, "value", r.value);
    field(o, "unit", r.unit);
    field(o, "device_class", r.device_class);
    field(o, "state_class", r.state_class);
    field(o, "entity_category", r.entity_category);
    field(o, "icon", r.icon);
    o += std::string("\"available\":") + (r.available ? "true" : "false") + ",";
    o += std::string("\"estimated\":") + (r.estimated ? "true" : "false") + ",";
    number(o, "age_s", r.age_s, true);
    o += "}";
  }
  o += "]";
  return o;
}

/* How full the profile store is. Not decoration: the limits are invisible
   otherwise, and the first time anyone learns about them would be a refused
   save. */
std::string storage_json() {
  const appstore::Usage u = appstore::usage();
  std::string o = "{";
  o += std::string("\"available\":") + (appstore::available() ? "true" : "false") + ",";
  number(o, "count", static_cast<long long>(u.count));
  number(o, "max_count", static_cast<long long>(appstore::kMaxProfiles));
  number(o, "used", static_cast<long long>(u.used));
  number(o, "total", static_cast<long long>(u.total));
  number(o, "max_bytes", static_cast<long long>(appstore::kMaxProfileBytes), true);
  o += "}";
  return o;
}

std::string stored_json() {
  std::string o = "[";
  bool first = true;
  for (const auto &s : stored()) {
    if (!first)
      o += ',';
    first = false;
    o += "{";
    field(o, "id", s.id);
    field(o, "slug", s.slug);
    field(o, "model", s.model);
    field(o, "product", s.product);
    field(o, "appliance", s.appliance);
    o += std::string("\"valid\":") + (s.valid ? "true" : "false") + ",";
    number(o, "entities", static_cast<long long>(s.entities), true);
    o += "}";
  }
  o += "]";
  return o;
}

/* Built-in and stored profiles in one list, because from the point of view of
   "which profile should this device use" they are the same kind of thing. The
   `stored` flag is what tells them apart where it matters: only a stored one
   can be edited, downloaded or deleted. */
/* No lock for the same reason as stored(): the built-in table is immutable and
   the rest comes from the store. */
std::string catalogue_json() {
  std::string o = "[";
  /* Counted across both loops rather than from the index of either: with no
     built-in profiles the second loop used to open with a comma and produce
     "[,{...}]". Nothing ships without profiles today, which is exactly why
     nobody would notice. */
  bool first = true;
  for (unsigned i = 0; i < bsh_profile_count; i++) {
    const bsh_profile_t &p = bsh_profiles[i];
    if (!first)
      o += ',';
    first = false;
    o += "{";
    field(o, "id", p.id);
    field(o, "model", p.model);
    field(o, "product", p.product ? p.product : "");
    field(o, "appliance", p.appliance);
    number(o, "entities", p.entities);
    number(o, "pairs", p.signature_len);
    o += std::string("\"generic\":") + (p.generic ? "true" : "false");
    o += "}";
  }

  /* The user's own profiles belong in the same list: from the point of view of
     "which profile should this device use", where one came from is beside the
     point. The flag is what tells them apart where it matters -- only a stored
     one can be edited, downloaded or deleted. */
  for (const auto &s : stored()) {
    if (!first)
      o += ',';
    first = false;
    o += "{";
    field(o, "id", s.id);
    field(o, "model", s.model);
    field(o, "product", s.product);
    field(o, "appliance", s.appliance);
    number(o, "entities", static_cast<long long>(s.entities));
    number(o, "pairs", 0);
    o += "\"generic\":false,\"stored\":true,";
    o += std::string("\"valid\":") + (s.valid ? "true" : "false");
    o += "}";
  }

  o += "]";
  return o;
}

std::string candidates_json() {
  Guard g;
  std::string o = "[";
  bool first = true;
  for (const auto &c : candidates()) {
    if (!first)
      o += ',';
    first = false;
    o += "{";
    field(o, "id", c.id);
    field(o, "model", c.model);
    field(o, "product", c.product);
    field(o, "appliance", c.appliance);
    number(o, "hits", c.hits);
    number(o, "expected", c.expected);
    number(o, "entities", c.entities, true);
    o += "}";
  }
  o += "]";
  return o;
}

}  // namespace appliance

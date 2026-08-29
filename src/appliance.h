/*
   The appliance layer: which profile is active, what the values are, and what
   is on the bus that no profile explains.

   Sits between the bus and everything that wants meaning rather than bytes --
   the web interface today, MQTT next.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/Frame.h"
#include "dbus2/decoder/Engine.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace appliance {

struct Reading {
  std::string id;
  std::string name;
  std::string kind;
  std::string value;
  std::string unit;
  std::string device_class;
  std::string state_class;
  std::string entity_category;
  std::string icon;
  bool available = false;
  int64_t age_s = 0;
  /* The engine carried this value forward itself rather than hearing it from
     the appliance -- see Behavior::countdown_every_s. */
  bool estimated = false;
};

struct Candidate {
  std::string id;
  std::string model;
  std::string product;
  std::string appliance;
  unsigned hits = 0;
  unsigned expected = 0;
  unsigned entities = 0;
};

void begin();

/* Called whenever a reading changes or goes stale. The decoder already knows
   the moment either happens, so publishing from here beats polling for a
   difference -- MQTT is meant to carry changes, not a heartbeat of sameness. */
using ReadingSink = std::function<void(const Reading &)>;
void set_sink(ReadingSink sink);

/* Called when the set of readings itself changes, i.e. a different profile was
   loaded. Whoever announces entities elsewhere has to redo it. */
void set_profile_hook(std::function<void()> hook);

/* Called for every frame. Feeds the decoder and records the address for
   recognition, whether or not a profile is loaded. */
void on_frame(const dbus2::Frame &f, int64_t rx_time_us);
void tick(int64_t now_us);

bool has_profile();
std::string profile_id();
std::string profile_model();
std::string profile_product();
std::string profile_appliance();
bool online();

std::vector<Reading> readings();

/* Ranked against what has actually been seen on the bus. Empty until frames
   arrive. */
std::vector<Candidate> candidates();

/* Distinct dest/cmd pairs observed since boot, however many profiles matched. */
size_t observed_pairs();

bool activate(const std::string &id, std::string *error);

/* Rendered for the HTTP layer, which passes them through unchanged. */
std::string profile_json();
std::string readings_json();
std::string candidates_json();
std::string catalogue_json();

/* The active profile's frame-to-entity map, so the monitor can say what a
   frame means rather than only what it contains. */
std::string layout_json();

/* The running profile as JSON, so the editor can start from it. */
std::string active_source();

/* Any profile the device holds, by id; empty id means the active one. */
std::string source_of(const std::string &id);

/* One of the user's own profiles, as the list needs it. Stored profiles are
   addressed as "custom:<slug>", which cannot collide with a built-in id. */
struct Stored {
  std::string id;
  std::string slug;
  std::string model;
  std::string product;
  std::string appliance;
  size_t entities = 0;
  bool valid = true;
};
std::vector<Stored> stored();
std::string stored_json();

/* How full the profile store is, and what the limits are. */
std::string storage_json();

/* Saves under the given slug, validating first. Activates it when asked, and
   also when it happens to be the profile already running -- otherwise an edit
   to the live profile would be stored but never take effect. */
bool save_custom(const std::string &id, const std::string &json, bool activate_it,
                 std::string *error);
bool erase_custom(const std::string &id, std::string *error);
void clear_profile();

}  // namespace appliance

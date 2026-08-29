/*
   Turning frames into appliance state.

   Deliberately free of ESP-IDF: it takes frames and a clock and hands out
   values, so the whole decoding path can be replayed against a captured trace on
   a host, with sanitizers, instead of only in front of a running machine.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/decoder/Profile.h"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace dbus2::decoder {

struct EntityState {
  Value value;
  /* Kept separately from `value`: Hold has to survive an expiry, or a machine
     that goes quiet would lose the state it last reported. */
  Value held;
  int64_t last_seen_us = 0;
  int64_t last_published_us = 0;
  int64_t clear_at_us = 0;   // momentary
  bool available = false;
  bool pending = false;      // debounce
  Value pending_value;
  int64_t pending_since_us = 0;
  /* True while the value is the engine carrying a countdown forward rather than
     something the appliance said. Cleared by the next real frame. */
  bool estimated = false;
};

class Engine {
 public:
  /* Called when a value changes, expires, or clears. `available` false means
     the reading went stale rather than took a new value. */
  using Sink = std::function<void(const Entity &, const Value &, bool available)>;

  void set_sink(Sink sink) { sink_ = std::move(sink); }

  /* Replaces the active profile and resets all state. */
  void load(Profile profile);
  bool loaded() const { return loaded_; }
  const Profile &profile() const { return profile_; }

  void on_frame(uint8_t dest, uint16_t cmd, std::span<const uint8_t> payload, int64_t now_us);

  /* Drives expiry, debounce and momentary clearing. Call regularly. */
  void tick(int64_t now_us);

  const std::vector<EntityState> &states() const { return states_; }

  /* True while frames are arriving at all -- distinct from any single entity
     being fresh, and what Home Assistant should treat as the device being
     present. */
  bool appliance_online(int64_t now_us) const;

  uint32_t skipped_short() const { return skipped_short_; }

 private:
  void publish(size_t idx, const Value &v, bool available, int64_t now_us);

  Profile profile_;
  bool loaded_ = false;
  Sink sink_;
  std::vector<EntityState> states_;
  /* Resolved once at load rather than looked up per tick: which entity gates
     each countdown, or -1 for none. A gate naming an entity that does not exist
     leaves the countdown switched off, which is the safe way to be wrong. */
  std::vector<int> gate_;
  int64_t last_frame_us_ = 0;
  uint32_t skipped_short_ = 0;
};

}  // namespace dbus2::decoder

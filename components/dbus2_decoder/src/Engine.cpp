/*
   Turning frames into appliance state.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/Engine.h"

namespace dbus2::decoder {

const char *to_string(Kind k) {
  switch (k) {
    case Kind::Sensor: return "sensor";
    case Kind::BinarySensor: return "binary_sensor";
    case Kind::TextSensor: return "text_sensor";
    case Kind::Event: return "event";
    case Kind::Hidden: return "hidden";
  }
  return "?";
}

void Profile::reindex() {
  index.clear();
  for (uint16_t i = 0; i < entities.size(); i++)
    index.emplace(key(entities[i].dest, entities[i].cmd), i);
}

std::vector<uint32_t> Profile::signature() const {
  std::vector<uint32_t> out;
  for (const auto &e : entities) {
    const uint32_t k = key(e.dest, e.cmd);
    bool seen = false;
    for (uint32_t x : out)
      if (x == k) { seen = true; break; }
    if (!seen)
      out.push_back(k);
  }
  return out;
}

void Engine::load(Profile p) {
  profile_ = std::move(p);
  profile_.reindex();
  states_.assign(profile_.entities.size(), EntityState{});

  gate_.assign(profile_.entities.size(), -1);
  for (size_t i = 0; i < profile_.entities.size(); i++) {
    const std::string &want = profile_.entities[i].behavior.countdown_gate_id;
    if (want.empty())
      continue;
    for (size_t j = 0; j < profile_.entities.size(); j++)
      if (profile_.entities[j].id == want) {
        gate_[i] = static_cast<int>(j);
        break;
      }
  }

  last_frame_us_ = 0;
  skipped_short_ = 0;
  loaded_ = true;
}

bool Engine::appliance_online(int64_t now_us) const {
  if (last_frame_us_ == 0)
    return false;
  const int64_t limit = static_cast<int64_t>(profile_.availability_timeout_s) * 1000000;
  return (now_us - last_frame_us_) <= limit;
}

void Engine::publish(size_t idx, const Value &v, bool available, int64_t now_us) {
  EntityState &st = states_[idx];
  st.value = v;
  st.available = available;
  st.last_published_us = now_us;
  if (available && !empty(v))
    st.held = v;
  if (sink_)
    sink_(profile_.entities[idx], v, available);
}

void Engine::on_frame(uint8_t dest, uint16_t cmd, std::span<const uint8_t> payload, int64_t now_us) {
  last_frame_us_ = now_us;
  if (!loaded_)
    return;

  const uint32_t k = Profile::key(dest, cmd);
  auto range = profile_.index.equal_range(k);

  /* Nothing claims this pair. Whoever is extending a profile reads the frame
     itself out of the monitor, which shows it either way. */
  if (range.first == range.second)
    return;

  for (auto it = range.first; it != range.second; ++it) {
    const size_t idx = it->second;
    const Entity &e = profile_.entities[idx];
    EntityState &st = states_[idx];

    /* A hidden marker exists only to claim this pair so the monitor can label
       it; it decodes nothing and is a display concern for the UI. */
    if (e.kind == Kind::Hidden)
      continue;

    if (payload.size() < e.min_len) {
      skipped_short_++;
      continue;
    }

    st.last_seen_us = now_us;

    Value v;
    bool guarded = false;
    for (const auto &g : e.guards) {
      auto probe = g.when.eval(payload);
      if (probe && as_number(*probe) == g.eq) {
        v = g.emit;
        guarded = true;
        break;
      }
    }

    if (!guarded) {
      auto raw = e.extract.eval(payload);
      if (!raw) {
        skipped_short_++;
        continue;
      }
      v = *raw;

      bool dropped = false;
      for (const auto &t : e.transforms) {
        auto out = t.apply(v, st.held);
        if (!out) { dropped = true; break; }
        v = *out;
      }
      if (dropped)
        continue;
    }

    /* Throttle before comparing: a rate limit that only applies to changes is
       no rate limit on a value that changes every frame. */
    if (e.behavior.throttle_ms &&
        (now_us - st.last_published_us) < static_cast<int64_t>(e.behavior.throttle_ms) * 1000)
      continue;

    if (e.behavior.on_change && st.available && same(v, st.value) && e.behavior.momentary_ms == 0)
      continue;

    if (e.behavior.debounce_ms) {
      st.pending = true;
      st.pending_value = v;
      st.pending_since_us = now_us;
      continue;
    }

    /* The appliance has spoken, so whatever the countdown had carried forward is
       superseded -- and the cycle starts again from this reading. */
    st.estimated = false;
    publish(idx, v, true, now_us);

    if (e.behavior.momentary_ms)
      st.clear_at_us = now_us + static_cast<int64_t>(e.behavior.momentary_ms) * 1000;
  }
}

void Engine::tick(int64_t now_us) {
  if (!loaded_)
    return;

  for (size_t i = 0; i < states_.size(); i++) {
    const Entity &e = profile_.entities[i];
    EntityState &st = states_[i];

    if (st.pending && (now_us - st.pending_since_us) >= static_cast<int64_t>(e.behavior.debounce_ms) * 1000) {
      st.pending = false;
      publish(i, st.pending_value, true, now_us);
      if (e.behavior.momentary_ms)
        st.clear_at_us = now_us + static_cast<int64_t>(e.behavior.momentary_ms) * 1000;
    }

    if (st.clear_at_us && now_us >= st.clear_at_us) {
      st.clear_at_us = 0;
      /* A momentary event is a press, not a state: it has to fall back on its
         own or it would read as held down forever. */
      publish(i, Value{false}, true, now_us);
    }

    /* No guard on last_seen_us being non-zero: zero is a perfectly good
       timestamp, and treating it as "nothing seen yet" silently disabled expiry
       for anything that arrived in the first microsecond after boot.
       st.available already means a frame was decoded. */
    /* Carry a countdown forward between two real readings -- see Behavior for
       why this is gated rather than free-running. */
    if (e.behavior.countdown_every_s && st.available && !empty(st.value)) {
      const int idx = gate_[i];
      const bool allowed = idx >= 0 && states_[idx].available &&
                           to_string(states_[idx].value) == e.behavior.countdown_gate_value;
      const int64_t period = static_cast<int64_t>(e.behavior.countdown_every_s) * 1000000;
      if (allowed && (now_us - st.last_published_us) >= period) {
        double next = as_number(st.value) - e.behavior.countdown_by;
        if (next < e.behavior.countdown_min)
          next = e.behavior.countdown_min;
        if (next != as_number(st.value)) {
          st.estimated = true;
          publish(i, Value{next}, true, now_us);
        } else {
          /* Already at the floor: hold the line rather than re-publishing the
             same number every period. */
          st.last_published_us = now_us;
        }
      }
    }

    if (e.behavior.expire_after_s && st.available &&
        (now_us - st.last_seen_us) > static_cast<int64_t>(e.behavior.expire_after_s) * 1000000) {
      /* Stale readings are worse than none: a temperature from an hour ago looks
         exactly like a current one. */
      publish(i, Value{}, false, now_us);
    }
  }
}

}  // namespace dbus2::decoder

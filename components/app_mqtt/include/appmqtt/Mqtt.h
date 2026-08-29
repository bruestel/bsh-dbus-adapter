/*
   Publishing decoded appliance state to an MQTT broker.

   The web interface answers "what is the machine doing right now" for someone
   looking at it. This answers the same question for something that is not:
   readings are retained, so a broker restart or a newly started consumer sees
   the current state immediately rather than waiting for the appliance to change
   its mind -- which, on a dryer that reports the door as an event and little
   else, could be hours.

   One availability topic. "The adapter is gone" and "the appliance is
   unplugged" were separate for a while, until it became clear that MQTT allows
   a single last will per connection -- so only the first could ever be
   corrected by the broker when the device died. On this board the two happen
   together anyway, since it draws its power from the bus. What a subscriber
   actually wants is one answer: is what I am reading current?

   It is republished on a slow cycle as well as on change, so a quiet appliance
   can be told apart from a vanished adapter.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace appmqtt {

/* What a subscriber needs: which reading, what it says, and whether it still
   means anything -- plus what it is called and what it measures, which is what
   the discovery announcement is built from. The metadata comes from the profile
   and is carried through untouched; nothing here decides what a value means. */
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
};

/* The adapter itself, as Home Assistant should show it: one device, with every
   reading as an entity under it. */
struct Description {
  /* Identity comes from the MAC, never from the name the user picked. Home
     Assistant keys a device on this string, so deriving it from an editable
     name would mean renaming the adapter silently orphaned every entity and
     created a duplicate device beside it. The chosen name is what gets
     displayed -- it just does not decide identity. */
  std::string device_id;
  std::string device_name;
  std::string model;
  std::string manufacturer;
  std::string firmware;
};

bool begin();

/* Called when the station comes up or goes away. The client is only started
   once there is a network, and stopped when there is not -- an MQTT client
   retrying against no route is just noise. */
void on_network(bool connected);

bool connected();
bool enabled();

/* Reports the current configuration back for the settings page. Never returns
   the password. */
std::string status_json();

/* The set of readings to publish, replacing whatever was set before. Called
   again when the profile changes, which is also when entities disappear: a
   discovery topic that is no longer wanted is cleared here, or Home Assistant
   would keep showing an entity nothing will ever update again.

   The state topics are left alone. Their retained payload is a value that was
   true when it was written, and clearing it would not make a subscriber better
   informed than an old reading does. */
void announce(const Description &device, const Reading *readings, size_t count);

/* One frame exactly as it came off the bus, when the raw setting is on.

   Not retained and at QoS 0, unlike everything else here: a frame is a moment,
   not a state, and keeping the last one on the broker forever would say nothing
   true about the appliance. Losing one under load costs a line in somebody's
   recording, which is cheaper than making the bus task wait for an
   acknowledgement.

   The payload is the frame as hex and nothing else -- no fields, no timestamp,
   no interpretation. Splitting it into dest and command was this firmware's
   reading of the bytes, and a recording that carries a reading is worth less
   than one that carries what arrived: anything consuming this can split it
   again, and will still be able to when our reading turns out to be wrong. */
void publish_raw_frame(const std::string &hex);

/* One reading changed. Retained, so a consumer that connects later still sees
   it; an unavailable reading publishes an empty payload. */
void publish(const Reading &r);

/* Whether the appliance itself is talking, as distinct from whether this
   adapter is. */
void set_appliance_online(bool online);

/* Applies newly stored settings without a restart. */
void reconfigure();

}  // namespace appmqtt

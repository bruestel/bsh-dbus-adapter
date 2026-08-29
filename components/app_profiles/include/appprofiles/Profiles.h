/*
   The appliance profiles that ship with the firmware, and recognising which one
   fits the bus.

   Identifying an appliance by asking it would mean transmitting, which this
   firmware never does. So it listens instead: each profile declares the dest/cmd
   pairs it expects, and whichever profile's pairs actually turn up wins. The
   same address means different things on different appliance families, so a
   wrong profile does not fail loudly -- it reports plausible nonsense. Matching
   against observed traffic is what prevents that.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *id;
  const char *model;
  /* What the thing is actually called in a shop, e.g. "Siemens iQ500
     Warmepumpentrockner". A model designation identifies an appliance but does
     not describe it, and nobody recognises their own machine from WT47R440.
     Looked up once and written into the profile rather than fetched at runtime:
     this device sits in a laundry room, has no business calling out to the
     internet, and would be reporting the user's appliance to a third party if
     it did. Empty when nobody has looked it up. */
  const char *product;
  const char *appliance;
  unsigned entities;
  const uint32_t *signature;  /* (dest << 16) | cmd, sorted */
  unsigned signature_len;
  const char *json;
  unsigned json_len;
  /* A family fallback rather than a specific machine: correct decoding,
     raw codes instead of programme names. */
  bool generic;
} bsh_profile_t;

extern const bsh_profile_t bsh_profiles[];
extern const unsigned bsh_profile_count;

#ifdef __cplusplus
}  // extern "C"

#include <cstddef>
#include <string>
#include <vector>

namespace appprofiles {

struct Match {
  const bsh_profile_t *profile = nullptr;
  unsigned hits = 0;      /* pairs of this profile actually seen */
  unsigned expected = 0;  /* pairs the profile declares */
  float score = 0;        /* hits / expected, tie-broken by coverage */
};

const bsh_profile_t *find(const std::string &id);

/* Ranks every shipped profile against the dest/cmd pairs observed on the bus,
   best first. Profiles that match nothing are left out. */
std::vector<Match> rank(const std::vector<uint32_t> &observed);

}  // namespace appprofiles
#endif

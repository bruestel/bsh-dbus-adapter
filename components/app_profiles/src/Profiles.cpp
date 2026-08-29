/*
   Recognising which shipped profile fits the bus.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "appprofiles/Profiles.h"

#include <algorithm>
#include <cstring>

namespace appprofiles {

const bsh_profile_t *find(const std::string &id) {
  for (unsigned i = 0; i < bsh_profile_count; i++)
    if (id == bsh_profiles[i].id)
      return &bsh_profiles[i];
  return nullptr;
}

std::vector<Match> rank(const std::vector<uint32_t> &observed) {
  std::vector<Match> out;
  if (observed.empty())
    return out;

  for (unsigned i = 0; i < bsh_profile_count; i++) {
    const bsh_profile_t &p = bsh_profiles[i];
    unsigned hits = 0;
    for (unsigned k = 0; k < p.signature_len; k++)
      if (std::find(observed.begin(), observed.end(), p.signature[k]) != observed.end())
        hits++;

    if (!hits)
      continue;
    /* A family fallback matches by construction -- it is built from what the
       family shares -- so ranking it against specific models would always put it
       near the top and drown them out. It stays available in the catalogue. */
    if (p.generic)
      continue;

    Match m;
    m.profile = &p;
    m.hits = hits;
    m.expected = p.signature_len;
    /* Kept for the tie-break and for the interface to show, but no longer what
       decides the order -- see the sort below. */
    m.score = p.signature_len ? static_cast<float>(hits) / p.signature_len : 0.f;
    out.push_back(m);
  }

  /* By how much of the traffic a profile accounts for, not by what share of
     itself it managed to place.

     Ranking by share rewarded small profiles for being small. The addresses a
     three-pair profile lists are the common ones -- door, state, remaining time
     -- so it matches everything by construction, while a profile that describes
     the same appliance in eight pairs drops behind the moment one of its rarer
     addresses has not been sent yet. A fault code appears when something goes
     wrong, which is exactly when nobody has been watching for hours.

     The share still breaks ties: between two profiles that both explain six
     addresses, the one that listed six is a better fit than the one that listed
     twenty. */
  std::sort(out.begin(), out.end(), [](const Match &a, const Match &b) {
    if (a.hits != b.hits)
      return a.hits > b.hits;
    if (a.score != b.score)
      return a.score > b.score;
    return a.profile->entities > b.profile->entities;
  });
  return out;
}

}  // namespace appprofiles

/*
   Reading a profile from JSON.

   Separated from the engine so the decoding core stays free of a JSON
   dependency and remains host-testable without one.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/decoder/Profile.h"

#include <string>
#include <vector>

namespace dbus2::decoder {

struct ParseResult {
  bool ok = false;
  Profile profile;
  /* Every problem found, not just the first. Uploading a profile and being told
     about one mistake per attempt is a miserable way to fix a file. */
  std::vector<std::string> errors;
};

ParseResult parse_profile(const char *json);

/* What a profile says about itself, without building it: the metadata and how
   many entities it has.

   For listing profiles, which used to parse every one in full only to show its
   name -- building every entity of every stored profile on each request, and
   with it the largest allocations this firmware makes. Not even the JSON tree
   is built: the text is walked, and only the small "profile" object parsed.
   `ok` means the file is
   a readable schema-1 profile with an id and entities. It does not vouch for
   the entities themselves; parse_profile() does that, and nothing is stored
   that it has not accepted. */
struct ProfileSummary {
  bool ok = false;
  Meta meta;
  size_t entities = 0;
};

ProfileSummary summarize_profile(const char *json);

}  // namespace dbus2::decoder

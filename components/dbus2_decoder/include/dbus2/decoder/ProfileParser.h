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

}  // namespace dbus2::decoder

/*
   Writable storage for user-authored appliance profiles.

   The profiles that ship with the firmware live in the image and cannot change
   without a rebuild, which is the right trade for machines somebody has already
   described. A machine nobody has described needs the opposite, and describing
   one is rarely a single attempt: a working profile and the variant being tried
   against it want to exist side by side, so several are kept rather than one.

   Each profile is a file named after its slug. SPIFFS has no directories, so the
   slug is also the whole namespace -- which is why it is validated rather than
   trusted: a slug containing a slash or a dot-dot would name a file outside the
   set and quietly become unlistable.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace appstore {

/* Mounts the storage partition, formatting it if it has never been used. Safe
   to call when the partition is missing: everything below then reports empty
   rather than failing, so a firmware built for a board without one still runs. */
bool begin();
bool available();

/* Lowercase letters, digits and dashes, 1..24 characters. Anything else is
   refused rather than sanitised, because silently renaming what the user typed
   makes the profile they later look for impossible to find. */
bool valid_slug(const std::string &slug);

/* Derives a usable slug from free text, for the case where a name is offered
   rather than a slug. Empty if nothing usable survives. */
std::string slugify(const std::string &text);

std::vector<std::string> list();
bool exists(const std::string &slug);

/* Empty if there is no such profile or the read fails -- the caller falls back
   to a built-in either way, so the two need no distinction. */
std::string load(const std::string &slug);

/* Written to a temporary file and renamed into place, so a power cut during a
   save leaves the previous version intact rather than half of the new one.
   Behind an appliance, an interrupted write is not a hypothetical. */
bool save(const std::string &slug, const std::string &json, std::string *error);

bool erase(const std::string &slug);

struct Usage {
  size_t total = 0;
  size_t used = 0;
  size_t count = 0;
};
Usage usage();

/* Refused above this. A profile describing the largest appliance in the
   collection is a few kilobytes; anything near the limit is a mistake or an
   attack, not a configuration. */
inline constexpr size_t kMaxProfileBytes = 32 * 1024;

/* One machine needs one profile; a second and a third exist while a variant is
   being tried against the one that works. Past that a list stops being
   something anyone navigates, and an unbounded set on a device with no file
   manager is a way to fill the flash by accident. */
inline constexpr size_t kMaxProfiles = 10;

}  // namespace appstore

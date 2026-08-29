/*
   Runs generated profiles through the parser the device uses.

   The converter and the firmware are separate programs; without this, a profile
   could be produced happily and rejected on the appliance, which is the worst
   place to find out.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/ProfileParser.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using namespace dbus2::decoder;

int main(int argc, char **argv) {
  if (argc < 2) {
    std::printf("usage: validate_profiles <profile.json>...\n");
    return 2;
  }

  int bad = 0, entities = 0;
  for (int i = 1; i < argc; i++) {
    std::ifstream f(argv[i]);
    if (!f) {
      std::printf("  %-28s cannot read\n", argv[i]);
      bad++;
      continue;
    }
    std::stringstream ss;
    ss << f.rdbuf();

    auto r = parse_profile(ss.str().c_str());
    const char *name = std::strrchr(argv[i], '/');
    name = name ? name + 1 : argv[i];

    if (!r.ok) {
      bad++;
      std::printf("  %-28s REJECTED\n", name);
      for (const auto &e : r.errors)
        std::printf("      %s\n", e.c_str());
      continue;
    }
    entities += static_cast<int>(r.profile.entities.size());
    std::printf("  %-28s ok  %2zu entities, %2zu distinct dest/cmd, %s\n", name,
                r.profile.entities.size(), r.profile.signature().size(),
                r.profile.meta.appliance.empty() ? "?" : r.profile.meta.appliance.c_str());
  }

  std::printf("\n%d files, %d entities, %d rejected\n", argc - 1, entities, bad);
  return bad ? 1 : 0;
}

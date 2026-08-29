/*
   LED signalling.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "indicator.h"
#include "board.h"

namespace indicator {
namespace {

/* Patterns are 40 slots of 50 ms, i.e. a 2 s cycle, LSB first. Writing them as
   bit literals keeps the shape of each pattern readable in the source and makes
   it obvious that they are distinguishable from one another. */
constexpr int64_t kSlotUs = 50000;
constexpr int kSlots = 40;

constexpr uint64_t bits(const char *s) {
  uint64_t v = 0;
  for (int i = 0; i < kSlots && s[i]; i++)
    if (s[i] == '#')
      v |= (1ULL << i);
  return v;
}

//                                       0    5    10   15   20   25   30   35
constexpr uint64_t kBooting      = bits("########################################");
constexpr uint64_t kApMode       = bits("##__##__##__##__##__##__##__##__##__##__");
constexpr uint64_t kDisconnected = bits("##_##___________________________________");
constexpr uint64_t kConnected    = bits("#_______________________________________");
constexpr uint64_t kNominal      = bits("_#######################################");
constexpr uint64_t kFatal        = bits("#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_#_");
constexpr uint64_t kOtaGreen     = bits("##__##__##__##__##__##__##__##__##__##__");
constexpr uint64_t kOtaBlue      = bits("__##__##__##__##__##__##__##__##__##__##");
constexpr uint64_t kGarbage      = bits("##___##___##___##___##___##___##___##___");
/* Both LEDs solid while the recovery hold is armed: unmistakable, and clearly
   different from every blinking pattern, so it reads as "something is about to
   happen, let go if you did not mean it". */
constexpr uint64_t kWiping       = bits("########################################");

uint64_t pattern_for(System s) {
  switch (s) {
    case System::Booting: return kBooting;
    case System::ApMode: return kApMode;
    case System::Disconnected: return kDisconnected;
    case System::Connected: return kConnected;
    case System::Nominal: return kNominal;
    case System::Fatal: return kFatal;
    case System::Ota: return kOtaGreen;
    case System::Wiping: return kWiping;
  }
  return kFatal;
}

System g_system = System::Booting;
Bus g_bus = Bus::Silent;

int64_t g_cycle_start_us = 0;

/* Frame blips are event driven rather than part of a pattern, so a busy bus
   reads as flicker instead of a solid light. note_frame() only raises a flag;
   tick() turns it into a deadline, so callers never need a clock. */
int64_t g_blip_until_us = 0;
bool g_blip_pending = false;
constexpr int64_t kBlipUs = 30000;

bool slot_set(uint64_t pattern, int slot) { return (pattern >> slot) & 1ULL; }

}  // namespace

void begin() {
  board::led_init(board::PIN_LED_STATUS);
  board::led_init(board::PIN_LED_ACTIVITY);
}

void set_system(System s) {
  if (s == g_system)
    return;
  g_system = s;
  /* Restart the cycle so a new pattern begins at its first slot; otherwise a
     two-flash group could be entered halfway and be misread. */
  g_cycle_start_us = 0;
}

void set_bus(Bus b) { g_bus = b; }

void note_frame() { g_blip_pending = true; }

void tick(int64_t now_us) {
  if (g_cycle_start_us == 0)
    g_cycle_start_us = now_us;

  if (g_blip_pending) {
    g_blip_pending = false;
    g_blip_until_us = now_us + kBlipUs;
  }

  const int slot = static_cast<int>(((now_us - g_cycle_start_us) / kSlotUs) % kSlots);


  /* Green: system state. */
  board::led_set(board::PIN_LED_STATUS, slot_set(pattern_for(g_system), slot));

  /* Blue: bus state. During OTA it joins the green LED in antiphase, which is
     unmistakable and says "do not remove power". */
  if (g_system == System::Ota) {
    board::led_set(board::PIN_LED_ACTIVITY, slot_set(kOtaBlue, slot));
    return;
  }
  if (g_system == System::Wiping) {
    board::led_set(board::PIN_LED_ACTIVITY, true);
    return;
  }

  switch (g_bus) {
    case Bus::Garbage:
      board::led_set(board::PIN_LED_ACTIVITY, slot_set(kGarbage, slot));
      break;
    case Bus::Healthy:
      board::led_set(board::PIN_LED_ACTIVITY, now_us < g_blip_until_us);
      break;
    case Bus::Silent:
      board::led_set(board::PIN_LED_ACTIVITY, false);
      break;
  }
}

const char *to_string(System s) {
  switch (s) {
    case System::Booting: return "booting";
    case System::ApMode: return "ap-mode";
    case System::Disconnected: return "disconnected";
    case System::Connected: return "connected";
    case System::Nominal: return "nominal";
    case System::Fatal: return "fatal";
    case System::Ota: return "ota";
    case System::Wiping: return "wiping";
  }
  return "?";
}

const char *to_string(Bus b) {
  switch (b) {
    case Bus::Silent: return "silent";
    case Bus::Healthy: return "healthy";
    case Bus::Garbage: return "garbage";
  }
  return "?";
}

}  // namespace indicator

/*
   Host tests for the D-Bus-2 receive state machine.

   The frames used here are real: they are taken from the appliance boot log in
   the upstream project's README, including their original CRCs. If the parser
   accepts these and rejects the mutations below, it agrees with hardware.

   Deliberately dependency-free so it runs anywhere with a C++20 compiler and no
   network access.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/Crc.h"
#include "dbus2/Protocol.h"

#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                  \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      std::printf("  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #cond);   \
      std::printf(__VA_ARGS__);                                           \
      std::printf("\n");                                                  \
    }                                                                     \
  } while (0)

static void section(const char *name) { std::printf("\n== %s ==\n", name); }

/* Strict: spaces are stripped first, then the remainder must be whole byte
   pairs of hex digits. An earlier, laxer version silently turned a stray space
   inside a pair into wrong bytes, which cost more time than it saved. */
static std::vector<uint8_t> hex(const std::string &s) {
  std::string clean;
  for (char c : s) {
    if (c == ' ')
      continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      std::printf("  FATAL: non-hex character '%c' in \"%s\"\n", c, s.c_str());
      std::exit(2);
    }
    clean.push_back(c);
  }
  if (clean.size() % 2 != 0) {
    std::printf("  FATAL: odd number of hex digits (%zu) in \"%s\"\n", clean.size(), s.c_str());
    std::exit(2);
  }
  std::vector<uint8_t> out;
  out.reserve(clean.size() / 2);
  for (size_t i = 0; i < clean.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16)));
  return out;
}

/* Collects frames the parser hands out, so tests can assert on them. */
struct Collector {
  std::vector<dbus2::Frame> frames;
  /* Receive times, kept so a test can check that an acknowledgement was paired
     to the right frame rather than merely to the most recent one. */
  std::vector<int64_t> times;
  bool accept = true;

  dbus2::Protocol::FrameHandler handler() {
    return [this](const dbus2::Frame &f, int64_t ts) {
      if (!accept)
        return false;
      frames.push_back(f);
      times.push_back(ts);
      return true;
    };
  }
};

/* Feeds bytes with a realistic 1 ms spacing (roughly one byte time at 9600). */
static void feed(dbus2::Protocol &p, const std::vector<uint8_t> &bytes, int64_t &clock_us,
                 int64_t step_us = 1042) {
  for (uint8_t b : bytes) {
    clock_us += step_us;
    p.tick(clock_us);
    p.feed(b, clock_us);
  }
}

static void gap(dbus2::Protocol &p, int64_t &clock_us, int64_t us = 10000) {
  clock_us += us;
  p.tick(clock_us);
}

int main() {
  // ---------------------------------------------------------------- CRC ----
  section("CRC16-XMODEM against real frames");
  {
    struct { const char *body; uint16_t crc; } vec[] = {
        {"040fe7000102", 0xa5ec}, {"030fe00000", 0x9a0d}, {"041fe8000102", 0x7558},
        {"05141005 00ff01", 0xce43}, {"0314100404", 0xf0a7}, {"020fefff", 0xdf25},
        {"121213000400010100000000010400000100015a", 0xe805},
    };
    for (auto &v : vec) {
      auto b = hex(v.body);
      uint16_t got = dbus2::crc16(b.data(), b.size());
      CHECK(got == v.crc, "body %s: got %04x want %04x", v.body, got, v.crc);
    }
    // Running the CRC over frame+crc must yield zero.
    auto full = hex("05141005 00ff01 ce43");
    CHECK(dbus2::crc16(full.data(), full.size()) == 0, "crc over frame+crc must be 0");
  }

  // -------------------------------------------------------- happy path ----
  section("Valid frame is parsed");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce43"), t);

    CHECK(c.frames.size() == 1, "expected 1 frame, got %zu", c.frames.size());
    if (c.frames.size() == 1) {
      auto &f = c.frames[0];
      CHECK(f.length == 5, "length %u", f.length);
      CHECK(f.dest == 0x14, "dest %02x", f.dest);
      CHECK(f.node_id() == 1, "node %u", f.node_id());
      CHECK(f.subsystem_id() == 4, "sub %u", f.subsystem_id());
      CHECK(f.has_command(), "should have a command");
      CHECK(f.command() == 0x1005, "cmd %04x", f.command());
      CHECK(f.payload().size() == 3, "payload %zu", f.payload().size());
      CHECK(!f.is_broadcast(), "0x14 is not broadcast");
    }
    CHECK(p.stats().frames_ok == 1, "frames_ok %u", p.stats().frames_ok);
    CHECK(p.stats().crc_errors == 0, "crc_errors %u", p.stats().crc_errors);
  }

  section("Broadcast frame is recognised");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("040fe7000102 a5ec"), t);
    CHECK(c.frames.size() == 1, "got %zu frames", c.frames.size());
    if (!c.frames.empty())
      CHECK(c.frames[0].is_broadcast(), "dest 0x0f must be broadcast");
  }

  // -------------------------------------------------- ACK byte handling ----
  section("Trailing ACK byte is consumed, not misread as a length");
  {
    // This is the subtle one: after a frame the receiver emits a single ACK
    // byte. If the parser treated it as the next length byte it would desync
    // and swallow the following frame.
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce43"), t);   // frame
    feed(p, hex("1a"), t);                      // ACK from node 1
    gap(p, t);                                  // inter-frame gap
    feed(p, hex("0314100404 f0a7"), t);         // next frame
    feed(p, hex("1a"), t);

    CHECK(c.frames.size() == 2, "expected 2 frames, got %zu", c.frames.size());
    CHECK(p.stats().acks_seen == 2, "acks_seen %u", p.stats().acks_seen);
    CHECK(p.stats().crc_errors == 0, "crc_errors %u", p.stats().crc_errors);
  }

  section("The acknowledgement is reported, paired to the frame it answers");
  {
    // The frame goes out before its acknowledgement exists, so the two are
    // matched afterwards by the frame's receive time. Getting that pairing
    // wrong would label one frame with another's answer.
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());

    std::vector<std::pair<uint8_t, int64_t>> acks;
    p.set_ack_handler([&](uint8_t a, int64_t ts) { acks.push_back({a, ts}); });

    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce43"), t);
    const int64_t first_frame_time = c.times.empty() ? -1 : c.times.back();
    feed(p, hex("1a"), t);                      // accepted
    gap(p, t);
    feed(p, hex("0314100404 f0a7"), t);
    const int64_t second_frame_time = c.times.empty() ? -1 : c.times.back();
    feed(p, hex("17"), t);                      // refused, bad checksum

    CHECK(acks.size() == 2, "expected 2 acks, got %zu", acks.size());
    CHECK(acks[0].first == 0x1a, "first ack %02x", acks[0].first);
    CHECK(acks[1].first == 0x17, "second ack %02x", acks[1].first);
    CHECK(acks[0].second == first_frame_time, "first ack paired to %lld, frame was %lld",
          (long long)acks[0].second, (long long)first_frame_time);
    CHECK(acks[1].second == second_frame_time, "second ack paired to %lld, frame was %lld",
          (long long)acks[1].second, (long long)second_frame_time);

    // Acceptance and refusal are counted apart.
    CHECK(p.stats().acks_seen == 1, "acks_seen %u", p.stats().acks_seen);
    CHECK(p.stats().naks_seen == 1, "naks_seen %u", p.stats().naks_seen);
  }

  section("A frame nobody answers reports no acknowledgement at all");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int acks = 0;
    p.set_ack_handler([&](uint8_t, int64_t) { acks++; });

    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce43"), t);
    gap(p, t);   // silence instead of an answer

    CHECK(c.frames.size() == 1, "frame should still be delivered, got %zu", c.frames.size());
    CHECK(acks == 0, "no ack expected, got %d", acks);
    CHECK(p.stats().naks_seen == 0, "silence is not a refusal");
  }

  // ------------------------------------------------------------ errors ----
  section("Corrupted CRC is rejected");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce44"), t);  // low byte flipped
    CHECK(c.frames.empty(), "corrupt frame must not be delivered");
    CHECK(p.stats().crc_errors == 1, "crc_errors %u", p.stats().crc_errors);
  }
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00ff01 cf43"), t);  // high byte flipped
    CHECK(c.frames.empty(), "corrupt frame must not be delivered");
    CHECK(p.stats().crc_errors == 1, "crc_errors %u", p.stats().crc_errors);
  }
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00fe01 ce43"), t);  // payload flipped
    CHECK(c.frames.empty(), "corrupt payload must not be delivered");
    CHECK(p.stats().crc_errors == 1, "crc_errors %u", p.stats().crc_errors);
  }

  section("Zero length byte is skipped without consuming the next byte");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("00"), t);
    feed(p, hex("05141005 00ff01 ce43"), t);
    CHECK(c.frames.size() == 1, "frame after a zero byte must still parse, got %zu", c.frames.size());
    CHECK(p.stats().zero_length == 1, "zero_length %u", p.stats().zero_length);
  }

  section("Truncated frame recovers after the inter-frame gap");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00"), t);   // cut short
    CHECK(p.state() != dbus2::BusState::WaitForLen, "parser should be mid-frame");
    gap(p, t);
    CHECK(p.state() == dbus2::BusState::WaitForLen, "gap must resync the parser");
    CHECK(p.stats().resyncs == 1, "resyncs %u", p.stats().resyncs);

    feed(p, hex("0314100404 f0a7"), t);
    CHECK(c.frames.size() == 1, "next frame must parse, got %zu", c.frames.size());
  }

  section("Resync timing follows the baud rate");
  {
    dbus2::Protocol p(9600);
    CHECK(p.byte_time_us() == 1041, "byte time %lld", (long long) p.byte_time_us());
    CHECK(p.resync_timeout_us() == 2602, "resync %lld", (long long) p.resync_timeout_us());
    p.set_baud(38400);
    CHECK(p.byte_time_us() == 260, "byte time %lld", (long long) p.byte_time_us());
  }
  {
    // A gap shorter than the timeout must NOT resync.
    dbus2::Protocol p(9600);
    int64_t t = 0;
    feed(p, hex("05141005"), t);
    t += 2000;  // below 2602 us
    p.tick(t);
    CHECK(p.state() != dbus2::BusState::WaitForLen, "short gap must not resync");
    CHECK(p.stats().resyncs == 0, "resyncs %u", p.stats().resyncs);
  }

  section("Long frame (18 data bytes) parses");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("121213000400010100000000010400000100015a e805"), t);
    CHECK(c.frames.size() == 1, "got %zu frames", c.frames.size());
    if (!c.frames.empty()) {
      CHECK(c.frames[0].length == 0x12, "length %u", c.frames[0].length);
      CHECK(c.frames[0].data.size() == 18, "data size %zu", c.frames[0].data.size());
    }
  }

  section("Short frame without a command is reported, not dropped");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("020fefff df25"), t);
    CHECK(c.frames.size() == 1, "got %zu frames", c.frames.size());
    if (!c.frames.empty())
      CHECK(c.frames[0].has_command(), "2 data bytes is exactly a bare command");
  }

  section("Handler refusal is counted as a drop");
  {
    Collector c;
    c.accept = false;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("05141005 00ff01 ce43"), t);
    CHECK(p.stats().frames_ok == 1, "frame was still valid");
    CHECK(p.stats().dropped == 1, "dropped %u", p.stats().dropped);
  }

  section("Garbage between frames does not break the next one");
  {
    Collector c;
    dbus2::Protocol p(9600);
    p.set_frame_handler(c.handler());
    int64_t t = 0;
    feed(p, hex("ffeeddccbbaa"), t);
    gap(p, t);
    feed(p, hex("05141005 00ff01 ce43"), t);
    CHECK(c.frames.size() == 1, "frame after garbage must parse, got %zu", c.frames.size());
  }

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

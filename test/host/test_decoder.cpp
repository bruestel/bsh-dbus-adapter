/*
   Host tests for the decoding engine.

   The cases mirror what the upstream ESPHome lambdas actually do, so passing
   means the schema really can replace them -- not merely that it is
   self-consistent.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/Engine.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace dbus2::decoder;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond, ...)                                                \
  do {                                                                  \
    g_checks++;                                                         \
    if (!(cond)) {                                                      \
      g_failures++;                                                     \
      std::printf("  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #cond); \
      std::printf(__VA_ARGS__);                                         \
      std::printf("\n");                                                \
    }                                                                   \
  } while (0)

static void section(const char *n) { std::printf("\n== %s ==\n", n); }

static std::vector<uint8_t> B(std::initializer_list<int> v) {
  std::vector<uint8_t> out;
  for (int x : v) out.push_back(static_cast<uint8_t>(x));
  return out;
}

static double num(const Extract &e, const std::vector<uint8_t> &p) {
  auto v = e.eval(p);
  return v ? as_number(*v) : -999999;
}

int main() {
  // ------------------------------------------------------------ extract ----
  section("Extraction mirrors the upstream lambdas");
  {
    auto p = B({0x2A, 0x80, 0xFF, 0x01});

    Extract u8{.op = Op::U8, .at = 0};
    Extract i8{.op = Op::I8, .at = 1};
    Extract u16be{.op = Op::U16BE, .at = 1};
    Extract i16be{.op = Op::I16BE, .at = 1};
    Extract u16le{.op = Op::U16LE, .at = 1};
    Extract len{.op = Op::Len};

    CHECK(num(u8, p) == 42, "return x[0]");
    CHECK(num(i8, p) == -128, "signed byte");
    CHECK(num(u16be, p) == 0x80FF, "u16be");
    CHECK(num(i16be, p) == -32513, "(int16_t)((x[1]<<8)|x[2])");
    CHECK(num(u16le, p) == 0xFF80, "u16le");
    CHECK(num(len, p) == 4, "payload length");

    /* (x[0] >> 1) & 0x01 on 0x2A = 0b00101010 -> bit 1 is set */
    Extract bit{.op = Op::Bit, .at = 0, .bit = 1};
    CHECK(as_bool(*bit.eval(p)), "bit 1 of 0x2A set");
    bit.bit = 0;
    CHECK(!as_bool(*bit.eval(p)), "bit 0 of 0x2A clear");
    bit.invert = true;
    CHECK(as_bool(*bit.eval(p)), "inverted, as in !((x[0]>>3)&1)");

    Extract mask8{.op = Op::Mask, .at = 0, .mask = 0x08};
    Extract mask4{.op = Op::Mask, .at = 0, .mask = 0x04};
    Extract eq1{.op = Op::Eq, .at = 3, .value = 1};
    Extract one{.op = Op::Const, .value = 1};
    CHECK(as_bool(*mask8.eval(p)), "x[0] & 0x08");
    CHECK(!as_bool(*mask4.eval(p)), "x[0] & 0x04 clear");
    CHECK(as_bool(*eq1.eval(p)), "x[3] == 1");
    CHECK(num(one, p) == 1, "return 1");

    Extract raw{.op = Op::Bytes, .at = 0, .len = 3};
    CHECK(std::get<std::string>(*raw.eval(p)) == "2A80FF", "raw bytes as hex");
  }

  section("Out-of-range reads are refused, not guessed");
  {
    auto p = B({0x01});
    Extract wide{.op = Op::U16BE, .at = 0};
    Extract far{.op = Op::U8, .at = 5};
    Extract run{.op = Op::Bytes, .at = 0, .len = 4};
    Extract ok{.op = Op::U8, .at = 0};

    CHECK(!wide.eval(p), "u16 needs two bytes");
    CHECK(!far.eval(p), "offset past the end");
    CHECK(!run.eval(p), "run past the end");
    CHECK(ok.eval(p).has_value(), "in range still works");
    /* The upstream lambdas index blindly; this is what stops a short frame from
       reading past the payload. */
  }

  // ---------------------------------------------------------- transform ----
  section("Transforms mirror the upstream filters");
  {
    Value held{};
    Transform mul{.op = TOp::Multiply, .value = 10};
    Transform div{.op = TOp::Divide, .value = 60};
    Transform off{.op = TOp::Offset, .value = -39};
    Transform inv{.op = TOp::Invert};
    Transform div0{.op = TOp::Divide, .value = 0};

    CHECK(as_number(*mul.apply(Value{5.0}, held)) == 50, "multiply: 10");
    CHECK(as_number(*div.apply(Value{120.0}, held)) == 2, "/60");
    CHECK(as_number(*off.apply(Value{50.0}, held)) == 11, "x - 39");
    CHECK(as_bool(*inv.apply(Value{false}, held)), "invert");
    CHECK(!div0.apply(Value{1.0}, held), "divide by zero is refused");

    /* calibrate_linear with the washing-machine temperature table */
    Transform cal;
    cal.op = TOp::Calibrate;
    cal.points = {{0, 20}, {1, 30}, {7, 90}};
    CHECK(as_number(*cal.apply(Value{0.0}, held)) == 20, "first point");
    CHECK(as_number(*cal.apply(Value{7.0}, held)) == 90, "last point");
    CHECK(as_number(*cal.apply(Value{4.0}, held)) == 60, "interpolated");

    cal.exact = true;
    CHECK(as_number(*cal.apply(Value{1.0}, held)) == 30, "exact hit");
    CHECK(!cal.apply(Value{4.0}, held), "exact mode refuses a value between codes");
  }

  section("Map turns codes into names");
  {
    Value held{};
    Transform m;
    m.op = TOp::Map;
    m.table = {{0, "Aus"}, {1, "Koch/Bunt"}, {5, "Wolle"}};
    CHECK(std::get<std::string>(*m.apply(Value{5.0}, held)) == "Wolle", "mapped");
    CHECK(!m.apply(Value{9.0}, held), "unmapped and no default -> dropped");
    m.fallback_text = "Unknown (%d)";
    CHECK(std::get<std::string>(*m.apply(Value{9.0}, held)) == "Unknown (9)",
          "default carries the raw code, which is what makes it actionable");
  }

  section("Lookup replaces the if/else chains and the globals idiom");
  {
    /* door: 3 or 57 -> open, 6/7/22 -> closed, anything else keeps the last
       known state -- exactly the shape of the upstream multi-line lambdas. */
    Transform l;
    l.op = TOp::Lookup;
    l.rules = {{{3, 57}, Value{std::string("open")}}, {{6, 7, 22}, Value{std::string("closed")}}};
    l.fallback = Fallback::Hold;

    Value held{};
    CHECK(std::get<std::string>(*l.apply(Value{57.0}, held)) == "open", "matched a set member");
    held = Value{std::string("open")};
    CHECK(std::get<std::string>(*l.apply(Value{99.0}, held)) == "open", "unmatched holds the previous value");

    Value none{};
    CHECK(!l.apply(Value{99.0}, none), "nothing to hold yet -> dropped");

    l.fallback = Fallback::Drop;
    CHECK(!l.apply(Value{99.0}, held), "explicit drop");
  }

  // ------------------------------------------------------------- engine ----
  section("Engine dispatches by dest and command");
  {
    Profile p;
    Entity temp;
    temp.id = "temperature"; temp.dest = 0x14; temp.cmd = 0x1004; temp.min_len = 1;
    temp.extract = Extract{.op = Op::U8, .at = 0};
    Entity rpm;
    rpm.id = "rpm"; rpm.dest = 0x14; rpm.cmd = 0x1006; rpm.min_len = 1;
    rpm.extract = Extract{.op = Op::U8, .at = 0};
    rpm.transforms = {Transform{.op = TOp::Multiply, .value = 10}};
    p.entities = {temp, rpm};

    Engine e;
    std::vector<std::pair<std::string, std::string>> got;
    e.set_sink([&](const Entity &en, const Value &v, bool avail) {
      got.emplace_back(en.id, avail ? to_string(v) : std::string("<unavailable>"));
    });
    e.load(p);

    auto d = B({40});
    e.on_frame(0x14, 0x1004, d, 1000);
    e.on_frame(0x14, 0x1006, d, 2000);

    CHECK(got.size() == 2, "two entities fired, got %zu", got.size());
    if (got.size() == 2) {
      CHECK(got[0].first == "temperature" && got[0].second == "40", "temperature %s", got[0].second.c_str());
      CHECK(got[1].first == "rpm" && got[1].second == "400", "rpm %s", got[1].second.c_str());
    }
  }

  section("Repeats are suppressed; the bus sends the same frame constantly");
  {
    Profile p;
    Entity en; en.id = "t"; en.dest = 1; en.cmd = 2; en.min_len = 1;
    en.extract = Extract{.op = Op::U8, .at = 0};
    p.entities = {en};

    Engine e;
    int fired = 0;
    e.set_sink([&](const Entity &, const Value &, bool) { fired++; });
    e.load(p);

    auto a = B({5}), b = B({6});
    e.on_frame(1, 2, a, 1000);
    e.on_frame(1, 2, a, 2000);
    e.on_frame(1, 2, a, 3000);
    CHECK(fired == 1, "same value three times -> one publish, got %d", fired);
    e.on_frame(1, 2, b, 4000);
    CHECK(fired == 2, "changed value publishes, got %d", fired);
  }

  section("Momentary events fall back on their own");
  {
    Profile p;
    Entity en; en.id = "button"; en.dest = 0x15; en.cmd = 0x1100; en.kind = Kind::BinarySensor;
    en.extract = Extract{.op = Op::Const, .value = 1};
    en.behavior.momentary_ms = 1000;
    p.entities = {en};

    Engine e;
    std::vector<std::string> got;
    e.set_sink([&](const Entity &, const Value &v, bool) { got.push_back(to_string(v)); });
    e.load(p);

    auto d = B({0});
    e.on_frame(0x15, 0x1100, d, 0);
    CHECK(got.size() == 1 && got[0] == "1", "press published");
    e.tick(500000);
    CHECK(got.size() == 1, "not yet cleared");
    e.tick(1'100'000);
    CHECK(got.size() == 2 && got[1] == "Off", "cleared on its own -- a press is not a held state");
  }

  section("Stale readings expire instead of lingering");
  {
    Profile p;
    Entity en; en.id = "t"; en.dest = 1; en.cmd = 2; en.min_len = 1;
    en.extract = Extract{.op = Op::U8, .at = 0};
    en.behavior.expire_after_s = 10;
    p.entities = {en};

    Engine e;
    bool last_avail = true;
    e.set_sink([&](const Entity &, const Value &, bool a) { last_avail = a; });
    e.load(p);

    auto d = B({7});
    e.on_frame(1, 2, d, 0);
    CHECK(last_avail, "fresh reading is available");
    e.tick(5'000'000);
    CHECK(last_avail, "still inside the window");
    e.tick(11'000'000);
    CHECK(!last_avail, "expired -- an hour-old temperature must not look current");
  }

  section("Throttling limits a value that changes every frame");
  {
    Profile p;
    Entity en; en.id = "u"; en.dest = 1; en.cmd = 2; en.min_len = 1;
    en.extract = Extract{.op = Op::U8, .at = 0};
    en.behavior.throttle_ms = 1000;
    p.entities = {en};

    Engine e;
    int fired = 0;
    e.set_sink([&](const Entity &, const Value &, bool) { fired++; });
    e.load(p);

    for (int i = 0; i < 10; i++) {
      auto d = B({i});
      e.on_frame(1, 2, d, i * 100'000);  // every 100 ms, always a new value
    }
    CHECK(fired <= 2, "throttled to at most two in a second, got %d", fired);
  }

  section("Frames too short for an entity are counted, not decoded");
  {
    Profile p;
    Entity en; en.id = "x"; en.dest = 1; en.cmd = 2; en.min_len = 4;
    en.extract = Extract{.op = Op::U16BE, .at = 2};
    p.entities = {en};

    Engine e;
    int fired = 0;
    e.set_sink([&](const Entity &, const Value &, bool) { fired++; });
    e.load(p);

    auto sh = B({1, 2});
    e.on_frame(1, 2, sh, 1000);
    CHECK(fired == 0, "short frame produced no value");
    CHECK(e.skipped_short() == 1, "and was counted, got %u", e.skipped_short());
  }

  section("Appliance availability is separate from any single entity");
  {
    Profile p;
    p.availability_timeout_s = 60;
    Entity en; en.id = "x"; en.dest = 1; en.cmd = 2; en.min_len = 1;
    en.extract = Extract{.op = Op::U8, .at = 0};
    p.entities = {en};

    Engine e;
    e.load(p);
    CHECK(!e.appliance_online(0), "nothing seen yet");
    auto d = B({1});
    e.on_frame(1, 2, d, 1'000'000);
    CHECK(e.appliance_online(2'000'000), "online after a frame");
    CHECK(!e.appliance_online(70'000'000), "offline once the bus goes quiet");
  }

  section("Signature identifies which appliance is on the bus");
  {
    Profile p;
    Entity a; a.dest = 0x11; a.cmd = 0x1006;
    Entity b; b.dest = 0x11; b.cmd = 0x1006;  // same pair, two entities
    Entity c; c.dest = 0x21; c.cmd = 0x1000;
    p.entities = {a, b, c};
    auto sig = p.signature();
    CHECK(sig.size() == 2, "distinct pairs only, got %zu", sig.size());
  }

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

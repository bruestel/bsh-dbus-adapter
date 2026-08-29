/*
   Host tests for the JSON profile parser.

   A profile arrives from a converter or a user's editor, so the parser is the
   place where a mistake either gets named or turns into silently wrong appliance
   readings. These tests care as much about the rejections as the acceptances.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/ProfileParser.h"
#include "dbus2/decoder/Engine.h"

#include <cstdio>
#include <string>

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

static void dump(const ParseResult &r) {
  for (const auto &e : r.errors)
    std::printf("      - %s\n", e.c_str());
}

int main() {
  section("A realistic profile parses");
  {
    /* Shaped like a converted washing-machine configuration, with the hex
       notation the upstream YAML uses for bus addresses. */
    const char *json = R"J({
      "schema": 1,
      "profile": {"id":"wm14s750","manufacturer":"Siemens","model":"WM14S750",
                  "appliance":"washing_machine","credits":["Hajo Noerenberg"]},
      "bus": {"baud": 9600},
      "device": {"availability": {"any_frame_timeout_s": 90}},
      "entities": [
        {"id":"temperature","name":"Temperatur","kind":"sensor",
         "match":{"dest":"0x14","cmd":"0x1004","min_len":1},
         "extract":{"op":"u8","at":0},
         "transforms":[{"op":"calibrate_linear","method":"exact",
                        "datapoints":[[0,20],[1,30],[7,90]]}],
         "behavior":{"expire_after_s":900},
         "ha":{"device_class":"temperature","unit_of_measurement":"°C",
               "state_class":"measurement","accuracy_decimals":0}},

        {"id":"program","name":"Waschprogramm","kind":"text_sensor",
         "match":{"dest":"0x14","cmd":"0x1005","min_len":3},
         "extract":{"op":"u8","at":2},
         "transforms":[{"op":"map","values":{"0":"Aus","1":"Koch/Bunt","5":"Wolle"},
                        "default":"Unbekannt (%d)"}],
         "ha":{"icon":"mdi:numeric"}},

        {"id":"door","name":"Tür","kind":"text_sensor",
         "match":{"dest":"0x26","cmd":"0x1200","min_len":1},
         "extract":{"op":"u8","at":0},
         "transforms":[{"op":"lookup","default":"hold",
                        "rules":[{"in":[0],"out":"Zu"},{"in":[1],"out":"Verriegelt"},
                                 {"in":[2],"out":"Offen"}]}]},

        {"id":"start","name":"Startknopf","kind":"binary_sensor",
         "match":{"dest":"0x15","cmd":"0x1100"},
         "extract":{"op":"const","value":1},
         "behavior":{"momentary_ms":1000}}
      ]})J";

    auto r = parse_profile(json);
    CHECK(r.ok, "should parse cleanly");
    if (!r.ok) dump(r);

    CHECK(r.profile.meta.id == "wm14s750", "id %s", r.profile.meta.id.c_str());
    CHECK(r.profile.meta.credits.size() == 1, "credits carried through");
    CHECK(r.profile.baud == 9600, "baud %u", r.profile.baud);
    CHECK(r.profile.availability_timeout_s == 90, "availability timeout");
    CHECK(r.profile.entities.size() == 4, "four entities, got %zu", r.profile.entities.size());

    if (r.profile.entities.size() == 4) {
      const auto &t = r.profile.entities[0];
      CHECK(t.dest == 0x14 && t.cmd == 0x1004, "hex strings decoded: %02x %04x", t.dest, t.cmd);
      CHECK(t.transforms.size() == 1 && t.transforms[0].exact, "exact calibration recognised");
      CHECK(t.transforms[0].points.size() == 3, "datapoints");
      CHECK(t.ha.unit == "°C", "unit survives UTF-8");
      CHECK(t.behavior.expire_after_s == 900, "expiry");

      const auto &d = r.profile.entities[2];
      CHECK(d.transforms[0].fallback == Fallback::Hold, "lookup default hold");
      CHECK(d.transforms[0].rules.size() == 3, "lookup rules");

      const auto &s = r.profile.entities[3];
      CHECK(s.kind == Kind::BinarySensor, "kind");
      CHECK(s.behavior.momentary_ms == 1000, "momentary");
    }
  }

  section("The parsed profile actually decodes");
  {
    /* Parsing is only half the claim; the result has to drive the engine. */
    const char *json = R"J({
      "schema":1,"profile":{"id":"t"},
      "entities":[{"id":"rpm","match":{"dest":"0x14","cmd":"0x1006","min_len":1},
                   "extract":{"op":"u8","at":0},
                   "transforms":[{"op":"multiply","value":10}]}]})J";
    auto r = parse_profile(json);
    CHECK(r.ok, "parses");

    Engine e;
    std::string got;
    e.set_sink([&](const Entity &, const Value &v, bool) { got = to_string(v); });
    e.load(r.profile);

    std::vector<uint8_t> p{89};
    e.on_frame(0x14, 0x1006, p, 1000);
    CHECK(got == "890", "89 * 10 = 890, got \"%s\"", got.c_str());
  }

  section("Broken files are named, not guessed at");
  {
    auto bad_json = parse_profile("{ not json");
    CHECK(!bad_json.ok && !bad_json.errors.empty(), "malformed JSON reported");

    auto no_schema = parse_profile(R"J({"profile":{"id":"x"},"entities":[]})J");
    CHECK(!no_schema.ok, "missing schema rejected");

    auto no_id = parse_profile(R"J({"schema":1,"entities":[
      {"id":"a","match":{"dest":1,"cmd":2},"extract":{"op":"u8"}}]})J");
    CHECK(!no_id.ok, "missing profile.id rejected");

    auto no_ents = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[]})J");
    CHECK(!no_ents.ok, "empty entities rejected");
  }

  section("A bad entity is reported with its own name");
  {
    const char *json = R"J({
      "schema":1,"profile":{"id":"x"},
      "entities":[
        {"id":"good","match":{"dest":1,"cmd":2},"extract":{"op":"u8","at":0}},
        {"id":"bad_op","match":{"dest":1,"cmd":3},"extract":{"op":"nonsense"}},
        {"id":"bad_dest","match":{"dest":999,"cmd":3},"extract":{"op":"u8"}},
        {"id":"bad_bit","match":{"dest":1,"cmd":4},"extract":{"op":"bit","at":0,"bit":9}}
      ]})J";
    auto r = parse_profile(json);
    CHECK(!r.ok, "should fail");
    CHECK(r.errors.size() == 3, "three problems reported, got %zu", r.errors.size());
    dump(r);

    /* All of them at once: fixing a file one error per upload is miserable. */
    bool named = false;
    for (const auto &e : r.errors)
      if (e.find("bad_op") != std::string::npos)
        named = true;
    CHECK(named, "errors identify the offending entity");

    /* The good entity still made it through, so a partial file can be inspected
       rather than vanishing entirely. */
    CHECK(r.profile.entities.size() == 1, "usable entities kept, got %zu", r.profile.entities.size());
  }

  section("Malformed transforms are caught");
  {
    auto no_pts = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"a","match":{"dest":1,"cmd":2},"extract":{"op":"u8"},
       "transforms":[{"op":"calibrate_linear"}]}]})J");
    CHECK(!no_pts.ok, "calibrate without datapoints rejected");

    auto bad_pair = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"a","match":{"dest":1,"cmd":2},"extract":{"op":"u8"},
       "transforms":[{"op":"calibrate_linear","datapoints":[[1,2,3]]}]}]})J");
    CHECK(!bad_pair.ok, "malformed datapoint rejected");

    auto no_rules = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"a","match":{"dest":1,"cmd":2},"extract":{"op":"u8"},
       "transforms":[{"op":"lookup"}]}]})J");
    CHECK(!no_rules.ok, "lookup without rules rejected");
  }

  section("Both decimal and hex notation work for addresses");
  {
    auto dec = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"a","match":{"dest":20,"cmd":4100},"extract":{"op":"u8"}}]})J");
    auto hex = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"a","match":{"dest":"0x14","cmd":"0x1004"},"extract":{"op":"u8"}}]})J");
    CHECK(dec.ok && hex.ok, "both notations accepted");
    if (dec.ok && hex.ok)
      CHECK(dec.profile.entities[0].dest == hex.profile.entities[0].dest &&
            dec.profile.entities[0].cmd == hex.profile.entities[0].cmd,
            "and mean the same thing");
  }

  section("A hidden marker parses without an extract and carries its payload");
  {
    auto r = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"keepalive","name":"Operating-state poll","kind":"hidden",
       "match":{"dest":"0x0F","cmd":"0xE000","payload":"05"}}]})J");
    CHECK(r.ok, "hidden entity without extract is valid");
    dump(r);
    if (r.ok && r.profile.entities.size() == 1) {
      const auto &h = r.profile.entities[0];
      CHECK(h.kind == Kind::Hidden, "kind is Hidden");
      CHECK(h.dest == 0x0F && h.cmd == 0xE000, "address parsed");
      CHECK(h.match_payload.size() == 1 && h.match_payload[0] == 0x05, "payload matcher parsed");
    }
  }

  section("A hidden marker claims its frame without decoding it");
  {
    /* One hidden marker for 0F.E000 and one real sensor for 21.1000. The
       marker claims the heartbeat so the monitor can fold it away, but it must
       never produce a reading of its own. */
    auto r = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"beat","kind":"hidden","match":{"dest":"0x0F","cmd":"0xE000"}},
      {"id":"status","name":"Status","kind":"sensor",
       "match":{"dest":"0x21","cmd":"0x1000","min_len":1},"extract":{"op":"u8","at":0}}]})J");
    CHECK(r.ok, "profile with a hidden marker parses");
    Engine e;
    int publishes = 0;
    e.set_sink([&](const Entity &, const Value &, bool) { publishes++; });
    e.load(r.profile);

    const uint8_t beat[] = {0x05};
    e.on_frame(0x0F, 0xE000, beat, 1000);   // the hidden keepalive
    const uint8_t status[] = {0x0C, 0x00};
    e.on_frame(0x21, 0x1000, status, 2000); // a claimed sensor
    const uint8_t mystery[] = {0xFF};
    e.on_frame(0x11, 0x1005, mystery, 3000); // claimed by nothing at all

    CHECK(publishes == 1, "only the real sensor published, got %d", publishes);
  }

  section("Out-of-range extract fields are refused, not narrowed");
  {
    /* Each of these used to survive the cast into a uint8_t as a different,
       plausible value: at 300 read byte 44, bit 256 became bit 0. A shift wider
       than the byte being shifted is undefined behaviour outright. Profiles are
       uploaded by users, so each has to be an error rather than a surprise. */
    auto at = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2},"extract":{"op":"u8","at":300}}]})J");
    CHECK(!at.ok, "at beyond a byte is rejected");

    auto bit = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2},"extract":{"op":"bit","at":0,"bit":256}}]})J");
    CHECK(!bit.ok, "bit 256 is rejected rather than wrapping to bit 0");

    auto shift = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2},"extract":{"op":"bits","at":0,"shift":40}}]})J");
    CHECK(!shift.ok, "a shift wider than the byte is rejected");

    auto ok = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2},"extract":{"op":"bits","at":3,"shift":4,"mask":15}}]})J");
    CHECK(ok.ok, "the same fields in range still parse");
  }

  section("Format strings reach printf, so the parser checks them");
  {
    /* A profile is a file people exchange, and these two strings are handed to
       printf. One conversion too many reads past the arguments: a crash on the
       device, or memory published as a reading. Reproduced with ASan before
       this check existed. */
    auto attack = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","kind":"text_sensor","match":{"dest":1,"cmd":2,"min_len":1},
       "extract":{"op":"u8","at":0},
       "transforms":[{"op":"map","values":{"1":"one"},"default":"%s %s %s %s"}]}]})J");
    CHECK(!attack.ok, "more conversions than arguments is refused");

    auto write = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","kind":"text_sensor","match":{"dest":1,"cmd":2,"min_len":1},
       "extract":{"op":"u8","at":0},
       "transforms":[{"op":"map","values":{"1":"one"},"default":"x%n"}]}]})J");
    CHECK(!write.ok, "%n is refused");

    auto star = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","match":{"dest":1,"cmd":2,"min_len":1},"extract":{"op":"u8","at":0},
       "transforms":[{"op":"format","fmt":"%*d"}]}]})J");
    CHECK(!star.ok, "a width taken from an argument is refused");

    auto wrong = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","kind":"text_sensor","match":{"dest":1,"cmd":2,"min_len":1},
       "extract":{"op":"u8","at":0},
       "transforms":[{"op":"map","values":{"1":"one"},"default":"code %s"}]}]})J");
    CHECK(!wrong.ok, "a map default takes an integer, not a string");

    auto real = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","kind":"text_sensor","match":{"dest":1,"cmd":2,"min_len":1},
       "extract":{"op":"u8","at":0},
       "transforms":[{"op":"map","values":{"1":"one"},"default":"Unknown (%d)"}]}]})J");
    CHECK(real.ok, "the form the shipped profiles use still parses");

    auto literal = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","match":{"dest":1,"cmd":2,"min_len":1},"extract":{"op":"u8","at":0},
       "transforms":[{"op":"format","fmt":"%d%% done"}]}]})J");
    CHECK(literal.ok, "an escaped per-cent sign is not a conversion");
  }

  section("A format string is given the type it asks for");
  {
    /* Whether the value arrives as a string or a number depends on the
       transforms before this one, so the profile author cannot know which. The
       value is converted to what the conversion expects rather than passed as
       whatever it happens to be. */
    auto r = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"v","match":{"dest":1,"cmd":2,"min_len":1},"extract":{"op":"u8","at":0},
       "transforms":[{"op":"format","fmt":"value %s"}]}]})J");
    CHECK(r.ok, "a string conversion on a numeric value parses");
    if (r.ok) {
      Engine e;
      std::string got;
      e.set_sink([&](const Entity &, const Value &v, bool) { got = to_string(v); });
      e.load(r.profile);
      const uint8_t payload[] = {9};
      e.on_frame(1, 2, payload, 1000);
      CHECK(got == "value 9", "the number was rendered, not dereferenced (got \"%s\")",
            got.c_str());
    }
  }

  section("A payload matcher is refused where nothing enforces it");
  {
    /* Only the monitor acts on match.payload, and only for a hidden marker. On
       an entity that decodes, the profile would promise a narrower match than
       the decoder performs. */
    auto bad = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2,"payload":"05"},"extract":{"op":"u8"}}]})J");
    CHECK(!bad.ok, "a decoding entity may not narrow by payload");

    auto good = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"h","kind":"hidden","match":{"dest":1,"cmd":2,"payload":"05"}}]})J");
    CHECK(good.ok, "a hidden marker still may");
  }

  section("A countdown is refused without a gate");
  {
    auto r = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"t","match":{"dest":1,"cmd":2},"extract":{"op":"u8"},
       "behavior":{"countdown":{"every_s":60}}}]})J");
    CHECK(!r.ok, "an ungated countdown is rejected");
  }

  section("A gated countdown carries the value forward, but only while gated");
  {
    auto r = parse_profile(R"J({"schema":1,"profile":{"id":"x"},"entities":[
      {"id":"remaining","name":"Remaining","kind":"sensor",
       "match":{"dest":"0x21","cmd":"0x1002","min_len":2},
       "extract":{"op":"u16be","at":0},
       "transforms":[{"op":"divide","value":60}],
       "behavior":{"countdown":{"every_s":60,"by":1,"min":0,
                   "while":{"entity":"run","is":"Running"}}}},
      {"id":"run","name":"Run","kind":"text_sensor",
       "match":{"dest":"0x11","cmd":"0x1001","min_len":1},
       "extract":{"op":"u8","at":0},
       "transforms":[{"op":"map","values":{"1":"Running","3":"Paused"}}]}]})J");
    CHECK(r.ok, "gated countdown parses");
    dump(r);
    if (!r.ok) return 1;

    Engine e;
    e.load(r.profile);
    const int64_t S = 1000000;

    /* Ten minutes remaining, and the machine says it is running. */
    const uint8_t t10[] = {0x04, 0xB0};   // 1200 s -> 20 min
    e.on_frame(0x21, 0x1002, t10, 1 * S);
    const uint8_t running[] = {1};
    e.on_frame(0x11, 0x1001, running, 1 * S);
    CHECK(as_number(e.states()[0].value) == 20, "starts at 20, got %g",
          as_number(e.states()[0].value));

    e.tick(30 * S);
    CHECK(as_number(e.states()[0].value) == 20, "nothing yet after 30 s");
    e.tick(70 * S);
    CHECK(as_number(e.states()[0].value) == 19, "one minute on, 19 left, got %g",
          as_number(e.states()[0].value));
    CHECK(e.states()[0].estimated, "and it is marked as carried forward");

    /* Paused: the appliance is not advancing, so neither do we. */
    const uint8_t paused[] = {3};
    e.on_frame(0x11, 0x1001, paused, 80 * S);
    e.tick(200 * S);
    e.tick(400 * S);
    CHECK(as_number(e.states()[0].value) == 19, "paused holds at 19, got %g",
          as_number(e.states()[0].value));

    /* A real reading wins over anything carried forward. */
    e.on_frame(0x11, 0x1001, running, 401 * S);
    const uint8_t t5[] = {0x01, 0x2C};    // 300 s -> 5 min
    e.on_frame(0x21, 0x1002, t5, 402 * S);
    CHECK(as_number(e.states()[0].value) == 5, "the bus overrides, got %g",
          as_number(e.states()[0].value));
    CHECK(!e.states()[0].estimated, "and the estimate flag is cleared");

    /* It stops at the floor rather than going negative. */
    for (int m = 1; m <= 8; m++) e.tick((402 + m * 61) * S);
    CHECK(as_number(e.states()[0].value) == 0, "floors at 0, got %g",
          as_number(e.states()[0].value));
  }

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

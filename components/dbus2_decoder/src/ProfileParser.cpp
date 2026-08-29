/*
   Reading a profile from JSON.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/ProfileParser.h"

#include <cJSON.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dbus2::decoder {
namespace {

struct Ctx {
  std::vector<std::string> *errors;
  std::string where;

  void fail(const char *what) const {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s: %s", where.c_str(), what);
    errors->push_back(buf);
  }
};

/* Numbers may appear as 20 or as "0x14". Hex is far more readable for bus
   addresses and commands, and the upstream configurations write them that way,
   so both are accepted. */
bool as_int(const cJSON *v, long *out) {
  if (!v)
    return false;
  if (cJSON_IsNumber(v)) {
    *out = static_cast<long>(v->valuedouble);
    return true;
  }
  if (cJSON_IsString(v) && v->valuestring) {
    char *end = nullptr;
    const long parsed = strtol(v->valuestring, &end, 0);  // base 0 -> 0x prefix works
    if (end && *end == '\0') {
      *out = parsed;
      return true;
    }
  }
  return false;
}

long int_or(const cJSON *obj, const char *key, long fallback) {
  long v = fallback;
  as_int(cJSON_GetObjectItem(obj, key), &v);
  return v;
}

std::string str_or(const cJSON *obj, const char *key, const char *fallback = "") {
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  return (cJSON_IsString(v) && v->valuestring) ? v->valuestring : fallback;
}

bool bool_or(const cJSON *obj, const char *key, bool fallback) {
  const cJSON *v = cJSON_GetObjectItem(obj, key);
  return cJSON_IsBool(v) ? cJSON_IsTrue(v) : fallback;
}

Value json_to_value(const cJSON *v) {
  if (cJSON_IsBool(v))
    return Value{static_cast<bool>(cJSON_IsTrue(v))};
  if (cJSON_IsNumber(v))
    return Value{v->valuedouble};
  if (cJSON_IsString(v) && v->valuestring)
    return Value{std::string(v->valuestring)};
  return Value{};
}

bool parse_extract(const cJSON *j, Extract *out, const Ctx &ctx) {
  if (!cJSON_IsObject(j)) {
    ctx.fail("extract must be an object");
    return false;
  }
  const std::string name = str_or(j, "op");
  auto op = op_from_string(name.c_str());
  if (!op) {
    ctx.fail(("unknown extract op \"" + name + "\"").c_str());
    return false;
  }
  out->op = *op;

  /* Checked before the narrowing, not after. These fields are uint8_t, so a
     value out of range used to arrive as a different, plausible one: at 300
     became 44 and read the wrong byte, bit 256 became bit 0 and passed the
     range check below it. Profiles are uploaded by users, so being told the
     number is wrong beats decoding something else. */
  struct Field {
    const char *name;
    long fallback, min, max;
    long value;
  } fields[] = {
      {"at", 0, 0, 255, 0},
      {"bit", 0, 0, 7, 0},
      /* One byte is read, so anything past its width is not a smaller shift but
         undefined behaviour in the shift itself. */
      {"shift", 0, 0, 7, 0},
      /* Both mask operations are applied to a single byte, so anything above
         0xFF is not a wider mask but a silently empty one. */
      {"mask", 0xFF, 0, 0xFF, 0},
      {"len", 1, 1, 255, 0},
  };
  for (auto &f : fields) {
    f.value = int_or(j, f.name, f.fallback);
    if (f.value < f.min || f.value > f.max) {
      ctx.fail((std::string(f.name) + " must be " + std::to_string(f.min) + "-" +
                std::to_string(f.max))
                   .c_str());
      return false;
    }
  }
  out->at = static_cast<uint8_t>(fields[0].value);
  out->bit = static_cast<uint8_t>(fields[1].value);
  out->shift = static_cast<uint8_t>(fields[2].value);
  out->mask = static_cast<uint32_t>(fields[3].value);
  out->len = static_cast<uint8_t>(fields[4].value);
  out->invert = bool_or(j, "invert", false);

  const cJSON *val = cJSON_GetObjectItem(j, "value");
  long n = 0;
  if (as_int(val, &n))
    out->value = static_cast<double>(n);
  else if (cJSON_IsNumber(val))
    out->value = val->valuedouble;

  return true;
}

bool parse_transform(const cJSON *j, Transform *out, const Ctx &ctx) {
  const std::string name = str_or(j, "op");
  auto op = top_from_string(name.c_str());
  if (!op) {
    ctx.fail(("unknown transform op \"" + name + "\"").c_str());
    return false;
  }
  out->op = *op;

  const cJSON *v = cJSON_GetObjectItem(j, "value");
  if (cJSON_IsNumber(v))
    out->value = v->valuedouble;
  out->min = static_cast<double>(int_or(j, "min", 0));
  out->max = static_cast<double>(int_or(j, "max", 0));
  out->decimals = static_cast<int>(int_or(j, "decimals", 0));
  out->exact = str_or(j, "method") == "exact";
  out->fmt = str_or(j, "fmt");
  out->fallback_text = str_or(j, "default");

  /* Both of these reach printf as the format string, and a profile is a file
     people send each other. One conversion too many reads past the arguments:
     on this chip a crash, or memory published as a reading. Checked here, in
     the same place that already refuses an out-of-range byte offset. */
  if (out->op == TOp::Format) {
    if (!check_format(out->fmt, &out->fmt_arg)) {
      ctx.fail("fmt must carry exactly one conversion, and no %n or * width");
      return false;
    }
  }
  if (out->op == TOp::Map && !out->fallback_text.empty()) {
    Transform::FmtArg expects = Transform::FmtArg::None;
    if (!check_format(out->fallback_text, &expects) ||
        (expects != Transform::FmtArg::None && expects != Transform::FmtArg::Int)) {
      /* The default is handed the raw code, which is an integer and nothing
         else, so %s or %f here would be reading something that was never
         passed. */
      ctx.fail("a map default takes at most one integer conversion, such as %d");
      return false;
    }
  }

  if (out->op == TOp::Calibrate) {
    const cJSON *pts = cJSON_GetObjectItem(j, "datapoints");
    if (!cJSON_IsArray(pts) || cJSON_GetArraySize(pts) == 0) {
      ctx.fail("calibrate_linear needs a non-empty datapoints array");
      return false;
    }
    const cJSON *pair = nullptr;
    cJSON_ArrayForEach(pair, pts) {
      if (!cJSON_IsArray(pair) || cJSON_GetArraySize(pair) != 2) {
        ctx.fail("each datapoint must be a pair [in, out]");
        return false;
      }
      out->points.emplace_back(cJSON_GetArrayItem(pair, 0)->valuedouble,
                               cJSON_GetArrayItem(pair, 1)->valuedouble);
    }
  }

  if (out->op == TOp::Map) {
    const cJSON *vals = cJSON_GetObjectItem(j, "values");
    if (!cJSON_IsObject(vals)) {
      ctx.fail("map needs a values object");
      return false;
    }
    const cJSON *item = nullptr;
    cJSON_ArrayForEach(item, vals) {
      if (!item->string || !cJSON_IsString(item))
        continue;
      out->table[strtod(item->string, nullptr)] = item->valuestring;
    }
  }

  if (out->op == TOp::Lookup) {
    const cJSON *rules = cJSON_GetObjectItem(j, "rules");
    if (!cJSON_IsArray(rules)) {
      ctx.fail("lookup needs a rules array");
      return false;
    }
    const cJSON *r = nullptr;
    cJSON_ArrayForEach(r, rules) {
      Rule rule;
      const cJSON *in = cJSON_GetObjectItem(r, "in");
      if (cJSON_IsArray(in)) {
        const cJSON *x = nullptr;
        cJSON_ArrayForEach(x, in) {
          long n = 0;
          if (as_int(x, &n))
            rule.in.push_back(static_cast<double>(n));
        }
      } else {
        long n = 0;
        if (as_int(in, &n))
          rule.in.push_back(static_cast<double>(n));
      }
      rule.out = json_to_value(cJSON_GetObjectItem(r, "out"));
      out->rules.push_back(std::move(rule));
    }

    const cJSON *fb = cJSON_GetObjectItem(j, "default");
    if (cJSON_IsString(fb) && std::strcmp(fb->valuestring, "hold") == 0)
      out->fallback = Fallback::Hold;
    else if (cJSON_IsString(fb) && std::strcmp(fb->valuestring, "drop") == 0)
      out->fallback = Fallback::Drop;
    else if (fb) {
      out->fallback = Fallback::Value;
      out->fallback_value = json_to_value(fb);
    } else {
      out->fallback = Fallback::Drop;
    }
  }

  if (out->op == TOp::FilterOut) {
    const cJSON *vals = cJSON_GetObjectItem(j, "values");
    const cJSON *x = nullptr;
    cJSON_ArrayForEach(x, vals) {
      long n = 0;
      if (as_int(x, &n))
        out->filter.push_back(static_cast<double>(n));
    }
  }
  return true;
}

Kind kind_from(const std::string &s) {
  if (s == "binary_sensor") return Kind::BinarySensor;
  if (s == "text_sensor") return Kind::TextSensor;
  if (s == "event") return Kind::Event;
  if (s == "hidden") return Kind::Hidden;
  return Kind::Sensor;
}

/* Parse a hex string like "0C00" or "0c 00" into bytes. Anything that is not a
   whole number of hex digit pairs yields nothing, which reads as "match any". */
std::vector<uint8_t> parse_hex(const std::string &s) {
  std::string clean;
  for (char c : s)
    if (std::isxdigit(static_cast<unsigned char>(c)))
      clean.push_back(c);
  std::vector<uint8_t> out;
  if (clean.size() % 2 != 0)
    return out;
  for (size_t i = 0; i < clean.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::strtol(clean.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

}  // namespace

ParseResult parse_profile(const char *json) {
  ParseResult res;
  Ctx ctx{&res.errors, "profile"};

  cJSON *root = cJSON_Parse(json);
  if (!root) {
    const char *at = cJSON_GetErrorPtr();
    char buf[128];
    snprintf(buf, sizeof(buf), "invalid JSON near \"%.32s\"", at ? at : "");
    res.errors.emplace_back(buf);
    return res;
  }

  const long schema = int_or(root, "schema", 0);
  if (schema != 1)
    ctx.fail("unsupported or missing \"schema\" (expected 1)");

  const cJSON *meta = cJSON_GetObjectItem(root, "profile");
  if (cJSON_IsObject(meta)) {
    res.profile.meta.id = str_or(meta, "id");
    res.profile.meta.manufacturer = str_or(meta, "manufacturer");
    res.profile.meta.model = str_or(meta, "model");
    res.profile.meta.product = str_or(meta, "product");
    res.profile.meta.appliance = str_or(meta, "appliance");
    res.profile.meta.source = str_or(meta, "source");
    const cJSON *cr = cJSON_GetObjectItem(meta, "credits");
    const cJSON *c = nullptr;
    cJSON_ArrayForEach(c, cr)
      if (cJSON_IsString(c))
        res.profile.meta.credits.emplace_back(c->valuestring);
  }
  if (res.profile.meta.id.empty())
    ctx.fail("profile.id is required");

  const cJSON *bus = cJSON_GetObjectItem(root, "bus");
  if (cJSON_IsObject(bus))
    res.profile.baud = static_cast<uint32_t>(int_or(bus, "baud", 0));

  const cJSON *dev = cJSON_GetObjectItem(root, "device");
  if (cJSON_IsObject(dev)) {
    const cJSON *av = cJSON_GetObjectItem(dev, "availability");
    if (cJSON_IsObject(av))
      res.profile.availability_timeout_s = static_cast<uint32_t>(int_or(av, "any_frame_timeout_s", 60));
  }

  const cJSON *ents = cJSON_GetObjectItem(root, "entities");
  if (!cJSON_IsArray(ents) || cJSON_GetArraySize(ents) == 0) {
    ctx.fail("entities must be a non-empty array");
    cJSON_Delete(root);
    return res;
  }

  int idx = 0;
  const cJSON *je = nullptr;
  cJSON_ArrayForEach(je, ents) {
    Entity e;
    e.id = str_or(je, "id");

    char where[64];
    snprintf(where, sizeof(where), "entity[%d]%s%s", idx, e.id.empty() ? "" : " ", e.id.c_str());
    Ctx ec{&res.errors, where};
    idx++;

    if (e.id.empty()) {
      ec.fail("id is required");
      continue;
    }
    e.name = str_or(je, "name", e.id.c_str());
    e.kind = kind_from(str_or(je, "kind", "sensor"));

    const cJSON *m = cJSON_GetObjectItem(je, "match");
    if (!cJSON_IsObject(m)) {
      ec.fail("match is required");
      continue;
    }
    long dest = -1, cmd = -1;
    if (!as_int(cJSON_GetObjectItem(m, "dest"), &dest) || dest < 0 || dest > 0xFF) {
      ec.fail("match.dest must be a byte");
      continue;
    }
    if (!as_int(cJSON_GetObjectItem(m, "cmd"), &cmd) || cmd < 0 || cmd > 0xFFFF) {
      ec.fail("match.cmd must be a 16-bit value");
      continue;
    }
    e.dest = static_cast<uint8_t>(dest);
    e.cmd = static_cast<uint16_t>(cmd);
    e.min_len = static_cast<uint8_t>(int_or(m, "min_len", 0));
    e.match_payload = parse_hex(str_or(m, "payload"));

    /* Only a hidden marker may narrow its match by payload, because only the
       monitor acts on it -- the decoder matches on the address alone. Accepting
       it on an entity that decodes would be the worst kind of quiet: the
       profile would say "only these frames" and the values would come from all
       of them. */
    if (!e.match_payload.empty() && e.kind != Kind::Hidden) {
      ec.fail("match.payload is only supported on a hidden marker");
      continue;
    }

    /* A hidden marker decodes nothing: it exists to claim this pair and tell
       the monitor to fold it away. It therefore carries no extract,
       transforms, guards or HA metadata, and the rest of the entity parse is
       skipped. */
    if (e.kind == Kind::Hidden) {
      res.profile.entities.push_back(std::move(e));
      continue;
    }

    if (!parse_extract(cJSON_GetObjectItem(je, "extract"), &e.extract, ec))
      continue;

    bool bad = false;
    const cJSON *tf = cJSON_GetObjectItem(je, "transforms");
    const cJSON *t = nullptr;
    cJSON_ArrayForEach(t, tf) {
      Transform tr;
      if (!parse_transform(t, &tr, ec)) { bad = true; break; }
      e.transforms.push_back(std::move(tr));
    }
    if (bad)
      continue;

    const cJSON *guards = cJSON_GetObjectItem(je, "guards");
    const cJSON *g = nullptr;
    cJSON_ArrayForEach(g, guards) {
      Guard gu;
      if (!parse_extract(cJSON_GetObjectItem(g, "when"), &gu.when, ec)) { bad = true; break; }
      gu.eq = static_cast<double>(int_or(g, "eq", 0));
      gu.emit = json_to_value(cJSON_GetObjectItem(g, "emit"));
      e.guards.push_back(std::move(gu));
    }
    if (bad)
      continue;

    const cJSON *b = cJSON_GetObjectItem(je, "behavior");
    if (cJSON_IsObject(b)) {
      e.behavior.on_change = str_or(b, "publish", "on_change") != "always";
      e.behavior.throttle_ms = static_cast<uint32_t>(int_or(b, "throttle_ms", 0));
      e.behavior.debounce_ms = static_cast<uint32_t>(int_or(b, "debounce_ms", 0));
      e.behavior.expire_after_s = static_cast<uint32_t>(int_or(b, "expire_after_s", 0));
      e.behavior.momentary_ms = static_cast<uint32_t>(int_or(b, "momentary_ms", 0));
      e.behavior.retain = bool_or(b, "retain", true);

      /* Carrying a value forward between two readings. Refused without a gate:
         a remaining time counts down only while the appliance runs, and at rest
         the same frame carries the expected duration instead -- so an ungated
         countdown would confidently report a number that is simply not true. */
      const cJSON *cd = cJSON_GetObjectItem(b, "countdown");
      if (cJSON_IsObject(cd)) {
        const cJSON *gate = cJSON_GetObjectItem(cd, "while");
        std::string gate_id, gate_val;
        if (cJSON_IsObject(gate)) {
          gate_id = str_or(gate, "entity");
          gate_val = str_or(gate, "is");
        }
        if (gate_id.empty() || gate_val.empty()) {
          ec.fail("countdown needs a while: {entity, is} gate saying when it may run");
          continue;
        }
        e.behavior.countdown_every_s = static_cast<uint32_t>(int_or(cd, "every_s", 60));
        const cJSON *by = cJSON_GetObjectItem(cd, "by");
        e.behavior.countdown_by = cJSON_IsNumber(by) ? by->valuedouble : 1.0;
        const cJSON *mn = cJSON_GetObjectItem(cd, "min");
        e.behavior.countdown_min = cJSON_IsNumber(mn) ? mn->valuedouble : 0.0;
        e.behavior.countdown_gate_id = gate_id;
        e.behavior.countdown_gate_value = gate_val;
      }
    }

    const cJSON *ha = cJSON_GetObjectItem(je, "ha");
    if (cJSON_IsObject(ha)) {
      e.ha.device_class = str_or(ha, "device_class");
      e.ha.unit = str_or(ha, "unit_of_measurement");
      e.ha.state_class = str_or(ha, "state_class");
      e.ha.icon = str_or(ha, "icon");
      e.ha.entity_category = str_or(ha, "entity_category");
      e.ha.decimals = static_cast<int>(int_or(ha, "accuracy_decimals", -1));
    }

    res.profile.entities.push_back(std::move(e));
  }

  cJSON_Delete(root);

  if (res.profile.entities.empty())
    ctx.fail("no usable entities");

  res.profile.reindex();
  res.ok = res.errors.empty();
  return res;
}

}  // namespace dbus2::decoder

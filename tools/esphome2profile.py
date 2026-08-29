#!/usr/bin/env python3
"""
Convert the upstream ESPHome configurations into decoder profiles.

The appliance knowledge in that project lives in YAML lambdas, contributed over
years by people with the machines in front of them. Rewriting it by hand would
lose some of it and introduce mistakes in the rest, so it gets translated
mechanically and whatever does not translate is listed rather than guessed at.

Usage:
    esphome2profile.py --upstream ../bsh-home-appliances --out profiles/
    esphome2profile.py --report report.md path/to/one.yaml

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import argparse
import json
import pathlib
import re
import sys

try:
    import yaml
except ImportError:
    sys.exit("Needs PyYAML:  pip install pyyaml")


# --------------------------------------------------------------------- YAML --

class ESPHomeLoader(yaml.SafeLoader):
    """ESPHome YAML is not plain YAML: it carries !secret, !lambda and friends.

    They are irrelevant to decoding but make a strict loader refuse the file, so
    they are absorbed into placeholders.
    """


def _tagged(loader, node):
    if isinstance(node, yaml.ScalarNode):
        return {"__tag__": node.tag, "value": loader.construct_scalar(node)}
    return {"__tag__": node.tag, "value": None}


for tag in ("!secret", "!lambda", "!include", "!extend", "!remove"):
    ESPHomeLoader.add_constructor(tag, _tagged)


def expand_substitutions(text: str) -> str:
    """Resolve ${x} / $x against the file's own substitutions block.

    Several contributed configurations parameterise pins and names this way; left
    alone, the placeholders end up in the output as literal text.
    """
    m = re.search(r"^substitutions:\n((?:[ \t]+.*\n|\n)*)", text, re.M)
    if not m:
        return text
    subs = {}
    for line in m.group(1).splitlines():
        kv = re.match(r"\s+([A-Za-z0-9_]+):\s*(.*?)\s*$", line)
        if kv:
            subs[kv.group(1)] = kv.group(2).strip("\"'")
    for k, v in subs.items():
        text = text.replace("${%s}" % k, v).replace("$%s" % k, v)
    return text


# ---------------------------------------------------------------- lambdas ----

HEX = r"(0x[0-9a-fA-F]+|\d+)"


def _n(s):
    return int(s, 0)


# Ordered; first match wins. Each returns (extract, [transforms]).
LAMBDA_RULES = [
    # return x[N];
    (rf"^return\s+x\[(\d+)\]\s*;$",
     lambda m: ({"op": "u8", "at": int(m[1])}, [])),

    # return std::to_string(x[N]);   -- a map filter almost always follows
    (rf"^return\s+std::to_string\s*\(\s*x\[(\d+)\]\s*\)\s*;$",
     lambda m: ({"op": "u8", "at": int(m[1])}, [])),

    # return (x[N] >> S) & 0x01;
    (rf"^return\s*\(?\s*x\[(\d+)\]\s*>>\s*(\d+)\s*\)?\s*&\s*0x0?1\s*;$",
     lambda m: ({"op": "bit", "at": int(m[1]), "bit": int(m[2])}, [])),

    # return !((x[N] >> S) & 0x01);
    (rf"^return\s*!\s*\(\s*\(?\s*x\[(\d+)\]\s*>>\s*(\d+)\s*\)?\s*&\s*0x0?1\s*\)\s*;$",
     lambda m: ({"op": "bit", "at": int(m[1]), "bit": int(m[2]), "invert": True}, [])),

    # return x[N] & 0xMM;
    (rf"^return\s*\(?\s*x\[(\d+)\]\s*\)?\s*&\s*{HEX}\s*;$",
     lambda m: ({"op": "mask", "at": int(m[1]), "mask": _n(m[2])}, [])),

    # return (int16_t)((x[a] << 8) | x[b]);
    (rf"^return\s*\(\s*int16_t\s*\)\s*\(\s*\(\s*x\[(\d+)\]\s*<<\s*8\s*\)\s*\|\s*x\[(\d+)\]\s*\)\s*;$",
     lambda m: ({"op": "i16be", "at": int(m[1])}, []) if int(m[2]) == int(m[1]) + 1 else None),

    # return (((int16_t) x[a] << 8 | x[b]) / D);
    (rf"^return\s*\(\s*\(\s*\(\s*int16_t\s*\)\s*x\[(\d+)\]\s*<<\s*8\s*\|\s*x\[(\d+)\]\s*\)\s*/\s*(\d+)\s*\)\s*;$",
     lambda m: ({"op": "i16be", "at": int(m[1])}, [{"op": "divide", "value": int(m[3])}])
     if int(m[2]) == int(m[1]) + 1 else None),

    # return (x[a] << 8 | x[b]) / D;
    (rf"^return\s*\(\s*x\[(\d+)\]\s*<<\s*8\s*\|\s*x\[(\d+)\]\s*\)\s*/\s*(\d+)\s*;$",
     lambda m: ({"op": "u16be", "at": int(m[1])}, [{"op": "divide", "value": int(m[3])}])
     if int(m[2]) == int(m[1]) + 1 else None),

    # return x[N] == V;  /  != V
    (rf"^return\s*\(?\s*x\[(\d+)\]\s*(==|!=)\s*{HEX}\s*\)?\s*;$",
     lambda m: ({"op": "eq", "at": int(m[1]), "value": _n(m[3]),
                 **({"invert": True} if m[2] == "!=" else {})}, [])),

    # return x[N] + V;  /  - V
    (rf"^return\s*\(?\s*x\[(\d+)\]\s*([-+])\s*(\d+)\s*\)?\s*;$",
     lambda m: ({"op": "u8", "at": int(m[1])},
                [{"op": "offset", "value": int(m[3]) * (-1 if m[2] == "-" else 1)}])),

    # return x[N] * V;
    (rf"^return\s*\(?\s*x\[(\d+)\]\s*\*\s*(\d+)\s*\)?\s*;$",
     lambda m: ({"op": "u8", "at": int(m[1])}, [{"op": "multiply", "value": int(m[2])}])),

    # return 1; / return true;
    (r"^return\s*(1|true)\s*;$",
     lambda m: ({"op": "const", "value": 1}, [])),

    # return (int16_t)(x[N] - V);   and   return ((int16_t)x[N]) - V;
    (rf"^return\s*\(\s*int16_t\s*\)\s*\(?\s*x\[(\d+)\]\s*([-+])\s*(\d+)\s*\)?\s*;$",
     lambda m: ({"op": "u8", "at": int(m[1])},
                [{"op": "offset", "value": int(m[3]) * (-1 if m[2] == "-" else 1)}])),

    # return std::to_string((int16_t)(x[N]));
    (rf"^return\s*std::to_string\s*\(\s*\(\s*int16_t\s*\)\s*\(?\s*x\[(\d+)\]\s*\)?\s*\)\s*;$",
     lambda m: ({"op": "i8", "at": int(m[1])}, [])),

    # return ((int16_t) x[a] << 8 | x[b]) / D;   (after the outer parens are stripped)
    (rf"^return\s*\(\s*\(?\s*int16_t\s*\)?\s*x\[(\d+)\]\s*<<\s*8\s*\|\s*x\[(\d+)\]\s*\)\s*/\s*(\d+)\s*;$",
     lambda m: ({"op": "i16be", "at": int(m[1])}, [{"op": "divide", "value": int(m[3])}])
     if int(m[2]) == int(m[1]) + 1 else None),

    # return ((int16_t) x[a] << 8 | x[b]);
    (rf"^return\s*\(\s*\(?\s*int16_t\s*\)?\s*x\[(\d+)\]\s*<<\s*8\s*\|\s*x\[(\d+)\]\s*\)\s*;$",
     lambda m: ({"op": "i16be", "at": int(m[1])}, [])
     if int(m[2]) == int(m[1]) + 1 else None),

    # return ((x[a] << 8) | x[b]) / D;
    (rf"^return\s*\(\s*\(\s*x\[(\d+)\]\s*<<\s*8\s*\)\s*\|\s*x\[(\d+)\]\s*\)\s*/\s*(\d+)\s*;$",
     lambda m: ({"op": "u16be", "at": int(m[1])}, [{"op": "divide", "value": int(m[3])}])
     if int(m[2]) == int(m[1]) + 1 else None),

    # return (int16_t)x[N];        -- a single signed byte
    (rf"^return\s*\(\s*int16_t\s*\)\s*x\[(\d+)\]\s*;$",
     lambda m: ({"op": "i8", "at": int(m[1])}, [])),

    # return ((int16_t)x[N]) - V;
    (rf"^return\s*\(\s*\(\s*int16_t\s*\)\s*x\[(\d+)\]\s*\)\s*([-+])\s*(\d+)\s*;$",
     lambda m: ({"op": "i8", "at": int(m[1])},
                [{"op": "offset", "value": int(m[3]) * (-1 if m[2] == "-" else 1)}])),

    # return (x[N] & M) == V;
    (rf"^return\s*\(\s*x\[(\d+)\]\s*&\s*{HEX}\s*\)\s*==\s*{HEX}\s*;$",
     lambda m: ({"op": "bits", "at": int(m[1]), "shift": 0, "mask": _n(m[2])},
                [{"op": "lookup", "rules": [{"in": [_n(m[3])], "out": True}], "default": False}])),
]


def strip_outer_parens(expr: str) -> str:
    """Remove redundant wrapping parentheses: `(x[0])` and `(x[6] & 0x08)`.

    Contributors bracket expressions to taste, and matching every variant with
    its own pattern would multiply the rule table for no gain.
    """
    expr = expr.strip()
    while expr.startswith("(") and expr.endswith(")"):
        depth = 0
        for i, c in enumerate(expr):
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0 and i != len(expr) - 1:
                    return expr  # the parens are not wrapping the whole thing
        expr = expr[1:-1].strip()
    return expr


RETURN_GLOBAL = re.compile(r"return\s+id\s*\(\s*(\w+)\s*\)\s*;\s*$")
ASSIGN_GLOBAL = re.compile(r"id\s*\(\s*(\w+)\s*\)\s*=\s*([^;]+);")


def unwrap_globals(body: str) -> str:
    """Strip the ESPHome globals cache idiom.

        id(g) = EXPR; return id(g);

    The variable exists so other lambdas can republish the value when Home
    Assistant reconnects -- bookkeeping for a mechanism we do not have, since
    retained MQTT does the same thing for free. The decoding is the right-hand
    side, and `lookup` with a `hold` fallback covers what the cache was for.

    Assignments to *other* entities' globals are side effects for those entities
    and are dropped here; each entity is converted from its own lambda.
    """
    flat = " ".join(body.split())
    m = RETURN_GLOBAL.search(flat)
    if not m:
        return body
    target = m.group(1)
    flat = flat[:m.start()].strip()

    def repl(a):
        return f"return {a.group(2).strip()};" if a.group(1) == target else ""

    flat = ASSIGN_GLOBAL.sub(repl, flat).strip()
    return flat if flat else body


def normalise(body: str) -> str:
    flat = " ".join(body.split())
    m = re.fullmatch(r"return\s+(.*?)\s*;", flat)
    if m:
        return "return " + strip_outer_parens(m.group(1)) + ";"
    return flat


# A trailing `return NAN;` after the closing brace is the common shape: the
# switch handles the known codes and anything else has no meaningful value.
SWITCH_HEAD = re.compile(
    r"^switch\s*\(\s*(?P<sel>[^)]+?)\s*\)\s*\{(?P<body>.*)\}"
    r"(?:\s*return\s+(?P<tail>NAN|\{\s*\}|\"\")\s*;)?\s*$", re.S)
SWITCH_CASE = re.compile(r"case\s+(0x[0-9a-fA-F]+|\d+)\s*:\s*return\s+([^;]+);")
SWITCH_DEFAULT = re.compile(r"default\s*:\s*return\s+([^;]+);")


def _literal(text: str):
    """A case's return value, as JSON."""
    text = text.strip()
    if text in ("true", "false"):
        return text == "true"
    if re.fullmatch(r"-?\d+", text):
        return int(text)
    if re.fullmatch(r"-?\d+\.\d+f?", text):
        return float(text.rstrip("f"))
    m = re.fullmatch(r'std::string\s*\(\s*"(.*)"\s*\)', text)
    if m:
        return m.group(1)
    if text.startswith('"') and text.endswith('"'):
        return text[1:-1]
    return None


def convert_switch(body: str):
    """A switch over one byte is a lookup written differently."""
    flat = " ".join(body.split())
    # Comments would otherwise swallow the following case label.
    flat = re.sub(r"//.*?(?=case|default|\}|$)", "", flat)
    m = SWITCH_HEAD.match(flat)
    if not m:
        return None

    sel = m.group("sel").strip()
    sm = re.fullmatch(r"x\[(\d+)\]", sel)
    shift = None
    if not sm:
        sm = re.fullmatch(r"x\[(\d+)\]\s*>>\s*(\d+)", sel)
        if not sm:
            return None
        shift = int(sm.group(2))
    at = int(sm.group(1))

    rules = []
    for value, ret in SWITCH_CASE.findall(m.group("body")):
        lit = _literal(ret)
        if lit is None:
            return None
        rules.append({"in": [int(value, 0)], "out": lit})
    if not rules:
        return None

    lookup = {"op": "lookup", "rules": rules}
    dm = SWITCH_DEFAULT.search(m.group("body"))
    if dm:
        lit = _literal(dm.group(1))
        lookup["default"] = lit if lit is not None else "hold"
    elif m.group("tail"):
        # NAN / {} / "" all mean "no value for this code" in ESPHome.
        lookup["default"] = "drop"
    else:
        lookup["default"] = "hold"

    extract = ({"op": "bits", "at": at, "shift": shift, "mask": 0xFF} if shift is not None
               else {"op": "u8", "at": at})
    return extract, [lookup]


TERNARY = re.compile(
    r"^return\s*\(?\s*x\[(\d+)\]\s*&\s*(0x[0-9a-fA-F]+|\d+)\s*\)?\s*"
    r"\?\s*([^:]+?)\s*:\s*([^;]+?)\s*;$")


def convert_ternary(body: str):
    """`x[N] & M ? a : b` -- a mask with two outcomes."""
    m = TERNARY.match(normalise(body))
    if not m:
        return None
    yes, no = _literal(m.group(3)), _literal(m.group(4))
    if yes is None or no is None:
        return None
    return ({"op": "mask", "at": int(m.group(1)), "mask": int(m.group(2), 0)},
            [{"op": "lookup", "rules": [{"in": [1], "out": yes}, {"in": [0], "out": no}],
              "default": no}])


def convert_lambda(body: str):
    """Translate one lambda. Returns (extract, transforms) or None."""
    flat = normalise(body)
    for pattern, build in LAMBDA_RULES:
        m = re.match(pattern, flat)
        if m:
            out = build(m)
            if out:
                return out
    return None


CHAIN_IF = re.compile(
    r"if\s*\(\s*(?P<cond>[^)]+?)\s*\)\s*\{?\s*return\s+(?P<ret>[^;]+);\s*\}?", re.S)
COND_EQ = re.compile(r"x\[(\d+)\]\s*==\s*(0x[0-9a-fA-F]+|\d+)")


def convert_chain(body: str):
    """Translate an if/else-if chain over one byte into a lookup.

    These are the bulk of the multi-line lambdas: a byte compared against a
    handful of values, falling through to a globals: variable holding the last
    known state. That fallback is what Lookup's `hold` exists for.
    """
    matches = list(CHAIN_IF.finditer(body))
    if not matches:
        return None

    at, rules = None, []
    for m in matches:
        vals = COND_EQ.findall(m.group("cond"))
        if not vals:
            return None
        offsets = {int(v[0]) for v in vals}
        if len(offsets) != 1:
            return None  # compares more than one byte -- out of scope
        off = offsets.pop()
        if at is None:
            at = off
        elif at != off:
            return None

        ret = m.group("ret").strip()
        if ret in ("true", "false"):
            out = ret == "true"
        elif re.fullmatch(r"-?\d+(\.\d+)?", ret):
            out = float(ret) if "." in ret else int(ret)
        elif ret.startswith('"') and ret.endswith('"'):
            out = ret[1:-1]
        else:
            return None  # references a global or another entity
        rules.append({"in": [_n(v[1]) for v in vals], "out": out})

    if at is None or not rules:
        return None

    tail = body[matches[-1].end():]
    if re.search(r"return\s+id\s*\(", tail):
        default = "hold"           # falls back to a globals: variable
    else:
        default = "hold"           # keeping the last value is the safe reading
    return {"op": "u8", "at": at}, [{"op": "lookup", "rules": rules, "default": default}]


# ---------------------------------------------------------------- filters ----

def convert_filters(filters, report, where):
    """Translate an ESPHome filter chain."""
    out = []
    behavior = {}
    if not isinstance(filters, list):
        return out, behavior

    for f in filters:
        if not isinstance(f, dict):
            continue
        for key, val in f.items():
            if key == "multiply":
                out.append({"op": "multiply", "value": float(val)})
            elif key == "offset":
                out.append({"op": "offset", "value": float(val)})
            elif key == "invert":
                out.append({"op": "invert"})
            elif key == "throttle":
                behavior["throttle_ms"] = _duration_ms(val)
            elif key == "debounce":
                behavior["debounce_ms"] = _duration_ms(val)
            elif key == "timeout":
                behavior["expire_after_s"] = max(1, _duration_ms(val) // 1000)
            elif key in ("map", "calibrate_linear"):
                conv = _convert_map(val) if key == "map" else _convert_calibrate(val)
                if conv:
                    out.append(conv)
                else:
                    # Losing a filter is worse than failing to convert the
                    # entity: the value still looks plausible, just wrong.
                    report.append(f"{where}: could not read \"{key}\" -- entity dropped")
                    return None, {}
            elif key == "lambda":
                report.append(f"{where}: filter lambda not converted -- needs review")
            else:
                report.append(f"{where}: unsupported filter \"{key}\"")
    return [o for o in out if o], behavior


def _duration_ms(v) -> int:
    s = str(v).strip()
    m = re.fullmatch(r"(\d+(?:\.\d+)?)\s*(ms|s|min|h)?", s)
    if not m:
        return 0
    n = float(m.group(1))
    return int(n * {"ms": 1, "s": 1000, "min": 60000, "h": 3600000}.get(m.group(2) or "s", 1000))


def _entries(val):
    """ESPHome writes map/calibrate entries as "in -> out" strings.

    calibrate_linear nests them under a datapoints key while map lists them
    directly, so the wrapper has to be unwrapped first. Missing this produced
    profiles that parsed cleanly and silently dropped the conversion -- a
    temperature reported as the raw code 0-7 instead of 20-90 degrees.
    """
    if isinstance(val, dict) and "datapoints" in val:
        val = val["datapoints"]
    items = val if isinstance(val, list) else [val]
    out = []
    for it in items:
        if isinstance(it, dict):
            for k, v in it.items():
                out.append((str(k), str(v)))
        elif isinstance(it, str) and "->" in it:
            a, b = it.split("->", 1)
            out.append((a.strip(), b.strip()))
    return out


def _convert_map(val):
    table = {}
    for a, b in _entries(val):
        try:
            table[str(int(a, 0))] = b
        except ValueError:
            continue
    return {"op": "map", "values": table} if table else None


def _convert_calibrate(val):
    pts = []
    for a, b in _entries(val):
        try:
            pts.append([float(a), float(b)])
        except ValueError:
            continue
    return {"op": "calibrate_linear", "datapoints": pts} if pts else None


# --------------------------------------------------------------- entities ----

PLATFORM_KIND = {
    "sensor": "sensor",
    "binary_sensor": "binary_sensor",
    "text_sensor": "text_sensor",
}
SUBKEY = {
    "sensor": "sensors",
    "binary_sensor": "binary_sensors",
    "text_sensor": "text_sensors",
}


def lambda_text(v):
    if isinstance(v, dict) and v.get("__tag__") == "!lambda":
        return v.get("value") or ""
    return str(v) if v is not None else ""


def convert_entity(raw, platform, dest, cmd, report, source):
    eid = str(raw.get("id") or "").strip()
    if not eid:
        return None
    where = f"{source}:{eid}"

    if raw.get("internal") is True:
        # The author marked this as feeding other ESPHome logic rather than being
        # shown. Publishing it to Home Assistant would contradict that.
        report.append(f"{where}: internal entity -- not exported")
        return None

    body = lambda_text(raw.get("lambda"))
    if not body:
        report.append(f"{where}: no lambda")
        return None

    unwrapped = unwrap_globals(body)
    conv = (convert_lambda(unwrapped) or convert_switch(unwrapped)
            or convert_ternary(unwrapped) or convert_chain(unwrapped))
    if not conv:
        first = " ".join(body.split())[:70]
        report.append(f"{where}: lambda not converted -- {first}")
        return None
    extract, transforms = conv

    filters, behavior = convert_filters(raw.get("filters"), report, where)
    if filters is None:
        return None
    transforms = transforms + filters

    ent = {
        "id": re.sub(r"^bsh_", "", eid),
        "name": str(raw.get("name") or eid),
        "kind": PLATFORM_KIND[platform],
        "match": {"dest": f"0x{dest:02x}", "cmd": f"0x{cmd:04x}"},
        "extract": extract,
    }

    # A const extract with an on_press that publishes NAN is the upstream idiom
    # for a momentary event; it is a press, not a state.
    if extract.get("op") == "const" and raw.get("on_press"):
        behavior["momentary_ms"] = 1000
        ent["kind"] = "binary_sensor"

    need = _min_len(extract)
    if need:
        ent["match"]["min_len"] = need
    if transforms:
        ent["transforms"] = transforms
    if behavior:
        ent["behavior"] = behavior

    ha = {}
    for key, out in (("device_class", "device_class"), ("unit_of_measurement", "unit_of_measurement"),
                     ("state_class", "state_class"), ("icon", "icon"),
                     ("entity_category", "entity_category"), ("accuracy_decimals", "accuracy_decimals")):
        if raw.get(key) is not None:
            ha[out] = raw[key]
    if ha:
        ent["ha"] = ha
    return ent


def _min_len(extract):
    op, at = extract.get("op"), extract.get("at", 0)
    width = {"u8": 1, "i8": 1, "bit": 1, "bits": 1, "mask": 1, "eq": 1,
             "u16be": 2, "u16le": 2, "i16be": 2, "i16le": 2, "u24be": 3, "u32be": 4}.get(op)
    return at + width if width else 0


# ------------------------------------------------------------------- file ----

def convert_file(path: pathlib.Path, report):
    text = expand_substitutions(path.read_text(encoding="utf-8"))
    # The leading comment block names the appliance far more reliably than any
    # model-number rule.
    header = "\n".join(l for l in text.splitlines()[:30] if l.startswith("#"))
    try:
        doc = yaml.load(text, Loader=ESPHomeLoader)
    except yaml.YAMLError as e:
        report.append(f"{path.name}: unreadable -- {e}")
        return None
    if not isinstance(doc, dict):
        return None

    name = doc.get("esphome", {}).get("friendly_name") or path.stem
    entities, groups, seen = [], 0, [0]

    for platform, subkey in SUBKEY.items():
        for group in doc.get(platform) or []:
            if not isinstance(group, dict) or group.get("platform") != "bshdbus":
                continue
            groups += 1
            dest, cmd = group.get("dest"), group.get("command")
            if dest is None or cmd is None:
                report.append(f"{path.name}: group without dest/command")
                continue
            for raw in group.get(subkey) or []:
                seen[0] += 1
                ent = convert_entity(raw, platform, int(dest), int(cmd), report, path.name)
                if ent:
                    entities.append(ent)

    if not entities:
        return None

    model = re.sub(r"^bsh-dbus-", "", path.stem)
    return seen[0], {
        "schema": 1,
        "profile": {
            "id": model,
            "model": model.upper(),
            "appliance": _guess_appliance(name, path.stem, header),
            "source": path.name,
            "credits": _credits(text),
        },
        "bus": {"baud": _baud(doc)},
        "entities": entities,
    }, groups


def _guess_appliance(friendly, stem, header=""):
    """The file's own header comment states the appliance type in almost every
    case; guessing from the model number is only a fallback.

    Looking at friendly_name and the filename alone left five files as unknown
    even though their first line said "dishwasher" or "Refrigerator".
    """
    hay = f"{friendly} {stem} {header}".lower()
    for needle, kind in (
        ("trockner", "dryer"), ("dryer", "dryer"),
        ("spülmaschine", "dishwasher"), ("spüler", "dishwasher"),
        ("geschirrspüler", "dishwasher"), ("dishwash", "dishwasher"), ("dish washer", "dishwasher"),
        ("waschmaschine", "washing_machine"), ("washing machine", "washing_machine"),
        ("refrigerator", "fridge"), ("fridge", "fridge"), ("kühlschrank", "fridge"),
        ("steamer", "steamer"), ("dampfgarer", "steamer"),
        ("extractor", "hood"), ("dunstabzug", "hood"), ("hood", "hood"),
        ("oven", "oven"), ("backofen", "oven"), ("herd", "oven"),
    ):
        if needle in hay:
            return kind
    # Model prefixes, as a last resort.
    for prefix, kind in (("wt", "dryer"), ("wa", "washing_machine"), ("wm", "washing_machine"),
                         ("wu", "washing_machine"), ("s", "dishwasher"), ("k", "fridge")):
        if stem.lower().replace("bsh-dbus-", "").startswith(prefix):
            return kind
    return "unknown"


def _credits(text):
    out = []
    for m in re.finditer(r"^#\s*\(C\)\s*\d{4}(?:-\d{4})?\s+(.+?)\s*$", text, re.M):
        who = m.group(1).strip()
        if who and who not in out:
            out.append(who)
    return out


def _baud(doc):
    uart = doc.get("uart")
    if isinstance(uart, list):
        uart = uart[0] if uart else None
    if isinstance(uart, dict):
        try:
            return int(uart.get("baud_rate", 9600))
        except (TypeError, ValueError):
            pass
    return 9600


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="*", type=pathlib.Path)
    ap.add_argument("--upstream", type=pathlib.Path,
                    help="upstream checkout; converts its YAML configurations")
    ap.add_argument("--out", type=pathlib.Path, default=pathlib.Path("profiles"))
    ap.add_argument("--report", type=pathlib.Path)
    ap.add_argument("--min-rate", type=float, default=0.0,
                    help="fail if the share of converted entities falls below this")
    args = ap.parse_args()

    files = list(args.files)
    if args.upstream:
        files += sorted(args.upstream.glob("bsh-dbus-*.yaml"))
        files += sorted((args.upstream / "contrib").glob("*.yaml"))
    if not files:
        ap.error("nothing to convert -- pass files or --upstream")

    args.out.mkdir(parents=True, exist_ok=True)
    report, lines = [], []
    total_entities = total_converted = 0

    for path in files:
        before = len(report)
        result = convert_file(path, report)
        if not result:
            lines.append(f"- **{path.name}** -- nothing convertible")
            continue
        seen, profile, groups = result

        n = len(profile["entities"])
        failed = seen - n
        total_entities += seen
        total_converted += n

        out = args.out / f"{profile['profile']['id']}.json"
        out.write_text(json.dumps(profile, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

        pct = 100 * n / seen if seen else 100
        lines.append(f"- **{path.name}** -> `{out.name}` -- {n}/{seen} entities ({pct:.0f}%), "
                     f"{groups} groups, {profile['profile']['appliance']}")
        for r in report[before:]:
            lines.append(f"  - {r}")

    rate = 100 * total_converted / total_entities if total_entities else 100
    summary = (f"{total_converted}/{total_entities} entities converted ({rate:.1f}%) "
               f"from {len(files)} files")
    print(summary)

    if args.report:
        args.report.write_text(
            "# Conversion report\n\n" + summary + "\n\n" + "\n".join(lines) + "\n",
            encoding="utf-8")
        print(f"report: {args.report}")

    if rate < args.min_rate:
        sys.exit(f"conversion rate {rate:.1f}% below the required {args.min_rate}%")


if __name__ == "__main__":
    main()

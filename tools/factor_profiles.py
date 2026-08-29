#!/usr/bin/env python3
"""
Factor shared decoding out of the model profiles into base profiles.

Derived from the data rather than assumed: a definition moves into a base when
the same address is decoded the same way by at least two models of the same
appliance type. Nothing is grouped because it looks similar.

Every rewrite is verified by resolving it again and comparing against the flat
profile it came from. If a single byte differs the tool fails, so the
factoring cannot quietly lose or alter a definition -- which matters, because
this converter has already produced five silent losses.

Usage:
    factor_profiles.py profiles/            # rewrite in place
    factor_profiles.py --check profiles/    # verify only

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import argparse
import collections
import copy
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import profile_lib as lib  # noqa: E402


# Fields that describe the byte, not the model. These go into a base.
SHARED_KEYS = ("match", "extract", "guards")
# Home Assistant metadata that follows from the physical quantity rather than
# the model -- an rpm reading is measured in rpm whatever machine sends it.
SHARED_HA = ("device_class", "unit_of_measurement", "state_class", "accuracy_decimals")

MIN_MODELS = 2

# Definitions that mean the same thing across appliance types within an address
# family, listed explicitly rather than inferred.
#
# Structural similarity is not evidence of shared meaning, and family A proves
# it: 0x11.1006 byte 2 is the drying target on a dryer and the temperature on a
# washing machine, byte 4 the drying degree versus the spin speed. Both look
# identical to a shape comparison. Only the addresses below were checked to
# carry the same concept, so only these are hoisted.
#
# (dest, cmd, op, at)
FAMILY_GENERIC = {
    "family-a": {
        (0x21, 0x1000, "u8", 0),     # machine state
        (0x21, 0x1000, "u8", 1),     # door
        (0x21, 0x1002, "u16be", 0),  # remaining time
        (0x11, 0x1001, "u8", 0),     # ready
        (0x11, 0x1006, "u8", 0),     # which programme is selected
    },
}


def family_of(entity: dict) -> str:
    """Which address family a definition belongs to, if any."""
    dest = int(str(entity["match"]["dest"]), 0)
    if dest in (0x11, 0x21):
        return "family-a"
    return ""


def generic_key(entity: dict):
    m, x = entity["match"], entity["extract"]
    return (int(str(m["dest"]), 0), int(str(m["cmd"]), 0), x["op"], x.get("at", 0))


def shape(entity: dict) -> str:
    """What makes two definitions the same decoding."""
    return json.dumps({k: entity[k] for k in SHARED_KEYS if k in entity},
                      sort_keys=True, ensure_ascii=False)


def canonical_name(ids) -> str:
    """A readable id for a base entity, from the most common model-side name.

    Model ids carry prefixes like wm_ or dryer_ that say what the appliance is,
    which a shared definition should not.
    """
    counts = collections.Counter()
    for i in ids:
        name = i
        for prefix in ("bsh_", "wm_", "dryer_", "machine_", "washing_", "dishwasher_"):
            while name.startswith(prefix):
                name = name[len(prefix):]
        counts[name or i] += 1
    return counts.most_common(1)[0][0]


def factor(root: pathlib.Path, write: bool) -> int:
    flat = {}
    for path in sorted(root.glob("*.json")):
        if path.parent.name == lib.BASE_DIR:
            continue
        doc = lib.load(path)
        if "extends" in doc:
            print(f"  {path.name}: already factored, skipping")
            continue
        flat[path] = doc

    if not flat:
        print("  nothing to factor")
        return 0

    # Group by appliance type: the byte layout is shared within a family, and
    # 0x11.1006 proves it is emphatically not shared across appliance types.
    by_kind = collections.defaultdict(list)
    for path, doc in flat.items():
        by_kind[doc["profile"].get("appliance", "unknown")].append(path)

    bases = {}
    rewritten = {}

    for kind, paths in sorted(by_kind.items()):
        if len(paths) < MIN_MODELS:
            continue

        seen = collections.defaultdict(list)  # shape -> [(path, entity)]
        for path in paths:
            for e in flat[path]["entities"]:
                seen[shape(e)].append((path, e))

        shared = {s: v for s, v in seen.items()
                  if len({p for p, _ in v}) >= MIN_MODELS}
        if not shared:
            continue

        base_entities, name_for = [], {}
        used = set()
        for s, items in sorted(shared.items(), key=lambda kv: -len(kv[1])):
            name = canonical_name([e["id"] for _, e in items])
            original = name
            n = 2
            while name in used:
                name = f"{original}_{n}"
                n += 1
            used.add(name)
            name_for[s] = name

            proto = items[0][1]
            entry = {"id": name}
            # A readable label so the base is usable on its own, for an appliance
            # of the right family whose exact model nobody has mapped yet.
            entry["name"] = name.replace("_", " ").capitalize()
            kinds = {e.get("kind", "sensor") for _, e in items}
            if len(kinds) == 1:
                entry["kind"] = kinds.pop()
            for k in SHARED_KEYS:
                if k in proto:
                    entry[k] = copy.deepcopy(proto[k])
            # Only metadata that every sharer agrees on belongs in the base.
            ha = {}
            for key in SHARED_HA:
                values = {json.dumps(e.get("ha", {}).get(key)) for _, e in items}
                if len(values) == 1 and proto.get("ha", {}).get(key) is not None:
                    ha[key] = proto["ha"][key]
            if ha:
                entry["ha"] = ha
            base_entities.append(entry)

        base_id = f"{lib.BASE_DIR}/{kind.replace('_', '-')}"
        bases[base_id] = {
            "schema": 1,
            "profile": {"id": base_id.split("/")[-1],
                        "model": f"Generic {kind.replace('_', ' ')}",
                        "appliance": kind,
                        "generic": True,
                        "note": "Shared decoding, factored from the model profiles. "
                                "Usable on its own for an appliance of this family "
                                "whose exact model has not been mapped: the values "
                                "are correct, but codes are reported raw because "
                                "programme names differ per model."},
            "entities": base_entities,
        }

        for path in paths:
            doc = flat[path]
            out = copy.deepcopy(doc)
            out["extends"] = base_id
            entities = []
            for e in doc["entities"]:
                s = shape(e)
                if s not in name_for:
                    entities.append(copy.deepcopy(e))
                    continue
                delta = {"id": e["id"], "from": name_for[s]}
                for k, v in e.items():
                    if k in SHARED_KEYS or k == "id":
                        continue
                    if k == "ha":
                        rest = {kk: vv for kk, vv in v.items()
                                if bases[base_id]["entities"][
                                    [b["id"] for b in base_entities].index(name_for[s])]
                                .get("ha", {}).get(kk) != vv}
                        if rest:
                            delta["ha"] = rest
                        continue
                    delta[k] = copy.deepcopy(v)
                entities.append(delta)
            out["entities"] = entities
            rewritten[path] = out

    # ---- hoist what the whole family shares ------------------------------
    # A second level, above the appliance-type bases: an appliance of the right
    # family whose type nobody has mapped still gets door, state and remaining
    # time right, because those genuinely do not depend on what the machine does.
    family_bases = {}
    for fam, allowed in FAMILY_GENERIC.items():
        # Pooled from the models directly, not from the type bases. A definition
        # used by one dryer and one washing machine is family-wide even though
        # neither appliance type shares it internally -- taking it from the type
        # bases would miss exactly those, and the door is one of them.
        pool = collections.defaultdict(list)   # key -> [(kind, entity)]
        for path, doc in flat.items():
            kind = doc["profile"].get("appliance", "unknown")
            for e in doc["entities"]:
                if family_of(e) == fam and generic_key(e) in allowed:
                    pool[generic_key(e)].append((kind, e))

        hoist = {k: v for k, v in pool.items()
                 if len({kind for kind, _ in v}) >= 2}
        if not hoist:
            continue

        entities = []
        for k, items in sorted(hoist.items()):
            entry = {"id": canonical_name([e["id"] for _, e in items])}
            entry["name"] = entry["id"].replace("_", " ").capitalize()
            kinds = {e.get("kind", "sensor") for _, e in items}
            if len(kinds) == 1:
                entry["kind"] = kinds.pop()
            proto = items[0][1]
            for key in SHARED_KEYS:
                if key in proto:
                    entry[key] = copy.deepcopy(proto[key])
            entities.append(entry)

        fam_id = f"{lib.BASE_DIR}/{fam}"
        family_bases[fam_id] = {
            "schema": 1,
            "profile": {"id": fam, "model": f"Generic {fam.replace('-', ' ')} appliance",
                        "appliance": "generic", "generic": True,
                        "note": "Decoding shared across appliance types of one address "
                                "family. Only definitions verified to carry the same "
                                "meaning are here -- 0x11.1006 bytes 2 and 4 look the "
                                "same but mean different things per appliance type, and "
                                "are deliberately left to the type bases."},
            "entities": entities,
        }

        by_key = {generic_key(e): e["id"] for e in entities}
        for base_id, doc in bases.items():
            keep, refs = [], []
            for e in doc["entities"]:
                src = by_key.get(generic_key(e)) if family_of(e) == fam else None
                if src:
                    delta = {"id": e["id"], "from": src}
                    for k, v in e.items():
                        if k in SHARED_KEYS or k == "id":
                            continue
                        delta[k] = copy.deepcopy(v)
                    refs.append(delta)
                else:
                    keep.append(e)
            if refs:
                doc["extends"] = fam_id
                doc["entities"] = refs + keep

    bases.update(family_bases)

    # ---- the safety net --------------------------------------------------
    (root / lib.BASE_DIR).mkdir(exist_ok=True)
    tmp_written = []
    for base_id, doc in bases.items():
        p = root / f"{base_id}.json"
        p.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        tmp_written.append(p)

    failures = 0
    for path, out in rewritten.items():
        resolved = lib.resolve(out, root)
        if lib.canonical(resolved) != lib.canonical(flat[path]):
            failures += 1
            print(f"  MISMATCH {path.name}: resolving the factored form does not "
                  f"reproduce the original")
            a = {e["id"]: e for e in flat[path]["entities"]}
            b = {e["id"]: e for e in resolved["entities"]}
            for k in sorted(set(a) | set(b)):
                if a.get(k) != b.get(k):
                    print(f"      {k}:")
                    print(f"        was: {json.dumps(a.get(k), sort_keys=True, ensure_ascii=False)[:160]}")
                    print(f"        now: {json.dumps(b.get(k), sort_keys=True, ensure_ascii=False)[:160]}")

    if failures:
        for p in tmp_written:
            p.unlink(missing_ok=True)
        print(f"\n  {failures} profiles would change meaning -- nothing written")
        return 1

    if not write:
        for p in tmp_written:
            p.unlink(missing_ok=True)
        print(f"  check only: {len(rewritten)} profiles would be factored against "
              f"{len(bases)} bases, all verified identical")
        return 0

    saved = 0
    for path, out in rewritten.items():
        before = len(json.dumps(flat[path], separators=(",", ":")))
        text = json.dumps(out, indent=2, ensure_ascii=False) + "\n"
        path.write_text(text, encoding="utf-8")
        saved += before - len(json.dumps(out, separators=(",", ":")))

    print(f"  {len(bases)} bases, {len(rewritten)} profiles factored, "
          f"all verified byte-identical when resolved")
    for base_id, doc in sorted(bases.items()):
        print(f"    {base_id}: {len(doc['entities'])} shared definitions")
    print(f"  {saved} bytes of repetition removed")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", type=pathlib.Path, nargs="?", default=pathlib.Path("profiles"))
    ap.add_argument("--check", action="store_true", help="verify without writing")
    args = ap.parse_args()
    sys.exit(factor(args.root, write=not args.check))


if __name__ == "__main__":
    main()

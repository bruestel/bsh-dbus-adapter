#!/usr/bin/env python3
"""
Print a profile in readable form: what each value is, and where it comes from.

The JSON says what to do with a byte; this says what it means. Useful when
comparing an unmapped appliance against an existing profile, since the value
tables are exactly the part that does not carry across models.

Usage:
    show_profile.py wt47w5w0
    show_profile.py --compare dryer     # every profile of one appliance type

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import argparse
import collections
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import profile_lib as lib  # noqa: E402

OPS = {"u8": "byte", "i8": "byte (signed)", "u16be": "2 bytes", "u16le": "2 bytes (LE)",
       "i16be": "2 bytes (signed)", "mask": "bit mask", "bit": "bit", "bits": "bits",
       "const": "constant", "eq": "equals", "len": "payload length"}


def resolved(root: pathlib.Path, name: str) -> dict:
    for candidate in (root / f"{name}.json", root / "base" / f"{name}.json"):
        if candidate.is_file():
            return lib.resolve(lib.load(candidate), root)
    sys.exit(f"no profile \"{name}\" in {root}")


def describe(e: dict) -> str:
    m, x = e["match"], e["extract"]
    out = f"{m['dest']}.{m['cmd']}  {OPS.get(x['op'], x['op'])} {x.get('at', 0)}"
    if x["op"] == "mask":
        out += f" & 0x{x['mask']:02X}"
    if x["op"] == "bit":
        out += f" {x['bit']}"
    return out


def show(doc: dict):
    p = doc["profile"]
    print(f"{p.get('model') or p['id']} — {p.get('appliance', '?')}"
          f"{'  (generic family fallback)' if p.get('generic') else ''}\n")
    for e in doc["entities"]:
        print(f"  {e.get('name', e['id'])}")
        print(f"      {describe(e)}")
        for t in e.get("transforms", []):
            if t["op"] == "map":
                items = sorted(t["values"].items(), key=lambda kv: int(kv[0]))
                for k, v in items:
                    print(f"        {k:>4} = {v}")
            elif t["op"] == "lookup":
                for r in t["rules"]:
                    print(f"        {', '.join(str(i) for i in r['in']):>4} = {r['out']}")
                if t.get("default") not in (None, "drop"):
                    print(f"        else   {t['default']}")
            elif t["op"] == "calibrate_linear":
                pts = ", ".join(f"{a:g}->{b:g}" for a, b in t["datapoints"])
                print(f"        {pts}")
            else:
                val = t.get("value")
                print(f"        {t['op']}" + (f" {val:g}" if isinstance(val, (int, float)) else ""))
        print()


def compare(root: pathlib.Path, kind: str):
    docs = []
    for path in sorted(root.glob("*.json")):
        d = lib.resolve(lib.load(path), root)
        if d["profile"].get("appliance") == kind:
            docs.append(d)
    if not docs:
        sys.exit(f"no profiles for \"{kind}\"")

    print(f"{len(docs)} {kind} profiles\n")
    rows = collections.defaultdict(dict)
    for d in docs:
        model = d["profile"].get("model") or d["profile"]["id"]
        for e in d["entities"]:
            rows[describe(e)][model] = e.get("name", e["id"])

    models = [d["profile"].get("model") or d["profile"]["id"] for d in docs]
    width = max(len(r) for r in rows) + 2
    print("  " + "address / byte".ljust(width) + "  ".join(m[:11].ljust(12) for m in models))
    print("  " + "-" * (width + 12 * len(models)))
    for addr in sorted(rows):
        line = "  " + addr.ljust(width)
        for m in models:
            line += (rows[addr].get(m, "—")[:11]).ljust(12)
        print(line)
    print("\n  Shared rows are the same decoding on the same address. What differs is\n"
          "  the naming, and the value tables, which show under --show.")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("name", nargs="?")
    ap.add_argument("--compare", metavar="APPLIANCE")
    ap.add_argument("--root", type=pathlib.Path, default=pathlib.Path("profiles"))
    args = ap.parse_args()

    if args.compare:
        compare(args.root, args.compare)
    elif args.name:
        show(resolved(args.root, args.name))
    else:
        ap.error("give a profile name or --compare")


if __name__ == "__main__":
    main()

"""
Profile inheritance: shared definitions in a base, per-model deltas on top.

Measured across the 22 converted profiles, 56% of entity definitions were exact
repetitions of another -- the same address decoded the same way. The value
tables were not: only 10 of 53 appeared on more than one model. So the byte
layout is a property of the appliance family, while the names and programme
tables belong to the model.

The inheritance follows that split. A base holds address, extraction and the
Home Assistant metadata that goes with the physical quantity; a model profile
picks the definitions it actually has and adds its own names and tables.

Inheritance is opt-in per entity, by "from". A model that simply omits a
definition does not get it -- appliances within a family do not all report
everything, and silently inheriting an entity the machine never sends would
produce an entity that is permanently unavailable.

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import copy
import json
import pathlib

BASE_DIR = "base"


def load(path: pathlib.Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def _merge_entity(base: dict, delta: dict) -> dict:
    """Delta wins per key; nothing is merged recursively.

    A half-inherited extract or match would be far harder to reason about than
    an overridden one, and no real case needs it.
    """
    out = copy.deepcopy(base)
    for key, value in delta.items():
        if key == "from":
            continue
        out[key] = copy.deepcopy(value)
    if "ha" in base and "ha" in delta:
        # Metadata is the exception: a model routinely adds an icon to an
        # inherited unit and device_class rather than restating them.
        merged = copy.deepcopy(base["ha"])
        merged.update(delta["ha"])
        out["ha"] = merged
    return out


def resolve(profile: dict, root: pathlib.Path) -> dict:
    """Return the profile with every "extends"/"from" reference expanded."""
    parent_id = profile.get("extends")
    if not parent_id:
        return copy.deepcopy(profile)

    parent_path = root / f"{parent_id}.json"
    if not parent_path.is_file():
        raise FileNotFoundError(f"base profile \"{parent_id}\" not found at {parent_path}")

    parent = resolve(load(parent_path), root)
    by_id = {e["id"]: e for e in parent.get("entities", [])}

    out = copy.deepcopy(profile)
    out.pop("extends", None)

    entities = []
    for delta in profile.get("entities", []):
        src = delta.get("from")
        if src is None:
            entities.append({k: v for k, v in copy.deepcopy(delta).items() if k != "from"})
            continue
        if src not in by_id:
            raise KeyError(f"{profile.get('profile', {}).get('id', '?')}: "
                           f"entity \"{delta.get('id')}\" inherits from \"{src}\", "
                           f"which \"{parent_id}\" does not define")
        entities.append(_merge_entity(by_id[src], delta))

    out["entities"] = entities

    # Anything the model does not state itself comes from the base.
    for key in ("bus", "device"):
        if key not in out and key in parent:
            out[key] = copy.deepcopy(parent[key])
    return out


def canonical(profile: dict) -> str:
    """A stable rendering, for comparing a resolved profile against a flat one."""
    return json.dumps(profile, sort_keys=True, ensure_ascii=False, separators=(",", ":"))


def load_resolved(path: pathlib.Path) -> dict:
    return resolve(load(path), path.parent)


def model_profiles(root: pathlib.Path):
    """Every profile that is not a base."""
    return sorted(p for p in root.glob("*.json"))

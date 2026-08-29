#!/usr/bin/env python3
"""
Write resolved copies of the profiles, for checking them with the device parser.

The firmware resolves inheritance at build time, so what it actually parses is
the flattened form -- that is what should be validated, not the source files.

Usage: resolve_profiles.py profiles/ out/

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import profile_lib as lib  # noqa: E402

if len(sys.argv) != 3:
    sys.exit(__doc__)

src, dst = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
dst.mkdir(parents=True, exist_ok=True)
n = 0
for path in sorted(src.glob("*.json")):
    doc = lib.resolve(lib.load(path), src)
    (dst / path.name).write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n",
                                 encoding="utf-8")
    n += 1
print(f"resolved {n} profiles into {dst}")

#!/usr/bin/env python3
"""
Check that the API reference in the interface still matches the routes served.

The reference on the API tab is hand-written prose, and prose does not follow
code: an endpoint added, renamed or withdrawn leaves the page quietly describing
a device that no longer exists. Documentation that lies is worse than none, so
the two are compared here and the build fails on any drift.

Only the method and the path are compared -- whether the prose beside them is
any good is not something a script can tell.

Usage:
    check_api_docs.py [--verbose]

(C) 2026 Jonas Brüstel
Licensed under the GNU General Public License version 3.0.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "components" / "app_web" / "src" / "Web.cpp"
UI = ROOT / "www" / "index.html"

# The page itself is not an API endpoint, and nothing would be gained by listing
# it beside the ones that answer JSON.
IGNORED_PATHS = {"/"}


def served_routes(text: str) -> set:
    """Every route registered with the HTTP server, as (method, path).

    Reads the designated initialisers of the route table. The websocket entry is
    spread over several lines, so the pattern tolerates whitespace between the
    uri and the method rather than assuming they share one.
    """
    found = set()
    for m in re.finditer(r'\{\s*\.uri\s*=\s*"([^"]+)"\s*,\s*\.method\s*=\s*HTTP_(\w+)', text):
        path, method = m.group(1), m.group(2)
        if path not in IGNORED_PATHS:
            found.add((method, path))
    return found


def documented_routes(text: str) -> set:
    """Every endpoint the API tab describes, as (method, path).

    Each is a method chip followed by the path. Query parameters are documented
    in a list of their own, so anything after "?" is not part of the identity.
    The websocket is labelled WS because that is what a reader is looking for,
    but it is served as a GET and compared as one.
    """
    start = text.find('<main id="view-api"')
    if start < 0:
        sys.exit("check_api_docs: no API section in the interface -- was the tab removed?")
    end = text.find("</main>", start)
    section = text[start:end]

    found = set()
    for m in re.finditer(r'<span class="m[^"]*">(\w+)</span>\s*<code[^>]*>([^<]*)', section):
        method, path = m.group(1), m.group(2).split("?")[0].strip()
        if method == "BASE":  # the address the endpoints hang off, not one of them
            continue
        if method == "WS":
            method = "GET"
        found.add((method, path))
    return found


def main() -> int:
    verbose = "--verbose" in sys.argv
    served = served_routes(WEB.read_text(encoding="utf-8"))
    documented = documented_routes(UI.read_text(encoding="utf-8"))

    if not served:
        sys.exit("check_api_docs: found no routes in Web.cpp -- has the route table moved?")

    undocumented = sorted(served - documented)
    invented = sorted(documented - served)

    if verbose:
        for method, path in sorted(served):
            print(f"  {method:6} {path}")

    if undocumented:
        print("Served but not documented on the API tab:")
        for method, path in undocumented:
            print(f"  {method:6} {path}")
    if invented:
        print("Documented on the API tab but not served:")
        for method, path in invented:
            print(f"  {method:6} {path}")

    if undocumented or invented:
        print()
        print("Add, correct or remove the entry in the API section of www/index.html")
        print("so the reference describes the device that is actually built.")
        return 1

    print(f"OK: all {len(served)} routes documented, none invented.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

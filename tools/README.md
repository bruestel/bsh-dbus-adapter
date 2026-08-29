# Tools

Ten scripts, and none of them is optional in the sense of being decoration:
three run on every build, three are checked by CI, and the rest are how a
profile gets written, inspected or converted. Each file's own docstring says
why it exists and what it is careful about; this is the map.

## Run on every build

The firmware embeds its web interface and its profiles in the binary, so that
one OTA update carries all three and they cannot drift apart. Two hooks in
`platformio.ini` produce them:

| Script | What it does |
| :--- | :--- |
| `gen_ui_asset.py` | Gzips `www/index.html` and writes it as a C array. A pre-build hook rather than a CMake rule, because PlatformIO does not re-run configure when only the HTML changes, and the failure mode was silent: builds kept succeeding while the firmware served a stale page. |
| `gen_profiles_asset.py` | Resolves every profile and compiles them in, with their address signatures precomputed so recognising an appliance costs no parsing at startup. |
| `check_ui.js` | Runs the interface's script against a stub browser, once per entry point. Called from `gen_ui_asset.py`, so a page that throws on load breaks the build rather than the device. It needs Node, and skips itself with a note when Node is absent, so the build works without it. |

`gen_ui_asset_standalone.py` does the same job as the first of those without
PlatformIO, for the CMake fallback on a first configure and for plain `idf.py`
builds.

## Checked by CI

| Script | The property it defends |
| :--- | :--- |
| `factor_profiles.py --check` | That resolving base + delta reproduces exactly what the converter produced. Across the 22 converted profiles more than half the entity definitions were exact duplicates of another, so inheritance carries a lot, and losing a definition inside it would be silent. |
| `resolve_profiles.py` | That every profile resolves at all, which is what the device parser will be handed. |
| `check_api_docs.py` | That the API reference served on the device still lists the routes the firmware actually serves, no more and no less. Prose does not follow code on its own. |

`resolve_profiles.py` is also how you get files for `test/host/build/validate_profiles`,
which runs the real device parser over them:

```bash
python3 tools/resolve_profiles.py profiles/ /tmp/resolved
./test/host/build/validate_profiles /tmp/resolved/*.json
```

## Working on profiles

| Script | Use |
| :--- | :--- |
| `esphome2profile.py` | Converts an upstream ESPHome configuration into a profile, and reports every lambda it could not translate rather than guessing. This is the path for a model that has been mapped for ESPHome but does not ship here yet. |
| `factor_profiles.py` | Without `--check`, rewrites profiles in place, moving decoding that two or more models of one appliance type share into a base. It works in bulk and will not fold a single new profile into an existing base. |
| `show_profile.py` | Prints a profile as prose: what each value means and which byte it comes from. `--compare <type>` puts every profile of one appliance kind side by side, which is the comparison `docs/address-families.md` describes. |
| `profile_lib.py` | Not a command. The shared loading and inheritance code the others import. |

## A note on generated files

`docs/conversion-report.md` (written by `esphome2profile.py --report`),
`components/app_web/src/ui_asset.c` and
`components/app_profiles/src/profiles_asset.c` are outputs. The last two are
ignored by git and rebuilt every time. Editing any of them by hand is work that
the next build throws away; change the generator instead.

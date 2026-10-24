#!/usr/bin/env python3
"""Battle sweep fixtures from the decomp's encounter catalogue (decomp/tools/encounter_catalog.py, docs/encounters.md).

    python3 decomp/tools/encounter_catalog.py --disc N --img disks/us/discN/FF9.IMG --encounters --json build/encounters_dN.json
    python3 tools/battle_fixtures.py OUT_DIR build/encounters_d1.json ... build/encounters_d4.json

One scenario per encounter that can start on its disc (its background is there). An encounter a field starts returns
to that field after the battle, as the game's own field -> battle handoff does; one only the world map starts returns
to a world map that lists it (9000 when the catalogue names none), as the world map's handoff does. Derived from game data: under build/.
"""
import json
import sys
from pathlib import Path


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    written = with_field = 0
    for path in sys.argv[2:]:
        for r in json.load(open(path)):
            if not r["bg_present"]:
                continue
            fields = [s["field"] for s in r["starters"] if "field" in s and s.get("bg_present", True)]
            worlds = [w for s in r["starters"] for w in s.get("worlds", [])] or [9000]
            if 9000 in worlds:  # the plain Mist Continent map; the others' scripts may send the party to other discs' fields
                worlds = [9000]
            lines = [f"# battle sweep: encounter {r['encounter']}, " +
                     (f"returns to field {fields[0]} (starts it)" if fields else f"returns to world map {worlds[0]}"),
                     "kind=battle", f"disc={r['disc']}", f"battle={r['encounter']}"]
            if fields:
                lines.append(f"field={fields[0]}")
                with_field += 1
            else:
                lines.append(f"world={worlds[0]}")
            # disc 4 has only the late world data (story 0x2B52 up: ovl_65 loads file 0x2716 to return there)
            story = 11100 if r["disc"] == 4 and not fields else 10000
            lines += ["party=0,1,2,3", "level=40", f"story={story}", "seed=1"]
            (out / f"d{r['disc']}_b{r['encounter']:04d}.cfg").write_text("\n".join(lines) + "\n")
            written += 1
    print(f"{written} fixtures, {with_field} returning to the field that starts them, the others to the world map")


main()

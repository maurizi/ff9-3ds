#!/usr/bin/env python3
"""Battle sweep fixtures from the decomp's encounter catalogue (decomp/tools/encounter_catalog.py, docs/encounters.md).

    python3 decomp/tools/encounter_catalog.py --disc N --img disks/us/discN/FF9.IMG --encounters --json build/encounters_dN.json
    python3 tools/battle_fixtures.py OUT_DIR build/encounters_d1.json ... build/encounters_d4.json

One scenario per encounter that can start on its disc (its background is there). An encounter a field starts returns
to that field after the battle, as the game's own field -> battle handoff does; one only the world map starts has no
field, and the scenario stands in the disc's first (port/game/scenario.c). Derived from game data: under build/.
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
            lines = [f"# battle sweep: encounter {r['encounter']}, " +
                     (f"returns to field {fields[0]} (starts it)" if fields else "started on the world map"),
                     "kind=battle", f"disc={r['disc']}", f"battle={r['encounter']}"]
            if fields:
                lines.append(f"field={fields[0]}")
                with_field += 1
            lines += ["party=0,1,2,3", "level=40", "story=10000", "seed=1"]
            (out / f"d{r['disc']}_b{r['encounter']:04d}.cfg").write_text("\n".join(lines) + "\n")
            written += 1
    print(f"{written} fixtures, {with_field} returning to the field that starts them")


main()

#!/usr/bin/env python3
"""The whole game's scenes as fixtures, and their speed on a 3DS.

    tools/sweep.py gen                       # build/sweep/cfg/{fields,battles,spells}/*.cfg from the discs in disks/us
    tools/sweep.py hw fields [--frames N]    # each fixture as a quiet job on the console (tools/hwrun.py serve running,
    tools/sweep.py hw battles                #   the agent build on it): its heartbeats into build/sweep/hw_<set>.jsonl
    tools/sweep.py hw spells                 #   (a set is resumed where it stopped; --redo NAME runs one again)
    tools/sweep.py report [--over MS]        # what ran slow, per set

Fields: every field id of every disc's directory 4 (tools/scenario_catalog.py), once, on the first disc that has it.
Battles: every encounter that can start on a disc (tools/battle_fixtures.py from the decomp's encounter catalogue),
once. Spells: every action ability a battle command lists (decomp/tools/ff9text.py dump cmdabilities), cast once by
the character whose command it is (the scenario's cast=). These are synthetic launches: a field may redirect or sit
in an event its story value doesn't fit, an encounter's enemies act as their scripts choose. A fixture that fails to
start says so in its row; it is not a measurement.

Everything written is derived from the discs and stays under build/ (gitignored). Runs on the host.
"""
import argparse, json, re, statistics, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/sweep"
DISKS = ROOT / "disks/us"
STORY = {1: 2500, 2: 5500, 3: 9000, 4: 11500}  # a story value within each disc's span
FULL = 2005  # ms per 120 VBlanks at full speed

# the character whose command lists the abilities (party ids: 0 Zidane, 1 Vivi, 2 Dagger, 3 Steiner, 4 Freya,
# 5 Quina, 6 Eiko, 7 Amarant), by the command's name and, where two characters share a name, its id
ACTOR = {"Skill": 0, "Dyne": 0, "Steal": 0, "Blk Mag": 1, "Dbl Blk": 1, "Focus": 1, "Eidolon": 2, "Swd Art": 3,
         "Swd Mag": 3, "Dragon": 4, "Jump": 4, "Blu Mag": 5, "Eat": 5, "Cook": 5, "Dbl Wht": 6, "Flair": 7, "Elan": 7,
         "Throw": 7}
ACTOR_BY_ID = {0x10: 2, 0x11: 2, 0x13: 6, 0x14: 6}  # Summon and Wht Mag: Dagger's, then Eiko's


def sh(*a, **k):
    return subprocess.run([str(x) for x in a], cwd=ROOT, capture_output=True, text=True, **k)


def gen(_):
    cfg = OUT / "cfg"
    for d in ("fields", "battles", "spells"):
        (cfg / d).mkdir(parents=True, exist_ok=True)
    # fields
    seen = set()
    for disc in range(1, 5):
        img = DISKS / f"disc{disc}/FF9.IMG"
        if not img.exists():
            continue
        r = sh("python3", "tools/scenario_catalog.py", img, "--disc", disc, "--fields", "--json")
        for f in json.loads(r.stdout):
            if f["id"] in seen:
                continue
            seen.add(f["id"])
            (cfg / "fields" / f"f{f['id']:05d}_d{disc}.cfg").write_text(
                f"# sweep: field {f['id']} (area {f['area']}), synthetic party and story\nkind=field\ndisc={disc}\n"
                f"field={f['id']}\nparty=0,1,2,3\nlevel=30\nstory={STORY[disc]}\nseed=1\n")
    # battles
    tmp = OUT / "battles_all"
    encs = []
    for disc in range(1, 5):
        img = DISKS / f"disc{disc}/FF9.IMG"
        if not img.exists():
            continue
        j = OUT / f"encounters_d{disc}.json"
        sh("python3", "decomp/tools/encounter_catalog.py", "--disc", disc, "--img", img, "--encounters", "--json", j)
        encs.append(j)
    sh("python3", "tools/battle_fixtures.py", tmp, *encs)
    done = set()
    for p in sorted(tmp.glob("d*_b*.cfg")):
        enc = p.stem.split("_b")[1]
        if enc in done:
            continue
        done.add(enc)
        (cfg / "battles" / f"b{enc}_{p.stem.split('_')[0]}.cfg").write_text(p.read_text())
    # spells
    r = sh("python3", "tools/ff9text.py", "dump", "cmdabilities", cwd=None) if False else subprocess.run(
        ["python3", "tools/ff9text.py", "dump", "cmdabilities"], cwd=ROOT / "decomp", capture_output=True, text=True)
    abil = {}
    for line in r.stdout.splitlines():
        m = re.match(r"0x([0-9a-f]+) ['\"](.*?): (.*)['\"]$", line)
        if not m:
            continue
        cmd, name = int(m[1], 16), m[2]
        actor = ACTOR_BY_ID.get(cmd, ACTOR.get(name))
        if actor is None or cmd in (0x0E, 0x0F):  # (Item's and Throw's lists are other commands' abilities)
            continue
        for a in m[3].split(", "):
            am = re.match(r"0x([0-9a-f]+) (.*)", a)
            if am and int(am[1], 16) and int(am[1], 16) not in abil:
                abil[int(am[1], 16)] = (cmd, actor, name, am[2])
    for a, (cmd, actor, cname, aname) in sorted(abil.items()):
        party = f"{actor},1" if cname == "Swd Mag" else str(actor)  # (Sword Magic needs Vivi beside Steiner)
        n = party.count(",") + 1
        lines = [f"# sweep: {cname} / {aname} (command {cmd:#x}, ability {a:#x}) by character {actor}", "kind=battle", "disc=3",
                 "battle=200", "field=350", "party=" + party + ",-1" * (4 - n), f"preset.0={actor}"]
        if n > 1:
            lines.append("preset.1=1")
        lines += ["level=99", "story=10000", "seed=1", f"cast={cmd},{a},-1"]
        (cfg / "spells" / f"s{a:03d}_c{cmd:02d}.cfg").write_text("\n".join(lines) + "\n")
    # enemy attacks: each distinct attack (its name and the effects its sequence plays) of the encounters' tables, in the
    # first encounter with a fixture that has it, played by the scenario's enemy_cast= whenever its own enemy acts
    sys.path.insert(0, str(ROOT / "decomp/tools"))
    import effect_labels
    (cfg / "enemies").mkdir(parents=True, exist_ok=True)
    battle_cfg = {int(p.stem[1:5]): p for p in sorted((cfg / "battles").glob("b*.cfg"))}
    attacks = {}
    for disc in range(1, 5):
        img = DISKS / f"disc{disc}/FF9.IMG"
        if not img.exists():
            continue
        for fid, d in effect_labels.encounters(img):
            if fid not in battle_cfg:
                continue
            try:
                enemies, ab = effect_labels.parse_encounter(d)
            except Exception:
                continue
            for k, (name, fx) in enumerate(ab):
                if name in ("None", "No action", "", "?") or k > 17:
                    continue
                attacks.setdefault((name, tuple(fx)), (fid, k, enemies))
    for n, ((name, fx), (fid, k, enemies)) in enumerate(sorted(attacks.items(), key=lambda x: (x[1][0], x[1][1]))):
        (cfg / "enemies" / f"e{fid:04d}_a{k:02d}.cfg").write_text(
            f"# sweep: enemy attack {k} of encounter {fid}\n" + "\n".join(
                l for l in battle_cfg[fid].read_text().splitlines() if not l.startswith("#")) + f"\nenemy_cast={k}\n")
    for d in ("fields", "battles", "spells", "enemies"):
        print(d, len(list((cfg / d).glob("*.cfg"))))


def beats_of(log):
    """(virtual frame, ms, mode, field) of each heartbeat"""
    out = []
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"beat (\d+): (\d+) ms, mode (-?\d+) field (-?\d+)", line)
        if m:
            out.append((int(m[1]), int(m[2]), int(m[3]), int(m[4])))
    return out


def hw(a):
    cfgs = sorted((OUT / "cfg" / a.set).glob("*.cfg"))
    tag = f"_{a.tag}" if a.tag else ""  # (--tag: a later build's timings beside the set's, the first kept to compare)
    res = OUT / f"hw_{a.set}{tag}.jsonl"
    done = {json.loads(l)["name"] for l in res.read_text().splitlines()} if res.exists() else set()
    names = set(Path(a.names).read_text().split()) if a.names else None
    cfgs = [c for c in cfgs if names is None or c.stem in names]
    frames = a.frames or {"fields": 1560, "battles": 2640, "spells": 3360, "enemies": 2640}[a.set]
    start = {"fields": 600, "battles": 600, "spells": 480, "enemies": 600}[a.set]  # heartbeats from here on count
    todo = [c for c in cfgs if (c.stem not in done or c.stem in a.redo) and (not a.only or re.search(a.only, c.stem))]
    print(f"{a.set}: {len(todo)} to run of {len(cfgs)} ({frames} frames each)", flush=True)
    for i, c in enumerate(todo):
        if (OUT / "STOP").exists():
            print("stopped (build/sweep/STOP)")
            return
        out = OUT / "hw" / (a.set + tag) / c.stem
        row = {"name": c.stem, "frames": frames}
        for attempt in range(3):
            subprocess.run(["rm", "-rf", str(out)])
            # (battles and spells: CROSS every 45 frames picks Attack and its target whenever a command window is up; a
            # spell's fixture turns each Attack into its cast)
            press = [] if a.set in ("fields", "enemies") else ["--press", ",".join(f"CROSS@{f}+3" for f in range(360, frames, 45))]
            r = sh("python3", "tools/hwrun.py", "run", "--scenario", c, "--frames", frames, "--hang", 0, "--quiet",
                   "--out", out, "--timeout", frames // 10 + 240, *press)
            st = {}
            try:
                st = json.loads((out / "status.json").read_text())
            except (OSError, ValueError):
                pass
            row["status"] = st.get("status", "none")
            if row["status"] == "done" and (out / "log.txt").exists():
                break
            time.sleep(20)  # (the console away, or a job lost: again)
        log = out / "log.txt"
        if log.exists():
            b = beats_of(log)
            row["end_frame"] = b[-1][0] if b else 0
            row["mode"] = b[-1][2] if b else -1
            row["field"] = b[-1][3] if b else -1
            ms = [x[1] for x in b[:-1] if x[0] >= start]  # (the last beat holds the job's end)
            if ms:
                row.update(median=int(statistics.median(ms)), max=max(ms), beats=ms,
                           over=sum(m > FULL * 1.02 for m in ms))
            text = log.read_text(errors="replace")
            for key in ("FATAL", "exception", "port_fatal"):
                if key in text:
                    row.setdefault("flags", []).append(key)
            if re.search(r"WATCHDOG: no frame for \d+s at frame [1-9]", text):  # (at frame 0: the start's loading)
                row.setdefault("flags", []).append("WATCHDOG")
            bt = re.findall(r"(\d+) batches drawn", text)
            if bt:
                row["batches_max"] = max(int(x) for x in bt)
            m = re.findall(r"scenario: enemy attack \d+ played \(for \d+\), (\d+) times", text)
            if a.set == "enemies":
                row["casts"] = int(m[-1]) if m else 0
            if "spu: on the game's core" in text:
                row.setdefault("flags", []).append("no-threads")
        with res.open("a") as f:
            f.write(json.dumps(row) + "\n")
        print(f"[{i + 1}/{len(todo)}] {c.stem}: {row.get('status')} median {row.get('median')} max {row.get('max')} "
              f"mode {row.get('mode')} field {row.get('field')} {' '.join(row.get('flags', []))}", flush=True)


SD = Path.home() / ".var/app/org.azahar_emu.Azahar/data/azahar-emu/sdmc/3ds/ff9"
AZCFG = Path.home() / ".var/app/org.azahar_emu.Azahar/config/azahar-emu"


def presses(frames):
    return ",".join(f"CROSS@{f}+3" for f in range(360, frames, 45))


def emu(a):
    """The 3DS renderer against the software rasteriser (psx/softgpu.c, the reference) on the same frames: a DUAL=1 build
    in Azahar draws every scene primitive with both and dumps both pictures of a frame, f<frame>_s and f<frame>_ref."""
    from PIL import Image, ImageChops
    cfgs = sorted((Path(a.dir) if a.dir else OUT / "cfg" / a.set).glob("*.cfg"))
    tag = f"_{a.tag}" if a.tag else ""  # (--tag: another build's results beside the set's, to compare a change by)
    res = OUT / f"emu_{a.set}{tag}.jsonl"
    done = {json.loads(l)["name"] for l in res.read_text().splitlines()} if res.exists() else set()
    frames = a.frames or {"fields": 1560, "battles": 2640, "spells": 3360, "enemies": 2640}[a.set]
    every = 60 if a.set != "fields" else 120
    pressed = a.set not in ("fields", "enemies")
    exe = OUT / f"dual_{'press' if pressed else 'plain'}_{frames}{tag}.3dsx"
    if not exe.exists() or a.rebuild:
        args = ["tools/docker.sh", "make", "-j16", "3ds", "SCENARIO=sdmc:/3ds/ff9/scenarios/sweep.cfg", f"DUMP_EVERY={every}",
                "SPUSYNC=1", "DUAL=1"] + ([f"AUTOPRESS={presses(frames)}"] if pressed else [])
        (ROOT / "build/ff9-test.3dsx").unlink(missing_ok=True)
        r = sh(*args)
        if not (ROOT / "build/ff9-test.3dsx").exists():
            sys.exit("the 3DS test build failed:\n" + r.stdout[-2000:] + r.stderr[-2000:])
        (ROOT / "build/ff9-test.3dsx").rename(exe)
    todo = [c for c in cfgs if (c.stem not in done or c.stem in a.redo) and (not a.only or re.search(a.only, c.stem))]
    print(f"{a.set}: {len(todo)} to compare of {len(cfgs)} ({frames} frames each)", flush=True)
    saved = (AZCFG / "qt-config.ini").read_bytes()
    (AZCFG / "qt-config.ini").write_bytes((AZCFG / "qt-config.ini.monofast").read_bytes())  # (an Old 3DS, its CPU and the frame rate unthrottled: the speed is the console's to tell)
    (SD / "scenarios").mkdir(parents=True, exist_ok=True)
    (OUT / "emu" / (a.set + tag)).mkdir(parents=True, exist_ok=True)
    try:
        for i, c in enumerate(todo):
            if (OUT / "STOP_EMU").exists():
                print("stopped (build/sweep/STOP_EMU)")
                return
            work = OUT / "emu" / (a.set + tag) / c.stem
            subprocess.run(["rm", "-rf", str(work)])
            (SD / "scenarios/sweep.cfg").write_bytes(c.read_bytes())
            dump = SD / "dump"
            dump.mkdir(exist_ok=True)
            for f in dump.glob("f*.ppm"):
                f.unlink()
            # (until the last frame's dump is there; the time is only the limit for a run that never gets to it)
            sh("tools/azahar.sh", "-t", int(frames / 60 * 2.7) + 25, "-u", dump / f"f{frames:06d}_ref.ppm", "-o", work, exe)
            row = {"name": c.stem, "frames": frames}
            rows = []
            for f in range(every, frames + 1, every):
                try:
                    d3 = Image.open(dump / f"f{f:06d}_s.ppm").convert("RGB")
                    ref = Image.open(dump / f"f{f:06d}_ref.ppm").convert("RGB")
                except (OSError, ValueError):
                    continue
                if d3.size != ref.size or d3.width < 64:
                    continue
                px = list(ImageChops.difference(d3, ref).getdata())
                n = len(px)
                big = sum(1 for p in px if max(p) > 48) * 100.0 / n
                some = sum(1 for p in px if max(p) > 16) * 100.0 / n
                rows.append((f, round(big, 2), round(some, 2), d3, ref))
            if rows:
                worst = max(rows, key=lambda r: r[1])
                row.update(compared=len(rows), worst=worst[1], worst_frame=worst[0],
                           mean=round(sum(r[1] for r in rows) / len(rows), 2), mean16=round(sum(r[2] for r in rows) / len(rows), 2),
                           per_frame=[(r[0], r[1]) for r in rows if r[1] > a.keep])
                if worst[1] > a.keep:  # (kept to look at: the 3DS's frame, the reference's, where they differ)
                    w, h = worst[3].size
                    sheet = Image.new("RGB", (w * 3, h))
                    sheet.paste(worst[3], (0, 0)), sheet.paste(worst[4], (w, 0))
                    sheet.paste(ImageChops.difference(worst[3], worst[4]).point(lambda v: 255 if v > 48 else v * 4), (2 * w, 0))
                    sheet.save(OUT / "emu" / (a.set + tag) / f"{c.stem}.png")
            else:
                row["compared"] = 0
            if a.dumps:  # (the run's dumps kept, for comparing two builds' pictures with each other)
                keep = OUT / "emu" / (a.set + tag) / f"{c.stem}_dumps"
                subprocess.run(["rm", "-rf", str(keep)])
                keep.mkdir(parents=True)
                for f in dump.glob("f*.ppm"):
                    (keep / f.name).write_bytes(f.read_bytes())
            log = work / "game.log"
            if log.exists() and re.search(r"FATAL|port_fatal|exception", log.read_text(errors="replace")):
                row["flags"] = ["FATAL"]
            subprocess.run(["rm", "-rf", str(work)])
            with res.open("a") as f:
                f.write(json.dumps(row) + "\n")
            print(f"[{i + 1}/{len(todo)}] {c.stem}: {row.get('compared')} frames, worst {row.get('worst')}% at {row.get('worst_frame')}, "
                  f"mean {row.get('mean')}% {' '.join(row.get('flags', []))}", flush=True)
    finally:
        (AZCFG / "qt-config.ini").write_bytes(saved)


def report(a):
    for s in ("fields", "battles", "spells", "enemies"):
        res = OUT / f"hw_{s}.jsonl"
        if not res.exists():
            continue
        rows = {}
        for l in res.read_text().splitlines():
            r = json.loads(l)
            rows[r["name"]] = r  # (the last run of a name)
        ok = [r for r in rows.values() if "median" in r]
        bad = [r for r in rows.values() if "median" not in r or r.get("flags")]
        slow = sorted((r for r in ok if r["max"] > a.over or r["median"] > FULL + 10), key=lambda r: -r["max"])
        print(f"== {s}: {len(rows)} run, {len(ok)} measured, {len(slow)} with a beat over {a.over} ms or a median over "
              f"{FULL + 10}, {len(bad)} not measured or flagged")
        for r in slow[:a.top]:
            print(f"  {r['name']}: median {r['median']} max {r['max']} beats over 2%: {r['over']}/{len(r['beats'])}")
        for r in bad[:a.top]:
            print(f"  {r['name']}: {r.get('status')} {' '.join(r.get('flags', []))} (ended at frame {r.get('end_frame')})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("gen").set_defaults(fn=gen)
    p = sub.add_parser("hw")
    p.add_argument("set", choices=("fields", "battles", "spells", "enemies"))
    p.add_argument("--frames", type=int)
    p.add_argument("--only", help="a regular expression: the fixtures whose name it finds")
    p.add_argument("--redo", action="append", default=[])
    p.add_argument("--tag", help="results in hw_<set>_<tag>.jsonl (the set's own rows kept)")
    p.add_argument("--names", help="a file of fixture names: only those")
    p.set_defaults(fn=hw)
    p = sub.add_parser("emu")
    p.add_argument("set", choices=("fields", "battles", "spells", "enemies"))
    p.add_argument("--dir", help="fixtures from this directory instead of the set's (its timing and presses kept)")
    p.add_argument("--frames", type=int)
    p.add_argument("--only")
    p.add_argument("--redo", action="append", default=[])
    p.add_argument("--rebuild", action="store_true", help="build the test 3dsx again (after a change to the port)")
    p.add_argument("--tag", help="results in emu_<set>_<tag>.jsonl, from a build of their own (the tree's, built at first use)")
    p.add_argument("--dumps", action="store_true", help="keep each run's dumps in emu/<set>/<name>_dumps")
    p.add_argument("--keep", type=float, default=3.0, help="percent of a frame differing that keeps its picture")
    p.set_defaults(fn=emu)
    p = sub.add_parser("report")
    p.add_argument("--over", type=int, default=2060)
    p.add_argument("--top", type=int, default=40)
    p.set_defaults(fn=report)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()

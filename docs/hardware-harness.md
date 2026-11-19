# Hardware test harness

A test build (the *agent*) that, launched once on a 3DS, takes jobs from a PC over Wi-Fi: each job is a scenario, scripted
input, switches to flip at given frames and frames to capture. It runs the job, sends back the log and the frames, and
starts itself again for the next one. The PC side is `tools/hwrun.py` (Python 3, standard library only).

It is for what Azahar cannot show: GPU timing, the CPU's data caches, the Old 3DS's second core.

## Setting up (once per agent build)

    tools/docker.sh make -j16 3ds AGENT=192.168.1.24          # the PC's address[:port]; writes build/ff9-test.3dsx
    cp build/ff9-test.3dsx build/ff9-agent.3dsx               # (the next test build overwrites ff9-test.3dsx)
    cp build/3ds/ff9ds.elf build/ff9-agent.elf                # for --set by name (below)
    tools/docker.sh make -j16 3ds                             # the player build's objects back

Put `build/ff9-agent.3dsx` on the card as `sdmc:/3ds/ff9-agent.3dsx` (it is small: it reads the discs from
`sdmc:/3ds/ff9-full.3dsx`, as every test build does). The Homebrew Launcher lists it as "FF9 TEST: agent for <ip>".
If the PC's address changes, write `ip[:port]` into `sdmc:/3ds/ff9/agent.txt` instead of rebuilding.

On the PC, leave the server running (port 5009, TCP, must be reachable from the console):

    tools/hwrun.py serve

Then launch the agent from the Homebrew Launcher. Its bottom screen says what it is doing ("connecting...", "waiting for
a job"). From then on the console is not touched:

    tools/hwrun.py run --scenario disc3_world_airship --frames 4800 --press "CROSS@900+3000" \
        --capture 2000-2120 --set ctr_slot_keep=0@2600 --out build/hw/airship
    tools/hwrun.py status      # the console last heard of, the queue
    tools/hwrun.py quit        # the console back to the launcher

`run` queues the job, waits for it, and prints the status and the heartbeat times. In `--out`:

| file | |
|---|---|
| `log.txt` | the game's log (what `sdmc:/3ds/ff9/log.txt` has), arriving every 2 s while the job runs |
| `top_fNNNNNN.png`, `bot_fNNNNNN.png` | the captured frames |
| `frames.csv` | per frame: the screen, the frame, the present count, how many pixels differ from the capture before |
| `top.mp4`, `bot.mp4` | the frames as a video, twice the size (when `ffmpeg` is installed) |
| `status.json` | `status`: `done`, `fatal` (port_fatal: the reason is in the log), `hang`, `refused` (a bad job: `reason`), `lost` (the connection ended: a crash or power) |
| `job.txt` | the job as sent |

## A job's options

- `--scenario NAME|FILE`: a fixture of `tests/scenarios/` (docs/scenario-testing.md), sent as text: nothing to copy to
  the card (a fixture's `characters=`/`events=` files must be on the card already). Without it: the title screen.
- `--frames N`: virtual frames (`plat_wait_frame`'s count, the number in the log's `beat N` lines).
- `--press "CROSS@900+3000,UP@1000"`, `--touch "160,120@500"`: as AUTOPRESS and AUTOTOUCH.
- `--capture A-B[/STEP][,bottom|both][,half|third|quarter]`: frames A to B, every STEP-th; `/60` alone is every 60th
  frame of the whole run. The top screen (left eye) unless `bottom` or `both`; `half` takes every other pixel each
  way. Up to 8 windows; where they overlap the first wins.
- `--set NAME=VALUE[@FRAME]`: an `int` of the build set at that frame (no frame: before the game starts). The build
  knows `ctr_defer` and `ctr_slot_keep` by name (port/3ds/agent.c `switches`). Any other 4-byte variable is found by
  name in the agent's ELF (`--elf`, default `build/ff9-agent.elf`) and set by address; the console first checks that
  its own code is that ELF's (a checksum of `.text`) and refuses the job otherwise, so a stale ELF cannot poke the
  wrong memory. The log says `agent: NAME = VALUE at frame F (was OLD)`.
- `--hang N`: seconds without a frame after which the job ends as `hang` (default 30).
- `--bufkb N`: the capture buffer (default 3072 KB).

## What a capture is, and what it costs

A captured frame is the screen's own buffer, as the LCD gets it: for the top screen the buffer of the last finished
display transfer (render.c's `top_fb`), for the bottom the front buffer. Nothing is waited for: no GPU sync, so the
frame's CPU/GPU overlap is as without capture. The price is that the picture taken at frame N is the last one
*presented*, usually frame N-1's; `frames.csv`'s `present` column is the count of presents so far, so two captures with
the same count are the same picture (the game drew nothing new, or the GPU had not finished), and the difference between
two captures' counts against their frame numbers says how many frames reached the screen.

Each frame is reduced to RGB565 and stored as its difference from the capture before, in memory; nothing is written to
the card. When the buffer fills it is sent to the PC, which stalls the game for as long as that takes; the log has a line
for each (`agent: N KB of frames sent in M ms at frame F`), and the job's last line has the totals
(`... 21 frames captured in 150 ms (longest 9000 us), 1 sends 2100 ms`). So:

- a frame with a capture is longer by the capture's time: one pass over the buffer (288 KB for the top screen), a
  quarter of it with `half`. Not yet measured on a 3DS; Azahar's clock says 7 ms for a full top screen (11 ms the
  longest), which is no measure of an Old 3DS. The job's last log line has the real figure. Heartbeats
  (`beat N: X ms`) that cover a capture window are slower by that much: measure speed in a job, or a stretch of one,
  without captures.
- a still scene costs little memory (only what changed is kept); a scene where everything moves costs 190 KB a frame,
  so the default buffer holds about 15 such frames before a send.
- the log is sent every 120 frames, beside the heartbeat, a few hundred bytes: about a millisecond every two seconds.

The colours are RGB565 (the low bits of each channel are lost: enough to see what was drawn, not to compare exact
colours; DUMP_AT's dumps of the VRAM mirror are exact).

## How it starts itself again

The game cannot be initialised twice in one process, so each job is one launch. Under Luma3DS the Homebrew Launcher
starts a `.3dsx` by telling Luma's loader (the `hb:ldr` port: SetTarget, SetArgv) which file the homebrew title loads
next, and exiting; libctru exits by starting that same title again (`aptExit`'s chainload to self, which is also how
every homebrew returns to the launcher). The agent does the same with its own path (`argv[0]`, else
`/3ds/ff9-agent.3dsx`) and then exits as the game always does. `hb:ldr` exists only under Luma3DS's loader: in Azahar
(and as a CIA) the agent says `relaunch=0`, runs one job and exits.

The agent never waits for ever: without the PC for 60 s it returns to the launcher; START while it waits does too; the
HOME button works as in any build; a connection that takes no data for 10 s is dropped (the job runs on, and frames not
sent are written to `sdmc:/3ds/ff9/agent/capture.bin`: `tools/hwrun.py decode capture.bin --out DIR`). A crash that
stops the system (an ARM exception: Luma's screen) cannot be recovered from: the PC reports `lost` with the log up to
the last heartbeat, and the console must be restarted by hand.

## Checked, and not

In Azahar (one job per launch): the connection and its retries, a job with a scenario, presses and a touch, a switch set
by address after the checksum, captures of both screens with sends mid-run, a refused job, a fatal scenario, quit. Not
yet on a 3DS: everything about the hardware, and above all starting again through `hb:ldr` (Azahar has no such port),
the end of a hung job from the watchdog's thread, and whether a New 3DS's larger caches ever show a stale buffer (the
buffer is flushed from the CPU's cache before it is read; two captures with different `present` counts and identical
pictures in a moving scene would be the sign).

## Without a console

`tools/hwrun.py sim` is a stand-in for the console (it speaks the protocol and sends synthetic frames): `serve`, `run`
and the decoding can be tried with it. In Azahar the agent connects to a server on the same machine
(`AGENT=127.0.0.1`), one job per launch.

The protocol and the frame format are described at the top of `port/3ds/agent.c`.

# v57 handoff — pick up here

Single pickup point for continuing the NetPak64 online work. Written 2026-07-11.
Everything below is committed on branch `netpak64` (private remote). See also
`RELEASES.md` (version→commit→md5 + issue triage) and the auto-memory under
`~/.claude/projects/-home-jsuppe-dev-mk64/memory/`.

## Current shipped state
- **Console + balthazar run v56** (product md5 `a8e69298`, commit `a1aa055fe`).
  Rollback ROMs for every version in `/mnt/micron/jsuppe/netpak/releases/`.
- Latest branch HEAD is autopilot/driver-AI work (commits `f61b8..8e664`),
  NOT yet cut to a version. The staged **test** ROM
  `/mnt/micron/jsuppe/netpak/mk64_test.z64` = md5 `592fa67a` (driver iteration).
- v56 fixed: race-start freeze (v54 reliable block), joiner camera at sim
  rate (v56). Spectator mode shipped v53. Relay diagnostics v55.

## Test-knob addresses in the staged ROM (592fa67a)
Layout-sensitive — RE-READ from `build/us/mk64.us.z64.map` after ANY rebuild:
- `gNetTestCourse`  @ `0x80417bbc`  (poke course id, e.g. `0a`=Toad's Turnpike, `ff`/0=off)
- `gNetTestPauseAt` @ `0x80417bc0`  (sim frame to pause at)
- `gNetTestPauseLen`@ `0x80417bc4`  (pause hold ticks, 0→300)
- `gNetSpecKart`/`gNetSpecView` @ `0x8041bdc0`/`0x8041bdc1`

## OPEN v57 ITEMS (priority order)

### 1. Sim-scheduled pause (the biggest gameplay bug)
Pause is a LOCAL wall-clock event — a racer pausing lets peers keep
simulating → **desyncs the room** and **wedges spectators** at the pause
frame. Confirmed both in the matrix (finding #1/#2) and in the user's live
races. Fix: broadcast "pause at frame N", everyone (incl. spectators) pauses
at exactly df N; resume the same way. Bounded change in `net_race.c` (size
class of the v54 block fix).
- Repro: `dismat.sh` SCEN=pause-bob / pause-alice / pause-long-bob (poke
  gNetTestPauseAt on one instance). Verify: relay diag shows no desync,
  spectator survives, room reaches clean race_summary.

### 2. Spectator freezes when ANY racer leaves (matrix finding #5)
All timings (host/joiner/entry kill). Racer↔racer drop recovery WORKS; only
spectators fail — they never apply/receive the LSDROP. Look at the spectator
LSDROP ingest path + `sLsLocalSpec` guards in `net_race.c`. Drop-applied
poke is `0x58`.

### 3. Toad's Turnpike (course 10) desync — real, no pause involved
Both the user's live desyncs were course 10 (`JAKJ2Y` f864, `GF7WB7` f1488).
Did NOT reproduce on loopback at full speed (`turnpike.sh`, 8600 frames
clean) — suspect a traffic-actor interaction with the STALL path. Repro
recipe: Turnpike + injected loss (`coursesweep.sh LOSS=4` or a `--dev-loss`
relay). Course actors are OUTSIDE the kart RenderSave isolation. Turnpike is
also the heaviest course (sim ~21ms vs ~15.7): the "misc race" section
tripled — that's the ~6ms traffic-car cost, a perf target too.

### 4. Relay drop-tag parse (one-liner)
`np64-relay/src/diag.rs` watches the wrong LSDROP tag (assumed 0x4F; ROM uses
`0x44` = LSDROP_TAG). Relay logs `drops:0` while recovery happens. Fix the
parser so drop events appear in JSONL.

### 5. PUBLIC-hosting default — DONE, rides the v57 cut
Committed `9911e61d6` (host visibility defaults to PUBLIC; the private
default made rooms invisible to FIND GAME). Menu-only; no separate flash.

### 6. Driver AI (representative test races) — DIAGNOSED, not solved
User wants bots that just drive the centerline and don't wall-grind (so
perf/desync data is representative — CAVEAT: all sweep data so far is from
wall-grinding bots; df_max counts SIM frames not race progress). After 11
focused iterations the bot reaches RANK 4 (beats CPUs, as the user predicted)
but WEDGES at one gentle-curve wall-ride per course → no lap completion.
Root cause (trail analysis, `botcheck.py`): follows the CENTERLINE, so on a
long gentle curve it understeers to the OUTER wall and wedges; the escape
frees it but re-wedges (reverse↔forward ping-pong). Next approach: a
RACING-LINE follower (aim biased to the INSIDE of curves), not centerline.
Tools: `botrace.sh` (COURSE=ff = no override) → `botcheck.py <alice_log>`
renders rank/progress + ASCII trail. Alternative to bots: TAPE CORPUS —
record real human races, replay bit-exact for representative analysis.

## The v57 cut (task #19 / when the fixes land)
Bump `NETPAK_ROM_VERSION` 56→57 in `include/net_menu.h`. Battery:
`replaytest.sh RECORD_SOLO=1` + `spec3.sh` + `xsmoke.sh` (cross-machine —
REQUIRED, loopback is blind to delivery/timing bugs) + re-run the
previously-failing `dismat.sh` scenarios until green. Then product build
(`GCC=1 ./buildtest.sh product`), archive to `releases/`, RELEASES.md entry,
`git tag v57`, `./flashcart.sh`, restage `mk64_netpak_human.z64`, scp to
balthazar. Riders: pause fix, spectator-drop fix, Turnpike fix if found,
PUBLIC default, relay drop-tag parse.

## Key harness commands (all in /mnt/micron/jsuppe/netpak/)
- `relaytail.py [-f] [ROOM]` — human view of relay diag JSONL (simrate,
  desync, block-ack, per-race slowdown %). Diag under `netpak/diag/<date>/`.
- `dismat.sh SCEN=<...>` — disruption matrix (kill/pause × role × timing).
- `xsmoke.sh` — cross-machine melchior↔balthazar smoke (REQUIRED release gate).
- `coursesweep.sh` / `turnpike.sh` / `botrace.sh` — course + driver testing.
- `vidcap.sh` — capture app-start + 4p race frames → host ffmpeg.
- Relay: `relay_deploy.sh` (has --diag-dir). Bridge: `bridge_start.sh`.

## Landmines (do not relearn the hard way)
- NETCODE FILES MUST STAY IDO (Makefile filter-out) — GCC -ffast-math broke
  live 2P while solo gates stayed green.
- Sim is ROM-LAYOUT-SENSITIVE: tapes verify only their own build; judge by
  within-build replay + harness8, never cross-build.
- Container harness rooms must use timestamps not `$$` (docker = pid 1).
- Canonical ares = `/home/melchior/dev/ares` (netpak/ares copy is STALE).
- balthazar ares must be kept in sync (staleness gate now in
  balthazar_online.sh) — a stale build silently drops FIND GAME etc.
- Loopback batteries are BLIND to loss/timing bugs — always run xsmoke.

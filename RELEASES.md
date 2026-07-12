# NetPak64 release manifest

Traceability for gameplay regressions: every handed-out ROM maps to an exact
commit, build flags, and md5. Rollback ROMs live in
`/mnt/micron/jsuppe/netpak/releases/`. Verification tapes are archived next to
them, named `<version>_<md5>.tape` — a tape verifies ONLY the exact ROM that
recorded it (sim is ROM-layout-sensitive; see HARNESS_NOTES.md).

## v58 — 2026-07-12 — QUIT NO LONGER FREEZES THE HOST
- commit: (tagged `v58`) — after 69b3ff69c
- product ROM: md5 4d57b42f (`releases/v58_product_4d57b42f.z64`)
- FIXES: joiner clicking QUIT RACE froze the host (user-hit, room 355EQU:
  host stalled + hard-froze ~36s without recovering). The quitter now
  broadcasts a SELF-DROP so every peer converts its kart to a CPU INSTANTLY
  (was relying on the 8s stall-timeout drop, which the console froze before
  reaching). Verified: host MAX stall_ticks=0 (was 81+/freeze), 0 desync.
- gates: loopback replay 318/0 + spec3 3-instance identical + quit self-drop
  (host stall 0). **xsmoke DEFERRED again (balthazar offline); the fix uses
  the existing LSDROP mechanism — transport unchanged from v54/v57 which
  passed xsmoke. Run when balthazar is up.**
- still open: spectator-freezes-on-leaver, Turnpike desync, single-screen
  battle (task #20, in progress — battle ends instantly, gPlayerBalloonCount/
  gModeSelection at spawn is the current lead).

## v57 — 2026-07-12 — ONLINE PAUSE OVERLAY (+ PUBLIC default)
- commit: (tagged `v57`) — e8449b960
- product ROM: md5 19d42331 (`releases/v57_product_19d42331.z64`)
- FIXES: online pause now shows a LOCAL overlay menu (CONTINUE / QUIT RACE)
  and the sim KEEPS RUNNING for everyone — pausing/quitting no longer
  desyncs the room (was the top live bug). QUIT leaves the race gracefully.
  Also: host visibility defaults to PUBLIC (FIND GAME sees hosted rooms).
- also fixed a replay-path regression the pause commit briefly introduced
  (capture ran during replay) — replay 100/0.
- gates: loopback replay 100/0 + spec3 3-instance identical + pause-continue/
  pause-quit desync=False. **xsmoke (cross-machine) DEFERRED: balthazar was
  offline at cut. Transport/delivery layer is UNCHANGED from v56 (which passed
  xsmoke); all v57 changes are input/menu/render/replay. Run xsmoke when
  balthazar is up; the next real console<->balthazar game is also a live check.**
- NOT in this cut (still open): spectator-freezes-on-leaver, Turnpike desync.
- WIP inert scaffolding shipped (gNetTestBattle defaults 0): 1P battle spawn
  probe for single-screen battle mode (task #20).

## v56 — 2026-07-10 — JOINER CAMERA AT SIM RATE
- commit: (tagged `v56`) — a1aa055fe
- product ROM: md5 a8e69298 (`releases/v56_product_a8e69298.z64`)
- FIXES: joiner's view wider than the host's, accentuated while skidding —
  the local chase cam integrated per RENDER frame (kept converging through
  lockstep micro-stalls); now gated to sim advance like the host's camera.
- gates: loopback replay 305/305 + 3p/spectator identical; cross-machine
  xsmoke 7141 frames identical (netpak/xsmoke.sh — the scripted required
  gate from now on). Also covers v55's pending cross-machine check.

## v55 — 2026-07-10 — RELAY DIAGNOSTICS
- commit: (tagged `v55`) — 628572a13
- product ROM: md5 b99e0119 (`releases/v55_product_b99e0119.z64`)
- new: diagnostic uplink to the relay (sink node 0xFE): batched debug pokes
  (console pokes observable at last), 2s wedge beacon (gamestate/df/stall/
  gate flags), 10s in-race perf summary (real-hardware sim timings). Paired
  with the relay-side passive monitor (np64-relay diag.rs): per-room JSONL
  under netpak/diag/ — simrate, DESYNC detection, block-ack latency, race
  summaries. Viewer: `python3 netpak/relaytail.py [-f] [room]`.
- gameplay-inert: telemetry only, session-gated, ~200B/s worst case.
- gates: loopback replay 302/302 + 3p/spectator identical; uplink verified
  end-to-end on the deployed relay. Cross-machine smoke PENDING (balthazar
  asleep at cut time) — transport path unchanged from v54 which passed it.

## v54 — 2026-07-10 — RELIABLE RACE-ENTRY (start-freeze fix)
- commit: (tagged `v54`) — 1f3cd35f8
- product ROM: md5 222067bb (`releases/v54_product_222067bb.z64`)
- FIXES: "all online games freeze at race start" (v52/v53 console+balthazar,
  3/3 repro): the host's CPU-block was streamed once over raw UDP; a lost or
  too-early chunk wedged the joiner at the gate forever (black screen), host
  dropped them and raced CPUs. Latent since v40-era; exposed by real-network
  loss + uneven course-load times (all earlier batteries were loopback).
- new protocol msg LSBLKACK (0x41): joiner ACKs block completion; host
  re-sweeps chunks to un-ACKed peers until done or ~30s. Chunk apply now
  seq-bitmask deduped.
- gates: cross-machine melchior<->balthazar race — joiner engaged, 6529
  frames compared ALL IDENTICAL; loopback replay 139/139 + 2p identical.
- NOTE: cross-machine autopilot smoke test (spec: xhost.sh pattern in
  scratchpad) should join every future release battery — loopback alone
  proved blind to this whole failure class.

## v53 — 2026-07-10 — SPECTATOR MODE
- commit: (tagged `v53`) — 4fa079c22 + d537143fa on top of v52
- product ROM: md5 4fc0825c (`releases/v53_product_4fc0825c.z64`)
- build: same as v52 (`GCC=1 ./buildtest.sh [product]`)
- PROTOCOL CHANGE: OnlineMsg 12->13 bytes (spec mask in GO) — v52 and v53
  cannot share a room (version gate blocks them; also the length check
  silently drops short messages).
- new: hold Z while confirming a join = enter the room as a SPECTATOR
  (watcher shown blue + W in the lobby; in-race: no HUD, SPECTATING <name>,
  L/R cycle karts, C-right chase / C-down Lakitu rear / C-up front /
  C-left cinematic). Replay tapes get the same camera controls.
- regression triage additions:
  | Symptom | Suspect |
  |---|---|
  | Lobby/READY/GO or race-start issues | OnlineMsg size change (4fa079c22) |
  | An extra CPU kart behaving oddly | spectator pre-drop path (4fa079c22) |
  | Camera/HUD weirdness during normal racing | phase-1 spectator layer (d537143fa) — should be inert unless replaying/watching |
- gates: 3-instance live spectator race hash-identical end-to-end; solo
  replay 279/279; spectator visuals verified by screenshot.

## v52 — 2026-07-10 — THE GCC ADOPTION CUT
- commit: (tagged `v52`) — version bump on top of 8e699f352
- product ROM: md5 0faae383 (`releases/v52_product_0faae383.z64`)
- test ROM: md5 990076a0 (staged `mk64_test.z64`)
- build: `GCC=1 ./buildtest.sh [product]` — 223 files GCC -O2
  (-ffast-math -fno-unsafe-math-optimizations), netcode files pinned IDO,
  GCC_OPT=-O2, NP_PERF_PTCL=0
- gates: within-build record+replay PASS, harness8 8/8 identical (see
  PERF_BASELINE.md for the battery history on the same source)
- perf: sim 15.7ms/frame vs 18.5 on v51 (autodrive workload, ares)

### What changed since v51 (regression triage map)
| Symptom you might see | First suspect (commit) |
|---|---|
| Kart handling/physics feel, drift behavior, CPU difficulty | f64→f32 sweep in player_controller.c (7166bd20a) or GCC -ffast-math on physics files (77c210a4b, 08260a03e) |
| Exhaust smoke / skid marks / dirt: missing, wrong color, wrong timing | particle round (8e699f352) |
| Particles visibly popping in when a kart enters the screen | visibility gate in func_80062C74 (8e699f352) — off-screen karts skip visual-only updates |
| Text/UI, menus, audio pitch | GCC file-set change (77c210a4b) — bisect with safe_gcc.mk |
| Online-only issues (desync, stalls, room browse) | NOT this cut: netcode files are pinned IDO. Check relay/bridge/ares first. |
| Boot hang on hardware | 1MB tail-DMA landmine class — compare .main size vs v51 |

### How to bisect
1. Flash `releases/v51_product_*.z64` — if the symptom disappears, it's this cut.
2. Rebuild at intermediate commits (b660a24cc=v51 → 8e699f352) with the SAME
   flags (`GCC=1`); for GCC-vs-IDO questions build the suspect commit without
   `GCC=1`.
3. Solo-reproducible sim issues: record a tape on the symptomatic build
   (replaytest.sh RECORD_SOLO=1) and replay it per-suspect-commit — but only
   within-build; cross-build hash compares are meaningless (layout sensitivity).

## v51 — 2026-07-09
- commit: e273621ef (+ b660a24cc bump), IDO -O2 (pre-GCC-adoption)
- product ROM: `releases/v51_product_*.z64` (was `mk64_netpak_human.z64`)
- fixed: console 'first frame frozen' race entry (lockstep episode-end debounce)

## v49/v50 — 2026-07-09
- ef6e96d5c tape replay; ad39748ba restored online kart effects (dropped the
  per-frame particle-pool bzero). v50 product previously on console.

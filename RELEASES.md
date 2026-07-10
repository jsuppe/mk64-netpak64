# NetPak64 release manifest

Traceability for gameplay regressions: every handed-out ROM maps to an exact
commit, build flags, and md5. Rollback ROMs live in
`/mnt/micron/jsuppe/netpak/releases/`. Verification tapes are archived next to
them, named `<version>_<md5>.tape` — a tape verifies ONLY the exact ROM that
recorded it (sim is ROM-layout-sensitive; see HARNESS_NOTES.md).

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

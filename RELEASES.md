# NetPak64 release manifest

Traceability for gameplay regressions: every handed-out ROM maps to an exact
commit, build flags, and md5. Rollback ROMs live in
`/mnt/micron/jsuppe/netpak/releases/`. Verification tapes are archived next to
them, named `<version>_<md5>.tape` — a tape verifies ONLY the exact ROM that
recorded it (sim is ROM-layout-sensitive; see HARNESS_NOTES.md).

## v65 — 2026-07-14 — battle: joiner camera feel + desync localization diagnostic
- commit: version-bump cut — after 6bf6c760e
- product ROM: md5 b22473b0 (`releases/v65_product_b22473b0.z64`)
- FIX (joiner "speedboat drift" / wide turning-radius feel): the joiner's isolated
  render-side chase-cam seeds its tuning region (zoom/height/distance) on the first
  render frame — the intro flyover, with sky-high values. GP re-seeds to chase
  values at the racing edge (camera mode 8→1); battle runs mode 9 (never that
  edge), so the joiner kept the intro-flyover altitude/zoom = a too-high, too-wide
  view whose turns read as drift. Added a one-shot battle re-seed
  (sCamBattleSeeded) mirroring GP. Render-only, joiner-side, cam_pop-isolated.
- DIAGNOSTIC (the desync is NOT fully fixed yet): v64's seed-in-hash proved the
  remaining cross-machine battle desync (relay: still frame ~608 on v64) is a
  direct physics-FIELD split, not the RNG seed (seed stayed in sync). Added
  per-kart sub-hash pokes (tag 0x78, kart<<20|subhash) for online battle, logged
  by the relay, so the NEXT console test reveals WHICH kart (host slot 0 / joiner
  slot 1 / other) diverges first — the key next clue to pin the field.
- STILL OPEN: the cross-machine battle desync itself (red squares) — need the v65
  per-kart relay data to localize. Can't reproduce in emulator-vs-emulator
  (TESTAB clean; console-only).
- Verified loopback: harness8 8/8 identical (152 frames); battleprobe 1224 frames
  0 desync, per-kart pokes firing.
- refreshed mk64_netpak_human.z64 -> 65.

## v64 — 2026-07-14 — battle: fix cross-machine DESYNC (balloon jitter advanced the sim RNG)
- commit: version-bump cut — after a77ff7521 (desync fix + seed-in-hash)
- product ROM: md5 624c5bfe (`releases/v64_product_624c5bfe.z64`)
- ROOT CAUSE (from v63 console play: red squares mid-game + joiner freeze at
  battle-end). Relay diag confirmed a real sim desync at frame 656/960/1024
  (varies per game, never at spawn) — combat-triggered. Agent-traced:
  update_player_one_balloon_position calls random_int for COSMETIC balloon jitter
  (code_80057C60.c:6001/6028/6034), mutating gRandomSeed16 — the shared sim RNG
  lockstep carries frame-to-frame. Battle-only path (GP never runs it). The float
  accumulators gating those calls are unhashed+unsynced, so a sub-hash per-console
  difference changes the random_int call count that frame → sim seed diverges →
  next frame's item rolls/physics split → a hashed field finally diverges hundreds
  of frames later. The 0x76/0x77 hash never covered the RNG seed, so it was
  invisible until it leaked — exactly the mid-game/varies signature.
- FIX (a77ff7521): for online, save+restore gRandomSeed16 around the battle
  balloon-position update — jitter stays cosmetic but never advances the shared
  sim stream. Also folded gRandomSeed16 into both sim hashes so this whole class
  of RNG-stream divergence is now caught on the frame it happens (confirmation +
  future safety). The joiner end-freeze was a symptom: a desynced sim reaches
  battle-over on different frames, so the clean-leave couldn't fire together — the
  desync fix should resolve it.
- Verified loopback: harness8 8/8 identical (153 frames, seed in hash, no false
  positives, GP intact); battleprobe 1210 frames 0 desync. CROSS-MACHINE console
  verification PENDING (this is the loopback blindspot — must test on device).
- STILL OPEN: joiner chase-cam angle/steering feel (needs console tuning; battle
  cam mode 9 func_8001EA0C tracks heading like GP mode 1, so likely not a tracking
  failure — may have been the desync perturbing the kart).
- refreshed mk64_netpak_human.z64 -> 64.

## v63 — 2026-07-13 — battle gameplay fixes: host drives, no end-freeze, HUD/minimap/balloons
- commit: version-bump cut — after fee2d947f (balloon visibility+scale)
- product ROM: md5 745abc7b (`releases/v63_product_745abc7b.z64`)
- FIXES (from v62 console play, all rooted in vanilla battle assuming 2-4
  split-screen players in slots 0-3):
  - **Host can't drive** (011ffe269): the 1P accel/steer handler
    (`handle_a_press_for_all_players_during_race`) had GRAND_PRIX/TIME_TRIALS but
    no BATTLE case, so slot 0 got zero driving input. Added BATTLE.
  - **End-freeze** (5a73d8aab): the vanilla battle-results menu indexes
    `D_800E7300[(gPlayerCount-2)*4]` and hard-loops (`while(true){;}`) on a
    missing item type; online battle runs gPlayerCount==1 → (1-2)*4=-4 OOB →
    both consoles froze after the winner shift. Online battle now skips the
    results flow and cleanly leaves to the menu (net_menu_online_end +
    session_leave + func_8028FBD4), deterministically.
  - **Lap HUD** (5a73d8aab): 1P HUD drew "LAP 1/3"; guarded on != BATTLE.
  - **Victory count** (5a73d8aab): update_player_battle_status looped slots 0-3;
    now 0-7 so an online survivor in 4-7 can win/end the battle.
  - **Joiner minimap** (5a73d8aab): battle radar looped `< gPlayerCountSelection1`
    (=1) so the joiner never saw its own blip; now draws every existing kart and
    highlights the local kart.
  - **Balloons don't pop on joiner's screen** (fee2d947f): render was frustum-
    culled per-screen and fix-scaled the wrong "own kart"; online battle now
    skips the cull and keys own-kart off net_lockstep_local_slot().
- STILL OPEN (needs console tuning with feedback): joiner chase-cam angle differs
  from the host's sim camera (general chase-cam approximation, affects GP too);
  other 3 arenas may cluster karts via the spawn fallback.
- Verified: battleprobe 1200+ frames 0 desync; harness8 8/8 identical across 153
  frames (GP determinism unaffected — all new code online+battle gated).
- refreshed mk64_netpak_human.z64 -> 63.

## v62 — 2026-07-13 — battle: no CPU fillers (fixes stray karts + balloons)
- commit: version-bump cut — after 5dd41cdc2 (no CPU fillers)
- product ROM: md5 8c128832 (`releases/v62_product_8c128832.z64`)
- FIXES (from v61 console play): the 1P-battle spawn made all 8 karts EXIST, so a
  2-player battle had 6 broken CPU karts (no path to follow on a battle stage)
  with ground-level balloons. Now only the room's np players EXIST (like a
  split-screen 2p battle); the rest are spawned deterministically but don't
  exist. battleprobe confirms just the 2 players, balloons floating correctly, 0
  desync. harness8 all-8-identical.
- STILL OPEN (need console re-test with reduced roster): host reported "can't
  drive my kart" and joiner "steers in tight circles toward the camera" — a
  control/camera issue not reproducible via the autodrive probe. Input/puppet
  paths verified sound (slot 0 never puppeted; karts sim-driven). Also still: HUD
  shows LAP 1/3 not balloons; other 3 arenas may cluster karts; victory not
  verified.
- refreshed mk64_netpak_human.z64 -> 62.

## v61 — 2026-07-13 — ONLINE BATTLE IS PLAYABLE 🎉
- commit: version-bump cut — after a42c4e205 (all-arenas + crash-proof spawn)
- product ROM: md5 9b28a800 (`releases/v61_product_9b28a800.z64`)
- ADDS: online single-screen BATTLE now RUNS. The launch crash is fixed — karts
  were spawning below the platform because spawn_players_online_battle used the
  UNINITIALIZED sp5A as the Y (func_8003C0F0 sets it only for racing courses);
  get_surface_height treated it as a downward ceiling and returned the -3000
  default -> off-mesh -> calculate_surface_height OOB. Now the spawn Y is derived
  from the arena surface, and each kart's position is validated (off-stage ->
  fall back to a known-good ring point), so no kart spawns off-mesh on any arena.
- FLOW: SELECT CLASS -> BATTLE -> host picks the arena on the vanilla battle
  course screen -> synchronized launch -> a real battle (balloons, minimap, each
  player sees their own kart). All 4 arenas selectable.
- gates: battleprobe = sim runs 1600+ frames, D_800DC510 0->1->2->3 (RACING), 0
  desync (bit-identical). harness8 = all 8 identical (GP untouched). xsmoke
  DEFERRED (balthazar offline).
- STATE: Big Donut is well-tuned and fully playable. Block Fort / Skyscraper /
  Double Deck run without crashing but may cluster karts where the octagon falls
  off-stage (per-arena position tuning is follow-up). HUD still shows LAP 1/3
  (GP HUD) not a balloon count. Victory (last kart standing) over 8 slots not yet
  verified. In-tree diagnostic markers (0x50-0x56) still present (gated, inert).
- also: refreshed mk64_netpak_human.z64 (balthazar cross-machine ROM) to 61.

## v60 — 2026-07-13 — HOST CAN PICK BATTLE + ARENA (menu flow)
- commit: version-bump cut — after e550f8611 (battle course-select refactor)
- product ROM: md5 567408ab (`releases/v60_product_567408ab.z64`)
- ADDS: online host menu for battle. SELECT CLASS now offers 50/100/150cc +
  BATTLE; picking BATTLE opens the room and the host chooses the arena on the
  vanilla battle course-select screen (map previews, same look as split-screen
  2p/3p/4p battle); joiners see WAITING FOR HOST and adopt it (like GP). Lobby
  shows "BATTLE".
- gates: harness8 = ALL 8 IDENTICAL (GP determinism untouched — battle code is
  additive and only runs when BATTLE is selected). xsmoke DEFERRED (balthazar
  offline); menu-only + render change, transport unchanged from v59.
- **KNOWN, NOT YET PLAYABLE**: launching a battle still CRASHES on the first sim
  frame — the kart falls off the Big Donut platform (calculate_surface_height
  OOB, posY ~= -2994). The menu is fully navigable but selecting an arena and
  starting will crash. Next fix = Lakitu start-hold (skip gravity until GO).
  Only Big Donut has a 1P spawn wired; the other 3 arenas spawn nothing.
- also: refreshed mk64_netpak_human.z64 (balthazar cross-machine ROM) to 60.

## v59 — 2026-07-13 — JOINER SEES THE LAKITU COUNTDOWN LIGHTS
- commit: version-bump cut — after 2e819c79a (joiner-Lakitu fix)
- product ROM: md5 118925cd (`releases/v59_product_118925cd.z64`)
- FIXES: in single-screen online the countdown Lakitu was drawn only for slot 0
  (the HOST's kart), so the joiner — whose local chase-cam follows its OWN kart —
  saw Lakitu off-screen ("Lakitu only shows up to the host"). render_lakitu now
  re-anchors the DRAW position to the local kart on the joiner (render-only; sim
  object + deterministic light state untouched). Host/offline unchanged.
- gates: harness8 = ALL 8 INSTANCES IDENTICAL (determinism preserved); live
  2-instance capture shows the light in front of each player's own kart; a poke
  confirmed the re-anchor engages on the joiner (local slot 1). xsmoke DEFERRED
  (balthazar offline); render-only change, transport unchanged from v58.
- NOTE: the branch also carries in-progress online-BATTLE work (barrier + 1P
  spawn/camera + diagnostics). All of it is INERT in product — battle can't be
  selected (online is always GRAND_PRIX unless the test-only gNetTestBattle poke
  is set), so v59 is functionally v58 + the joiner-Lakitu fix.

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

# NetPak64 — current state (pick up here)

Living status doc so any session can resume. Update this at every cut; the
per-version history lives in `RELEASES.md` (version → commit → md5 → root
cause). Last updated: 2026-07-16 (v75 current).

## Shipped state
- **v75 on console + balthazar** (2026-07-15, product md5 `d3b2389c`, tag `v75`).
  Rollback ROMs + verification tapes: `/mnt/micron/jsuppe/netpak/releases/`.
- v75 = joiner camera pan ×gTickSpeed (the REAL rate fix; v74 was a no-op) +
  second-game e2e verified (`battleend2.sh`) in both join-by-code and host-menu
  flows.
- **Pending user retest on hardware:** camera pan feel; the v74 "host
  black-screen at game-2 start-match" (does NOT reproduce at HEAD in harness).

## Open items (priority order)
1. **Cross-machine kart-1 physics desync, Big Donut only** — THE blocker.
   Console-vs-emulator only (loopback is blind to it). Two causes already
   killed, same pattern both times: a conditional `random_int` draw inside the
   sim block, gated by a float threshold `(speed/18)*216 >= 8.0` that rounds
   differently N64-FPU vs ares → diverges the shared `gRandomSeed16`
   (balloon jitter v64, hop-bob `func_80030A34` v72). v73+ still desyncs on
   kart-1 physics (0x5B mask=0x02) = a third cause: another surface-dependent
   float branch OR an uninitialized battle field (console garbage vs ares
   zeros). Big Donut is the only BRIDGE-surface arena. NEXT: (a) have the user
   try Block Fort / Skyscraper / Double Deck (ASPHALT — may already be clean);
   (b) extend the 0x5B localizer per-FIELD (position vs motion) to pin what in
   kart 1 diverges.
2. **Pause at battle-end freezes both instances** (user-reported, v72 era).
3. **Spectator freezes when ANY racer leaves** (open since v57, all timings).
   Racer↔racer drop recovery works; spectators never apply the LSDROP. Look at
   the spectator LSDROP ingest + `sLsLocalSpec` guards in `net_race.c`.
   Drop-applied poke = `0x58`.
4. **Toad's Turnpike (course 10) live desync** (open since v57). Never reproduced
   on clean loopback (8600f clean); suspect traffic-actor × STALL-path
   interaction — course actors are OUTSIDE the kart RenderSave isolation.
   Repro: Turnpike + injected loss (`coursesweep.sh LOSS=4`). Also the heaviest
   course (~21ms sim vs 15.7 baseline) — perf target too.
5. **Relay drop-tag parse one-liner** (still unfixed, verified 2026-07-16):
   `np64-relay/src/diag.rs` watches `0x4F` for LSDROP; the ROM sends
   `0x44` (`src/net_race.c` LSDROP_TAG). Relay logs `drops:[]` while real
   recoveries happen.
6. **Driver AI** (test-infra, diagnosed not solved): centerline-following bots
   wedge on gentle-curve outer walls (rank 4, no lap). Next approach =
   racing-line follower (aim inside of curves). Tools: `botrace.sh` →
   `botcheck.py`. Alternative: tape corpus of real human races.
7. **Perf** (paused during battle arc): sim CPU is THE bottleneck, 15.7ms after
   the particle round. Next targets: kart-physics memory behavior (4.7ms),
   misc-race split (2.6ms), pool1 skid (0.9ms). See
   `/mnt/micron/jsuppe/netpak/PERF_BASELINE.md`. `NP_PERF_PTCL=1` for fine
   particle profiling (costs 0.7ms).

## Rotating-host battle battery (2026-07-16/17)
- `battle4g.sh` (netpak dir): 4 instances, 4 Big Donut battles, host rotating
  alice→bob→carol→dave through the REAL host-menu/FIND flow — **4/4 green, 0
  desyncs**, relay-confirmed room creators. Fresh quartet per game;
  `gNetTestHostSeat` poked at boot (commit ed62e98a0; addresses in the script
  header are layout-sensitive per test-ROM build).
- `battle4x4.sh` = the same-session variant (autopilot re-arm between games);
  blocked by an open finding: **after a joiner has raced once, external GDB RAM
  pokes to it silently stop taking effect** (host memory stays pokeable) —
  same-session rotation needs a ROM-side mechanism (e.g. lockstep-synced game
  counter). Second finding: a multi-second emulator freeze at the battle-over
  moment can wedge that console (same family as the open pause-at-end bug).
- np_cmd() now paces its CMD_STATUS poll (~170us) — stalled commands used to
  burn the 4M-line io-trace budget in seconds. NOTE: rides into the next
  product cut.

## Desync-hunt playbook
- **0x5B poke** = one-poke desync localizer: returns a kart bitmask naming
  which kart's per-kart hash (inside the LSHASH, v71) diverged; mask=0 means
  the RNG seed itself diverged. Next refinement = per-field (pos vs motion).
- **The recurring cross-machine pattern:** any cosmetic/render RNG in the sim
  block that draws a CONDITIONAL number of `random_int` calls silently
  diverges `gRandomSeed16` when the condition is a float threshold (N64 FPU
  rounds differently than ares). Fix pattern: save/restore `gRandomSeed16`
  around cosmetic draws when online.
- Relay-side: diag JSONL per room (simrate/desync/block-ack/summaries), viewer
  `relaytail.py [-f] [ROOM]`; v55 ROM uplink sends pokes/beacon/perf to sink
  0xFE. ch1 payloads carry a 4-byte transport header.
- Race-entry log marker: `io wW 0020 <- 630004` (0x63 tracer). Solo online
  battle ends instantly by design (aliveCounter==1 at start).
- **Loopback blindspot:** emulator-vs-emulator cannot reproduce console FP or
  uninitialized-memory desyncs, and loopback batteries cannot see loss/timing
  bugs. Console + cross-machine (`xsmoke.sh`) tests are the only proof.

## Cut checklist (EVERY version — landmine if skipped)
1. Bump `NETPAK_ROM_VERSION` in `include/net_menu.h`.
2. Gates: `harness8` 8/8 identical + `battleprobe2` 0 desync; netcode files
   (`net_menu.c`/`net_race.c`) stay IDO.
3. Artifacts: console flash (`flashcart.sh`, product ROM) • `mk64_test.z64`
   (harness, buildtest stages) • `mk64_netpak_human.z64` restaged + scp to
   balthazar (stale = balthazar runs the old version) •
   `releases/vNN_product_<MD5>.z64` • `RELEASES.md` entry •
   `balthazar_online.sh` expect-md5 • `git tag vNN`.
4. Cross-machine smoke (melchior↔balthazar) before calling it good.

## Landmines (do not relearn)
- **Netcode files MUST stay IDO** (Makefile filter-out) — GCC broke live 2P
  while solo gates stayed green.
- **Sim is ROM-layout-sensitive:** any function-size shift changes the sim
  trajectory; tapes verify ONLY their own build. Judge changes by within-build
  replay + harness8, never cross-build. Canary check21/kart4 failing = layout
  shift, not a specific bug.
- **1MB boot:** IPL3 loads exactly 1MB; anything past that needs the entry.s
  tail-DMA (v40). `.main` segment is FULL — diag/new code goes in the nettext
  overlay.
- **Big-endian extern trap:** `extern s16` for an s32 (e.g. `gTickSpeed`)
  reads the HIGH half = 0 on MIPS.
- `D_800DC5EC` → camera write = race-entry deadlock (never repeat).
- Container harness rooms need unique names (timestamps, not `$$` — docker
  pid 1); watch for container sleep timeouts mid-battery.
- Canonical ares = `/home/melchior/dev/ares` (the netpak/ares copy is STALE —
  silent harness death). Balthazar's ares has a staleness gate in
  `balthazar_online.sh`.
- Test-knob addresses (gNetTestCourse etc.) are layout-sensitive — re-read
  from `build/us/mk64.us.z64.map` after ANY rebuild.

## Machines & tooling
- **melchior** (this machine): repo `~/dev/mk64` (branch `netpak64` → private
  remote jsuppe/mk64-netpak64; origin = upstream, pull-only). All harness
  scripts in `/mnt/micron/jsuppe/netpak/` (`battleend2.sh`, `battleprobe*.sh`,
  `dismat.sh`, `xsmoke.sh`, `coursesweep.sh`, `botrace.sh`, `relaytail.py`,
  `flashcart.sh`, tapes, releases/).
- **balthazar**: second live machine, runs `mk64_netpak_human.z64` via
  `balthazar_online.sh`.
- **Console**: SC64 flashcart (`flashcart.sh` or `sc64deployer upload
  --save-type eeprom4k`), fw v2.20.2.
- **Relay**: `~/dev/np64/np64-relay` (`relay_deploy.sh --diag-dir`).

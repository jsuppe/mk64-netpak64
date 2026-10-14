# v77 automated race integration — qualification report

Date: 2026-10-13. Scope: four-peer Mario Raceway automated demos in ares.

## What changed

- Removed the unconditional one-lap limit from automated test builds. Tests now
  use the same three-lap rule as the product build.
- Track AI-controlled online participants from the currently applied shared
  input marker, not console-local automation flags.
- Route those participants through the participant finish/results path rather
  than treating them as background CPUs.
- In sessions with automated participants, wait for all still-participating
  slots before starting results. Human-only finish behavior remains unchanged.
- Disable the misleading controller-input overlay in normal demo capture.
- Record binary driver telemetry over the device trace: frame, seat, lap, path,
  player flags, camera mode/yaw, speed, X/Z position, and low-24-bit effects.
  Non-DEBUG libultra makes osSyncPrintf a no-op in this build.

## Reproduced v76 defect

The v76 baseline compared 3,164 shared frames without divergence, but every
client switched from chase camera mode 1 to cinematic mode 3 after the test's
one-lap finish. Later footage therefore showed post-finish behavior under a
HUD that still displayed a three-lap race. Synchronization did not catch that
player-experience defect.

## Final-build results

Same staged test ROM in all three runs; four independent clients per run.
Device witness `cc410008`: input delay 4, CC selection 1, course ID 8.

| Run | Shared frames compared | Divergence | Participants reached lap 3 | Premature finish samples | Non-chase active samples |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1 | 9,241 | 0 | 4/4 | 0 | 0 |
| 2 | 7,664 | 0 | 4/4 | 0 | 0 |
| 3 | 8,040 | 0 | 4/4 | 0 | 0 |

Total: 12 participant completions; 24,945 compared frames across three separate
runs, not one continuous run. X/Z telemetry showed progress with no sampled
stationary intervals unaccompanied by the recorded low-bit effects. Camera yaw
was checked against each local kart. Large yaw differences in run 3 seat 1
occurred during banana spinouts (effect 0x80): the kart spins while the stock
camera holds its heading, which must not be mistaken for an unsolicited orbit.

[Machine-readable participant summaries](https://github.com/jsuppe/mk64-online/blob/main/docs/v77-driving-results.json) include raw
and non-spin yaw percentiles, speed samples, displacement, and the exact gates.
The checks sample about once per 60 logical frames; they are not continuous
collision counters and cannot establish zero brief contacts. Visual review of
captured frames across the full runs showed on-course progress and legitimate
item-hit/recovery events, not sustained wall pin/reverse loops.

## Build identity

- Product ROM MD5: `ce94c976521b215d7b4dee0b7151beeb`.
- Test ROM MD5: `180f31ee075cfd186973a4f779b2b50f`.
- Source: [MK64 v77](https://github.com/jsuppe/mk64-netpak64/tree/v77).
- Test and product automation flags differ. Do not mix binaries in a session.
- Replay/hash tapes are build-specific; no cross-build replay pass is claimed.

## Recordings

[Media release](https://github.com/jsuppe/netpak64/releases/tag/showcase-v77).

- Host-view excerpt: 90 seconds, 30 fps, run 3; native CPU, emulator chrome
  cropped, no controller overlay. Source interval 00:10–01:40 of the host capture.
- Uncut host capture: 210 seconds, 30 fps, run 3, starts during lap 1 and continues
  through lap 3, legitimate post-finish camera changes, and results.
- Four-peer excerpt: 90 seconds, 15 fps, run 1, source interval 00:45–02:15.
- Uncut four-peer capture: 240 seconds, 15 fps, run 1, retains chronology through
  race completion/results. Emulator chrome is cropped and the grid downscaled.
- Captures are silent. There is no frame interpolation, driving teleportation,
  collision disabling, or speed-up edit in the published excerpts. CFR normalization
  may duplicate/drop capture frames to maintain the nominal recording frame rate.

## Still outside this qualification

All other race courses/classes, Battle navigation, mixed human/automated sessions,
normal human joiner-camera regressions, drop/rejoin cases, and physical-console
parity remain separate work. The inherited Big Donut console desync and pause-at-end
findings are not closed by this emulator race test. The results UI can show a
50cc class label although the race witness reports CC=1; class display parity
needs separate investigation. v77 is a scoped demo-integration improvement, not
an all-scenarios automation release.

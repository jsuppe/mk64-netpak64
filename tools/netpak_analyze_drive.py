#!/usr/bin/env python3
"""Summarize actual demo telemetry; reject premature cinematic/finish state.

This is a lifecycle/camera gate, not a replacement for uncut visual review.
Speed is not proof of displacement, and post-finish cinematics are legitimate.
"""
import argparse
import json
import math
import re
from pathlib import Path

SAMPLE = re.compile(
    r"DRIVE frame=(\d+) seat=(\d+) lap=(-?\d+) path=(-?\d+) "
    r"type=([0-9a-fA-F]+) cam=(\d+) yaw=(-?\d+) speed=(-?\d+)"
)


def decode_trace(text):
    """Decode ordered B1..B7 device packets; use the first racing episode."""
    pending = {}
    previous_frame = -1
    for value in re.findall(r"io wW 0020 <- ([bB][1-7][0-9a-fA-F]{6})", text):
        word = int(value, 16)
        tag, payload = word >> 24, word & 0xFFFFFF
        if tag == 0xB1:
            if payload < previous_frame:
                return
            previous_frame = payload
            pending = {tag: payload}
        elif pending:
            pending[tag] = payload
            if tag == 0xB7 and all(t in pending for t in range(0xB1, 0xB8)):
                kind = pending[0xB2]
                lap = (kind >> 16) & 15
                yaw = (pending[0xB4] >> 8) & 65535
                signed24 = lambda v: v - 0x1000000 if v & 0x800000 else v
                yield dict(frame=pending[0xB1], seat=(kind >> 20) & 15,
                           lap=lap - 16 if lap & 8 else lap,
                           type=kind & 65535, path=pending[0xB3] & 65535,
                           cam=pending[0xB3] >> 16,
                           yaw=yaw - 65536 if yaw & 32768 else yaw,
                           speed=(pending[0xB4] & 255) * 10,
                           x=signed24(pending[0xB5]) / 10,
                           z=signed24(pending[0xB6]) / 10,
                           effects=pending[0xB7])
                pending = {}


def summarize(path):
    # A stalled render can print the same logical-frame sample again.
    samples = {}
    text = path.read_text(errors="replace")
    for match in SAMPLE.finditer(text):
        frame, seat, lap, point, kind, cam, yaw, speed = match.groups()
        sample = dict(frame=int(frame), seat=int(seat), lap=int(lap),
                      path=int(point), type=int(kind, 16), cam=int(cam),
                      yaw=int(yaw), speed=int(speed))
        samples[(sample["frame"], sample["seat"])] = sample
    for sample in decode_trace(text):
        samples[(sample["frame"], sample["seat"])] = sample
    rows = list(samples.values())
    active = [s for s in rows if 0 <= s["lap"] < 3 and not (s["type"] & 0x0800)]
    premature = [s for s in rows if s["lap"] < 3 and s["type"] & 0x0800]
    wrong_camera = [s for s in active if s["cam"] != 1]
    yaw = sorted(abs(s["yaw"]) * 360.0 / 65536.0 for s in active)
    # Stock chase cameras intentionally do not spin along with an item-hit kart.
    spin_mask = 0x40 | 0x80 | 0x400 | 0x4000 | 0x10000
    non_spin_yaw = sorted(abs(s["yaw"]) * 360.0 / 65536.0 for s in active
                          if "effects" in s and not s["effects"] & spin_mask)
    stationary_frames = longest_stationary = 0
    displacement = 0.0
    for before, after in zip(active, active[1:]):
        if "x" not in before or "x" not in after:
            continue
        moved = math.hypot(after["x"] - before["x"], after["z"] - before["z"])
        displacement += moved
        if moved < 2 and not before["effects"] and not after["effects"]:
            stationary_frames += after["frame"] - before["frame"]
            longest_stationary = max(longest_stationary, stationary_frames)
        else:
            stationary_frames = 0
    return dict(log=str(path), samples=len(rows), seats=sorted({s["seat"] for s in rows}),
                max_lap=max((s["lap"] for s in rows), default=-1),
                active_samples=len(active), premature_finish_samples=len(premature),
                non_chase_active_samples=len(wrong_camera),
                distinct_active_path_points=len({s["path"] for s in active}),
                yaw_error_p95_degrees=round(yaw[min(len(yaw)-1, int(len(yaw)*.95))], 2) if yaw else None,
                non_spin_yaw_p95_degrees=round(non_spin_yaw[min(len(non_spin_yaw)-1, int(len(non_spin_yaw)*.95))], 2) if non_spin_yaw else None,
                low_speed_active_samples=sum(s["speed"] < 100 for s in active),
                sampled_displacement_units=round(displacement, 1),
                longest_unexplained_stationary_frames=longest_stationary,
                lifecycle_camera_gate=bool(active) and not premature and not wrong_camera)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--require-finish", action="store_true",
                        help="also require each log to witness lap 3")
    args = parser.parse_args()
    results = [summarize(path) for path in args.logs]
    print(json.dumps(results, indent=2))
    return 0 if all(r["lifecycle_camera_gate"] and
                    (not args.require_finish or r["max_lap"] >= 3) for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())

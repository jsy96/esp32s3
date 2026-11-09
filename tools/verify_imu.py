#!/usr/bin/env python3
"""Bounded serial smoke test; keep the IMU stationary during capture.

Run: uv run --no-project --with pyserial tools/verify_imu.py --port COM9
This checks telemetry and stationary acceleration, not USB HID input or gyro bias.
"""
import argparse
import math
import statistics
import time

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds < 2:
        parser.error("--seconds must be finite and at least 2")

    samples = []
    version = None
    malformed = 0
    with serial.Serial(args.port, 115200, timeout=0.2) as port:
        try:
            time.sleep(0.3)
            port.reset_input_buffer()
            port.write(b"vs")
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                line = port.readline().decode("utf-8", "replace").strip()
                if line.startswith("#V "):
                    version = line
                    print(line)
                elif line.startswith("#T "):
                    try:
                        fields = line.split()
                        if len(fields) != 10:
                            raise ValueError("wrong field count")
                        values = [float(v) for v in fields[1:]]
                        if not all(math.isfinite(v) for v in values):
                            raise ValueError("nonfinite telemetry")
                        samples.append(values)
                    except ValueError:
                        malformed += 1
                elif line:
                    print(line)
        finally:
            port.write(b"x")

    running = [s for s in samples if s[8] == 2]
    print(f"Telemetry frames={len(samples)}, running={len(running)}, malformed={malformed}")
    if version is None or len(running) < 20 or malformed:
        print("FAIL: missing version, insufficient running frames, or malformed telemetry")
        return 1

    norms = [math.sqrt(sum(v * v for v in s[5:8])) for s in running]
    mean_g = statistics.mean(norms)
    std_g = statistics.pstdev(norms)
    print(f"Acceleration norm: mean={mean_g:.4f}g, std={std_g:.4f}g, "
          f"min={min(norms):.4f}g, max={max(norms):.4f}g")
    for index, label in [(1, "roll_deg"), (2, "pitch_deg"), (3, "outX"), (4, "outY")]:
        values = [s[index] for s in running]
        print(f"{label}: min={min(values):.3f}, max={max(values):.3f}, "
              f"mean={statistics.mean(values):.3f}")
    if not 0.85 <= mean_g <= 1.15 or std_g > 0.05:
        print("FAIL: acceleration outside stationary smoke-test limits; check motion/orientation/hardware")
        return 1
    if any(abs(s[3]) > 127 or abs(s[4]) > 127 for s in running):
        print("FAIL: joystick output out of range")
        return 1
    print("PASS: running telemetry and stationary acceleration; HID and motion directions not verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

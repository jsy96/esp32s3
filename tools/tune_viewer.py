#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tune_viewer.py — ESP32-S3 体感倾斜手柄 实时调参/观测工具

通过 USB 串口读取固件遥测 (#T 开头的行), 实时显示身体横滚/俯仰角度、
摇杆输出值与原始加速度, 用于佩戴后核对方向、调整死区/满偏角度。

用法 (本机 uv 管理 Python):
    uv run --no-project --with pyserial python tools/tune_viewer.py
    uv run --no-project --with pyserial python tools/tune_viewer.py --port COM7

串口自动识别 VID=0x303A (Espressif) 的设备; 找不到时用 --port 指定。
建议在 Windows Terminal 中运行 (需要 ANSI 转义支持)。

运行中按键:
    r = 远程回中 (以当前姿势为新中位)
    q = 退出 (自动关闭固件遥测)
"""

import argparse
import sys
import time

import serial
import serial.tools.list_ports

try:
    import msvcrt
except ImportError:
    msvcrt = None  # 非 Windows 平台无按键支持, 用 Ctrl+C 退出

ESP_VID = 0x303A        # Espressif USB VID
BAR_W = 24              # 条形图半宽(字符数)
VIEW_RANGE = 45.0       # 条形图满偏对应角度
STATES = {0: "陀螺校准", 1: "姿态收敛", 2: "运行"}


def pick_port(prefer):
    """优先命令行指定; 否则找第一个 VID=0x303A 的串口"""
    if prefer:
        return prefer
    hit = [p for p in serial.tools.list_ports.comports() if p.vid == ESP_VID]
    return hit[0].device if hit else None


def bar(v, rng):
    """把 [-rng, rng] 的角度画成带中线的条形图"""
    frac = max(-1.0, min(1.0, v / rng)) if rng else 0.0
    pos = int(round(frac * BAR_W))
    cells = [" "] * (2 * BAR_W + 1)
    cells[BAR_W] = "|"
    if pos:
        step = 1 if pos > 0 else -1
        for i in range(BAR_W + step, BAR_W + pos, step):
            cells[i] = "-"
        cells[BAR_W + pos] = "#"
    return "[" + "".join(cells) + "]"


def main():
    ap = argparse.ArgumentParser(description="ESP32-S3 体感倾斜手柄遥测查看器")
    ap.add_argument("--port", help="串口号, 如 COM7; 缺省自动识别 VID=0x303A 设备")
    ap.add_argument("--baud", type=int, default=115200, help="波特率, 默认 115200")
    args = ap.parse_args()

    port = pick_port(args.port)
    if not port:
        print("未找到 ESP32 (VID 0x303A) 串口设备。当前可用串口:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}  {p.description}")
        print("请插好开发板的 \"USB\" 口后重试, 或用 --port 指定串口号。")
        return 1

    ser = serial.Serial(port, args.baud, timeout=0.05)
    time.sleep(0.3)          # 等设备枚举稳定
    ser.reset_input_buffer()
    ser.write(b"s")          # 开启固件遥测

    print(f"已连接 {port} @ {args.baud}, 遥测已开启。  按键: r=回中  q=退出")
    print("(若固件刚上电, 等它完成陀螺校准和姿态收敛)")
    print("\n" * 4)          # 预留 4 行状态区

    tel = None
    dirty = False
    exit_code = 0
    try:
        while True:
            # ---- 读串口 ----
            raw = ser.readline().decode("utf-8", "replace").strip()
            if raw.startswith("#T"):
                parts = raw.split()
                if len(parts) == 10:
                    try:
                        tel = (int(parts[1]), float(parts[2]), float(parts[3]),
                               int(parts[4]), int(parts[5]),
                               float(parts[6]), float(parts[7]), float(parts[8]),
                               int(parts[9]))
                        dirty = True
                    except ValueError:
                        pass
            elif raw:
                print("  " + raw)   # 固件日志透传 (校准进度/错误等)

            # ---- 刷新状态区 ----
            if tel is not None and dirty:
                t, roll, pitch, ox, oy, ax, ay, az, st = tel
                lines = [
                    f"状态: {STATES.get(st, str(st)):<4s}   运行时长 {t / 1000:8.1f}s",
                    f"横滚(右倾+): {roll:7.2f}°  {bar(roll, VIEW_RANGE)}",
                    f"俯仰(前倾+): {pitch:7.2f}°  {bar(pitch, VIEW_RANGE)}",
                    f"摇杆输出: X={ox:+4d}  Y={oy:+4d}  (中位 0, 满偏 ±127)",
                    f"加速度(g): X={ax:+.2f} Y={ay:+.2f} Z={az:+.2f}",
                ]
                sys.stdout.write("\x1b[5A" + "\n".join(lines) + "\n")
                sys.stdout.flush()
                dirty = False

            # ---- 按键 ----
            if msvcrt is not None and msvcrt.kbhit():
                k = msvcrt.getwch().lower()
                if k == "q":
                    break
                if k == "r":
                    ser.write(b"r")
                    print("  >> 已发送回中指令")
    except KeyboardInterrupt:
        pass
    except serial.SerialException as e:
        print(f"\n串口错误: {e}")
        exit_code = 1
    finally:
        try:
            ser.write(b"x")   # 关闭固件遥测
            ser.close()
        except Exception:
            pass
    print("\n已断开。")
    return exit_code


if __name__ == "__main__":
    sys.exit(main())

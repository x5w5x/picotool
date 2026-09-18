#!/usr/bin/env python3
"""
Pico UF2 flasher in loop mode. Detect BOOT drive -> wait 2s -> copy UF2.
After flashing, continues watching for the next BOOT drive.

Usage:
    python flash_pico.py [path/to/firmware.uf2]

If no UF2 path is given, defaults to ./build/pico.uf2

Workflow:
  1. Start the script (it watches indefinitely).
  2. Hold BOOTSEL while plugging in USB, or run 'picotool reboot -uf2'.
  3. Script detects drive, waits 2s, copies UF2 -> Pico auto-resets and flashes.
  4. Script continues watching for the next flash request.

Press Ctrl+C to abort.
"""
import shutil
import sys
import time
from pathlib import Path

import serial
import serial.tools.list_ports

# Skip A:/B: (legacy floppy) and C: (system) by default.
SCAN_LETTERS = "DEFGHIJKLMNOPQRSTUVWXYZ"
SCAN_INTERVAL_SEC = 0.5
PRE_COPY_DELAY_SEC = 2.0

# How long to wait after a flash before expecting the next BOOT drive.
# Pico resets after copying; we don't want to re-detect the SAME drive
# in its reset window. 4s is enough.
COOLDOWN_SEC = 4.0

# UF2 boot volume always contains this file at the root.
PICO_BOOT_MARKER = "INFO_UF2.TXT"


def uf2_signature(path):
    """返回 UF2 文件签名，用于检测编译是否生成了新文件。"""
    try:
        stat = path.stat()
    except OSError:
        return None
    return (stat.st_mtime_ns, stat.st_size)


def find_pico_cdc_port():
    """查找当前运行应用固件的 Pico CDC 串口。"""
    for port in serial.tools.list_ports.comports():
        text = " ".join([
            port.device or "",
            port.description or "",
            port.manufacturer or "",
            port.product or "",
        ]).lower()

        # Raspberry Pi VID=2E8A；同时兼容 Windows 描述信息不完整的情况。
        if port.vid == 0x2E8A or "pico" in text or "raspberry" in text:
            return port.device

    return None


def trigger_bootsel_by_1200():
    """对当前 Pico CDC 发送 1200 baud，触发进入 BOOTSEL。"""
    port_name = find_pico_cdc_port()
    if not port_name:
        print("未找到 Pico CDC，保留手动 BOOTSEL 方式。")
        return False

    print(f"检测到新 UF2，发送 1200 baud 进入 BOOTSEL: {port_name}")
    try:
        serial_port = serial.Serial(port_name, 1200, timeout=0.2)
        interruptible_sleep(0.25)
        serial_port.close()
        return True
    except (serial.SerialException, OSError) as exc:
        print(f"1200 baud 触发 BOOTSEL 失败: {exc}")
        return False


def wait_for_uf2_change(uf2_path, previous_signature):
    """等待 UF2 签名变化；返回新签名，支持 Ctrl+C。"""
    while True:
        current_signature = uf2_signature(uf2_path)
        if current_signature is not None and current_signature != previous_signature:
            return current_signature
        interruptible_sleep(0.25)


def find_pico_boot_drive():
    """Scan all drives for one containing Pico's INFO_UF2.TXT marker.

    Returns the Path to the drive root (e.g. E:\\), or None if no drive
    was found YET (call again to keep watching).
    """
    for letter in SCAN_LETTERS:
        drive = Path(f"{letter}:/")
        try:
            if not drive.exists():
                continue
            marker = drive / PICO_BOOT_MARKER
            if marker.exists():
                return drive
        except (PermissionError, OSError):
            # Drive not accessible / not ready, skip it.
            continue
    return None


# On Windows, msvcrt lets us poll for Ctrl+C even when the script is
# spawned as a subprocess (where SIGINT delivery is unreliable).
if sys.platform == "win32":
    try:
        import msvcrt
    except ImportError:
        msvcrt = None
else:
    msvcrt = None


def interruptible_sleep(seconds):
    """Sleep in 50ms chunks so Ctrl+C is honored on Windows.

    Windows does not interrupt time.sleep() with SIGINT reliably, so we
    poll KeyboardInterrupt by sleeping in short slices. On Windows we
    additionally poll msvcrt.kbhit() so that even subprocess-launched
    scripts (where SIGINT delivery is broken) can be aborted with Ctrl+C.
    """
    end = time.time() + seconds
    while time.time() < end:
        remaining = end - time.time()
        time.sleep(min(0.05, remaining))
        if msvcrt is not None and msvcrt.kbhit():
            ch = msvcrt.getch()
            # Ctrl+C = 0x03, ESC = 0x1b (also aborts)
            if ch in (b'\x03', b'\x1b'):
                raise KeyboardInterrupt()


def flash_once(uf2_path, flash_count):
    """Wait for a BOOT drive, flash the UF2, return when done.

    Returns True on success, False if user aborted.
    """
    print(f"[#{flash_count}] 等待 BOOTSEL 盘...")
    while True:
        drive = find_pico_boot_drive()
        if drive is not None:
            break
        interruptible_sleep(SCAN_INTERVAL_SEC)

    print(f"[#{flash_count}] 已发现 {drive}，准备烧录")
    interruptible_sleep(PRE_COPY_DELAY_SEC)

    target = drive / uf2_path.name
    try:
        shutil.copy(uf2_path, target)
    except OSError as e:
        print(f"[#{flash_count}] ERROR: copy failed: {e}")
        # Wait a bit so we don't immediately retry on the same drive.
        interruptible_sleep(2.0)
        return True  # Continue loop despite error

    print(f"[#{flash_count}] 烧录完成")
    interruptible_sleep(COOLDOWN_SEC)
    return True


def main():
    uf2_arg = sys.argv[1] if len(sys.argv) > 1 else "build/pico.uf2"
    uf2_path = Path(uf2_arg).resolve()

    print("Pico UF2 自动烧录已启动")

    if not uf2_path.exists():
        print(f"ERROR: UF2 file not found: {uf2_path}")
        sys.exit(1)

    flash_count = 0
    uf2_signature_now = uf2_signature(uf2_path)
    print("等待 UF2 更新...")
    try:
        while True:
            # 不改变现有烧录流程：只有本地 UF2 更新后，才自动请求 Pico 进入 BOOTSEL。
            uf2_signature_now = wait_for_uf2_change(uf2_path, uf2_signature_now)
            trigger_bootsel_by_1200()

            flash_count += 1
            flash_once(uf2_path, flash_count)
    except KeyboardInterrupt:
        print(f"已退出，共烧录 {flash_count - 1} 次")
        sys.exit(0)


if __name__ == "__main__":
    main()

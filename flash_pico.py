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
    print()
    print(f"[#{flash_count}] Scanning drives for Pico BOOT ({PICO_BOOT_MARKER})...")
    next_heartbeat = time.time() + 5.0  # first heartbeat 5s after start
    while True:
        drive = find_pico_boot_drive()
        if drive is not None:
            break
        # Heartbeat every 5s so user knows we're alive.
        if time.time() >= next_heartbeat:
            print(f"  ... waiting (elapsed {int(time.time())}s)")
            next_heartbeat = time.time() + 5.0
        interruptible_sleep(SCAN_INTERVAL_SEC)

    print(f"[#{flash_count}] Found BOOT drive: {drive}")
    print(f"        Marker:    {drive / PICO_BOOT_MARKER}")
    print(f"        Waiting {PRE_COPY_DELAY_SEC:.0f}s for drive to stabilize...")
    interruptible_sleep(PRE_COPY_DELAY_SEC)

    target = drive / uf2_path.name
    print(f"[#{flash_count}] Copying: {uf2_path}")
    print(f"             to: {target}")
    try:
        shutil.copy(uf2_path, target)
    except OSError as e:
        print(f"[#{flash_count}] ERROR: copy failed: {e}")
        # Wait a bit so we don't immediately retry on the same drive.
        interruptible_sleep(2.0)
        return True  # Continue loop despite error

    print(f"[#{flash_count}] Done. Pico will reset and boot.")
    print(f"[#{flash_count}] Cooling down {COOLDOWN_SEC:.0f}s before next scan...")
    interruptible_sleep(COOLDOWN_SEC)
    return True


def main():
    uf2_arg = sys.argv[1] if len(sys.argv) > 1 else "build/pico.uf2"
    uf2_path = Path(uf2_arg).resolve()

    print("=" * 60)
    print(" Pico UF2 Flasher (loop mode)")
    print("=" * 60)

    if not uf2_path.exists():
        print(f"ERROR: UF2 file not found: {uf2_path}")
        sys.exit(1)

    print(f"UF2 file: {uf2_path} ({uf2_path.stat().st_size:,} bytes)")
    print(f"Scan letters: {SCAN_LETTERS}")
    print(f"Press Ctrl+C to abort.")
    print()

    flash_count = 0
    try:
        while True:
            flash_count += 1
            flash_once(uf2_path, flash_count)
    except KeyboardInterrupt:
        print()
        print("=" * 60)
        print(f" Aborted. Total flashes: {flash_count - 1}")
        print("=" * 60)
        sys.exit(0)


if __name__ == "__main__":
    main()
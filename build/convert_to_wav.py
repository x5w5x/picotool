"""
Convert MP3 -> WAV for RP2040 + MAX98357A I2S playback.

Why 48 kHz? 240 MHz / 48 kHz = 5000 (pure integer), so the PIO clkdiv
becomes a clean integer without fractional jitter, eliminating the periodic
BCLK dithering that causes audible noise at non-integer ratios (e.g. 44100).

Usage:
    python convert_to_wav.py
"""
import subprocess
from pathlib import Path

# === Audio target (must match audio_i2s.c PIO program) ===
TARGET_SAMPLE_RATE  = 48000   # 48 kHz gives 240 MHz / 5000 = exact integer
TARGET_CHANNELS     = 1       # mono (downmix L+R to single channel)
TARGET_SAMPLE_FMT   = "s16"   # 16-bit signed PCM little-endian (RIFF/WAVE standard)

INPUT_DIR  = Path(r"E:\pico\pico\build\music")
OUTPUT_DIR = INPUT_DIR / "wav_48k_mono"
OUTPUT_DIR.mkdir(exist_ok=True)

FFMPEG = r"D:\ffmpeg\ffmpeg-7.0.2-full_build\bin\ffmpeg.exe"


def convert_one(mp3_path: Path) -> Path:
    wav_path = OUTPUT_DIR / (mp3_path.stem + ".wav")
    cmd = [
        FFMPEG, "-y", "-loglevel", "error",
        "-i", str(mp3_path),
        "-ar", str(TARGET_SAMPLE_RATE),
        "-ac", str(TARGET_CHANNELS),
        "-sample_fmt", TARGET_SAMPLE_FMT,
        str(wav_path),
    ]
    subprocess.run(cmd, check=True)
    return wav_path


def main() -> None:
    mp3_files = sorted(INPUT_DIR.glob("*.mp3"))
    if not mp3_files:
        print(f"No MP3 files found in {INPUT_DIR}")
        return
    print(f"Converting {len(mp3_files)} MP3 files at {TARGET_SAMPLE_RATE} Hz "
          f"{TARGET_SAMPLE_FMT} {TARGET_CHANNELS}-ch -> {OUTPUT_DIR}")
    for mp3 in mp3_files:
        wav = convert_one(mp3)
        print(f"  {mp3.name} -> {wav.name}  ({wav.stat().st_size // 1024} KB)")
    print("Done.")


if __name__ == "__main__":
    main()

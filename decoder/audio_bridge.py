#!/usr/bin/env python3
"""
audio_bridge.py — FT8/CB audio bridge
Reads audio from RTL-SDR or KiwiSDR and pipes to ft8cb_decode.
Writes decoded JSON lines to /tmp/ft8cb/messages.jsonl.
"""
import os
import subprocess
import json
import time
import sys
import signal
from pathlib import Path

SOURCE      = os.environ.get('SOURCE', 'rtlsdr')
FREQUENCY   = int(os.environ.get('FREQUENCY', 27265000))
GAIN        = os.environ.get('RTLSDR_GAIN', '40')
DEVICE      = os.environ.get('RTLSDR_DEVICE', '0')
PPM         = os.environ.get('RTLSDR_PPM', '0')
KIWI_HOST   = os.environ.get('KIWISDR_HOST', '')
KIWI_PORT   = os.environ.get('KIWISDR_PORT', '8073')
OUTPUT_DIR  = Path('/tmp/ft8cb')
OUTPUT_DIR.mkdir(exist_ok=True)
OUTPUT_FILE = OUTPUT_DIR / 'messages.jsonl'

# Signal handler for graceful shutdown
running = True

def handle_signal(signum, frame):
    global running
    print(f"[audio_bridge] Got signal {signum}, shutting down...", flush=True)
    running = False

signal.signal(signal.SIGTERM, handle_signal)
signal.signal(signal.SIGINT, handle_signal)


def process_output(decode_proc):
    """Read decoded JSON lines from ft8cb_decode and write to output file."""
    with open(OUTPUT_FILE, 'a') as f:
        for line in decode_proc.stdout:
            if not running:
                break
            line = line.strip()
            if line:
                # Validate JSON
                try:
                    msg = json.loads(line)
                    print(f"[decoded] {line}", flush=True)
                except json.JSONDecodeError:
                    print(f"[decoded] (raw) {line}", flush=True)
                f.write(line + '\n')
                f.flush()


def run_rtlsdr():
    """Launch rtl_fm and pipe to ft8cb_decode."""
    rtl_cmd = [
        'rtl_fm',
        '-f', str(FREQUENCY),
        '-M', 'usb',
        '-s', '12000',
        '-g', GAIN,
        '-p', PPM,
        '-d', DEVICE,
        '-'
    ]
    decode_cmd = ['ft8cb_decode']

    print(f"[audio_bridge] RTL-SDR source: {FREQUENCY} Hz, gain={GAIN}, device={DEVICE}", flush=True)

    rtl_proc = subprocess.Popen(
        rtl_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    decode_proc = subprocess.Popen(
        decode_cmd,
        stdin=rtl_proc.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    rtl_proc.stdout.close()

    process_output(decode_proc)

    rtl_proc.terminate()
    decode_proc.terminate()
    rtl_proc.wait()
    decode_proc.wait()


def run_kiwisdr():
    """Launch kiwirecorder and pipe to ft8cb_decode."""
    freq_khz = FREQUENCY / 1000.0

    # Try kiwiclient module first, fall back to kiwirecorder command
    kiwi_cmd = [
        'python3', '-m', 'kiwiclient.kiwirecorder',
        '-s', KIWI_HOST,
        '-p', KIWI_PORT,
        '-f', str(freq_khz),
        '-m', 'usb',
        '--s-meter=0',
        '--rate=12000',
        '-T', '15',   # 15 second recording
    ]
    decode_cmd = ['ft8cb_decode']

    print(f"[audio_bridge] KiwiSDR source: {KIWI_HOST}:{KIWI_PORT} @ {freq_khz} kHz", flush=True)

    kiwi_proc = subprocess.Popen(
        kiwi_cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    decode_proc = subprocess.Popen(
        decode_cmd,
        stdin=kiwi_proc.stdout,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    kiwi_proc.stdout.close()

    process_output(decode_proc)

    kiwi_proc.terminate()
    decode_proc.terminate()
    kiwi_proc.wait()
    decode_proc.wait()


if __name__ == '__main__':
    print(f"[audio_bridge] Starting, source={SOURCE}, freq={FREQUENCY} Hz", flush=True)

    while running:
        try:
            if SOURCE == 'rtlsdr':
                run_rtlsdr()
            elif SOURCE == 'kiwisdr':
                run_kiwisdr()
            else:
                print(f"[audio_bridge] Unknown SOURCE: {SOURCE}. Set SOURCE=rtlsdr or SOURCE=kiwisdr", flush=True)
                time.sleep(10)
        except Exception as e:
            print(f"[audio_bridge] Error: {e}, retrying in 5s...", flush=True)
            time.sleep(5)

    print("[audio_bridge] Stopped.", flush=True)

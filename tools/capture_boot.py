"""Reset a USB-Serial/JTAG ESP32 and save a bounded boot-log capture.

Bytes are streamed to --output as they arrive, so an interrupted long soak
still leaves usable evidence instead of losing the whole run.
"""
import argparse
from pathlib import Path
import sys
import time
import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--seconds', type=float, default=18)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--script', default='',
                    help='comma-separated SECONDS:COMMAND test inputs, e.g. "20:wave,26:jump"')
parser.add_argument('--cycle', default='',
                    help='comma-separated commands to repeat, e.g. "wave,jump,sleep,idle"')
parser.add_argument('--cycle-start', type=float, default=30)
parser.add_argument('--cycle-period', type=float, default=8)
args = parser.parse_args()

# Build one timed stimulus schedule so a soak is reproducible run to run.
schedule = []
for item in filter(None, (s.strip() for s in args.script.split(','))):
    when, _, command = item.partition(':')
    schedule.append((float(when), command.strip()))
if args.cycle:
    commands = [c.strip() for c in args.cycle.split(',') if c.strip()]
    count = max(0, int((args.seconds - args.cycle_start) // args.cycle_period))
    for i in range(count):
        schedule.append((args.cycle_start + i * args.cycle_period, commands[i % len(commands)]))
schedule.sort(key=lambda pair: pair[0])

port = serial.Serial()
port.port, port.baudrate, port.timeout = args.port, 115200, 0.25
port.dtr = False
port.rts = False
port.open()
total = 0
started = time.monotonic()
# Wall-clock backstop: monotonic can stall across host sleep, which would
# otherwise leave a "600 s" capture running indefinitely and holding the port.
wall_deadline = time.time() + args.seconds + 30
next_report = started + 30
with args.output.open('wb') as sink:
    try:
        port.reset_input_buffer()
        HardReset(port, uses_usb=True)()
        deadline = started + args.seconds
        pending = list(schedule)
        while time.monotonic() < deadline and time.time() < wall_deadline:
            data = port.read(max(1, port.in_waiting))
            if data:
                sink.write(data)
                sink.flush()
                total += len(data)
            now = time.monotonic()
            while pending and now - started >= pending[0][0]:
                when, command = pending.pop(0)
                port.write((command + '\n').encode())
                port.flush()
                print(f'[capture] {when:.0f}s -> {command}', file=sys.stderr, flush=True)
            if now >= next_report:
                print(f'[capture] {now - started:.0f}s elapsed, {total} bytes',
                      file=sys.stderr, flush=True)
                next_report = now + 30
    finally:
        port.dtr = False
        port.rts = False
        port.close()
    # Ground truth for the soak gate: the board only logs periodically, so the
    # last timestamp can lag the real capture window by a whole stats period.
    sink.write(f"\n[capture] duration={time.monotonic() - started:.1f}s bytes={total}\n".encode())
    sink.flush()
print(f'[capture] wrote {total} bytes to {args.output}', file=sys.stderr, flush=True)
if not total:
    raise SystemExit('No serial log captured')

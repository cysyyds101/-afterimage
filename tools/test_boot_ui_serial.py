"""Serial verification for the BOOT/state-machine fix and the Chinese UI build.

Drives the exact voice_client_begin_turn/end_turn entry points used by the
physical BOOT key (console hooks talk/endtalk), twice in a row, to prove a turn
can always start again after the previous one ends or errors. Then exercises
resync (同步中 status path) and the persistent-overlay dismissal regression.

Usage: C:/Espressif/tools/python/v6.1/venv/Scripts/python.exe tools/test_boot_ui_serial.py
Requires the voice bridge on 8780 and the board already connected (devices>=1).
"""
import re
import sys
import time

import serial

sys.stdout.reconfigure(encoding='utf-8')

PORT = 'COM7'
BAUD = 115200
OVERLAY_URL = 'http://10.1.41.11:8780/overlay/latest.jpg'

ser = serial.Serial(PORT, BAUD, timeout=0.2)
log = []
failed = False


def pump(seconds):
    end = time.time() + seconds
    buf = b''
    while time.time() < end:
        data = ser.read(4096)
        if not data:
            continue
        buf += data
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            text = line.decode('utf-8', 'replace').rstrip()
            log.append(text)
            print(text, flush=True)


def wait_for(pattern, timeout):
    rx = re.compile(pattern)
    start = len(log)
    end = time.time() + timeout
    while time.time() < end:
        pump(0.3)
        if any(rx.search(line) for line in log[start:]):
            return True
    return False


def send(cmd):
    print(f'>>> {cmd}', flush=True)
    ser.write((cmd + '\n').encode())


def check(name, ok):
    global failed
    print(f'[{"PASS" if ok else "FAIL"}] {name}', flush=True)
    if not ok:
        failed = True


def talk_cycle(index):
    started = False
    for _ in range(8):
        mark = len(log)
        send('talk')
        pump(1.0)
        if any('BOOT turn started' in line for line in log[mark:]):
            started = True
            break
        if any('rejected' in line for line in log[mark:]):
            pump(2.0)  # previous turn still draining; retry
    check(f'cycle {index}: turn started (BOOT hold path)', started)
    if not started:
        return
    pump(1.5)  # record ambient silence
    send('endtalk')
    check(f'cycle {index}: recording sent (BOOT release path)',
          wait_for(r'BOOT turn ended; recording sent', 5))
    # Silence usually fails ASR -> ERROR; a hallucinated transcript would play
    # a reply -> READY. Either terminal state proves the turn did not stick.
    check(f'cycle {index}: state reset to standby/error (no stuck turn)',
          wait_for(r'Voice state (2|6):', 30))


print('=== cycle 1 ===', flush=True)
talk_cycle(1)
print('=== cycle 2 (repeatability: the reported BOOT failure) ===', flush=True)
talk_cycle(2)

print('=== resync (同步中 path) ===', flush=True)
send('resync')
check('character bundle applied', wait_for(r'Apply bundle: ok', 30))

print('=== persistent overlay regression ===', flush=True)
send(f'overlay {OVERLAY_URL} 0')
check('overlay shown (persistent)', wait_for(r'Overlay shown \(persistent\)', 20))
pump(3)
send('listen')
check('overlay dismissed on new turn', wait_for(r'Overlay dismissed \(boot\)', 5))
check('overlay image released', wait_for(r'Overlay image released', 5))

crash = [line for line in log if re.search(
    r'Guru Meditation|panic|abort\(\)|Task watchdog|assert failed|Stack canary', line, re.I)]
check('no crash/watchdog during the whole run', not crash)
for line in crash:
    print('CRASH LINE:', line, flush=True)

ser.close()
print('RESULT:', 'FAIL' if failed else 'ALL PASS', flush=True)
sys.exit(1 if failed else 0)
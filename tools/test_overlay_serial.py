"""Reproducible hardware test for the display-overlay path (no mic/ASR needed).

Opens COM7, waits for boot, sends the console command
`overlay <url> [ms]`, then prints serial logs until the overlay countdown
expires so you can confirm: download -> decode -> shown -> dismissed/resume.

Usage: python test_overlay_serial.py [url] [ms]
"""
import sys
import time
import serial

url = sys.argv[1] if len(sys.argv) > 1 else 'http://10.1.41.11:8780/overlay/latest.jpg'
ms = int(sys.argv[2]) if len(sys.argv) > 2 else 8000

s = serial.Serial('COM7', 115200, timeout=1)
start = time.time()
sent = False
deadline = start + 4 + ms / 1000 + 8
while time.time() < deadline:
    if not sent and time.time() - start >= 4:
        s.write(f'overlay {url} {ms}\n'.encode())
        s.flush()
        sent = True
        print(f'>>> sent: overlay {url} {ms}', flush=True)
    line = s.readline()
    if line:
        print(line.decode('utf-8', 'replace').rstrip(), flush=True)
s.close()
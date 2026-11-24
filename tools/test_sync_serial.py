import sys, time, serial

PORT = "COM7"
BAUD = 115200

def main():
    s = serial.Serial(PORT, BAUD, timeout=0.2)
    time.sleep(0.3)
    s.reset_input_buffer()
    start = time.time()
    sent = False
    deadline = start + 90
    print(">>> listening; will send 'resync' at t=8s", flush=True)
    while time.time() < deadline:
        if not sent and time.time() - start >= 8:
            s.write(b"resync\n")
            s.flush()
            sent = True
            print(">>> sent: resync", flush=True)
        line = s.readline()
        if line:
            text = line.decode("utf-8", "replace").rstrip()
            if any(k in text for k in ("net_sync", "whale_maid", "Bundle", "Character", "Persisted", "Wi-Fi connected", "Apply")):
                print(f"[{time.time()-start:6.2f}s] {text}", flush=True)
            if "Persisted" in text:
                time.sleep(1.0)
                break
    s.close()
    print(">>> done", flush=True)

if __name__ == "__main__":
    main()
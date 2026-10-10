import socket, time, sys

IP = "192.168.1.4"
PORT = 23

def send_and_wait(s, cmd, timeout=15.0):
    print(f">>> {cmd}")
    s.sendall(cmd.encode("ascii") + b"\n")
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            out += chunk
            if b"sh-3.00#" in out:
                break
        except socket.timeout:
            continue
    text = out.decode("utf-8", errors="replace")
    print(text)
    return text

s = socket.socket()
s.settimeout(2.0)
s.connect((IP, PORT))

# RFC 854 negotiation
end = time.time() + 2.0
while time.time() < end:
    try:
        c = s.recv(1024)
        if not c: break
        out = b""
        i = 0
        while i < len(c):
            if c[i] == 0xff and i + 1 < len(c):
                op = c[i+1]
                if op in (0xfb, 0xfc):
                    if i + 2 < len(c): out += b"\xff\xfe" + bytes([c[i+2]])
                elif op in (0xfd, 0xfe):
                    if i + 2 < len(c): out += b"\xff\xfc" + bytes([c[i+2]])
                i += 3
            else:
                i += 1
        if out: s.sendall(out)
        if b"#" in c: break
    except (socket.timeout, socket.error):
        break

print("=== Starting Gate 2 Sequence in Single Shell ===")
send_and_wait(s, "rmmod kjump 2>/dev/null || true", timeout=3.0)
send_and_wait(s, "insmod /share/kjump.ko", timeout=5.0)
send_and_wait(s, "echo load > /proc/kjump", timeout=20.0)
status = send_and_wait(s, "cat /proc/kjump", timeout=3.0)
dmesg = send_and_wait(s, "dmesg | tail -n 25", timeout=3.0)

s.sendall(b"exit\n")
s.close()

if "Status: LOADED" in status and "WDT: ARMED" in status:
    print("[✓] GATE 2 PASS: Staged and verified in physical DDR2 DRAM!")
    sys.exit(0)
else:
    print("[!] GATE 2 FAIL: Module not loaded or WDT not armed!")
    sys.exit(1)

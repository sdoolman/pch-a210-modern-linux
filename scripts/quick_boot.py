#!/usr/bin/env python3
"""
scripts/quick_boot.py
All-In-One 1-Click Bringup Orchestrator for Popcorn Hour A-210.
Executes Gate 1 Quiescing, Gate 2 DRAM Staging, Fires In-RAM Jump,
holds the Watchdog Keepalive, and drops into an interactive root shell.
"""

import sys
import time
import socket
import argparse
import subprocess

def log(msg):
    print(f"[*] {msg}", flush=True)

def error(msg):
    print(f"[!] ERROR: {msg}", file=sys.stderr, flush=True)

def run_cmd(ip, cmd, port=23, timeout=5.0):
    s = socket.socket()
    s.settimeout(2.0)
    try:
        s.connect((ip, port))
    except Exception as e:
        return None, f"Connect error: {e}"

    # Simple Telnet IAC negotiation
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

    s.sendall(cmd.encode("ascii") + b"\n")
    time.sleep(1.0)
    out = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            out += chunk
            if b"#" in out: break
        except socket.timeout:
            break

    try: s.sendall(b"exit\n")
    except Exception: pass
    s.close()
    return out.decode("latin-1", errors="replace"), None

def main():
    parser = argparse.ArgumentParser(description="Popcorn Hour A-210 Modern Linux 4.9 Quick Boot")
    parser.add_argument("--ip", default="192.168.1.4", help="Target IP address (default: 192.168.1.4)")
    args = parser.parse_args()

    ip = args.ip
    print("=" * 70)
    print(" 🍿 Popcorn Hour A-210 Modern Linux 4.9 LTS 1-Click Bootloader")
    print("=" * 70)

    # 1. Gate 1: Quiesce Board
    log(f"Step 1: Quiescing stock board at {ip}...")
    res, err = run_cmd(ip, "killall -9 syb_framework gaya nmbd smbd upnpd transmission-daemon 2>/dev/null; sync; echo 3 > /proc/sys/vm/drop_caches; free")
    if err:
        error(f"Failed to connect to stock system: {err}")
        sys.exit(1)
    log("Stock processes quiesced, caches dropped.")

    # 2. Gate 2: Insert Module & Stage Kernel
    log("Step 2: Staging Linux 4.9 into physical DDR2 DRAM via kjump.ko...")
    res, _ = run_cmd(ip, "rmmod kjump 2>/dev/null; insmod /share/kjump.ko && echo load > /proc/kjump && cat /proc/kjump", timeout=15.0)
    if not res or "Status: LOADED" not in res:
        error(f"kjump staging failed or module not loaded!\n{res}")
        sys.exit(1)
    log("Kernel staged and entry opcodes verified in physical DDR2 DRAM!")

    # 3. Fire Direct Jump
    log("Step 3: Arming 120s watchdog lease and executing in-RAM jump...")
    run_cmd(ip, "echo 1 > /proc/kjump", timeout=2.0)

    # 4. Acquire Live Linux 4.9 Shell
    log("Step 4: Waiting for Modern Linux 4.9 to boot and initialize Ethernet...")
    start_time = time.time()
    connected = False
    for attempt in range(40):
        time.sleep(1.0)
        s = socket.socket()
        s.settimeout(1.0)
        try:
            s.connect((ip, 2323))
            connected = True
            s.close()
            break
        except Exception:
            pass
        print(".", end="", flush=True)

    print()
    elapsed = time.time() - start_time
    if not connected:
        error("Timed out waiting for Linux 4.9 shell on port 2323.")
        sys.exit(1)

    log(f"SUCCESS! Modern Linux 4.9 root shell active in {elapsed:.1f}s!")

    # 5. Lock keepalive and print banner
    res, _ = run_cmd(ip, "touch /tmp/keepalive; uname -a; cat /proc/cpuinfo | grep 'model name\\|cpu model'; free -m", port=2323)
    if res:
        print("\n--- Live Silicon Environment ---")
        print(res.strip())
        print("--------------------------------\n")

    log("Watchdog keepalive armed (/tmp/keepalive). Launching interactive shell...")
    # Interactive telnet connection
    try:
        subprocess.run(["python", "scripts/board_shell.py"])
    except Exception:
        log(f"Connect anytime via: telnet {ip} 2323")

if __name__ == "__main__":
    main()

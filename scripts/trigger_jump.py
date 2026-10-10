#!/usr/bin/env python3
"""
scripts/trigger_direct_jump_and_acquire.py
Triggers direct in-RAM jump to Linux 4.9.0 (echo 1 > /proc/kjump)
and acquires interactive shell on port 2323, 23, 9999, or via reverse shell on port 7777.
"""

import socket
import time
import sys
import os
import threading

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from pch_telnet import run_cmd
def execute_cmd(cmd, wait=2.0, **kwargs):
    return run_cmd(cmd, wait=wait)

IP = '192.168.1.4'
acquired = threading.Event()
acquired_output = []

def log(msg):
    ts = time.strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)

def is_port_open(port, timeout=0.5):
    try:
        s = socket.socket()
        s.settimeout(timeout)
        res = s.connect_ex((IP, port))
        s.close()
        return res == 0
    except:
        return False

def reverse_shell_listener():
    """Listens on 0.0.0.0:7777 for proactive incoming shell connection from /init"""
    try:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind(('0.0.0.0', 7777))
        server.listen(1)
        server.settimeout(160.0)
        log("[LISTENER] Reverse shell listener active on 0.0.0.0:7777")
        client, addr = server.accept()
        log(f"[REVERSE CONNECT] Connection received from {addr}!")

        client.settimeout(10.0)
        # Touch keepalive
        client.sendall(b"touch /tmp/keepalive\n")
        time.sleep(0.3)
        cmd = b"touch /tmp/keepalive; uname -a; ip a || ifconfig; cat /proc/cpuinfo; echo ===ACQUISITION_SUCCESS===\n"
        client.sendall(cmd)

        raw = ""
        read_end = time.time() + 8.0
        while time.time() < read_end:
            try:
                chunk = client.recv(4096).decode('latin1', errors='replace')
                if not chunk:
                    break
                raw += chunk
                if "===ACQUISITION_SUCCESS===" in raw:
                    break
            except socket.timeout:
                break
        client.close()
        server.close()

        if "4.9" in raw:
            log("******************************************************************")
            log("*** LINUX 4.9.0 ROOT SHELL ACQUIRED VIA REVERSE CONNECTION!    ***")
            log("******************************************************************")
            log(f"Shell Telemetry:\n{raw}")
            acquired_output.append(raw)
            acquired.set()
        else:
            log(f"[REVERSE CONNECT] Received connection, but kernel is NOT 4.9: {raw.strip()[:60]}")
    except Exception as e:
        log(f"[LISTENER] Listener exited: {e}")

def main():
    log("==================================================================")
    log("=== SPRINT 4: TRIGGERING DIRECT UNCOMPRESSED IN-RAM JUMP       ===")
    log("==================================================================")

    # 1. Final verification of /proc/kjump
    status = execute_cmd("cat /proc/kjump", wait=1.5, timeout=5.0)
    log(f"Pre-jump check:\n{status}")

    if "Status: LOADED" not in status or "WDT: ARMED" not in status:
        log("FATAL: Gate check failed! Aborting jump.")
        sys.exit(1)

    # 2. Start reverse shell listener thread
    listener_th = threading.Thread(target=reverse_shell_listener, daemon=True)
    listener_th.start()

    # 3. Fire the jump
    log("Firing jump: echo 1 > /proc/kjump ...")
    try:
        execute_cmd("echo 1 > /proc/kjump", wait=1.0, timeout=3.0)
    except Exception as e:
        log(f"Jump command sent (connection terminated as expected: {e})")

    log("Monitoring ports 2323, 23, 9999, 8080 and reverse port 7777 (160s window)...")
    jump_start = time.time()

    # Poll for 160s (watchdog lease is 180s)
    while time.time() - jump_start < 160:
        if acquired.is_set():
            return True

        elapsed = int(time.time() - jump_start)
        for target_port in [2323, 23, 9999, 8080]:
            if is_port_open(target_port, timeout=0.4):
                log(f"[PORT OPEN] Port {target_port} open after {elapsed}s! Connecting...")
                try:
                    s = socket.socket()
                    s.settimeout(5.0)
                    s.connect((IP, target_port))
                    time.sleep(0.3)

                    # CRITICAL FAILSAFE: HOLD LEASE
                    s.sendall(b"touch /tmp/keepalive\n")
                    time.sleep(0.2)

                    # Query kernel identity and network
                    cmd = b"touch /tmp/keepalive; uname -a; ip a || ifconfig; cat /proc/cpuinfo; echo ===ACQUISITION_SUCCESS===\n"
                    s.sendall(cmd)

                    raw = ""
                    read_end = time.time() + 6.0
                    while time.time() < read_end:
                        try:
                            chunk = s.recv(4096).decode('latin1', errors='replace')
                            if not chunk:
                                break
                            raw += chunk
                            if "===ACQUISITION_SUCCESS===" in raw:
                                break
                        except socket.timeout:
                            break
                    s.close()

                    if "4.9" in raw:
                        log("******************************************************************")
                        log(f"*** LINUX 4.9.0 ROOT SHELL ACQUIRED ON PORT {target_port}!      ***")
                        log("******************************************************************")
                        log(f"Shell Telemetry:\n{raw}")
                        acquired.set()
                        return True
                    else:
                        log(f"Port {target_port} connected, but kernel is NOT 4.9 (got: {raw.strip()[:60]}). Continuing poll...")
                except Exception as e:
                    log(f"Port {target_port} connection error: {e}")

        if elapsed % 5 == 0:
            log(f"Awaiting Linux 4.9.0 boot... ({elapsed}s elapsed)")
        time.sleep(1)

    if acquired.is_set():
        return True

    log("[-] Did not connect within 160s.")
    return False

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)

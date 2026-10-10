#!/usr/bin/env python3
"""
scripts/quiesce_board.py
Mandatory Pre-Jump Board Quiescing & Memory Reclamation Specialist Script.
Terminates unnecessary stock daemons and memory hogs, flushes caches,
and asserts minimum RAM headroom before any staging or jump is permitted.
"""

import sys
import os
import time
import re

SCRIPTS_DIR = os.path.dirname(os.path.abspath(__file__))
if SCRIPTS_DIR not in sys.path:
    sys.path.insert(0, SCRIPTS_DIR)

from pch_telnet import run_cmd
def execute_cmd(cmd, wait=1.5, **kwargs):
    return run_cmd(cmd, wait=wait)

TARGET_PROCESSES = [
    "syb_framework",
    "gaya",
    "nmbd",
    "smbd",
    "upnpd",
    "appcontrol",
    "transmission-daemon",
]

MIN_FREE_RAM_KB = 70000  # Require at least 70MB clean free RAM

def quiesce_board():
    print("[*] Initiating board quiescing sequence on 192.168.1.4...")

    # 1. Terminate background memory hogs and stock media frameworks
    kill_cmd = f"killall -9 {' '.join(TARGET_PROCESSES)} 2>/dev/null || true"
    print(f"[*] Terminating unneeded background services: {', '.join(TARGET_PROCESSES)}")
    execute_cmd(kill_cmd, wait=1.0)

    # Clean up spawned loops and zombie child shells by PID
    ps = execute_cmd("ps", wait=1.0)
    pids = [l.split()[0] for l in ps.splitlines() if any(k in l for k in ('start_app', 'busybox_stoc', 'sleep 1', 'syb_framework', 'webservices.cgi', 'start.cgi')) and l.split()[0].isdigit() and int(l.split()[0]) > 500]
    if pids:
        print(f"[*] Cleaning up {len(pids)} lingering child processes/loops...")
        for i in range(0, len(pids), 20):
            execute_cmd("kill -9 " + " ".join(pids[i:i+20]), wait=0.5)

    # 2. Sync filesystems and drop page/dentry/inode caches
    print("[*] Flushing filesystem buffers and dropping kernel caches (echo 3 > /proc/sys/vm/drop_caches)...")
    execute_cmd("sync; echo 3 > /proc/sys/vm/drop_caches", wait=1.0)

    # 3. Read and parse memory status
    print("[*] Querying /proc/meminfo...")
    meminfo = ""
    for attempt in range(3):
        meminfo = execute_cmd("cat /proc/meminfo", wait=1.5, timeout=10.0)
        if meminfo and "MemFree:" in meminfo:
            break
        time.sleep(1.0)

    mem_total_kb = 0
    mem_free_kb = 0
    buffers_kb = 0
    cached_kb = 0

    for line in meminfo.splitlines():
        if "MemTotal:" in line:
            m = re.search(r"(\d+)", line)
            if m: mem_total_kb = int(m.group(1))
        elif "MemFree:" in line:
            m = re.search(r"(\d+)", line)
            if m: mem_free_kb = int(m.group(1))
        elif "Buffers:" in line:
            m = re.search(r"(\d+)", line)
            if m: buffers_kb = int(m.group(1))
        elif "Cached:" in line:
            m = re.search(r"(\d+)", line)
            if m: cached_kb = int(m.group(1))

    available_kb = mem_free_kb + buffers_kb + cached_kb
    print(f"[+] Memory Status:")
    print(f"    - MemTotal : {mem_total_kb / 1024:.1f} MB ({mem_total_kb} kB)")
    print(f"    - MemFree  : {mem_free_kb / 1024:.1f} MB ({mem_free_kb} kB)")
    print(f"    - Available: {available_kb / 1024:.1f} MB ({available_kb} kB)")

    if mem_free_kb < MIN_FREE_RAM_KB:
        print(f"[!] FATAL: Insufficient free memory! Required >= {MIN_FREE_RAM_KB/1024:.1f} MB, got {mem_free_kb/1024:.1f} MB.")
        return False

    # 4. Verify running process list
    print("[*] Verifying remaining active processes...")
    ps_output = execute_cmd("ps", wait=1.0)
    surviving = []
    for p in TARGET_PROCESSES:
        if p in ps_output:
            surviving.append(p)

    if surviving:
        print(f"[!] Warning: Some target processes still present in ps: {surviving}")
        print("[*] Re-issuing forceful killall -9...")
        execute_cmd(f"killall -9 {' '.join(surviving)} 2>/dev/null || true", wait=1.0)

    print(f"[✓] QUIESCE PASS: Board successfully quiesced. {mem_free_kb/1024:.1f} MB RAM free and ready for staging.")
    return True

if __name__ == "__main__":
    success = quiesce_board()
    sys.exit(0 if success else 1)

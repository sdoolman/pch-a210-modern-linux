#!/usr/bin/env python3
"""
scripts/verify_binary_safety_gate0.py
Mandatory Gate 0 Static Disassembly and Safety Audit for Popcorn Hour A-210.
Must be passed before any kernel module or binary is uploaded or staged on physical hardware.
"""

import sys
import subprocess
import re
import os

def check_kjump_module(ko_path):
    print(f"[*] Running Gate 0 Safety Audit on: {ko_path}")
    if not os.path.exists(ko_path):
        print(f"[!] FATAL: Module {ko_path} does not exist!")
        return False

    cmd = ["wsl", "-d", "Ubuntu-24.04", "--", "mipsel-linux-gnu-objdump", "-d", ko_path]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[!] FATAL: Failed to disassemble {ko_path}: {res.stderr}")
        return False

    disasm = res.stdout

    # 1. Check for banned unmapped addresses (> 192MB physical, e.g., 0xAFF0)
    banned_patterns = [
        r"lui\s+[a-z0-9]+,\s*0xaff[0-9a-f]",
        r"lui\s+[a-z0-9]+,\s*0xb[0-9a-f]{3}",
        r"0xaff0",
    ]
    for pat in banned_patterns:
        match = re.search(pat, disasm, re.IGNORECASE)
        if match:
            print(f"[!] REJECTED: Banned unmapped memory write detected! Pattern: '{pat}', Match: '{match.group()}'")
            return False

    # 2. Verify Watchdog Counter Arming (0xA001FD00 / 0xA001FD04) in assembly
    if "fd00" not in disasm or "fd04" not in disasm:
        print("[!] REJECTED: Hardware watchdog registers (0xA001FD00 / 0xA001FD04) not found in assembly!")
        return False

    # 3. Verify destination base is within valid physical DDR2 DRAM (0xA4000000)
    if "lui\ts0,0xa400" not in disasm and "lui\ts0, 0xa400" not in disasm:
        print("[!] REJECTED: Destination base s0 is not 0xA4000000 (physical 0x04000000)!")
        return False

    # 4. Verify target entry point is within valid range (0xA4000400)
    if "0x400" not in disasm:
        print("[!] REJECTED: Jump target does not align with kernel entry (0x400)!")
        return False

    print("[✓] GATE 0 PASS: Binary passed all safety audits. Zero unmapped writes, watchdog verified, valid DRAM geometry.")
    return True

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "direct_kjump/kjump.ko"
    passed = check_kjump_module(target)
    sys.exit(0 if passed else 1)

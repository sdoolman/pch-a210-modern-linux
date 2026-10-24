#!/usr/bin/env python3
"""
scripts/make_station_image.py
Generates Popcorn Hour A-210 loopback ext4 system image (pch_station.img)
and optional swap image (swap.img). These can be copied directly to the root
of any FAT32 or NTFS formatted USB drive without repartitioning.
"""

import os
import sys
import argparse
import subprocess

def create_ext4_image(out_file, size_mb):
    print(f"[*] Creating {size_mb} MB ext4 station image: {out_file}...")
    with open(out_file, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    cmd = ["wsl", "-d", "Ubuntu-24.04", "--", "mkfs.ext4", "-F", "-L", "PCH_STATION", out_file]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[!] Warning during mkfs.ext4: {res.stderr}")
    else:
        print(f"[✓] Formatted {out_file} as ext4 successfully.")

def create_swap_image(out_file, size_mb):
    print(f"[*] Creating {size_mb} MB swap image: {out_file}...")
    with open(out_file, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    cmd = ["wsl", "-d", "Ubuntu-24.04", "--", "mkswap", "-L", "PCH_SWAP", out_file]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[!] Warning during mkswap: {res.stderr}")
    else:
        print(f"[✓] Formatted {out_file} as swap successfully.")

def main():
    parser = argparse.ArgumentParser(description="Generate Popcorn Hour Station & Swap Loopback Images")
    parser.add_argument("--station", action="store_true", default=True, help="Create station ext4 image (default: True)")
    parser.add_argument("--station-size", type=int, default=32, help="Station image size in MB (default: 32 MB)")
    parser.add_argument("--station-out", default="pch_station.img", help="Station image filename (default: pch_station.img)")
    parser.add_argument("--swap", action="store_true", help="Also generate a swapfile image (swap.img)")
    parser.add_argument("--swap-size", type=int, default=64, help="Swap image size in MB (default: 64 MB)")
    parser.add_argument("--swap-out", default="swap.img", help="Swap image filename (default: swap.img)")
    args = parser.parse_args()

    if args.station:
        create_ext4_image(args.station_out, args.station_size)

    if args.swap:
        create_swap_image(args.swap_out, args.swap_size)

    print("\n[✓] SUCCESS: Target image(s) ready!")
    print("----------------------------------------------------------------------")
    print("HOW TO DEPLOY:")
    print("1. Copy 'pch_station.img' and 'swap.img' directly to the root of ANY")
    print("   USB flash drive (FAT32, ext3, or ext4).")
    print("2. Plug the USB flash drive into your Popcorn Hour A-210.")
    print("3. Boot Modern Linux 4.9 LTS.")
    print("4. /init will automatically:")
    print("   a) Pet the 60s hardware watchdog timer continuously.")
    print("   b) Mount your USB drive at /share.")
    print("   c) Activate swap: swapon /share/swap.img.")
    print("   d) Loop-mount /share/pch_station.img on /mnt/station.")
    print("   e) Execute switch_root /mnt/station /init into your persistent environment.")
    print("   f) If USB is absent, fail-safe fallback boots into initramfs rescue shell.")
    print("----------------------------------------------------------------------")

if __name__ == "__main__":
    main()

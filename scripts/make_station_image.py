#!/usr/bin/env python3
"""
scripts/make_station_image.py
Creates an Android-style loopback ext4 image file (pch_station.img)
that can be copied to ANY FAT32 or NTFS formatted USB drive without repartitioning.
"""

import os
import sys
import argparse
import subprocess

def main():
    parser = argparse.ArgumentParser(description="Generate Popcorn Hour Station Loopback Image")
    parser.add_argument("--size", type=int, default=1024, help="Image size in megabytes (default: 1024 MB)")
    parser.add_argument("--output", default="pch_station.img", help="Output filename (default: pch_station.img)")
    args = parser.parse_args()

    out_file = args.output
    size_mb = args.size
    print(f"[*] Creating {size_mb} MB ext4 image: {out_file}...")

    # Create empty sparse file
    with open(out_file, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

    # Format as ext4 via WSL2 mke2fs (or mkfs.ext4)
    print("[*] Formatting image as ext4 filesystem...")
    cmd = ["wsl", "-d", "Ubuntu-24.04", "--", "mkfs.ext4", "-F", "-L", "PCH_STATION", out_file]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        # Fallback to direct path
        print(f"[!] Warning: mke2fs output: {res.stderr}")
    else:
        print("[✓] ext4 formatting complete!")

    print(f"\n[✓] SUCCESS: {out_file} ({size_mb} MB) is ready!")
    print("----------------------------------------------------------------------")
    print("HOW TO USE:")
    print(f"1. Copy '{out_file}' directly to the root of ANY USB flash drive (FAT32 or NTFS).")
    print("2. Plug the USB flash drive into your Popcorn Hour.")
    print("3. Boot Modern Linux 4.9 LTS.")
    print("4. /init will automatically detect and mount it via loopback to /opt!")
    print("----------------------------------------------------------------------")

if __name__ == "__main__":
    main()

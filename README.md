# 🍿 Popcorn Hour A-210 Modern Linux 4.9 LTS Bringup (`pch-a210-modern-linux`)

> Run **Modern Linux 4.9.0 LTS** on your **Popcorn Hour A-210 (Sigma Designs SMP8643)** via in-RAM injection.  
> **100% Non-Destructive • Zero Flash Writes • Autonomous Clean Warm Reboot • Interactive Root Shell in 7 Seconds.**

[![Kernel CI](https://github.com/sdoolman/linux-tangox/actions/workflows/kernel-ci.yml/badge.svg)](https://github.com/sdoolman/linux-tangox/actions/workflows/kernel-ci.yml)
[![Toolkit CI](https://github.com/sdoolman/pch-a210-modern-linux/actions/workflows/build.yml/badge.svg)](https://github.com/sdoolman/pch-a210-modern-linux/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

---

## 🌟 Why this project?

The **Popcorn Hour A-210** (and its siblings A-200, C-200, C-300) is powered by the **Sigma Designs SMP8643** SoC—a capable **MIPS 74Kf core @ 661.5 MHz** with 192 MB of DDR2 system RAM and hardware media acceleration.

However, these units have remained stranded in time on ancient **Linux 2.6.22 (from 2007)**:
* 🚫 **No modern networking:** Missing modern crypto, IPv6, modern socket options, and contemporary network stacks.
* 🔒 **Ancient userspace:** Limited by obsolete libc libraries, making modern software incompatible.
* ⚠️ **High risk of bricking:** Attempting to flash experimental kernels to on-board NAND flash risks permanently soft-bricking the device.

**`pch-a210-modern-linux` bridges 17 years of Linux kernel history:**
It bootstraps **modern Linux 4.9.0 LTS entirely in DDR2 DRAM** using a custom loadable kernel module trampoline (`kjump.ko`), leaving your factory flash, Yamon bootloader, and Syabas firmware **completely untouched**.

```text
~ # uname -a
Linux (none) 4.9.0-g3f9893d24f-dirty #81 Fri Oct 9 23:46:46 IDT 2026 mips GNU/Linux

~ # ip a
2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast qlen 1000
    link/ether 00:06:dc:81:4f:a3 brd ff:ff:ff:ff:ff:ff
    inet 192.168.1.4/24 brd 192.168.1.255 scope global eth0
```

---

## 🍓 Turning your Popcorn Hour into a Micro Utility Station

Instead of just an obsolete video player, you can repurpose the SMP8643 as an always-on, low-power Linux server (like a Raspberry Pi):

| Feature | Description | Status |
| :--- | :--- | :---: |
| **🔑 Modern SSH (Port 22)** | **Dropbear SSH server** compiled from source with `musl libc`. Auto-generates host keys on boot with secure key login and passwordless root access. | **Supported** 🟢 |
| **🐚 True GNU Bash Shell** | Full **GNU Bash 5.2** shell replacing primitive BusyBox ash (tab completion, command history, syntax colors, `.bashrc`). | **Supported** 🟢 |
| **💾 Internal SATA Storage** | Native **SMP8643 Synopsys DWC SATA + SATA PHY + SCSI disk drivers**, auto-mounting internal 3.5" or 2.5" SATA hard drives and SSDs to `/mnt/sata`. | **Supported** 🟢 |
| **💿 Zero-Format Loopback Storage** | **Android-style loopback ext4 image (`pch_station.img`)**. Copy one file to any FAT32 or NTFS USB drive—no repartitioning or formatting needed! | **Supported** 🟢 |
| **🧰 Modern Core Suite** | Static **BusyBox 1.36.1**, **Curl 8.7.1**, and **Nano 7.2** compiled from source with `musl`—zero library dependencies. | **Supported** 🟢 |

### 💾 Zero-Format Storage: Use ANY USB Flash Drive (FAT32 or NTFS)
You do not need to format your USB drive to Linux `ext3` or `ext4`!
1. Plug your standard FAT32 or NTFS USB drive into your PC.
2. Generate a 1 GB or 2 GB image file:
   ```bash
   python scripts/make_station_image.py --size 1024 --output pch_station.img
   ```
3. Copy `pch_station.img` to your USB drive.
4. Plug it into the Popcorn Hour. On boot, `/init` automatically mounts the USB drive and attaches `pch_station.img` via loopback to `/opt`, providing a full native Linux ext4 filesystem with full permissions, symlinks, and persistence!

---

## 🚀 Quickstart: Boot Modern Linux in 3 Minutes

You do not need to open the case, solder serial headers, or touch NAND flash.

### Prerequisites
* Popcorn Hour A-210 connected to your home LAN (e.g. `192.168.1.4`).
* A USB flash drive formatted as FAT32 or EXT3 plugged into the Popcorn Hour (mounted at `/share`).
* A host PC running Python 3.8+ on the same network.

---

### Option 1: 1-Click Automated Boot (Fastest)

Clone the repository and run the automated boot orchestrator:

```bash
git clone https://github.com/sdoolman/pch-a210-modern-linux.git
cd pch-a210-modern-linux

# Copy kjump.ko and vmlinux.bin to your Popcorn Hour USB drive (via FTP port 21, user: nmt / pass: 1234)
# Then run:
python scripts/quick_boot.py --ip 192.168.1.4
```

**What the script does automatically:**
1. 🧹 Quiesces stock background services and drops caches (**Gate 1**).
2. 📦 Stages Linux 4.9 into physical DDR2 DRAM via `kjump.ko` and verifies physical memory canaries (**Gate 2**).
3. ⚡ Arms the hardware watchdog safety lease and executes the in-RAM jump.
4. ⏱️ Waits ~7 seconds, connects to the newly booted Linux 4.9 root shell on port 2323, locks the watchdog keepalive, and drops you into an interactive terminal!

---

### Option 2: Step-by-Step Manual Staging

If you prefer to inspect every step over Telnet:

1. **Connect to stock Telnet:**
   ```bash
   telnet 192.168.1.4
   ```

2. **Quiesce background services & free RAM:**
   ```bash
   killall -9 syb_framework gaya nmbd smbd upnpd transmission-daemon 2>/dev/null
   sync && echo 3 > /proc/sys/vm/drop_caches
   free  # Confirm MemFree >= 70 MB
   ```

3. **Insert the in-RAM trampoline loader:**
   ```bash
   insmod /share/kjump.ko
   ```

4. **Stage the uncompressed kernel payload:**
   ```bash
   echo load > /proc/kjump
   cat /proc/kjump
   # Must report: Status: LOADED and WDT: ARMED
   ```

5. **Fire the jump into Modern Linux 4.9:**
   ```bash
   echo 1 > /proc/kjump
   ```

6. **Connect to your new root shell (within 7 seconds):**
   ```bash
   telnet 192.168.1.4 2323
   ```
   *Immediately touch `/tmp/keepalive` to hold the session active:*
   ```bash
   touch /tmp/keepalive
   ```

---

## 🛡️ The 4-Gate Safety Framework (Zero Bricks Guarantee)

To guarantee that the physical board can never be bricked by wrong writes, illegal instructions, or hung resets, every deployment passes 4 mandatory safety gates:

| Gate | Name | Purpose | Tool |
| :---: | :--- | :--- | :--- |
| **Gate 0** | **Static Disassembly Audit** | Scans binaries for banned addresses (`> 192 MB`, `0xAFF00000`) and verifies watchdog arming in assembly prior to staging. | `scripts/gate0_binary_safety.py` |
| **Gate 1** | **Board Quiescing Gate** | Terminates stock media services and drops caches to guarantee $\ge 70\text{ MB}$ clean RAM headroom before allocating staging pages. | `scripts/gate1_quiesce_board.py` |
| **Gate 2** | **Silicon Canary Gate** | Verifies `0xDEADBEEF` canaries and compares physical DDR2 DRAM entry opcodes against the binary before handover. | `scripts/gate2_stage_verify.py` |
| **Gate 3** | **Reboot Cleanliness Gate** | Validates `_machine_restart` hook restoring Yamon crossbars before watchdog trip, eliminating "half-brick" lockups. | `scripts/gate3_reboot_audit.py` |

---

## 💡 How It Works (For the Curious & Silicon Hackers)

Porting a modern kernel to the Sigma SMP8643 presented several deep silicon challenges. Here are the core discoveries that made this bringup possible:

```text
+--------------------------------------------------------------------------+
|                     Sigma Designs SMP8643 Memory Map                    |
+------------------------------------+-------------------------------------+
| Physical 0x00000000 - 0x03FFFFFF   | DSP Audio/Video DRAM (64 MB)       |
|                                    | (CPU writes dropped by controller!) |
+------------------------------------+-------------------------------------+
| Physical 0x04000000 - 0x0FFFFFFF   | System DDR2 DRAM (192 MB)          |
|                                    | -> CPU Physical Offset: +64 MB      |
|                                    | -> GBUS Crossbar Offset: 0x80000000 |
+------------------------------------+-------------------------------------+
| Physical 0x00060000 - 0x00061FFF   | On-Chip LRAM (8 KB SRAM)            |
|                                    | (Survives warm watchdog resets!)    |
+------------------------------------+-------------------------------------+
```

### 1. The GBUS DMA Crossbar Remapping Breakthrough (`CONFIG_TANGOX_REMAP=y`)
* **The Trap:** On the SMP8643, CPU physical addresses (`0x04000000`) are translated to GBUS crossbar physical address `0x80000000` via `CPU_remap[2]`. GBUS bus masters—including the Aurora NB8800 Ethernet MAC DMA—access DDR2 DRAM at **`0x80000000`**, not `0x04000000`.
* **The Solution:** We enabled `CONFIG_TANGOX_REMAP=y`, initialized `tangox_remap[64]` from Yamon registers in `prom.c`, and mapped all DMA operations in `dma-coherence.h` to GBUS `0x80xxxxxx`. Network packets and DMA transfers now operate at full Gigabit speed.

### 2. Solving the "Half-Brick" Warm Reset Lockup
* **The Trap:** The SMP8643 hardware watchdog resets the MIPS CPU core, but **never resets the GBUS `CPU_remap[0..7]` (`0xA006F000`) crossbar registers**. When Linux 4.9 remapped the memory layout, a warm reboot caused Yamon bootloader to crash on invalid memory geometry, locking the board until a manual AC power cycle.
* **The Solution:** In [`arch/mips/tangox/setup.c`](https://github.com/sdoolman/linux-tangox/blob/master/arch/mips/tangox/setup.c), `tangox_machine_restart()` snapshots boot-time Yamon registers into `tangox_boot_remap[8]` during `prom_init()` and restores all 8 crossbar registers before asserting the watchdog trip. The board now reboots autonomously back to stock Linux with 0 power cycles.

### 3. Vitesse VSC8601 PHY Tuning
* **The Trap:** Linux 4.9 wrote `0x004A` to Extended Page 1 Reg 16, which asserted ActiPHY low-power sleep mode and killed physical Ethernet link pulses.
* **The Solution:** All Extended Page 1 writes were purged. We configure Standard Page 0 Reg 23 bit 8 (`MII_RESV1`, `0x0100`) for 2.0 ns RGMII clock skew compensation matching vendor silicon, and preserve the upper nibble `0xf0` in NB8800 `PAD_MODE`.

### 4. On-Chip LRAM Flight Recorder (`0x00062000`)
* The 8 KB on-chip LRAM survives CPU warm resets. Userspace `/init` writes link status, carrier, and the tail of `dmesg` to physical `0x00062000` so diagnostics can be inspected post-reboot via `/dev/mem` in the stock kernel.

---

## 🧰 Repository Structure

```text
pch-a210-modern-linux/
├── .github/workflows/
│   └── build.yml               # Automated CI linting and artifact packaging
├── initramfs/
│   └── init                    # Standalone rescue initramfs script
├── kjump/
│   ├── kjump_main.c            # Safe in-RAM DRAM injector kernel module
│   ├── trampoline.S            # MIPS cache flush & CPU jump trampoline
│   └── Makefile                # Kbuild module Makefile
├── scripts/
│   ├── quick_boot.py           # 1-Click automated boot orchestrator
│   ├── gate0_binary_safety.py  # Gate 0: Static disassembly audit
│   ├── gate1_quiesce_board.py  # Gate 1: Live RAM quiescing & memory audit
│   ├── gate2_stage_verify.py   # Gate 2: In-RAM staging & canary verification
│   ├── gate3_reboot_audit.py   # Gate 3: Clean reboot & remap restoration check
│   ├── trigger_jump.py         # Direct in-RAM jump trigger & acquisition
│   ├── board_shell.py          # Fast interactive Telnet shell (port 2323)
│   └── read_flight_recorder.py # Post-mortem on-chip LRAM telemetry reader
└── README.md                   # This documentation
```

---

## 🛠️ For Developers: Compiling from Source

### 1. Build the Linux 4.9 Kernel
Kernel source is maintained in our fork: **[`sdoolman/linux-tangox`](https://github.com/sdoolman/linux-tangox)**.

```bash
# In Ubuntu / Debian / WSL2:
sudo apt-get install -y gcc-mipsel-linux-gnu binutils-mipsel-linux-gnu bc bison flex libelf-dev libssl-dev

git clone https://github.com/sdoolman/linux-tangox.git
cd linux-tangox

# Configure and compile
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- pch_a210_defconfig
make -j$(nproc) ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- vmlinux
mipsel-linux-gnu-objcopy -O binary vmlinux vmlinux.bin
```

### 2. Build the `kjump.ko` Module
```bash
cd pch-a210-modern-linux/kjump
make KDIR=/path/to/vendor-2.6.22-kernel ARCH=mips CROSS_COMPILE=mipsel-linux-gnu-
```

---

## 🤝 Contributing: Join the Revival!

Have a Popcorn Hour or Sigma SMP864x device sitting around? Contributions and tests are warmly welcomed!

### Areas Where You Can Help:
* 🧪 **Device Fleet Testing:** Test `kjump` and the bringup sequence on other SMP864x/SMP865x models (Popcorn Hour A-200, C-200, C-300, or Western Digital TV Live).
* 🐧 **Root Filesystem / Alpine Linux:** Help package a lightweight Alpine Linux or Debian root filesystem on USB storage.
* 🎬 **Hardware Media Engine (SMPlayer / DSP):** Reverse engineer the Tango3 DSP video firmware loader (`xfload`) under Linux 4.9 for native 1080p hardware video playback!
* 🐛 **Bug Reports & Feedback:** Open an issue or submit a pull request!

---

## ⚖️ License & Disclaimer

* **License:** [MIT License](LICENSE). Free and open-source.
* **Disclaimer:** *Popcorn Hour* and *Syabas* are trademarks of Cloud Media / Syabas Technology. *Sigma Designs* is a trademark of its respective owners. This is an independent community project and is not affiliated with or endorsed by Syabas Technology or Cloud Media.

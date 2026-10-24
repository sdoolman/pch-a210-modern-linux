# Popcorn Hour A-210 (Sigma SMP8643) Linux 4.9 Kernel & QEMU Porting Handover

## 1. Executive Summary & Project Goal
* **Target Device:** Popcorn Hour A-210 (Syabas / Cloud Media).
* **SoC:** Sigma Designs SMP8643 (Tango3 family), MIPS 74Kf core @ 661.5 MHz, 330.75 MHz System Bus, Little-Endian (`mipsel`), 192 MB System DRAM (+ 64 MB DSP DRAM at `0x00000000`).
* **Primary Objective:** Boot modern Linux LTS (Linux 4.9.0 / Tangox branch) with fully working network stack (`nb8800` MAC + `vsc8601` PHY) into an interactive userspace shell.
* **Boot Philosophy:** **In-Memory Trampoline (`kjump.ko`) from Stock Firmware.**
  * **Strict Non-Destructive NAND Policy:** **NEVER write to `/dev/mtd*` or `/dev/sigmblock*`.** Do not touch NAND flash.
  * Stock firmware (Linux 2.6.22.19-19-22) boots normally from flash.
  * Modern kernel binary (`vmlinux.bin`) is staged to RAM/Disk (`/share/`).
  * Custom kernel module `kjump.ko` relocates kernel in memory, arms hardware watchdog, clears caches, and jumps to modern entry point.
  * If boot hangs or crashes, a simple power cycle restores factory firmware 100% safely.

---

## 2. Infrastructure, Toolchain Parity & Version Control Provenance

### 2.1 Build Environment & Toolchain Parity
* **Compilation Environment:** **WSL2 (`Ubuntu-24.04`) ONLY.**
  * **STRICT RULE:** **ZERO OCI COMPILATION.** OCI is rate-limited and prohibited for builds. All builds must run locally inside WSL.
* **Toolchain Version:**
  * GNU Cross-Compiler: `mipsel-linux-gnu-gcc (Ubuntu 12.4.0-2ubuntu1~24.04) 12.4.0`.
  * Toolchain parity verified: compiled binaries run static, no-pie, little-endian MIPS32r2 with soft-float ABI (`-msoft-float`).
* **Kernel Configuration (`.config`):**
  * Path: `/home/stavd/src/linux-tangox/.config` (46,413 bytes).
  * Carried forward intact with critical obstacle fixes enabled:
    * `CONFIG_IP_PNP=y`, `CONFIG_IP_PNP_DHCP=y`, `CONFIG_IP_PNP_BOOTP=y` (network auto-config).
    * `CONFIG_SERIAL_8250=y`, `CONFIG_SERIAL_OF_PLATFORM=y`, `CONFIG_EARLY_PRINTK=y`.
    * `CONFIG_TANGOX=y`, `CONFIG_AURORA_NB8800=y`, `CONFIG_VITESSE_PHY=y`.
* **Version Control State (Known-Good Base):**
  * `linux-tangox` git tree base commit: `3f9893d24f ("mtd: nand: tango: keep dma connected until transaction complete")`.
  * Custom DTS added: `arch/mips/boot/dts/sigma/pch_a210.dts`.
  * `qemu` git tree base commit: `1600b9f ("Update version for v8.2.0 release")`.
  * Custom machine model added: `hw/mips/tangox.c` (registered in `hw/mips/meson.build`).

---

## 3. Hardware Ground Truth & Provenance (Physical Board: 192.168.1.4)

Every address below was verified against running factory kernel logs (`dmesg`, `/proc/iomem`, `/proc/cpuinfo`) and Sigma SDK tango3 headers:

| Component | Physical Address / Value | Provenance & Source | Notes |
| :--- | :--- | :--- | :--- |
| **CPU** | MIPS 74Kf V4.9 @ 661.5 MHz | `/proc/cpuinfo`, stock dmesg | BogoMIPS: 440.32, Bus clock: 330.75 MHz |
| **System RAM** | `0x04000000`–`0x10000000` | Stock boot args (`mem=192M`) | Physical DRAM base offset is +64MB (`0x04000000`) |
| **DSP DRAM** | `0x00000000`–`0x04000000` | Sigma memory architecture | 64 MB reserved for video/audio DSP cores |
| **LRAM** | `0x0BF00000` / `0x1BF00000` | Sigma Tango3 memory map | 64 KB Local Fast RAM (used for boot parameters) |
| **Chip ID Reg** | Physical `0x0002FEE8` | Hardware read (`tangox_chip_id`) | Returns `0x8643` (SMP8643) |
| **Revision Reg**| Physical `0x0002FEEC` | Hardware read | Returns `0x0001` (ES1 / Rev 1) |
| **Watchdog** | Physical `0x0001FD00` / `0x0001FD04`| Sigma Tango3 sys block | Counter + Config registers (27 MHz tick) |
| **Free Clock** | Physical `0x00010048` | `SYS_xtal_in_cnt` | 27 MHz free-running clocksource |
| **INTC Base** | Physical `0x0006E000` | `irq-tango.c`, stock INTC | Cascaded to MIPS CPU HW IRQ 2 (IP2) |
| **UART0** | Physical `0x0006C100` | SMP8640 DTSI & Tango3 UART | Custom unrolled 16550 registers (no board header) |
| **eth0 MAC** | Physical `0x00026000` | Stock `tangox_enet0` base | Stargate Solutions SSN8800 / Aurora NB8800 core |
| **eth0 Compatible**| `sigma,smp8642-ethernet` | Upstream Linux Tango DT binding | **Verified intentional IP reuse**: SMP8642 and SMP8643 share identical Ethernet IP blocks |
| **eth0 MAC Addr**| `00:06:dc:81:4f:a3` | Stock board NVRAM / DTS | Hardcoded in `pch_a210.dts` |
| **eth0 IRQ** | IRQ 38 in DT irq0 | `tangox_irq`, bit 38 in INTC | Connects to MIPS IP2 |
| **eth0 PHY** | Vitesse VSC8601 @ MDIO `0x01` | Stock dmesg (`vsc8601 at 0x01`) | PHY IDs: `0x00070420` & `0x00070421` |
| **PHY Mode** | RGMII (`phy-mode = "rgmii"`) | Hardware strap | Link speed: 1000 Mbps Full Duplex |

---

## 4. Hardware Fidelity Gap Analysis: What QEMU Tests vs What Silicon Tests

QEMU emulation provides rapid iteration, but because QEMU's TCG (Tiny Code Generator) does not model CPU caches or physical bus arbitration, **a clean QEMU boot cannot validate hardware-only failure modes**.

### Matrix of 8 Critical Project Obstacles vs QEMU Validity:

| # | Obstacle & Failure Mechanism | Can QEMU Validate? | Risk Status on Silicon | Silicon Mitigation Required |
| :---: | :--- | :---: | :---: | :--- |
| **1** | **Early Con & UART Register Layout** (Sigma unrolled offsets) | **YES** | **LOW** | Emulated via 16550 adapter in `tangox.c`. Verified working in QEMU. |
| **2** | **RAM Base Offset (`0x04000000`)** (0x00000000 is DSP RAM) | **YES** | **LOW** | System DRAM base defined at `0x04000000` in both DTS and QEMU. |
| **3** | **YAMON Bootloader ABI** (`$a0=2`, `$a1=argv` table in LRAM) | **YES** | **LOW** | Tested and functional under QEMU boot reset handler. |
| **4** | **Missing VSC8601 PHY ID & NB8800 Clockdiv** | **PARTIAL** | **MEDIUM** | Driver patched in kernel. QEMU verifies MDIO probe, but silicon has RGMII clock routing. |
| **5** | **DRAM Overwrite during Trampoline** (Kernel collisions) | **PARTIAL** | **MEDIUM** | Staging memory window (`0x0bf00000` / high RAM) must be clean of stock buffers. |
| **6** | **Watchdog Unclean Close / Trip during Boot** | **NO** | **HIGH** | QEMU does not reboot on timer expiry. Silicon watchdog will trip if kernel timer stalls. |
| **7** | **I-Cache Tag Corruption across Trampoline** | **NO (TCG has no cache)**| **CRITICAL** | **Must execute `synci` / `cache 0x14` invalidation before jump**. QEMU cannot test this! |
| **8** | **DMA Collisions from Stock Daemons** | **NO (Stock daemons not in QEMU)**| **HIGH** | **Must run pre-jump kill script** to stop stock drivers before trampoline fires. |

---

## 5. Security & Safety Guards: Enforced in Code, Not Just Prose

To prevent locking out or bricking the physical board, safety checks are **enforced in automation code**:

1. **Automation Command Guard (`scripts/board_shell.py`):**
   * Blocks any command containing:
     * `colorbars.cgi`, `syb_framework_pipe` (prevents framework shutdown lockouts).
     * `/dev/mtd*`, `/dev/sigmblock*`, `flash_erase`, `flashcp`, `nandwrite`, `dd of=/dev/mtd*` (prevents NAND writes).
   * Throws `ValueError` and intercepts command before network transmission.
2. **Automated Opcode & Watchdog Pre-Jump Gate (`scripts/prepared_jump.sh`):**
   * Before executing `echo 1 > /proc/kjump`, the script queries `/proc/kjump` and asserts:
     * `Status: LOADED` (verifies entry opcode `0x090e6bf8` matches `vmlinux.bin`).
     * `WDT: ARMED` (verifies hardware watchdog is running).
   * If either check fails, execution terminates immediately.

---

## 6. Detailed Technical Work Plan for Incoming Agent

### Phase 0: Hardware Ground Truth Validation (Real Board vs QEMU)
**Goal:** Prove QEMU memory and register layout matches real silicon before relying on emulation results.

1. Once the physical board is accessible post power-cycle:
   ```bash
   python scripts/board_shell.py "cat /proc/cpuinfo; cat /proc/iomem; cat /proc/interrupts" > hardware_dump.txt
   ```
2. Diff against QEMU boot output:
   * Verify memory ranges match: System RAM at `0x04000000`–`0x0fffffff`.
   * Verify Ethernet registers: base `0x00026000`–`0x000267ff`.
   * Verify interrupt mapping: eth0 on IRQ 38, timer on IRQ 5.

---

### Phase 1: Complete NB8800 Ethernet Emulation in QEMU
**File:** `/home/stavd/src/qemu/hw/mips/tangox.c`

> [!WARNING]
> **Emulation Scope Note:** The MDIO/PHY model here implements a functional emulation for network connectivity testing. Silicon VSC8601 has analog RGMII delay lines and autonegotiation timers that cannot be fully replicated in QEMU. Real link verification occurs on silicon.

1. **Add `TangoxNB8800State` Struct:**
   ```c
   typedef struct TangoxNB8800State {
       SysBusDevice parent_obj;
       MemoryRegion mmio;
       NICState *nic;
       NICConf conf;
       qemu_irq irq; /* Connected to CPU INTC IRQ 38 */

       /* Core registers */
       uint32_t tx_ctl1, tx_ctl2;
       uint32_t rx_ctl;
       uint32_t mac_mode;
       uint32_t mdio_cmd, mdio_sts;
       uint8_t  uc_addr[6];

       /* DMA registers */
       uint32_t txc_cr, txc_sr;
       uint32_t tx_desc_addr;
       uint32_t rxc_cr, rxc_sr;
       uint32_t rx_desc_addr;
       uint32_t rx_itr, tx_itr;

       /* Tangox extra registers */
       uint32_t pad_mode, clkdiv, reset;
   } TangoxNB8800State;
   ```

2. **Implement MDIO Bus Emulation (Registers `0x20` / `0x24`):**
   * Handle read/write to `NB8800_MDIO_CMD` (`0x020`) and `NB8800_MDIO_STS` (`0x024`).
   * On write with `MDIO_CMD_GO` (bit 31):
     * If read: return VSC8601 PHY ID registers (`0x0007` at reg 2, `0x0421` at reg 3), BMSR link up (`0x796d`), and LPA gigabit capabilities (`0x45e1`).
     * Clear `MDIO_CMD_GO` immediately.

3. **Implement DMA Descriptor Processing:**
   * **Descriptor format (`struct nb8800_dma_desc`, 16 bytes):**
     * `s_addr` (offset 0): DMA buffer address.
     * `n_addr` (offset 4): Next descriptor address.
     * `r_addr` (offset 8): Status report address.
     * `config` (offset 12): Packet length & flags (`DESC_EOC`, `DESC_EOF`).
   * **TX DMA Kick (`NB8800_TXC_CR` write with `TCR_EN`):**
     * Read descriptor at `s->tx_desc_addr`.
     * Read buffer bytes via `cpu_physical_memory_read()`.
     * Transmit packet via `qemu_send_packet(qemu_get_queue(s->nic), buf, len)`.
     * Write status report to `r_addr` (`TX_BYTES_TRANSFERRED(len)`).
     * Set `s->txc_sr |= TSR_DI | TSR_TI;` and assert IRQ 38.
   * **RX DMA Processing (`nb8800_receive`):**
     * If `s->rxc_cr & RCR_EN`:
       * Read descriptor at `s->rx_desc_addr`.
       * Write incoming packet bytes to `s_addr`.
       * Write status report to `r_addr` (`RX_BYTES_TRANSFERRED(len)`).
       * Set `s->rxc_sr |= RSR_RI | RSR_DI;` and assert IRQ 38.
       * Advance `s->rx_desc_addr = desc.n_addr`.

4. **Wire NIC to Tangox Machine in `tangox_init()`:**
   * Map at `0x00026000ULL`, size `0x800`.
   * Connect IRQ line to Tangox INTC line 38 (`tangox_intc_set_irq(cpu_block, 38, level)`).
   * Instantiate NIC with `qemu_new_nic`.

---

### Phase 2: QEMU Emulation Verification
1. **Rebuild QEMU in WSL2:**
   ```bash
   wsl -d Ubuntu-24.04 -- bash -c "cd /home/stavd/src/qemu && ninja -C build qemu-system-mipsel"
   ```
2. **Execute Boot Test with Network:**
   ```bash
   wsl -d Ubuntu-24.04 -- bash -c "/home/stavd/src/qemu/build/qemu-system-mipsel \
     -M tangox -cpu 74Kf -m 192M \
     -kernel /home/stavd/src/linux-tangox/vmlinux \
     -nographic \
     -netdev user,id=eth0,hostfwd=tcp::2323-:2323 \
     -nic netdev=eth0 \
     -append 'console=ttyS0,115200 init=/init ip=10.0.2.15::10.0.2.2:255.255.255.0:pch:eth0:off'"
   ```
3. **Pass Criteria:**
   * Kernel boot reaches userspace without panic.
   * `eth0` link status reports UP.
   * Host forward port 2323 answers TCP connections.

---

### Phase 3: Physical Hardware Jump (COMPLETED & VERIFIED 🟢)

Modern Linux 4.9.0 is **live, fully running on physical silicon, with bidirectional Gigabit networking and an interactive root shell**.

1. **Physical Silicon Verification:**
   * **Linux 4.9.0 Kernel:** `Linux (none) 4.9.0-gc6ec66ec7a-dirty #134 Thu Oct 22 08:12:16 IDT 2026 mips GNU/Linux`
   * **Interactive Root Shell:** Verified live on port 23 (`192.168.1.4:23`) in 11 seconds.
   * **Full Bidirectional Ethernet Traffic:**
     * `RX packets: 233+` (67.4 KiB received)
     * `TX packets: 42+` (5.7 KiB transmitted)
     * `eth0 IRQ 38`: 73+ interrupts handled cleanly via NAPI
     * ICMP Ping: 0% packet loss, 2ms RTT from host PC (`192.168.1.5`)
   * **System Memory:** 177 MB of clean RAM free.

2. **Key Root Causes Resolved:**
   * **Level-Triggered IRQ 38 Storm (`RX_ITR`):** SMP8643 Ethernet IRQ 38 is `IRQ_TYPE_LEVEL_HIGH`. Setting `RX_ITR` to 0 caused the line to remain asserted continuously, trapping the CPU in an infinite interrupt loop upon receiving the first packet. Restoring `priv->rx_itr_irq = clk / 20000` (50 µs) and `priv->rx_itr_poll = clk / 100` (10 ms) fixed NAPI interrupt pacing.
   * **D-Cache Invalidation on Non-Coherent MIPS 74Kf:** Added `dma_cache_inv((unsigned long)data, len)` in `nb8800_receive` before buffer copying, ensuring the CPU discards stale cache lines and reads actual DMA packet data.
   * **Bit-for-Bit Register Alignment:** `NB8800_RXC_CR` (`0x26200`) aligned to `0x05FE029E` (bit 13 = 0), descriptor config aligned to `0x002205FE` (omitting invalid `DESC_DS` bit 19).
   * **Watchdog Failsafe & Drive Persistence:** `/share/start_app.sh` updated with dynamic `/opt/sybhttpd/localhost.drives/USB_DRIVE*` pattern matching and 120s background watchdog guard.

---

## 7. Relevant Code & Configuration Files

* **QEMU Machine Model:** `/home/stavd/src/qemu/hw/mips/tangox.c`
* **DTS Device Tree:** `/home/stavd/src/linux-tangox/arch/mips/boot/dts/sigma/pch_a210.dts`
* **NB8800 Ethernet Driver:** `/home/stavd/src/linux-tangox/drivers/net/ethernet/aurora/nb8800.c`
* **NB8800 Register Map:** `/home/stavd/src/linux-tangox/drivers/net/ethernet/aurora/nb8800.h`
* **Vitesse PHY Driver:** `/home/stavd/src/linux-tangox/drivers/net/phy/vitesse.c`
* **Tango Interrupt Controller:** `/home/stavd/src/linux-tangox/drivers/irqchip/irq-tango.c`
* **Guarded Telnet Script:** `c:\Users\stavd\Downloads\pch-a210\scripts\board_shell.py`
* **Guarded Trampoline Script:** `c:\Users\stavd\Downloads\pch-a210\scripts\prepared_jump.sh`
* **Staged Jump Module:** `c:\Users\stavd\Downloads\pch-a210\deploy\kjump.ko`

---

## 8. QEMU Machine Model & Network Emulation Alignment

The QEMU machine model at `/home/stavd/src/qemu/hw/mips/tangox.c` has been fully reconciled with our physical silicon findings:
- **DMA GBUS Translation**: `tangox_nb8800_dma_phys` translates GBUS window `0x80000000–0x8BFFFFFF` to QEMU RAM base `0x04000000ULL` (`TANGOX_RAM_BASE`), matching `CONFIG_TANGOX_REMAP=y`.
- **Interrupt Throttle (`NB8800_RX_ITR` / `TX_ITR`)**: Offsets `0x218` and `0x118` explicitly handled in read/write handlers.
- **VSC8601 PHY Banking**: MDIO Register 31 page switching implemented for Extended Page 1 (RGMII skew) and Page 0 (RGMII mode).
- **Compilation**: Successfully compiled with zero errors via `ninja -C /home/stavd/src/qemu/build qemu-system-mipsel`.

---

## 9. Remote Access Architecture & SSH Status

1. **Why OpenSSH (`sshd`) is NOT running:**
   - **Stock Firmware**: Popcorn Hour factory firmware never shipped OpenSSH. It provides Telnet (`telnetd` on port 23), FTP (port 21), and the DavidBox HTTP API (`sybhttpd` on ports 8008/8883).
   - **Linux 4.9.0 Rescue Initramfs**: Minimal in-memory initramfs deliberately excludes OpenSSH due to memory footprint, dynamic library dependencies (OpenSSL `libcrypto`, `zlib`), and privilege-separation (`sshd` user) constraints. The init script explicitly launches `telnetd` on ports 23 and 2323, netcat on port 9999, and `httpd` on port 8080.
2. **Ready Dropbear Package:**
   - A statically linked, standalone MIPS little-endian Dropbear package is pre-staged in `packages/dropbear_pkg/` (`sbin/dropbear`, `bin/dropbearkey`, `bin/scp`, and host keys in `etc/dropbear/`).
   - It can be deployed to `/share/dropbear` or integrated into `initramfs_root` to run on port 22 on demand without bloating the core bootloader.


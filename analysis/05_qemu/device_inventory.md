# QEMU Device Inventory: Sigma Designs SMP8643 (Tango3)

This inventory lists every peripheral device, MMIO window, and register block accessed during early boot (from reset vector / `prom_init` through `start_kernel` to PID 1 `/init` execution), based on the source code of both the vendor 2.6.22 kernel (`~/src/vendor-2.6.22`) and the modern 4.9 kernel (`~/src/linux-tangox`), verified against ground-truth board captures (`proc_iomem.txt`, `proc_interrupts.txt`, `proc_cpuinfo.txt`).

---

## 1. Early Boot Device Inventory Table

| Device Block | Physical Address | Size | Registers Actually Accessed | Access Mode | Emulation Level Required | Notes / Rationale |
|---|---|---|---|---|---|---|
| **System DRAM** | `0x04000000` | 192 MB (`0x0C000000`) | Full physical address space | RW (32-bit) | **Native RAM** | Backed by QEMU `memory_region_init_ram`. Mapped to KSEG0 (`0x84000000`) & KSEG1 (`0xA4000000`). |
| **SoC Chip ID & Revision** | `0x0002FEE8` (`HOST_BASE + 0xFEE8`) | 8 Bytes (`0x08`) | `0x0002FEE8` (`chip_type`), `0x0002FEEC` (`chip_rev`) | RO (32-bit) | **RAM-like stub** | Returns `0x8643` for type and `0x0001` for rev (ES1). Required by `tangox_systype_init()`. |
| **Address Remap Controller** | `0x0006F000` (`CPU_BASE + 0xF000`) | 32 Bytes (`0x20`) | Slots 0–7: `0x00` (`0x1FC00000`), `0x04` (`0x0`), `0x08` (`0x04000000`), `0x0C`–`0x1C` | RW (32-bit) | **RAM-like stub** (or ignore) | `prom_init()` writes remap table. In QEMU, flat memory maps make hardware bus remapping unnecessary. |
| **Primary UART (ttyS0)** | `0x0006C100` (`CPU_BASE + 0xC100`) | 64 Bytes (`0x40`) | `UART_RBR`/`THR` (0x00), `IER` (0x08), `IIR` (0x0C), `LCR` (0x14), `LSR` (0x1C), `CLKDIV` (0x28) | RW (32-bit stride) | **Real model needed** (Adapted 16550) | Standard 16550-like core with 32-bit register stride (4 bytes). Connects to QEMU `chardev` for console. |
| **Secondary UARTs (ttyS1, ttyS2)** | `0x0006C200`, `0x0006CD00` | 64 Bytes each | None during early boot | RW | **Read-zero / write-ignore** | Idle unless explicit dual console is configured. |
| **Hardware Watchdog** | `0x0001FD00` (`SYS_BASE + 0xFD00`) | 8 Bytes (`0x08`) | `0x00` (`WD_COUNTER`), `0x04` (`WD_CONFIG`) | RW (32-bit words only!) | **Read-zero / write-ignore** (or simple down-counter) | Petting writes 32-bit words. If unmodeled, kernel auto-petting proceeds silently without hang. |
| **TangoX Interrupt Controller** | `0x00060000` (`CPU_BASE`) | 256 Bytes (`0x100`) | `CPU_irq_status`/`hi`, `CPU_irq_enable`/`hi`, `CPU_irq_ack`/`hi`, `CPU_fiq_*`, `CPU_iiq_*` | RW (32-bit) | **Real model needed** | Cascades up to 64 peripheral interrupts into MIPS CPU IRQ 2, 3, 4. Powers timer and UART interrupts. |
| **CPU Timer / Clock Counter** | `0x00060100` / MIPS CP0 Count | 64 Bytes | `timer0_val`, `timer0_ctrl`, `timer_ack` | RW (32-bit) | **Real model needed** (or MIPS CP0 Count/Compare) | Linux 2.6.22 and 4.9 use SoC timer IRQ 13 or standard MIPS CP0 timer interrupt (IRQ 7). |
| **PLL & Clock Frequency Config** | `0x00010000` (`SYS_BASE`) | 128 Bytes | `SYS_clk_ctrl`, `SYS_pll_cfg` | RO/RW (32-bit) | **RAM-like stub** | Returns fixed clock frequencies (CPU: 661.5 MHz, SYS: 330.75 MHz, XTAL: 27 MHz). |
| **Ethernet MAC (tangox_enet0 / nb8800)** | `0x00026000` | 2 KB (`0x800`) | DMA desc ring, TXC/RXC CR/SR, MDIO CMD/STS, TX_ITR (`0x118`), RX_ITR (`0x218`) | RW (32-bit) | **Real model implemented** | IRQ 38. VSC8601 PHY (ID 1) with Page 0 (RGMII) / Page 1 (skew 28E=0xA036) banking. GBUS crossbar remap (`0x80000000` -> `0x04000000 + offset`) enforced for TX/RX DMA buffers. |
| **USB Host (EHCI / OHCI)** | `0x00020000` | 4 KB | EHCI/OHCI standard operational registers | RW (32-bit) | **Read-zero / write-ignore** | Not touched during early kernel initialization. |
| **SATA Controller** | `0x00028000` | 4 KB | TangoX SATA DMA registers | RW (32-bit) | **Read-zero / write-ignore** | Not touched if booting from RAM / initramfs. |
| **Diagnostic RAM Buffer** | `0x07000000` | 64 KB (`0x10000`) | Telemetry markers (`0x52414D4C`), milestone stages (`0x44`..`0x99`) | RW (32-bit uncached) | **Native RAM** | Resides directly inside System DRAM at 48 MB offset. Zero extra modeling needed! |

---

## 2. Kernel Early Boot Sequence Mapping

### Stage 1: Reset & Assembly (`head.S`)
- Configures CP0 Status, Config, Cause registers.
- Zeroes BSS.
- In modern 4.9: Writes early debug milestone (`0x44`) directly to uncached DRAM at `0xA7000000`.
- **Devices accessed**: None (Pure CPU & DRAM).

### Stage 2: `prom_init()` (`arch/mips/tangox/prom.c`)
- Calls `prom_console_init()` -> accesses `UART0_BASE` (`0x0006C100`).
- Reads Chip ID / Revision at `0x0002FEE8` and `0x0002FEEC`.
- Writes Address Remap table at `0x0006F000`–`0x0006F01C`.
- Extracts command-line from `fw_arg0` / `fw_arg1` (bootloader handoff).
- **Devices accessed**: UART0, Chip ID, Remap Controller.

### Stage 3: `setup_arch()` (`arch/mips/tangox/setup.c`)
- Registers memory block `0x04000000` - `0x0FFFFFFF` (192 MB) via `add_memory_region`.
- Queries SoC clock tree (`SYS_BASE` / `0x00010000`).
- Initializes TangoX IRQ cascaded controller (`CPU_BASE` / `0x00060000`).
- Registers `ttyS0` serial platform device.
- **Devices accessed**: Memory controller, Clock/PLL registers, IRQ controller, UART0.

### Stage 4: `init_IRQ()` & Timers
- Configures MIPS CP0 interrupt lines 2, 3, 4.
- Enables SoC Timer IRQ 13 or MIPS CP0 Compare IRQ 7.
- **Devices accessed**: TangoX IRQ Controller, Timer counter.

### Stage 5: Userspace Execution (`/init`)
- Kernel mounts `rootfs` (initramfs).
- Dispatches PID 1 (`/init`).
- `/init` pets watchdog at `0x0001FD00`.
- **Devices accessed**: Watchdog, UART0 (console).

---

## 3. Emulation Complexity Summary

To achieve **Boot-to-Shell** in QEMU:
- **Must Have (Real Model)**:
  1. Primary UART (16550 with 4-byte stride at `0x0006C100`).
  2. TangoX IRQ controller (cascade 64 IRQs to MIPS CPU lines at `0x00060000`).
- **Must Have (Stubs / Static Values)**:
  1. Chip ID (`0x0002FEE8` returning `0x8643`, `0x0002FEEC` returning `0x0001`).
  2. Remap Controller (`0x0006F000`, 32 bytes read/write storage).
  3. Watchdog (`0x0001FD00`, read-zero / write-ignore).
  4. System clocks (`0x00010000`, returning fixed PLL frequencies).
- **Can Be Ignored (Deferred)**:
  - Ethernet, USB, SATA, Flash/NAND controllers, Media/DSP coprocessors.

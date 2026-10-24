# QEMU Solution Designer Handout — Linux 4.9 on Popcorn Hour A-210 (Sigma SMP8643)

**Handout Version**: 1.0 — 26 Sep 2026
**For**: the QEMU solution designer continuing this project
**Workspace**: `C:\Users\stavd\Downloads\pch-a210\`
**Board**: Popcorn Hour A-210, Sigma SMP8643 (Tango3), MIPS 74Kf @ 661.5 MHz, mipsel, no FPU, 192 MB DRAM

---

## 1. TL;DR of Where the Project Stands

- **The mission** is to boot a modern kernel (Linux 4.9 from `mansr/linux-tangox`) on the physical A-210 using the in-RAM `kjump.ko` loader with a hardware-watchdog fail-safe, then make that boot fire automatically and verifiably (`gates_auto.sh` + `/share/AUTO_JUMP`).
- **Option A is ALREADY proven end-to-end in QEMU** on your machine model (see §4): stock 2.6.22 → `kjump.ko` staging → trampoline → 4.9 → PID 1 `/init` banner. The physical board has NOT yet reached a verified 4.9 boot via this path.
- **Right now the physical board is degraded**: its 8008 command server (the Syabas XML/HTTP API in `syb_framework`) is wedged (my earlier bad `deployment?arg0=set_database&arg1=/tmp/app.db` probe killed it). Only FTP(21), mute 23, mute 30000, and sybhttpd(8883) respond. No shell (22/2323/8008 down). **This is the main reason we pivot to you: iterate in QEMU, stop burning physical-board cycles.**
- **Your job**: extend/use the QEMU `tangox` machine to (A) model/verify the **auto-start + deployment + WDT-gates** mechanism that is currently broken on real hardware, and (B) keep the full Option A path reproducible headlessly so fixes can be validated before touching the box.

---

## 2. The Physical Board Right Now (updated 26 Sep 2026)

| Port | State |
|---|---|
| 21 FTP (Pure-FTPd, `nmt`/`1234`) | OPEN — allowed PWD=/, NLST shows only `NETWORK_SHARE`, `USB_DRIVE`, `USB_DRIVE_2d01be89-...`; full read/write access to `/share` |
| 23 telnet | **OPEN & ACTIVE ROOT SHELL** — requires standard telnet CRLF negotiation; immediately responds with `sh-3.00#` root prompt |
| 8008 theDavidBox API | **OPEN & RESPONSIVE** — e.g. `/system?arg0=get_mac_address` returns `00:06:DC:81:4F:A3` with `returnValue 0` |
| 8883 sybhttpd | OPEN — raw file read works with `Host: localhost.drives` on `/USB_DRIVE/<file>` |
| 22, 2323 | Closed / not listening (dropbear / secondary telnetd) |
| 30000 (gaya) | Open |

- Board IP `192.168.1.4`; host PC `192.168.1.213`.
- `start_app.log` on `/share` is still the ORIGINAL 888-byte version (mtime `Sep 25 20:12:04 2026`) → **the hooked `start_app.sh` has NOT auto-run since the last cold boot**. `gates_auto.log` and `/share/AUTO_JUMP` are still absent → gates have never fired on hardware.
- Deployed-but-not-yet-proven on `/share/Apps` deployment: hooked `start_app.sh` (4888 B, md5 `38dc76a9…`), `gates_auto.sh` (938 B), `gates_auto_appinfo.json`. `kjump.ko` and `vmlinuz.bin` remain in `/share` (and on OCI).

---

## 3. The Problem You Are Asked to Solve in Emulation

The **NMT app auto-start loop** is the missing link. On real hardware it has never auto-fired after the 23:19 power-cycle, and we can no longer poke the 8008 command server to fix it (it's dead). The loop is:

```
cold boot ─► S70nmt ─► appcenter (BD-J JVM / syb_framework) ─► deployment DB ─►
    auto-run pch-toolkit/start_app.sh ─► gates_auto.sh ─► arm WDT + echo 1 > /proc/kjump
```

### Facts you need (from static RE of the squashfs extraction):
1. **`/share/start_app.sh` is executed by the NMT app framework at boot IF the deployment DB lists the app as autostart.** No firmware binary references `start_app` by name — it is dispatched via the appcenter DB, not a hardcoded filename.
2. **The deployment module lives in `squashfs_root/app/lib/libsyabas_command_server_command_deployment.so`.** Dispatch table at file-va `0xa7b0`; command strings in rodata `0xd6cc–0xd840` (`get_app_status`, `uninstall_app`, `get_app_info`, `install_app`, `update_app`, `list_app_id`, `list_application`, `list_available_widget`, `list_enabled_widget`, `enable_widget`, `search_application`, `load_app`, `regenerate_app_database`, `set_database`, `get_database`, …).
3. **`set_database` handler @ `0x9578` requires THREE non-null args** (arg1=DB path, arg2, arg3). Bare `set_database&arg1=…` → rv=8. **`regenerate_app_database` handler @ `0x9150` requires BOTH arg0 and arg1 non-null** (0x30/0x34 in the cmd struct).
4. **rv=8 semantics**: `"Get Null DATABASE_PATH or Null APP_ID"` (0xbf04) / `"Get Null DATABASE_PATH"` (0xd2f0). On a cold boot DATABASE_PATH is NULL — nobody called `set_database`. That's exactly why deployment probes return rv=8 and auto-start never fires.
5. **DB path strings in the module**: `/nmt/popApps/` (0xbf34), `/opt/syb/usr/popApps/` (0xbf70), `/nmt/apps/` (0xc13c), `/etc/media.db` (0xd178), `/tmp/.appinstall`.
6. **MIPS gp-relative string trick (reusable)**: in these command modules, after `lw $v0,-0x7fd8($gp); addiu $aN,$v0,-0xXXXX`, the target string address = `0x10000 - 0xXXXX` (GOT[0x1d8f8] holds static base `0x10000`). Verified against actual rodata content.
7. Who calls `set_database` at first boot is (at this moment) not fully proven — prime suspect: the appcenter (BD-J) calling the command server, or `nmt_services.cgi`. A concrete deliverable: determine the exact arg schema + DB path the box expects, so the fix can be scripted in QEMU first, then on the board.

### Suggested QEMU work items (priority order):
1. **Model the auto-start path** so a cold boot in QEMU eventually runs a hook equivalent to `/share/start_app.sh → gates_auto.sh`. You can stub `syb_framework`/appcenter behavior if a full BD-J JVM is infeasible — the contract is: "an app whose descriptor declares autostart gets its `start` command executed after boot".
2. **Validate the `set_database`/`regenerate_app_database` arg contract** by building a tiny emulated deployment DB + command-server shim and confirming which path/args make `get_app_status` return a healthy status (not rv=8). Then encode that as the board fix script.
3. **Keep Option A reproducible**: a headless `-M tangox` run that boots 2.6.22 → kjump → 4.9 → PID 1 banner — a single command, with a greppable PASS/FAIL marker.
4. Once auto-start is reproducible in QEMU, re-run the same steps against the physical board (FTP is still up; 8883 still serves file reads) and confirm `gates_auto.log` + `/share/AUTO_JUMP` appear after a cold boot.

---

## 4. QEMU State You Inherit (already working)

- **Build server (OCI)**: `ubuntu@84.13.66.234` (SSH key `C:\Users\stavd\.ssh\id_rsa_openssh`). Ampere A1 (ARM64) Ubuntu host.
- **QEMU source**: `~/src/qemu` (upstream, at `1600b9f Update version for v8.2.0 release`).
- **Your machine model**: `~/src/qemu/hw/mips/tangox.c` (600 lines, untracked, wired into `hw/mips/meson.build` + `hw/mips/Kconfig`).
- **Built binary**: `~/src/qemu/qemu-system-mipsel` (in-tree build; meson). `-M help` lists:
  ```
  malta   MIPS Malta Core LV (default)
  mipssim MIPS MIPSsim platform
  none    empty machine
  tangox  Popcorn Hour A-210 (Sigma Designs SMP8643)
  ```
- **Machine model features implemented and hardened in tangox.c**:
  - **192 MB RAM at `0x04000000` strictly**: stripped `dsp_ram` (`0x00000000–0x03FFFFFF`) so accesses below `0x04000000` hit `unassigned_mem_ops` (reads return `0x0`, unmapped writes drop/trap). This silicon-accurate void immediately caught bugs in `kjump.ko`.
  - **Cycle-accurate 27 MHz Hardware Watchdog Timer**: down-counter at `SYS_BLOCK_BASE + 0xFD00` and config at `0xFD04`. Enforces strict 32-bit word access, calculates real elapsed ticks on read, and triggers `qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET)` on timeout.
  - CPU block `0x00060000` (IRQ ctrl + timer0); host block `0x00020000` (chip ID `0x8643`/rev); flash `0x40000000–0x50000000`; UART0 `0x0006C100` (custom layout, chardev `serial_hd(0)`, 460800 baudbase); ELF→DRAM kernel loading with KSEG0/`0x84000000` mapping; reset hook writing cmdline via `$a0/$a1/$a2`.
- **Option A verified in QEMU (2026-09-25, see `analysis/LOG.md`)**: stock 2.6.22 boot → `kjump.ko` (786-page staging, canary `0xDEADBEEF`, `0x3c08a006`/`0x3508c104` entry opcodes verified) → trampoline (WDT 30s) → 4.9 decompress (`Z`/`K`/`E` markers) → `kernel_entry` → ttyS0 console handover → PID 1 `/init` banner: *"WELCOME TO POPCORN HOUR A-210 (LINUX 4.9.0)"*.
- **Kernel sources on OCI**: `~/src/linux-tangox` (defconfig `arch/mips/configs/pch_a210_defconfig`), vendor tree `~/src/vendor-2.6.22`. Build: `compile_kernel.sh`, targeting `vmlinuz.bin`.
- **Artifacts on OCI** (`~/deploy/`): `vmlinuz.bin` (3,217,536 B, md5 `25e73b53…`), `kjump.ko` (14,284 B, md5 `9fe06def…`). Local copies in `deploy_verified/` and `kjump_modules/`.
- **Local tools**: `tools/run_smoke_test.py` (boots `kernels/vmlinux.elf` under `-M malta` with `-d in_asm` tracing — legacy; prefer `-M tangox`), `tools/elf_survey.py`, `tools/zbf_header.py`, `analysis/05_qemu/{device_inventory,machine_design,memory_map,smoke.log}`.

---

## 5. Design Docs You Should Read First

- `analysis/05_qemu/device_inventory.md` — every MMIO block + emulation-level classification (UART0/INTC = real models; WDT/chip-id/remap = stubs; eth/USB/SATA = deferred).
- `analysis/05_qemu/machine_design.md` — architecture diagram, the "log & halt on unmodeled MMIO" policy, bootloader handoff (`xenv`), and the 8-obstacle regression matrix (6/8 catchable in QEMU; only cache-tag corruption and bus lockups escape TCG).
- `analysis/05_qemu/memory_map.md` — full physical/virtual memory map incl. KSEG0/KSEG1 aliases, WDT `0xA001FD00`, RAM telemetry `0xA7000000`.
- `docs/MASTER_HANDOVER_PLAN.md` (v3) — the 8 solved obstacles, the "zero manual power-cycles" guarantee, and the watch/interlock rules.
- `docs/PHYSICAL_MIGRATION_PLAN.md` — the intended physical-boot runbook (quiesce services, stage via FTP, HARD-STOP GATE before jump, Scenario A/B telemetry).
- `analysis/LOG.md` — command history; the 2026-09-25 section documents the successful QEMU Option A run.
- `analysis/03_board_truth/*` — ground truth captures: `mount.txt`, `dmesg.txt`, `proc_iomem.txt`, `proc_modules.txt`, `init_d_ls.txt` (init scripts list: `S50syb_framework`, `S70nmt`, etc.), `stock_kernel_ram.bin`.

---

## 6. Key Gotchas / Hard Rules (learned the hard way)

1. **NEVER probe the physical 8008 command server with malformed deployment args** — it wedges and does not recover (that's the current board state). If you ever regain 8008, only send well-formed commands after confirming arg schema in QEMU.
2. **`screen:` / `set_database:` need their real 3-arg schema**; don't fire-and-forget on hardware.
3. **Physical DRAM starts at `0x04000000`**; uncached KSEG1 alias is `KSEG0 + 0x24000000` (`0x80000000→0xA4000000`), NOT the standard `+0x20000000`.
4. **No FPU**: build init/userspace with `-msoft-float -static -no-pie` (Musl). GAS Loongson3 LL/SC workaround flag `-Wa,-mno-fix-loongson3-llsc` was required for 4.9 on 74Kf.
5. **Watchdog** is 32-bit-word-only MMIO at `0xA001FD00`; pet/poke with `sw`, never byte ops. 4.9 has an in-kernel 10-second auto-pet wrapper (`tangox_wdt`).
6. Keep everything in `/share` (USB EXT3), never `/tmp` (ramfs steals the ~192 MB). Never touch NAND (`/dev/mtd*`).
7. **`appcontrol`** (`sigma/bdj/jvm/bin/appcontrol`) is the framework supervisor `while [true]` shell — it restarts `syb_framework` only when the *process exits*, and it will pick `flashlite`/`qt`/etc. from `/tmp/app.txt`. A wedged in-process thread does NOT trigger a respawn (matches observed dead 8008).

---

## 7. Constraints / Handoff Preferences

- Work as much as possible **headlessly on OCI** (SSH), copying results back into the local workspace under `analysis/05_qemu/` and the appropriate `docs/` file.
- Test on the physical board **last and minimally** — only after the exact commands are proven in QEMU and only via channels that are currently healthy (FTP for file reads/writes on `/share`, 8883 for file reads). The board needs a cold boot to recover 8008; coordinate any power-cycle with the user.
- Update `analysis/LOG.md` with a dated entry for each significant QEMU milestone (same format as the existing table).
- If the QEMU auto-start model requires more than stubs (i.e., you want to emulate the BD-J/appcenter), scope it explicitly: it may be large; a contract-level shim is acceptable for validating the deployment/gates logic.
- Deliverables that unblock hardware: (1) one script/command set that reconstructs the deployment DB correctly (confirmed in QEMU) and (2) a headless `-M tangox` smoke runner with a PASS/FAIL marker.

---

## 8. QEMU Silicon Alignment Updates (Oct 2026 Milestone)

Recent physical board bring-up on modern Linux 4.9.0 uncovered crucial silicon discrepancies that have now been ported and reconciled directly into QEMU (`hw/mips/tangox.c`):

1. **GBUS Crossbar DDR2 DMA Physical Translation**:
   - *Physical Mechanism*: When `CONFIG_TANGOX_REMAP=y` is enabled, the Sigma SMP8643 hardware crossbar routes DMA master traffic through the `0x80000000 - 0x8BFFFFFF` window to physical DDR2 RAM (`0x04000000 - 0x0FFFFFFF`).
   - *Previous QEMU Bug*: Masking `addr & 0x1fffffffU` erroneously placed DMA accesses at `0x00000000` (DSP RAM / unmapped memory) instead of `0x04000000` (`TANGOX_RAM_BASE`).
   - *Fix Applied*: Re-anchored DMA addresses via `(addr - 0x80000000U) + TANGOX_RAM_BASE`.
2. **Interrupt Throttle Registers (`NB8800_RX_ITR` / `TX_ITR`)**:
   - Explicitly mapped `NB8800_TX_ITR` (`0x118`) and `NB8800_RX_ITR` (`0x218`) registers in `tangox.c` with dedicated state tracking, preventing driver rate-limiting configuration writes (`rx_itr_irq = clk / 20000` and `rx_itr_poll = clk / 100`) from falling through into unmodeled register holes.
3. **Vitesse VSC8601 PHY Page Switching**:
   - Model extended to support MDIO Register 31 Extended Page banking.
   - Standard Page 0 exposes operational RGMII registers (`Reg 23 = 0x1100`).
   - Extended Page 1 exposes RGMII clock-to-data skew registers (`Reg 28E = 0xA036`).

/*
 * QEMU Tangox / Sigma Designs SMP8643 Machine Model
 *
 * Copyright (c) 2026 Antigravity Team
 *
 * Target: Popcorn Hour A-210 (Sigma Designs SMP8643 / Tango3, MIPS 74Kf)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "hw/clock.h"
#include "hw/mips/mips.h"
#include "hw/mips/bios.h"
#include "target/mips/cpu.h"
#include "hw/irq.h"
#include "hw/char/serial.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "net/net.h"

#define TANGOX_RAM_BASE       0x04000000ULL
#define TANGOX_DEFAULT_RAM    (192 * MiB)

#define SYS_BLOCK_BASE        0x00010000ULL
#define SYS_BLOCK_SIZE        0x00010000ULL

#define HOST_BLOCK_BASE       0x00020000ULL
#define HOST_BLOCK_SIZE       0x00010000ULL

#define CPU_BLOCK_BASE        0x00060000ULL
#define CPU_BLOCK_SIZE        0x00010000ULL

#define UART0_BASE            0x0006C100ULL

typedef struct TangoxResetData {
    MIPSCPU *cpu;
    uint64_t entry;
    const char *cmdline;
} TangoxResetData;

/* -------------------------------------------------------------------------
 * CPU Block (INTC, Timer0, Remap, Heartbeat)
 * ------------------------------------------------------------------------- */
typedef struct TangoxCpuBlockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq parent_irq;        /* Cascaded to MIPS CPU HW IRQ 2 */

    /* INTC State */
    uint32_t irq_status;
    uint32_t irq_rawstat;
    uint32_t irq_enable;
    uint32_t irq_status_hi;
    uint32_t irq_rawstat_hi;
    uint32_t irq_enable_hi;

    /* Timer0 State */
    uint32_t timer0_load;
    uint32_t timer0_ctrl;
    QEMUTimer *timer0;

    /* Remap State (8 32-bit registers) */
    uint32_t remap[8];

    /* Heartbeat Counter */
    uint32_t heartbeat;
} TangoxCpuBlockState;

static void tangox_cpu_block_update_irq(TangoxCpuBlockState *s)
{
    s->irq_status = s->irq_rawstat & s->irq_enable;
    s->irq_status_hi = s->irq_rawstat_hi & s->irq_enable_hi;
    int level = (s->irq_status != 0 || s->irq_status_hi != 0) ? 1 : 0;
    qemu_set_irq(s->parent_irq, level);
}

static void tangox_intc_set_irq(void *opaque, int irq, int level)
{
    TangoxCpuBlockState *s = opaque;
    if (irq < 32) {
        if (level) {
            s->irq_rawstat |= (1U << irq);
        } else {
            s->irq_rawstat &= ~(1U << irq);
        }
    } else if (irq < 64) {
        int bit = irq - 32;
        if (level) {
            s->irq_rawstat_hi |= (1U << bit);
        } else {
            s->irq_rawstat_hi &= ~(1U << bit);
        }
    }
    tangox_cpu_block_update_irq(s);
}

static void tangox_timer0_cb(void *opaque)
{
    TangoxCpuBlockState *s = opaque;
    if (s->timer0_ctrl & 0x01) { /* Timer enabled */
        s->irq_rawstat |= (1U << 5); /* IRQ 13: CPU_TIMER0_INT (bit 5) */
        tangox_cpu_block_update_irq(s);
        /* 100 Hz periodic tick */
        timer_mod(s->timer0, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (NANOSECONDS_PER_SECOND / 100));
    }
}

static uint64_t tangox_cpu_block_read(void *opaque, hwaddr addr, unsigned size)
{
    TangoxCpuBlockState *s = opaque;

    switch (addr) {
    case 0x1994: /* LR_ZBOOTXENV_LOCATION */
        return 0;
    case 0x19d8: /* LR_HB_CPU */
        return s->heartbeat++;

    case 0xc500: /* CPU_time0_load */
        return s->timer0_load;
    case 0xc504: /* CPU_time0_value */
        return s->timer0_load;
    case 0xc508: /* CPU_time0_ctrl */
        return s->timer0_ctrl;
    case 0xc50c: /* CPU_time0_clr */
        return 0;

    case 0xe000: /* CPU_irq_status */
        return s->irq_rawstat & s->irq_enable;
    case 0xe004: /* CPU_irq_rawstat */
        return s->irq_rawstat;
    case 0xe008: /* CPU_irq_enableset */
        return s->irq_enable;
    case 0xe00c: /* CPU_irq_enableclr */
        return 0;
    case 0xe010: /* CPU_irq_softset */
        return 0;
    case 0xe014: /* CPU_irq_softclr */
        return 0;
    case 0xe018: /* CPU_irq_status_hi */
        return s->irq_rawstat_hi & s->irq_enable_hi;
    case 0xe01c: /* CPU_irq_rawstat_hi */
        return s->irq_rawstat_hi;
    case 0xe020: /* CPU_irq_enableset_hi */
        return s->irq_enable_hi;
    case 0xe024: /* CPU_irq_enableclr_hi */
        return 0;

    case 0xf000 ... 0xf01c: /* CPU_remap */
        return s->remap[(addr - 0xf000) >> 2];

    default:
        qemu_log_mask(LOG_UNIMP, "tangox_cpu_block_read: unhandled offset 0x%" HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void tangox_cpu_block_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TangoxCpuBlockState *s = opaque;

    switch (addr) {
    case 0x19d8: /* LR_HB_CPU */
        s->heartbeat = val;
        break;

    case 0xc500: /* CPU_time0_load */
        s->timer0_load = val;
        break;
    case 0xc508: /* CPU_time0_ctrl */
        s->timer0_ctrl = val;
        if (val & 0x01) {
            timer_mod(s->timer0, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (NANOSECONDS_PER_SECOND / 100));
        } else {
            timer_del(s->timer0);
        }
        break;
    case 0xc50c: /* CPU_time0_clr */
        s->irq_rawstat &= ~(1U << 5); /* Clear CPU_TIMER0_INT */
        tangox_cpu_block_update_irq(s);
        break;

    case 0xe008: /* CPU_irq_enableset */
        s->irq_enable |= (uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;
    case 0xe00c: /* CPU_irq_enableclr */
        s->irq_enable &= ~(uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;
    case 0xe010: /* CPU_irq_softset */
        s->irq_rawstat |= (uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;
    case 0xe014: /* CPU_irq_softclr */
        s->irq_rawstat &= ~(uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;
    case 0xe020: /* CPU_irq_enableset_hi */
        s->irq_enable_hi |= (uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;
    case 0xe024: /* CPU_irq_enableclr_hi */
        s->irq_enable_hi &= ~(uint32_t)val;
        tangox_cpu_block_update_irq(s);
        break;

    case 0xf000 ... 0xf01c: /* CPU_remap */
        s->remap[(addr - 0xf000) >> 2] = (uint32_t)val;
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "tangox_cpu_block_write: unhandled offset 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", addr, val);
        break;
    }
}

static const MemoryRegionOps tangox_cpu_block_ops = {
    .read = tangox_cpu_block_read,
    .write = tangox_cpu_block_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* -------------------------------------------------------------------------
 * System Block (Clock Counter 27MHz, Watchdog)
 * ------------------------------------------------------------------------- */
typedef struct TangoxSysBlockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t wd_config;
} TangoxSysBlockState;

static uint64_t tangox_sys_block_read(void *opaque, hwaddr addr, unsigned size)
{
    TangoxSysBlockState *s = opaque;

    switch (addr) {
    case 0x0048: /* SYS_xtal_in_cnt: 27 MHz free-running clock */
        return (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) * 27) / 1000);

    case 0xfd00: /* SYS_watchdog_counter */
        return 0x00100000;

    case 0xfd04: /* SYS_watchdog_configuration */
        return s->wd_config;

    default:
        return 0;
    }
}

static void tangox_sys_block_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TangoxSysBlockState *s = opaque;

    switch (addr) {
    case 0xfd00: /* SYS_watchdog_counter */
        break;
    case 0xfd04: /* SYS_watchdog_configuration */
        s->wd_config = (uint32_t)val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tangox_sys_block_ops = {
    .read = tangox_sys_block_read,
    .write = tangox_sys_block_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* -------------------------------------------------------------------------
 * Host Interface Block (Chip ID SMP8643 ES1)
 * ------------------------------------------------------------------------- */
static uint64_t tangox_host_block_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    case 0xfee8: /* Chip ID: SMP8643 */
        return 0x8643;
    case 0xfeec: /* Chip Revision: ES1 */
        return 0x0001;
    default:
        return 0;
    }
}

static void tangox_host_block_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps tangox_host_block_ops = {
    .read = tangox_host_block_read,
    .write = tangox_host_block_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* -------------------------------------------------------------------------
 * Flash Region (0x40000000 - 0x50000000, CS0-CS3 dummy response)
 * ------------------------------------------------------------------------- */
static uint64_t tangox_flash_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0xffffffffffffffffULL;
}

static void tangox_flash_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps tangox_flash_ops = {
    .read = tangox_flash_read,
    .write = tangox_flash_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};


/* -------------------------------------------------------------------------
 * Tangox UART Adapter (Maps Tangox unrolled register offsets to standard 16550)
 * ------------------------------------------------------------------------- */
typedef struct TangoxUART {
    SerialState *serial;
    MemoryRegion mmio;
} TangoxUART;

static uint64_t tangox_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    SerialState *s = opaque;
    hwaddr reg;

    switch (addr) {
    case 0x00: /* UART_RBR */
        reg = 0;
        break;
    case 0x08: /* UART_IER */
        reg = 1;
        break;
    case 0x0c: /* UART_IIR */
        reg = 2;
        break;
    case 0x14: /* UART_LCR */
        reg = 3;
        break;
    case 0x18: /* UART_MCR */
        reg = 4;
        break;
    case 0x1c: /* UART_LSR */
        reg = 5;
        break;
    case 0x20: /* UART_MSR */
        reg = 6;
        break;
    case 0x24: /* UART_SCR */
        reg = 7;
        break;
    case 0x28: /* UART_CLKDIV */
    case 0x2c: /* UART_CLKSEL */
    default:
        return 0;
    }
    return serial_io_ops.read(s, reg, 1);
}

static void tangox_uart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    SerialState *s = opaque;
    hwaddr reg;

    switch (addr) {
    case 0x00:
    case 0x04: /* UART_THR */
        reg = 0;
        break;
    case 0x08: /* UART_IER */
        reg = 1;
        break;
    case 0x10: /* UART_FCR */
        reg = 2;
        break;
    case 0x14: /* UART_LCR */
        reg = 3;
        break;
    case 0x18: /* UART_MCR */
        reg = 4;
        break;
    case 0x24: /* UART_SCR */
        reg = 7;
        break;
    case 0x28: /* UART_CLKDIV */
    case 0x2c: /* UART_CLKSEL */
    default:
        return;
    }
    serial_io_ops.write(s, reg, val & 0xff, 1);
}

static const MemoryRegionOps tangox_uart_ops = {
    .read = tangox_uart_read,
    .write = tangox_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};


/* -------------------------------------------------------------------------
 * Tangox NB8800 Ethernet MAC Model
 * ------------------------------------------------------------------------- */
#define NB8800_BASE           0x00026000ULL
#define NB8800_SIZE           0x800

#define NB8800_MDIO_CMD       0x020
#define NB8800_MDIO_STS       0x024
#define NB8800_MC_INIT        0x02e
#define NB8800_TXC_CR         0x100
#define NB8800_TXC_SR         0x104
#define NB8800_TX_DESC_ADDR   0x10c
#define NB8800_TX_ITR         0x118
#define NB8800_RXC_CR         0x200
#define NB8800_RXC_SR         0x204
#define NB8800_RX_DESC_ADDR   0x20c
#define NB8800_RX_ITR         0x218

#define TCR_EN                (1U << 0)
#define TSR_TI                (1U << 0)
#define TSR_DI                (1U << 2)

#define RCR_EN                (1U << 0)
#define RSR_RI                (1U << 0)
#define RSR_DI                (1U << 2)

#define DESC_EOF              (1U << 21)
#define DESC_EOC              (1U << 22)

typedef struct TangoxNB8800State {
    MemoryRegion mmio;
    NICState *nic;
    NICConf conf;
    qemu_irq irq;

    uint32_t mdio_cmd;
    uint32_t mdio_sts;
    uint8_t phy_page;
    uint16_t phy_regs[32];
    uint16_t phy_ext_regs[32];

    uint32_t txc_cr;
    uint32_t txc_sr;
    uint32_t tx_desc_addr;
    uint32_t tx_itr;

    uint32_t rxc_cr;
    uint32_t rxc_sr;
    uint32_t rx_desc_addr;
    uint32_t rx_itr;

    uint8_t regs[NB8800_SIZE];
} TangoxNB8800State;

static inline hwaddr tangox_nb8800_dma_phys(uint32_t addr)
{
    /*
     * Tangox GBUS Crossbar Remap:
     * When CONFIG_TANGOX_REMAP is active, DDR2 RAM (physical 0x04000000 - 0x0FFFFFFF)
     * is accessed by peripheral DMA masters via GBUS address window 0x80000000 - 0x8BFFFFFF.
     * Translate GBUS DDR2 addresses to QEMU physical memory offsets (TANGOX_RAM_BASE).
     */
    if (addr >= 0x80000000U && addr < 0x8c000000U) {
        return (hwaddr)(addr - 0x80000000U) + TANGOX_RAM_BASE;
    }
    return addr;
}

static void tangox_nb8800_update_irq(TangoxNB8800State *s)
{
    int level = ((s->txc_sr & 0x0f) != 0 || (s->rxc_sr & 0x0f) != 0) ? 1 : 0;
    qemu_set_irq(s->irq, level);
}

static void tangox_nb8800_reset(TangoxNB8800State *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    memset(s->phy_ext_regs, 0, sizeof(s->phy_ext_regs));

    s->mdio_cmd = 0;
    s->mdio_sts = 0;
    s->phy_page = 0;
    s->txc_cr = 0;
    s->txc_sr = 0;
    s->tx_desc_addr = 0;
    s->tx_itr = 0;
    s->rxc_cr = 0;
    s->rxc_sr = 0;
    s->rx_desc_addr = 0;
    s->rx_itr = 0;

    /* Vitesse VSC8601 PHY default registers */
    s->phy_regs[0] = 0x1140; /* BMCR */
    s->phy_regs[1] = 0x796d; /* BMSR - Link UP, 1000/100/10 FD/HD, Auto-neg complete */
    s->phy_regs[2] = 0x0007; /* PHYSID1 */
    s->phy_regs[3] = 0x0421; /* PHYSID2 */
    s->phy_regs[4] = 0x01e1; /* ADVERTISE */
    s->phy_regs[5] = 0x45e1; /* LPA */
    s->phy_regs[9] = 0x0e00; /* CTRL1000 */
    s->phy_regs[10] = 0x3800; /* STAT1000 */
    s->phy_regs[15] = 0x3000; /* ESTATUS */
    s->phy_regs[0x17] = 0x0100; /* EPHY_CTL */

    s->regs[0x424] = 1; /* NB8800_TANGOX_RESET: reset cleared */
    tangox_nb8800_update_irq(s);
}

static void tangox_nb8800_reset_cb(void *opaque)
{
    tangox_nb8800_reset((TangoxNB8800State *)opaque);
}

static void tangox_nb8800_mdio_cmd_write(TangoxNB8800State *s, uint32_t val)
{
    s->mdio_cmd = val;

    if (val & (1U << 31)) { /* MDIO_CMD_GO */
        uint32_t phy_id = (val >> 21) & 0x1f;
        uint32_t reg = (val >> 16) & 0x1f;
        bool is_write = (val & (1U << 26)) != 0; /* MDIO_CMD_WR */

        if (phy_id == 1) {
            if (is_write) {
                uint16_t data = val & 0xffff;
                if (reg == 31) {
                    /* VSC8601 Extended Page Access Register */
                    s->phy_page = (uint8_t)(data & 0x01);
                } else if (s->phy_page == 1) {
                    s->phy_ext_regs[reg] = data;
                } else {
                    s->phy_regs[reg] = data;
                    if (reg == 0) {
                        s->phy_regs[0] &= ~0x8000; /* Auto-clear reset */
                    }
                    s->phy_regs[1] = 0x796d;
                    s->phy_regs[2] = 0x0007;
                    s->phy_regs[3] = 0x0421;
                    s->phy_regs[10] = 0x3800;
                    s->phy_regs[15] = 0x3000;
                }
                s->mdio_sts = 0;
            } else {
                if (reg == 31) {
                    s->mdio_sts = s->phy_page;
                } else if (s->phy_page == 1) {
                    s->mdio_sts = s->phy_ext_regs[reg];
                } else {
                    s->mdio_sts = s->phy_regs[reg];
                }
            }
        } else {
            s->mdio_sts = 0x80000000; /* MDIO_STS_ERR */
        }

        s->mdio_cmd &= ~(1U << 31); /* Immediately clear bit 31 (MDIO_CMD_GO) */
    }
}

static void tangox_nb8800_tx_start(TangoxNB8800State *s)
{
    uint32_t cur = s->tx_desc_addr;
    uint8_t pkt[4096];
    uint32_t pkt_len = 0;
    int limit = 512;

    while ((s->txc_cr & TCR_EN) && cur && limit-- > 0) {
        struct {
            uint32_t s_addr;
            uint32_t n_addr;
            uint32_t r_addr;
            uint32_t config;
        } desc;

        cpu_physical_memory_read(tangox_nb8800_dma_phys(cur), &desc, sizeof(desc));
        desc.s_addr = le32_to_cpu(desc.s_addr);
        desc.n_addr = le32_to_cpu(desc.n_addr);
        desc.r_addr = le32_to_cpu(desc.r_addr);
        desc.config = le32_to_cpu(desc.config);

        uint32_t len = desc.config & 0xffff;
        if (len > 0 && desc.s_addr && (pkt_len + len <= sizeof(pkt))) {
            cpu_physical_memory_read(tangox_nb8800_dma_phys(desc.s_addr),
                                     pkt + pkt_len, len);
            pkt_len += len;
        }

        if ((desc.config & DESC_EOF) || (desc.config & DESC_EOC)) {
            if (pkt_len > 0) {
                if (s->nic) {
                    qemu_send_packet(qemu_get_queue(s->nic), pkt, pkt_len);
                }
                if (desc.r_addr) {
                    uint32_t report = cpu_to_le32(pkt_len << 16);
                    cpu_physical_memory_write(tangox_nb8800_dma_phys(desc.r_addr),
                                             &report, sizeof(report));
                }
                pkt_len = 0;
            }
        }

        if (desc.config & DESC_EOC) {
            s->txc_cr &= ~TCR_EN;
        }

        cur = desc.n_addr;
    }

    s->tx_desc_addr = cur;
    s->txc_sr |= (TSR_DI | TSR_TI); /* 0x05 */
    tangox_nb8800_update_irq(s);
}

static bool tangox_nb8800_can_receive(NetClientState *nc)
{
    TangoxNB8800State *s = qemu_get_nic_opaque(nc);
    return (s->rxc_cr & RCR_EN) && (s->rx_desc_addr != 0);
}

static ssize_t tangox_nb8800_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    TangoxNB8800State *s = qemu_get_nic_opaque(nc);

    if (!(s->rxc_cr & RCR_EN) || !s->rx_desc_addr) {
        return 0;
    }

    struct {
        uint32_t s_addr;
        uint32_t n_addr;
        uint32_t r_addr;
        uint32_t config;
    } desc;

    cpu_physical_memory_read(tangox_nb8800_dma_phys(s->rx_desc_addr),
                             &desc, sizeof(desc));
    desc.s_addr = le32_to_cpu(desc.s_addr);
    desc.n_addr = le32_to_cpu(desc.n_addr);
    desc.r_addr = le32_to_cpu(desc.r_addr);
    desc.config = le32_to_cpu(desc.config);

    if (!desc.s_addr || !desc.r_addr) {
        return 0;
    }

    size_t len = size;
    if (len > 1536) {
        len = 1536;
    }

    cpu_physical_memory_write(tangox_nb8800_dma_phys(desc.s_addr), buf, len);
    uint32_t report = cpu_to_le32(len << 16);
    cpu_physical_memory_write(tangox_nb8800_dma_phys(desc.r_addr), &report, sizeof(report));

    if (desc.config & DESC_EOC) {
        s->rxc_cr &= ~RCR_EN;
    }

    s->rx_desc_addr = desc.n_addr;
    s->rxc_sr |= (RSR_RI | RSR_DI); /* 0x05 */
    tangox_nb8800_update_irq(s);

    return size;
}

static void tangox_nb8800_cleanup(NetClientState *nc)
{
    TangoxNB8800State *s = qemu_get_nic_opaque(nc);
    s->nic = NULL;
}

static NetClientInfo net_tangox_nb8800_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = tangox_nb8800_can_receive,
    .receive = tangox_nb8800_receive,
    .cleanup = tangox_nb8800_cleanup,
};

static uint64_t tangox_nb8800_read(void *opaque, hwaddr addr, unsigned size)
{
    TangoxNB8800State *s = opaque;

    if (addr == NB8800_MDIO_CMD) {
        return s->mdio_cmd;
    }
    if (addr == NB8800_MDIO_STS) {
        return s->mdio_sts;
    }
    if (addr == NB8800_MC_INIT) {
        return 0; /* NB8800_MC_INIT: busy bit 0 */
    }
    if (addr == NB8800_TXC_CR) {
        return s->txc_cr;
    }
    if (addr == NB8800_TXC_SR) {
        return s->txc_sr;
    }
    if (addr == NB8800_TX_DESC_ADDR) {
        return s->tx_desc_addr;
    }
    if (addr == NB8800_TX_ITR) {
        return s->tx_itr;
    }
    if (addr == NB8800_RXC_CR) {
        return s->rxc_cr;
    }
    if (addr == NB8800_RXC_SR) {
        return s->rxc_sr;
    }
    if (addr == NB8800_RX_DESC_ADDR) {
        return s->rx_desc_addr;
    }
    if (addr == NB8800_RX_ITR) {
        return s->rx_itr;
    }

    if (addr + size <= sizeof(s->regs)) {
        if (size == 1) {
            return s->regs[addr];
        } else if (size == 2) {
            return (uint32_t)s->regs[addr] | ((uint32_t)s->regs[addr + 1] << 8);
        } else if (size == 4) {
            return (uint32_t)s->regs[addr] |
                   ((uint32_t)s->regs[addr + 1] << 8) |
                   ((uint32_t)s->regs[addr + 2] << 16) |
                   ((uint32_t)s->regs[addr + 3] << 24);
        }
    }
    return 0;
}

static void tangox_nb8800_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    TangoxNB8800State *s = opaque;

    if (addr == NB8800_MDIO_CMD) {
        tangox_nb8800_mdio_cmd_write(s, (uint32_t)val);
        return;
    }
    if (addr == NB8800_MDIO_STS) {
        s->mdio_sts = (uint32_t)val;
        return;
    }
    if (addr == NB8800_MC_INIT) {
        s->regs[NB8800_MC_INIT] = 0;
        return;
    }
    if (addr == NB8800_TXC_CR) {
        s->txc_cr = (uint32_t)val;
        if (s->txc_cr & TCR_EN) {
            tangox_nb8800_tx_start(s);
        }
        return;
    }
    if (addr == NB8800_TXC_SR) {
        s->txc_sr &= ~(uint32_t)val;
        tangox_nb8800_update_irq(s);
        return;
    }
    if (addr == NB8800_TX_DESC_ADDR) {
        s->tx_desc_addr = (uint32_t)val;
        return;
    }
    if (addr == NB8800_TX_ITR) {
        s->tx_itr = (uint32_t)val;
        return;
    }
    if (addr == NB8800_RXC_CR) {
        s->rxc_cr = (uint32_t)val;
        if ((s->rxc_cr & RCR_EN) && s->rx_desc_addr && s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }
    if (addr == NB8800_RXC_SR) {
        s->rxc_sr &= ~(uint32_t)val;
        tangox_nb8800_update_irq(s);
        return;
    }
    if (addr == NB8800_RX_DESC_ADDR) {
        s->rx_desc_addr = (uint32_t)val;
        if ((s->rxc_cr & RCR_EN) && s->rx_desc_addr && s->nic) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }
    if (addr == NB8800_RX_ITR) {
        s->rx_itr = (uint32_t)val;
        return;
    }

    if (addr + size <= sizeof(s->regs)) {
        if (size == 1) {
            s->regs[addr] = (uint8_t)val;
        } else if (size == 2) {
            s->regs[addr] = (uint8_t)val;
            s->regs[addr + 1] = (uint8_t)(val >> 8);
        } else if (size == 4) {
            s->regs[addr] = (uint8_t)val;
            s->regs[addr + 1] = (uint8_t)(val >> 8);
            s->regs[addr + 2] = (uint8_t)(val >> 16);
            s->regs[addr + 3] = (uint8_t)(val >> 24);
        }
    }
}

static const MemoryRegionOps tangox_nb8800_ops = {
    .read = tangox_nb8800_read,
    .write = tangox_nb8800_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/* -------------------------------------------------------------------------
 * Bootloader Handoff & Machine Setup
 * ------------------------------------------------------------------------- */
static void tangox_cpu_reset(void *opaque)
{
    TangoxResetData *s = opaque;
    CPUMIPSState *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->active_tc.PC = s->entry;

    /* Write argv table and strings into LRAM at safe physical 0x0bf01000 (KSEG0 0x8bf01000) */
    uint32_t argv_ptrs[3] = {
        cpu_to_le32(0x8bf01020), /* argv[0] -> "vmlinux" */
        cpu_to_le32(0x8bf01030), /* argv[1] -> cmdline */
        0
    };
    cpu_physical_memory_write(0x0bf01000, argv_ptrs, sizeof(argv_ptrs));
    cpu_physical_memory_write(0x0bf01020, "vmlinux\0", 8);

    const char *cmd = (s->cmdline && strlen(s->cmdline)) ?
                      s->cmdline : "console=ttyS0,115200 mem=192M earlyprintk";
    cpu_physical_memory_write(0x0bf01030, cmd, strlen(cmd) + 1);

    /* YAMON / Tangox bootloader ABI */
    env->active_tc.gpr[4] = 2;              /* $a0 = argc */
    env->active_tc.gpr[5] = 0x8bf01000;     /* $a1 = argv table */
    env->active_tc.gpr[6] = 0;              /* $a2 = envp */
}

static void tangox_init(MachineState *machine)
{
    const char *kernel_filename = machine->kernel_filename;
    const char *kernel_cmdline = machine->kernel_cmdline;
    MemoryRegion *address_space_mem = get_system_memory();
    Clock *cpuclk;
    MIPSCPU *cpu;
    CPUMIPSState *env;
    TangoxResetData *reset_data;
    uint64_t entry = 0x84000400;

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, 661500000); /* 661.5 MHz MIPS 74Kf core */

    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk);
    env = &cpu->env;

    /* Initialize MIPS CPU IRQ lines */
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    /* Map System DRAM at 0x04000000 */
    memory_region_add_subregion(address_space_mem, TANGOX_RAM_BASE, machine->ram);

    /* DSP DRAM (64MB at physical 0x00000000, priority -2 under MMIO blocks) */
    MemoryRegion *dsp_ram = g_new(MemoryRegion, 1);
    memory_region_init_ram(dsp_ram, NULL, "tangox.dsp_ram", 64 * MiB, &error_fatal);
    memory_region_add_subregion_overlap(address_space_mem, 0x00000000ULL, dsp_ram, -2);

    /* LRAM (64KB at physical 0x0BF00000 and 0x1BF00000) */
    MemoryRegion *lram = g_new(MemoryRegion, 1);
    memory_region_init_ram(lram, NULL, "tangox.lram", 64 * KiB, &error_fatal);
    memory_region_add_subregion(address_space_mem, 0x0BF00000ULL, lram);

    MemoryRegion *lram_alias = g_new(MemoryRegion, 1);
    memory_region_init_alias(lram_alias, NULL, "tangox.lram_alias", lram, 0, 64 * KiB);
    memory_region_add_subregion(address_space_mem, 0x1BF00000ULL, lram_alias);

    /* Instantiate CPU Block Device */
    TangoxCpuBlockState *cpu_block = g_new0(TangoxCpuBlockState, 1);
    cpu_block->timer0 = timer_new_ns(QEMU_CLOCK_VIRTUAL, tangox_timer0_cb, cpu_block);
    cpu_block->parent_irq = env->irq[2]; /* MIPS IP2 */
    /* Set default remap table matching Yamamon silicon registers */
    cpu_block->remap[0] = 0x1fc00000;
    cpu_block->remap[1] = 0x00000000;
    cpu_block->remap[2] = 0x80000000;
    cpu_block->remap[3] = 0x84000000;
    cpu_block->remap[4] = 0x88000000;
    cpu_block->remap[5] = 0x10000000;
    cpu_block->remap[6] = 0x00000000;
    cpu_block->remap[7] = 0xcc000000;
    memory_region_init_io(&cpu_block->mmio, OBJECT(machine), &tangox_cpu_block_ops,
                          cpu_block, "tangox.cpu_block", CPU_BLOCK_SIZE);
    memory_region_add_subregion_overlap(address_space_mem, CPU_BLOCK_BASE, &cpu_block->mmio, -1);

    /* Instantiate System Block Device */
    TangoxSysBlockState *sys_block = g_new0(TangoxSysBlockState, 1);
    memory_region_init_io(&sys_block->mmio, OBJECT(machine), &tangox_sys_block_ops,
                          sys_block, "tangox.sys_block", SYS_BLOCK_SIZE);
    memory_region_add_subregion(address_space_mem, SYS_BLOCK_BASE, &sys_block->mmio);

    /* Instantiate Host Block Device */
    MemoryRegion *host_block_mr = g_new(MemoryRegion, 1);
    memory_region_init_io(host_block_mr, OBJECT(machine), &tangox_host_block_ops,
                          NULL, "tangox.host_block", HOST_BLOCK_SIZE);
    memory_region_add_subregion(address_space_mem, HOST_BLOCK_BASE, host_block_mr);

    /* Flash Region (0x40000000 - 0x50000000, CS0-CS3) */
    MemoryRegion *flash_mr = g_new(MemoryRegion, 1);
    memory_region_init_io(flash_mr, OBJECT(machine), &tangox_flash_ops,
                          NULL, "tangox.flash", 256 * MiB);
    memory_region_add_subregion(address_space_mem, 0x40000000ULL, flash_mr);

    /* Primary UART0 at 0x0006C100 (Tangox custom register layout) */
    if (serial_hd(0)) {
        /* Connect UART IRQ to Tangox INTC line 1 (IRQ 9: CPU_UART0_INT) */
        qemu_irq uart_irq = qemu_allocate_irq(tangox_intc_set_irq, cpu_block, 1);
        DeviceState *dev = qdev_new(TYPE_SERIAL);
        SerialState *s = SERIAL(dev);
        s->irq = uart_irq;
        qdev_prop_set_chr(dev, "chardev", serial_hd(0));
        qdev_prop_set_uint32(dev, "baudbase", 460800);
        qdev_realize_and_unref(dev, NULL, &error_fatal);

        TangoxUART *tuart = g_new0(TangoxUART, 1);
        tuart->serial = s;
        memory_region_init_io(&tuart->mmio, OBJECT(machine), &tangox_uart_ops,
                              s, "tangox.uart0", 0x100);
        memory_region_add_subregion(address_space_mem, UART0_BASE, &tuart->mmio);
    }

    /* Tangox NB8800 Ethernet MAC (0x00026000, 0x800) */
    TangoxNB8800State *nb8800 = g_new0(TangoxNB8800State, 1);
    nb8800->irq = qemu_allocate_irq(tangox_intc_set_irq, cpu_block, 38);
    tangox_nb8800_reset(nb8800);
    qemu_register_reset(tangox_nb8800_reset_cb, nb8800);
    memory_region_init_io(&nb8800->mmio, OBJECT(machine), &tangox_nb8800_ops,
                          nb8800, "tangox.nb8800", NB8800_SIZE);
    memory_region_add_subregion_overlap(address_space_mem, NB8800_BASE,
                                        &nb8800->mmio, 1);

    if (nd_table[0].used) {
        qemu_check_nic_model(&nd_table[0], "nb8800");
        nb8800->conf.macaddr = nd_table[0].macaddr;
        nb8800->conf.peers.ncs[0] = nd_table[0].netdev;
        nb8800->conf.peers.queues = 1;
        nd_table[0].instantiated = 1;
        nb8800->nic = qemu_new_nic(&net_tangox_nb8800_info, &nb8800->conf,
                                   "nb8800", nd_table[0].model, NULL, nb8800);
        qemu_format_nic_info_str(qemu_get_queue(nb8800->nic), nb8800->conf.macaddr.a);
    }


    /* Kernel Loading */
    if (kernel_filename) {
        uint64_t kernel_high;
        long kernel_size;

        /* Attempt ELF loading first */
        kernel_size = load_elf(kernel_filename, NULL,
                               cpu_mips_kseg0_to_phys, NULL,
                               &entry, NULL,
                               &kernel_high, NULL, TARGET_BIG_ENDIAN,
                               EM_MIPS, 1, 0);

        if (kernel_size < 0) {
            uint64_t load_addr = TANGOX_RAM_BASE;
            entry = 0x84000400;
            if (strstr(kernel_filename, "vmlinuz") || strstr(kernel_filename, "zImage")) {
                load_addr = 0x04800000ULL;
                entry = 0x84800000ULL;
            }
            kernel_size = load_image_targphys(kernel_filename, load_addr,
                                              machine->ram_size - (load_addr - TANGOX_RAM_BASE));
            if (kernel_size < 0) {
                error_report("tangox: could not load kernel '%s'", kernel_filename);
                exit(1);
            }
            kernel_high = entry + kernel_size;
            printf("[TANGOX QEMU] Loaded raw binary '%s' (%ld bytes) at 0x%08llx, Entry=0x%08llx\n",
                   kernel_filename, kernel_size, (unsigned long long)load_addr, (unsigned long long)entry);
        }
    }

    reset_data = g_new0(TangoxResetData, 1);
    reset_data->cpu = cpu;
    reset_data->entry = entry;
    reset_data->cmdline = kernel_cmdline;
    qemu_register_reset(tangox_cpu_reset, reset_data);
}

static void tangox_machine_init(MachineClass *mc)
{
    mc->desc = "Popcorn Hour A-210 (Sigma Designs SMP8643)";
    mc->init = tangox_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("74Kf");
    mc->default_ram_id = "tangox.ram";
    mc->default_ram_size = TANGOX_DEFAULT_RAM;
}

DEFINE_MACHINE("tangox", tangox_machine_init)

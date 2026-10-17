#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/addrspace.h>
#include <asm/cacheflush.h>
#include "pch_blackbox.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("In-RAM modern kernel trampoline for SMP8643 (16KB native page allocator + remap-safe WDT)");

/*
 * Stock 2.6.22.19-27-4 kernel on Popcorn Hour A-210 uses CONFIG_PAGE_SIZE_16KB=y
 * (PAGE_SHIFT = 14, 16,384 bytes per page).
 * __get_free_pages(gfp, 0) is exported by the stock kernel and returns the true
 * KSEG0 virtual address (0x80000000 + (pfn << 14)) of a 16KB page in System RAM
 * (0x84000000 .. 0x8FFFFFFF).
 * Adding 0x20000000UL converts KSEG0 (0x84xxxxxx) to uncached KSEG1 (0xA4xxxxxx).
 */
#define STOCK_PAGE_SIZE   16384UL
#define SUBPAGE_SIZE      4096UL
#define KSEG0_TO_KSEG1(va) ((unsigned long)(va) + 0x20000000UL)
#define TARGET_ENTRY      0xA4000400UL
#define SAFE_KSEG0_MIN    0x84800000UL   /* Above first 8MB of DRAM (0x84000000..0x847FFFFF) */

#define MAX_16K_PAGES     1024           /* 1024 * 16KB = 16MB max kernel size */
#define MAX_SUBPAGES      2048           /* 2048 * 4KB  = 8MB max subpages (t0 + t1) */
#define MAX_SKIPPED       1024

extern unsigned long em8xxx_remap_registers[9];
extern void tangox_flush_cache_all(void);
extern void kjump_trampoline(unsigned long *table0, unsigned long *table1,
			     unsigned long count0, unsigned long count1);
extern void kjump_trampoline_end(void);

static unsigned long source_16k_pages[MAX_16K_PAGES];
static unsigned long skipped_16k_pages[MAX_SKIPPED];
static int total_16k_pages = 0;
static int skip_count = 0;

static unsigned long table_page0_va = 0;
static unsigned long table_page1_va = 0;
static unsigned long *source_t0 = NULL;
static unsigned long *source_t1 = NULL;
static int total_subpages = 0;
static int count_t0 = 0;
static int count_t1 = 0;
static size_t kernel_file_size = 0;

static unsigned long tramp_page_va = 0;
static void (*jump_code_ptr)(unsigned long *, unsigned long *,
			     unsigned long, unsigned long) = NULL;

static struct proc_dir_entry *kjump_proc_entry = NULL;

static void pch_restore_boot_remap(void)
{
	volatile u32 *remap = (volatile u32 *)0xA006F000UL;
	volatile u32 *lram_remap = (volatile u32 *)0xA0061940UL;
	int i;

	for (i = 0; i < 8; i++) {
		lram_remap[i] = (u32)em8xxx_remap_registers[i];
		remap[i] = (u32)em8xxx_remap_registers[i];
	}
	iob();
}

static void pch_arm_watchdog(unsigned int seconds)
{
	volatile u32 *wdt_counter = (volatile u32 *)0xA001FD00UL;
	volatile u32 *wdt_config  = (volatile u32 *)0xA001FD04UL;
	u32 ticks = seconds * 27000000UL;

	/* Ensure CPU_remap[0..7] matches YAMON boot state so watchdog reset never hangs YAMON */
	pch_restore_boot_remap();

	*wdt_config  = 0x00000001UL;
	*wdt_counter = ticks;
	iob();

	printk(KERN_INFO "kjump: [WDT] Armed %u seconds (0x%08x ticks), boot remap restored\n",
	       seconds, ticks);
}

static void free_allocated_kernel_pages(void)
{
	int i;
	for (i = 0; i < total_16k_pages; i++) {
		if (source_16k_pages[i]) {
			free_pages(source_16k_pages[i], 0);
			source_16k_pages[i] = 0;
		}
	}
	total_16k_pages = 0;
	total_subpages = 0;
	count_t0 = 0;
	count_t1 = 0;

	if (table_page0_va) { free_pages(table_page0_va, 0); table_page0_va = 0; source_t0 = NULL; }
	if (table_page1_va) { free_pages(table_page1_va, 0); table_page1_va = 0; source_t1 = NULL; }
	if (tramp_page_va)  { free_pages(tramp_page_va, 0);  tramp_page_va = 0;  jump_code_ptr = NULL; }
	kernel_file_size = 0;
}

static inline int is_safe_16k_page(unsigned long k0_va)
{
	if (k0_va < SAFE_KSEG0_MIN || k0_va >= 0x90000000UL)
		return 0;
	if (k0_va >= 0x88000000UL && k0_va < 0x88100000UL)
		return 0;
	return 1;
}

static unsigned long alloc_safe_16k_page(void)
{
	unsigned long va;
	while (1) {
		va = __get_free_pages(GFP_KERNEL | __GFP_NORETRY, 0);
		if (!va) {
			printk(KERN_ERR "kjump: [FATAL] __get_free_pages NULL (OOM)!\n");
			return 0;
		}
		if (is_safe_16k_page(va))
			return va;
		if (skip_count < MAX_SKIPPED) {
			skipped_16k_pages[skip_count++] = va;
		} else {
			printk(KERN_ERR "kjump: [FATAL] exceeded MAX_SKIPPED (%d) at va=0x%08lx!\n",
			       MAX_SKIPPED, va);
			free_pages(va, 0);
			return 0;
		}
	}
}

static int load_kernel_binary(const char *path)
{
	struct file *f;
	mm_segment_t old_fs;
	int i, j, sub;
	loff_t pos = 0;
	size_t remaining;
	unsigned long min_alloc_va = 0xFFFFFFFFUL;
	unsigned long max_alloc_va = 0UL;

	printk(KERN_INFO "kjump: === Starting 16KB-page load sequence for %s ===\n", path);

	free_allocated_kernel_pages();
	skip_count = 0;

	f = filp_open(path, O_RDONLY | O_LARGEFILE | O_NOATIME, 0);
	if (IS_ERR(f)) {
		printk(KERN_ERR "kjump: [ERROR] cannot open %s (err=%ld)\n",
		       path, PTR_ERR(f));
		return PTR_ERR(f);
	}

	kernel_file_size = i_size_read(f->f_path.dentry->d_inode);
	printk(KERN_INFO "kjump: file size: %zu bytes\n", kernel_file_size);

	if (kernel_file_size == 0 || kernel_file_size > MAX_SUBPAGES * SUBPAGE_SIZE) {
		printk(KERN_ERR "kjump: [ERROR] file size %zu out of range\n",
		       kernel_file_size);
		filp_close(f, NULL);
		return -EINVAL;
	}

	total_subpages = (kernel_file_size + SUBPAGE_SIZE - 1) / SUBPAGE_SIZE;
	total_16k_pages = (total_subpages + 3) / 4;
	count_t0 = (total_subpages > 1024) ? 1024 : total_subpages;
	count_t1 = (total_subpages > 1024) ? (total_subpages - 1024) : 0;
	printk(KERN_INFO "kjump: %zu bytes = %d 16KB pages (%d 4KB subpages: t0=%d, t1=%d)\n",
	       kernel_file_size, total_16k_pages, total_subpages, count_t0, count_t1);

	table_page0_va = alloc_safe_16k_page();
	if (!table_page0_va) { filp_close(f, NULL); goto err_free; }
	source_t0 = (unsigned long *)KSEG0_TO_KSEG1(table_page0_va);

	table_page1_va = alloc_safe_16k_page();
	if (!table_page1_va) { filp_close(f, NULL); goto err_free; }
	source_t1 = (unsigned long *)KSEG0_TO_KSEG1(table_page1_va);

	tramp_page_va = alloc_safe_16k_page();
	if (!tramp_page_va) { filp_close(f, NULL); goto err_free; }
	jump_code_ptr = (void *)KSEG0_TO_KSEG1(tramp_page_va);

	for (i = 0; i < total_16k_pages; i++) {
		unsigned long k0_va = alloc_safe_16k_page();
		unsigned long k1_va;
		if (!k0_va) { filp_close(f, NULL); goto err_free; }
		source_16k_pages[i] = k0_va;
		k1_va = KSEG0_TO_KSEG1(k0_va);

		for (sub = 0; sub < 4; sub++) {
			int sub_idx = i * 4 + sub;
			if (sub_idx < total_subpages) {
				unsigned long sub_k1 = k1_va + (sub * SUBPAGE_SIZE);
				if (sub_idx < 1024)
					source_t0[sub_idx] = sub_k1;
				else
					source_t1[sub_idx - 1024] = sub_k1;
			}
		}

		if (k1_va < min_alloc_va) min_alloc_va = k1_va;
		if (k1_va > max_alloc_va) max_alloc_va = k1_va;

		if ((i & 63) == 0)
			cond_resched();
	}

	printk(KERN_INFO "kjump: %d 16KB pages allocated, %d skipped\n",
	       total_16k_pages, skip_count);
	printk(KERN_INFO "kjump: KSEG1 alloc range: 0x%08lx - 0x%08lx\n",
	       min_alloc_va, max_alloc_va);
	printk(KERN_INFO "kjump: trampoline=%p t0=%p t1=%p\n",
	       jump_code_ptr, source_t0, source_t1);

	pch_bb_set_milestone(MS_KJUMP_PAGES_READY);

	/* Release skipped pages immediately */
	for (j = 0; j < skip_count; j++) {
		if (skipped_16k_pages[j]) {
			free_pages(skipped_16k_pages[j], 0);
			skipped_16k_pages[j] = 0;
		}
	}
	skip_count = 0;

	/* Canary test on page 0 */
	{
		volatile u32 *k1 = (volatile u32 *)KSEG0_TO_KSEG1(source_16k_pages[0]);
		*k1 = 0xDEADBEEF;
		if (*k1 != 0xDEADBEEF) {
			printk(KERN_ERR "kjump CANARY FAILED: Memory at %p returned 0x%08x\n", k1, *k1);
			filp_close(f, NULL);
			goto err_free;
		}
		printk(KERN_INFO "kjump CANARY PASS: 0xDEADBEEF at KSEG1 DRAM %p\n", k1);
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	remaining = kernel_file_size;
	pos = 0;

	printk(KERN_INFO "kjump: vfs_read loop starting (%d x 16KB reads)...\n", total_16k_pages);
	pch_arm_watchdog(60);

	for (i = 0; i < total_16k_pages; i++) {
		size_t chunk = (remaining > STOCK_PAGE_SIZE) ? STOCK_PAGE_SIZE : remaining;
		void *buf = (void *)KSEG0_TO_KSEG1(source_16k_pages[i]);
		ssize_t bytes;

		if ((i & 31) == 0) {
			pch_arm_watchdog(60);
			cond_resched();
		}

		bytes = vfs_read(f, buf, chunk, &pos);
		if (bytes != (ssize_t)chunk) {
			printk(KERN_ERR "kjump: read error 16KB page %d (got %ld, want %zu)\n",
			       i, (long)bytes, chunk);
			set_fs(old_fs);
			filp_close(f, NULL);
			goto err_free;
		}
		if (chunk < STOCK_PAGE_SIZE)
			memset((char *)buf + chunk, 0, STOCK_PAGE_SIZE - chunk);

		remaining -= chunk;
	}
	set_fs(old_fs);
	filp_close(f, NULL);

	/* Flush all caches */
	tangox_flush_cache_all();
	mb();

	/* Copy trampoline to uncached KSEG1 */
	{
		size_t tramp_sz = (unsigned long)kjump_trampoline_end -
				  (unsigned long)kjump_trampoline;
		u32 *check;

		memcpy((void *)jump_code_ptr, (void *)kjump_trampoline, tramp_sz);
		mb();
		tangox_flush_cache_all();
		mb();

		check = (u32 *)jump_code_ptr;
		printk(KERN_INFO "kjump: trampoline %zu bytes @ %p: 0x%08x 0x%08x 0x%08x\n",
		       tramp_sz, jump_code_ptr, check[0], check[1], check[2]);
	}

	/* Verify entry opcodes at offset 0x400 */
	{
		unsigned long k0_p0 = source_16k_pages[0];
		volatile u32 *entry = (volatile u32 *)(KSEG0_TO_KSEG1(k0_p0) + 0x400);

		printk(KERN_INFO "kjump DIAG: p0 KSEG0=0x%08lx, KSEG1_DRAM=0x%08lx, t0[0]=0x%08lx\n",
		       k0_p0, KSEG0_TO_KSEG1(k0_p0), source_t0[0]);
		printk(KERN_INFO "kjump ENTRY @ 0x400: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		       entry[0], entry[1], entry[2], entry[3]);

		if (((entry[0] & 0xFC000000) == 0x08000000) && entry[1] == 0x00000000) {
			printk(KERN_INFO "kjump: [SUCCESS] Entry opcodes VERIFIED in physical DDR2 DRAM!\n");
		} else {
			printk(KERN_ERR "kjump: [FAIL] Entry opcodes mismatch! Read 0x%08x 0x%08x\n",
			       entry[0], entry[1]);
			goto err_free;
		}
	}

	mb();
	pch_arm_watchdog(60);
	pch_bb_set_milestone(MS_KJUMP_LOADED);
	printk(KERN_INFO "kjump: === LOAD COMPLETE - ready to jump ===\n");
	return 0;

err_free:
	for (j = 0; j < skip_count; j++) {
		if (skipped_16k_pages[j]) {
			free_pages(skipped_16k_pages[j], 0);
			skipped_16k_pages[j] = 0;
		}
	}
	skip_count = 0;
	free_allocated_kernel_pages();
	return -ENOMEM;
}

static int kjump_read_proc(char *page, char **start, off_t off,
			   int count, int *eof, void *data)
{
	int len = 0;
	volatile u32 *wdt_counter = (volatile u32 *)0xA001FD00UL;
	volatile u32 *wdt_config  = (volatile u32 *)0xA001FD04UL;
	volatile u32 *remap       = (volatile u32 *)0xA006F000UL;
	u32 cur_ticks, cur_cfg;
	int r_i;

	if (off > 0) { *eof = 1; return 0; }

	cur_ticks = *wdt_counter;
	cur_cfg   = *wdt_config;

	len += sprintf(page + len, "=== kjump SMP8643 (16KB page native) ===\n");
	len += sprintf(page + len, "CPU_remap:");
	for (r_i = 0; r_i < 8; r_i++)
		len += sprintf(page + len, " [%d]=0x%08x", r_i, remap[r_i]);
	len += sprintf(page + len, "\n");

	len += sprintf(page + len, "boot_remap:");
	for (r_i = 0; r_i < 9; r_i++)
		len += sprintf(page + len, " [%d]=0x%08lx", r_i, em8xxx_remap_registers[r_i]);
	len += sprintf(page + len, "\n");

	if (cur_ticks > 0 && !(cur_cfg & 0x80000000UL))
		len += sprintf(page + len, "WDT: ARMED (%lu s left)\n",
			       (unsigned long)(cur_ticks / 27000000UL));
	else
		len += sprintf(page + len, "WDT: DISARMED\n");

	if (total_subpages > 0 && jump_code_ptr) {
		len += sprintf(page + len, "Status: LOADED (%zu bytes, %d 16KB pages, %d 4KB subpages)\n",
			       kernel_file_size, total_16k_pages, total_subpages);
		len += sprintf(page + len, "Trampoline: %p\n", jump_code_ptr);
		len += sprintf(page + len, "To jump: echo 1 > /proc/kjump\n");
	} else {
		len += sprintf(page + len, "Status: NOT LOADED\n");
		len += sprintf(page + len, "To load: echo load > /proc/kjump\n");
	}
	*eof = 1;
	return len;
}

#include <linux/reboot.h>
#include <linux/notifier.h>
#include <asm/kdebug.h>

static int kjump_oops_panic_event(struct notifier_block *nb, unsigned long event, void *data)
{
	pch_restore_boot_remap();
	emergency_restart();
	return NOTIFY_DONE;
}

static struct notifier_block kjump_panic_nb = {
	.notifier_call = kjump_oops_panic_event,
	.priority = 200,
};

static struct notifier_block kjump_die_nb = {
	.notifier_call = kjump_oops_panic_event,
	.priority = 200,
};

static int kjump_write_proc(struct file *file, const char __user *buffer,
			    unsigned long count, void *data)
{
	char kbuf[32];
	if (count >= sizeof(kbuf)) return -EINVAL;
	if (copy_from_user(kbuf, buffer, count)) return -EFAULT;
	kbuf[count] = '\0';

	if (strncmp(kbuf, "reboot", 6) == 0) {
		printk(KERN_EMERG "kjump: Triggering immediate kernel emergency_restart()...\n");
		pch_restore_boot_remap();
		emergency_restart();
		return count;
	}

	if (strncmp(kbuf, "load", 4) == 0) {
		int ret = load_kernel_binary("/share/vmlinux.bin");
		if (ret < 0) return ret;
		return count;
	}

	if (strncmp(kbuf, "disarm", 6) == 0) {
		volatile u32 *wdt_config = (volatile u32 *)0xA001FD04UL;
		*wdt_config = 0x80000000UL;
		iob();
		printk(KERN_INFO "kjump: [WDT] Disarmed\n");
		return count;
	}

	if (kbuf[0] == '1') {
		if (total_subpages == 0 || !jump_code_ptr) {
			printk(KERN_ERR "kjump: not loaded!\n");
			return -EINVAL;
		}

		printk(KERN_EMERG "kjump: JUMPING TO LINUX 4.9! Entry=0x%08lx\n",
		       TARGET_ENTRY);

		{
			u32 expected_opcode = *(volatile u32 *)kjump_trampoline;
			u32 actual_opcode   = *(volatile u32 *)jump_code_ptr;
			if (actual_opcode != expected_opcode) {
				printk(KERN_EMERG "kjump: [SECURITY TRAP] Trampoline mismatch! Read 0x%08x, expected 0x%08x\n",
				       actual_opcode, expected_opcode);
				return -EIO;
			}
		}

		local_irq_disable();
		tangox_flush_cache_all();
		pch_arm_watchdog(60);

		jump_code_ptr(source_t0, source_t1, count_t0, count_t1);
		return count;
	}

	return -EINVAL;
}

static int __init kjump_init(void)
{
	printk(KERN_INFO "kjump: SMP8643 In-RAM Kernel Boot Loader v4 (16KB page native + boot remap WDT)\n");
	pch_bb_init(TARGET_ENTRY);
	pch_restore_boot_remap();

	atomic_notifier_chain_register(&panic_notifier_list, &kjump_panic_nb);
	register_die_notifier(&kjump_die_nb);

	kjump_proc_entry = create_proc_entry("kjump", 0666, NULL);
	if (!kjump_proc_entry) {
		printk(KERN_ERR "kjump: failed to create /proc/kjump\n");
		return -ENOMEM;
	}
	kjump_proc_entry->read_proc  = kjump_read_proc;
	kjump_proc_entry->write_proc = kjump_write_proc;
	return 0;
}

static void __exit kjump_exit(void)
{
	unregister_die_notifier(&kjump_die_nb);
	atomic_notifier_chain_unregister(&panic_notifier_list, &kjump_panic_nb);
	if (kjump_proc_entry) remove_proc_entry("kjump", NULL);
	free_allocated_kernel_pages();
	printk(KERN_INFO "kjump: unloaded\n");
}

module_init(kjump_init);
module_exit(kjump_exit);

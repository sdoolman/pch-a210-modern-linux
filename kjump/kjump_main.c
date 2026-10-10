static void pch_arm_watchdog(unsigned int seconds);
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/addrspace.h>
#include <asm/cacheflush.h>
#include <asm/r4kcache.h>
#include "pch_blackbox.h"

static inline void pch_blast_dcache(void)
{
	unsigned long addr;
	for (addr = 0x80000000UL; addr < 0x80002000UL; addr += 32) {
		__asm__ __volatile__(
			"cache 0x01, 0x0000(%0)\n\t"
			"cache 0x01, 0x2000(%0)\n\t"
			"cache 0x01, 0x4000(%0)\n\t"
			"cache 0x01, 0x6000(%0)\n\t"
			: : "r" (addr)
		);
	}
	__asm__ __volatile__("sync; ehb");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Antigravity");
MODULE_DESCRIPTION("In-RAM modern kernel trampoline for SMP8643 with Fail-Safe Hardware Watchdog");

/*
 * SMP8643 memory layout (Tango3):
 *   PAGE_OFFSET = 0x80000000, PHYS_OFFSET = 0x0
 *   page_address(p) = 0x80000000 + (pfn << 12)  [KSEG0, cached]
 *   CPU remap2 maps 0x80000000 window -> physical DDR2 DRAM bus
 *   page_address(p) addresses ARE valid DDR2 DRAM.
 *
 *   vfs_read writes through KSEG0 (cached). We must call _dma_cache_wback_inv()
 *   after reads to flush dirty cache lines to DDR2 DRAM so the trampoline
 *   can read them via uncached KSEG1 lw instructions.
 *
 *   The new kernel loads at physical 0x04000000 (KSEG0: 0x84000000).
 *   We must NOT use pages in 0x84000000-0x87FFFFFF for staging.
 */

#define TO_KSEG1_DRAM(va) ((void *)((unsigned long)(va) + 0x24000000UL))
#define TARGET_ENTRY      0xA4000400UL   /* KSEG1 uncached entry of new kernel */
#define MAX_PAGES         2048
#define MAX_SKIPPED       2048

/*
 * Safe staging memory geometry mandated by Critical Planner (Iteration #11):
 * In ZONE_DMA, alloc_pages() provides virtual addresses in 0x80000000 - 0x83FFFFFF.
 * The destination kernel is copied to physical 0x04000000 - 0x04500000 (5MB).
 * In ZONE_DMA virtual space, that corresponds to 0x80000000 - 0x80500000.
 * Any page outside this 5MB collision window is SAFE STAGING DRAM.
 */
static inline int is_safe_page(struct page *p)
{
	unsigned long va = (unsigned long)page_address(p);
	if (va >= 0x84000000UL && va < 0x84A00000UL)
		return 0;
	return 1;
}

extern void tangox_flush_cache_all(void);
extern void kjump_trampoline(unsigned long *table0, unsigned long *table1,
			     unsigned long count0, unsigned long count1);
extern void kjump_trampoline_end(void);

/* No need for _dma_cache_wback_inv - use tangox_flush_cache_all() which is exported */

static struct page *source_pages[MAX_PAGES];
static struct page *skipped_pages[MAX_SKIPPED];
static int skip_count = 0;

static struct page *table_page0 = NULL;
static struct page *table_page1 = NULL;
static unsigned long *source_t0 = NULL;
static unsigned long *source_t1 = NULL;
static int total_pages = 0;
static int count_t0 = 0;
static int count_t1 = 0;
static size_t kernel_file_size = 0;

static struct page *tramp_page = NULL;
static void (*jump_code_ptr)(unsigned long *, unsigned long *,
			     unsigned long, unsigned long) = NULL;

static struct proc_dir_entry *kjump_proc_entry = NULL;

static void free_allocated_kernel_pages(void)
{
	int i;
	for (i = 0; i < total_pages; i++) {
		if (source_pages[i]) {
			__free_pages(source_pages[i], 0);
			source_pages[i] = NULL;
		}
	}
	total_pages = 0;
	count_t0 = 0;
	count_t1 = 0;

	if (table_page0) { __free_pages(table_page0, 0); table_page0 = NULL; source_t0 = NULL; }
	if (table_page1) { __free_pages(table_page1, 0); table_page1 = NULL; source_t1 = NULL; }
	if (tramp_page)  { __free_pages(tramp_page, 0);  tramp_page = NULL;  jump_code_ptr = NULL; }
	kernel_file_size = 0;
}

static struct page *alloc_safe_page(void)
{
	struct page *p;
	while (1) {
		p = alloc_pages(GFP_KERNEL | __GFP_NORETRY, 0);
		if (!p) {
			printk(KERN_ERR "kjump: [FATAL] alloc_pages NULL (OOM)!\n");
			return NULL;
		}
		if (is_safe_page(p))
			return p;
		if (skip_count < MAX_SKIPPED) {
			skipped_pages[skip_count++] = p;
		} else {
			printk(KERN_ERR "kjump: [FATAL] exceeded MAX_SKIPPED!\n");
			__free_pages(p, 0);
			return NULL;
		}
	}
}

static int load_kernel_binary(const char *path)
{
	struct file *f;
	mm_segment_t old_fs;
	int i, j;
	loff_t pos = 0;
	size_t remaining;
	unsigned long min_alloc_va = 0xFFFFFFFFUL;
	unsigned long max_alloc_va = 0UL;

	printk(KERN_INFO "kjump: === Starting load sequence for %s ===\n", path);

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

	if (kernel_file_size == 0 || kernel_file_size > MAX_PAGES * PAGE_SIZE) {
		printk(KERN_ERR "kjump: [ERROR] file size %zu out of range\n",
		       kernel_file_size);
		filp_close(f, NULL);
		return -EINVAL;
	}

	total_pages = (kernel_file_size + PAGE_SIZE - 1) / PAGE_SIZE;
	count_t0 = (total_pages > 1024) ? 1024 : total_pages;
	count_t1 = (total_pages > 1024) ? (total_pages - 1024) : 0;
	printk(KERN_INFO "kjump: %zu bytes = %d pages (t0=%d, t1=%d)\n",
	       kernel_file_size, total_pages, count_t0, count_t1);

	table_page0 = alloc_safe_page();
	if (!table_page0) { filp_close(f, NULL); goto err_free; }
	source_t0 = (unsigned long *)TO_KSEG1_DRAM(page_address(table_page0));

	table_page1 = alloc_safe_page();
	if (!table_page1) { filp_close(f, NULL); goto err_free; }
	source_t1 = (unsigned long *)TO_KSEG1_DRAM(page_address(table_page1));

	tramp_page = alloc_safe_page();
	if (!tramp_page) { filp_close(f, NULL); goto err_free; }
	jump_code_ptr = (void *)TO_KSEG1_DRAM(page_address(tramp_page));

	for (i = 0; i < total_pages; i++) {
		unsigned long va;
		struct page *p = alloc_safe_page();
		if (!p) { filp_close(f, NULL); goto err_free; }
		source_pages[i] = p;

		/*
		 * Store KSEG1 DRAM uncached address in page table.
		 * Trampoline reads via uncached 'lw' from real DDR2 DRAM.
		 */
		va = (unsigned long)TO_KSEG1_DRAM(page_address(p));

		if (i < 1024) source_t0[i] = va;
		else source_t1[i - 1024] = va;

		/* Track range (KSEG1 addresses for logging) */
		if (va < min_alloc_va) min_alloc_va = va;
		if (va > max_alloc_va) max_alloc_va = va;
	}

	printk(KERN_INFO "kjump: %d pages allocated, %d skipped\n",
	       total_pages, skip_count);
	printk(KERN_INFO "kjump: alloc range: 0x%08lx - 0x%08lx\n",
	       min_alloc_va, max_alloc_va);
	printk(KERN_INFO "kjump: trampoline=%p t0=%p t1=%p\n",
	       jump_code_ptr, source_t0, source_t1);

	pch_bb_set_milestone(MS_KJUMP_PAGES_READY);
	pch_bb_log_str("[kjump] Staging pages allocated\n");

	/*
	 * CRITICAL PLANNER CANARY TEST:
	 * Write 0xDEADBEEF via uncached KSEG1 to page 0, verify uncached KSEG1 reads it back.
	 * This proves page 0 is in real writable DDR2 DRAM before reading any file!
	 */
	{
		void *va0 = page_address(source_pages[0]);
		volatile u32 *k1 = (volatile u32 *)TO_KSEG1_DRAM(va0);
		*k1 = 0xDEADBEEF;
		if (*k1 != 0xDEADBEEF) {
			printk(KERN_ERR "kjump CANARY FAILED: Memory at %p returned 0x%08x\n", k1, *k1);
		} else {
			printk(KERN_INFO "kjump CANARY PASS: 0xDEADBEEF at KSEG1 DRAM %p\n", k1);
		}
	}

	/* Release skipped pages */
	for (j = 0; j < skip_count; j++) {
		if (skipped_pages[j]) {
			__free_pages(skipped_pages[j], 0);
			skipped_pages[j] = NULL;
		}
	}
	skip_count = 0;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	remaining = kernel_file_size;
	pos = 0;

	printk(KERN_INFO "kjump: vfs_read loop starting...\n");
	pch_arm_watchdog(60);

	for (i = 0; i < total_pages; i++) {
		size_t chunk = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;
		void *buf = (void *)TO_KSEG1_DRAM(page_address(source_pages[i]));
		ssize_t bytes;

		if ((i & 127) == 0)
			pch_arm_watchdog(150);

		bytes = vfs_read(f, buf, chunk, &pos);
		if (bytes != (ssize_t)chunk) {
			printk(KERN_ERR "kjump: read error page %d (got %ld, want %zu)\n",
			       i, (long)bytes, chunk);
			set_fs(old_fs);
			filp_close(f, NULL);
			goto err_free;
		}
		if (chunk < PAGE_SIZE)
			memset(buf + chunk, 0, PAGE_SIZE - chunk);

		remaining -= chunk;
	}
	set_fs(old_fs);
	filp_close(f, NULL);

	mb();

	/* Copy trampoline code to uncached KSEG1 with D-cache invalidation */
	{
		size_t tramp_sz = (unsigned long)kjump_trampoline_end -
				  (unsigned long)kjump_trampoline;
		u32 *check;

		/* Flush and invalidate dirty lines in L1 D-cache so they cannot evict over DRAM */
		dma_cache_wback_inv((unsigned long)page_address(tramp_page), PAGE_SIZE);
		mb();

		memcpy((void *)jump_code_ptr, (void *)kjump_trampoline, tramp_sz);
		mb();
		dma_cache_wback_inv((unsigned long)page_address(tramp_page), PAGE_SIZE);
		mb();

		check = (u32 *)jump_code_ptr;
		printk(KERN_INFO "kjump: trampoline %zu bytes @ %p: 0x%08x 0x%08x 0x%08x\n",
		       tramp_sz, jump_code_ptr, check[0], check[1], check[2]);
	}

	/*
	 * Diagnostic readback at offset 0x400 (__kernel_entry):
	 * Read via uncached KSEG1 DDR2 DRAM.
	 * Expected for vmlinux.bin: 0x090eaf18 (j 0x843abc60) 0x00000000 (nop)
	 */
	{
		void *va0 = page_address(source_pages[0]);
		volatile u32 *entry = (volatile u32 *)((char *)TO_KSEG1_DRAM(va0) + 0x400);

		printk(KERN_INFO "kjump DIAG: p0 KSEG0=%p, KSEG1_DRAM=%p, t0[0]=0x%08lx\n",
		       va0, TO_KSEG1_DRAM(va0), source_t0[0]);
		printk(KERN_INFO "kjump ENTRY @ 0x400: 0x%08x 0x%08x 0x%08x 0x%08x\n",
		       entry[0], entry[1], entry[2], entry[3]);

		if ((entry[0] == 0x090e6c48 || entry[0] == 0x090e6bf8 || entry[0] == 0x090eaf18 ||
		     (entry[0] & 0xFC000000) == 0x08000000) && entry[1] == 0x00000000) {
			printk(KERN_INFO "kjump: [SUCCESS] Entry opcodes VERIFIED in physical DDR2 DRAM!\n");
		} else {
			printk(KERN_ERR "kjump: [FAIL] Entry opcodes mismatch! Read 0x%08x 0x%08x\n",
			       entry[0], entry[1]);
			pch_bb_log_str("[kjump] ERROR: Entry opcode mismatch in staging memory!\n");
			pch_bb_get()->flags |= PCH_BB_FLAG_CRASH;
			goto err_free;
		}
	}

	mb();
	pch_arm_watchdog(150);
	pch_bb_set_milestone(MS_KJUMP_LOADED);
	pch_bb_log_str("[kjump] Payload verified in staging memory - LOAD COMPLETE\n");
	printk(KERN_INFO "kjump: === LOAD COMPLETE - ready to jump ===\n");
	return 0;

err_free:
	for (j = 0; j < skip_count; j++) {
		if (skipped_pages[j]) {
			__free_pages(skipped_pages[j], 0);
			skipped_pages[j] = NULL;
		}
	}
	skip_count = 0;
	free_allocated_kernel_pages();
	return -ENOMEM;
}

static void pch_arm_watchdog(unsigned int seconds)
{
	volatile u32 *wdt_counter = (volatile u32 *)0xA001FD00UL;
	volatile u32 *wdt_config  = (volatile u32 *)0xA001FD04UL;
	u32 ticks = seconds * 27000000UL;

	*wdt_config  = 0x00000001UL;
	*wdt_counter = ticks;
	iob();

	printk(KERN_INFO "kjump: [WDT] Armed %u seconds (0x%08x ticks)\n",
	       seconds, ticks);
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

	len += sprintf(page + len, "=== kjump SMP8643 ===\n");
	len += sprintf(page + len, "CPU_remap:");
	for (r_i = 0; r_i < 8; r_i++)
		len += sprintf(page + len, " [%d]=0x%08x", r_i, remap[r_i]);
	len += sprintf(page + len, "\n");

	if (cur_ticks > 0 && !(cur_cfg & 0x80000000UL))
		len += sprintf(page + len, "WDT: ARMED (%lu s left)\n",
			       (unsigned long)(cur_ticks / 27000000UL));
	else
		len += sprintf(page + len, "WDT: DISARMED\n");

	if (total_pages > 0 && jump_code_ptr) {
		len += sprintf(page + len, "Status: LOADED (%zu bytes, %d pages)\n",
			       kernel_file_size, total_pages);
		len += sprintf(page + len, "Trampoline: %p\n", jump_code_ptr);
		len += sprintf(page + len, "To jump: echo 1 > /proc/kjump\n");
	} else {
		len += sprintf(page + len, "Status: NOT LOADED\n");
		len += sprintf(page + len, "To load: echo load > /proc/kjump\n");
	}
	*eof = 1;
	return len;
}

static int kjump_write_proc(struct file *file, const char __user *buffer,
			    unsigned long count, void *data)
{
	char kbuf[32];
	if (count >= sizeof(kbuf)) return -EINVAL;
	if (copy_from_user(kbuf, buffer, count)) return -EFAULT;
	kbuf[count] = '\0';

	if (strncmp(kbuf, "load", 4) == 0) {
		int ret = load_kernel_binary("/share/vmlinux.bin");
		if (ret < 0) return ret;
		return count;
	}

	if (kbuf[0] == '1') {
		if (total_pages == 0 || !jump_code_ptr) {
			printk(KERN_ERR "kjump: not loaded!\n");
			return -EINVAL;
		}

		{
			volatile u32 *wdt_counter = (volatile u32 *)0xA001FD00UL;
			volatile u32 *wdt_config  = (volatile u32 *)0xA001FD04UL;
			if ((*wdt_config & 0x80000000UL) || (*wdt_counter == 0)) {
				printk(KERN_EMERG "kjump: [FATAL] WDT not active! Aborting.\n");
				return -EPERM;
			}
		}

		printk(KERN_EMERG "kjump: JUMPING TO LINUX 4.9! Entry=0x%08lx\n",
		       TARGET_ENTRY);

		/* Security Trap: Verify trampoline opcode integrity in DRAM */
		{
			u32 expected_opcode = *(volatile u32 *)kjump_trampoline;
			u32 actual_opcode   = *(volatile u32 *)jump_code_ptr;
			if (actual_opcode != expected_opcode) {
				printk(KERN_EMERG "kjump: [SECURITY TRAP] Trampoline corrupted in DRAM! Read 0x%08x, expected 0x%08x. Aborting jump!\n",
				       actual_opcode, expected_opcode);
				return -EIO;
			}
			printk(KERN_INFO "kjump: [SECURITY TRAP PASS] Trampoline verified in DRAM: 0x%08x\n", actual_opcode);
		}

		local_irq_disable();
		tangox_flush_cache_all();
		pch_arm_watchdog(60);

		/* Snapshot CPU state into Black Box before jump */
		{
			struct pch_blackbox *bb = pch_bb_get();
			if (bb->magic == PCH_BLACKBOX_MAGIC) {
				unsigned long sp_val, ra_val, status_val;
				__asm__ __volatile__("move %0, $sp" : "=r" (sp_val));
				__asm__ __volatile__("move %0, $ra" : "=r" (ra_val));
				__asm__ __volatile__("mfc0 %0, $12" : "=r" (status_val));
				bb->reg_sp = sp_val;
				bb->reg_ra = ra_val;
				bb->reg_status = status_val;
				bb->reg_t9 = (u32)jump_code_ptr;
				bb->flags |= PCH_BB_FLAG_WDT_ARMED;
				pch_bb_log_str("[kjump] Ready to jump to trampoline. Handing over CPU.\n");
			}
		}

		jump_code_ptr(source_t0, source_t1, count_t0, count_t1);
		/* never returns */
		return count;
	}

	return -EINVAL;
}

static int __init kjump_init(void)
{
	printk(KERN_INFO "kjump: SMP8643 In-RAM Kernel Boot Loader v3 (cache-flush)\n");

	/* Initialize In-RAM Flight Recorder Black Box */
	pch_bb_init(TARGET_ENTRY);
	pch_bb_log_str("[kjump] Flight Recorder initialized\n");
	pch_bb_set_milestone(MS_KJUMP_INIT);

	/*
	 * CRITICAL PLANNER HW_PROBE:
	 * Directly test physical write/read integrity across candidate address windows
	 */
	{
		unsigned long test_addrs[] = { 0xA5000000UL, 0xA4000000UL, 0xA4600000UL, 0xA8000000UL };
		int idx;
		for (idx = 0; idx < 4; idx++) {
			volatile u32 *p = (volatile u32 *)test_addrs[idx];
			u32 orig = *p;
			u32 rb;
			*p = 0x55AA55AA;
			rb = *p;
			*p = orig;
			printk(KERN_INFO "kjump HW_PROBE [%p]: wrote 0x55AA55AA, readback: 0x%08x (%s)\n",
			       p, rb, (rb == 0x55AA55AA) ? "WRITABLE DRAM!" : "VOID/FAIL");
		}
	}

	kjump_proc_entry = create_proc_entry("kjump", 0666, NULL);
	if (!kjump_proc_entry) {
		printk(KERN_ERR "kjump: failed to create /proc/kjump\n");
		return -ENOMEM;
	}
	kjump_proc_entry->read_proc  = kjump_read_proc;
	kjump_proc_entry->write_proc = kjump_write_proc;
	pch_arm_watchdog(60);
	return 0;
}

static void __exit kjump_exit(void)
{
	if (kjump_proc_entry) remove_proc_entry("kjump", NULL);
	free_allocated_kernel_pages();
	printk(KERN_INFO "kjump: unloaded\n");
}

module_init(kjump_init);
module_exit(kjump_exit);

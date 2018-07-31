/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cma.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/profile.h>
#include <linux/notifier.h>
#include <linux/ratelimit.h>
#include <linux/circ_buf.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/vmpressure.h>
#include <linux/freezer.h>
#include <linux/devfreq_boost.h>
#include <linux/cpu_input_boost.h>
#include <linux/compaction.h>
#include <linux/memory.h>

#define CREATE_TRACE_POINTS
#include <trace/events/almk.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#ifdef CONFIG_PROCESS_RECLAIM
#define PROCESS_RECLAIM_ENABLE_LOG
#include <linux/hrtimer.h>
#endif

#define CREATE_TRACE_POINTS
#include "trace/lowmemorykiller.h"

extern int extra_free_kbytes;

static u32 lowmem_debug_level;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};

static int lowmem_adj_size = 6;
static int lowmem_minfree[6] = {
	6 *  512,	/* Foreground App: 	12 MB	*/
	4 * 1024,	/* Visible App: 	16 MB	*/
	8 * 1024,	/* Secondary Server: 	32 MB	*/
	32 * 1024,	/* Hidden App: 		128 MB	*/
	54 * 1024,	/* Content Provider: 	224 MB	*/
	64 * 1024,	/* Empty App: 		256 MB	*/
};

static int lowmem_minfree_size = 6;
static int lmk_fast_run = 1;
/*
 * This parameter tracks the kill count per minfree since boot.
 * the last index is the kills of almk triggers which should not
 * been killed
 */
static int lowmem_per_minfree_count[7];

static short lowmem_direct_adj[6];
static int lowmem_direct_adj_size;
static int lowmem_direct_minfree[6];
static int lowmem_direct_minfree_size;

static u32 lowmem_lmkcount;
static int lmkd_count;
static int lmkd_cricount;

static unsigned long lowmem_deathpending_timeout;

static inline enum compact_result compact_nodes(bool sync)
{
    return COMPACT_CONTINUE;
}

#ifdef CONFIG_PROCESS_RECLAIM
extern ssize_t reclaim_walk_mm(struct task_struct *task, char *type_buf);
#endif

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)


bool lmk_kill_possible(void);
static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;
module_param_named(adj_max_shift, adj_max_shift, short, 0644);

enum {
	ADAPTIVE_LMK_DISABLED = 0,
	ADAPTIVE_LMK_ENABLED,
	ADAPTIVE_LMK_WAS_ENABLED,
};

/* User knob to enable/disable adaptive lmk feature */
static int enable_adaptive_lmk = ADAPTIVE_LMK_ENABLED;
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int, 0444);

/*
 * This parameter controls the behaviour of LMK when vmpressure is in
 * the range of 90-94. Adaptive lmk triggers based on number of file
 * pages wrt vmpressure_file_min, when vmpressure is in the range of
 * 90-94. Usually this is a pseudo minfree value, higher than the
 * highest configured value in minfree array.
 */
static int vmpressure_file_min = 53059;
module_param_named(vmpressure_file_min, vmpressure_file_min, int, 0644);

/* User knob to enable/disable oom reaping feature */
static int oom_reaper = 1;
module_param_named(oom_reaper, oom_reaper, int, 0444);

/* Variable that helps in feed to the reclaim path  */
static atomic64_t lmk_feed = ATOMIC64_INIT(0);

/*
 * This function can be called whether to include the anon LRU pages
 * for accounting in the page reclaim.
 */
bool lmk_kill_possible(void)
{
	unsigned long val = atomic64_read(&lmk_feed);

	return !val || time_after_eq(jiffies, val);
}

enum {
	VMPRESSURE_NO_ADJUST = 0,
	VMPRESSURE_ADJUST_ENCROACH,
	VMPRESSURE_ADJUST_NORMAL,
};

static int adjust_minadj(short *min_score_adj)
{
	int ret = VMPRESSURE_NO_ADJUST;

	if (enable_adaptive_lmk != ADAPTIVE_LMK_ENABLED)
		return 0;

	if (atomic_read(&shift_adj) &&
	    (*min_score_adj > adj_max_shift)) {
		if (*min_score_adj == OOM_SCORE_ADJ_MAX + 1)
			ret = VMPRESSURE_ADJUST_ENCROACH;
		else
			ret = VMPRESSURE_ADJUST_NORMAL;
		*min_score_adj = adj_max_shift;
	}
	atomic_set(&shift_adj, 0);

	return ret;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
				   unsigned long action, void *data)
{
	int other_free, other_file;
	unsigned long pressure = action;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (enable_adaptive_lmk != ADAPTIVE_LMK_ENABLED)
		return 0;

	if (pressure >= 90) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			global_node_page_state(NR_UNEVICTABLE) -
			total_swapcache_pages();
		other_free = global_page_state(NR_FREE_PAGES);

		atomic_set(&shift_adj, 1);
		trace_almk_vmpressure(pressure, other_free, other_file);
	} else if (pressure >= 85) {
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;

		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			global_node_page_state(NR_UNEVICTABLE) -
			total_swapcache_pages();

		other_free = global_page_state(NR_FREE_PAGES);

		if ((other_free < lowmem_minfree[array_size - 1]) &&
		    (other_file < vmpressure_file_min)) {
			atomic_set(&shift_adj, 1);
			trace_almk_vmpressure(pressure, other_free, other_file);
		}
	} else if (atomic_read(&shift_adj)) {
		other_file = global_node_page_state(NR_FILE_PAGES) -
			global_node_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_page_state(NR_FREE_PAGES);
		/*
		 * shift_adj would have been set by a previous invocation
		 * of notifier, which is not followed by a lowmem_shrink yet.
		 * Since vmpressure has improved, reset shift_adj to avoid
		 * false adaptive LMK trigger.
		 */
		trace_almk_vmpressure(pressure, other_free, other_file);
		atomic_set(&shift_adj, 0);
	}

	return 0;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static inline int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		if (test_tsk_thread_flag(t, flag))
			return 1;
	}

	return 0;
}

static void show_memory(void)
{
	unsigned long nr_rbin_free, nr_rbin_pool, nr_rbin_alloc, nr_rbin_file;

	nr_rbin_free = global_page_state(NR_FREE_RBIN_PAGES);
	nr_rbin_pool = atomic_read(&rbin_pool_pages);
	nr_rbin_alloc = atomic_read(&rbin_allocated_pages);
	nr_rbin_file = totalrbin_pages - nr_rbin_free - nr_rbin_pool
					- nr_rbin_alloc;

#define K(x) ((x) << (PAGE_SHIFT - 10))
	printk("Mem-Info:"
		" totalram_pages:%lukB"
		" free:%lukB"
		" active_anon:%lukB"
		" inactive_anon:%lukB"
		" active_file:%lukB"
		" inactive_file:%lukB"
		" unevictable:%lukB"
		" isolated(anon):%lukB"
		" isolated(file):%lukB"
		" dirty:%lukB"
		" writeback:%lukB"
		" mapped:%lukB"
		" shmem:%lukB"
		" slab_reclaimable:%lukB"
		" slab_unreclaimable:%lukB"
		" kernel_stack:%lukB"
		" pagetables:%lukB"
		" free_cma:%lukB"
		" rbin_free:%lukB"
		" rbin_pool:%lukB"
		" rbin_alloc:%lukB"
		" rbin_file:%lukB"
		"\n",
		K(totalram_pages),
		K(global_page_state(NR_FREE_PAGES)),
		K(global_node_page_state(NR_ACTIVE_ANON)),
		K(global_node_page_state(NR_INACTIVE_ANON)),
		K(global_node_page_state(NR_ACTIVE_FILE)),
		K(global_node_page_state(NR_INACTIVE_FILE)),
		K(global_node_page_state(NR_UNEVICTABLE)),
		K(global_node_page_state(NR_ISOLATED_ANON)),
		K(global_node_page_state(NR_ISOLATED_FILE)),
		K(global_node_page_state(NR_FILE_DIRTY)),
		K(global_node_page_state(NR_WRITEBACK)),
		K(global_node_page_state(NR_FILE_MAPPED)),
		K(global_node_page_state(NR_SHMEM)),
		K(global_page_state(NR_SLAB_RECLAIMABLE)),
		K(global_page_state(NR_SLAB_UNRECLAIMABLE)),
		global_page_state(NR_KERNEL_STACK_KB),
		K(global_page_state(NR_PAGETABLE)),
		K(global_page_state(NR_FREE_CMA_PAGES)),
		K(nr_rbin_free),
		K(nr_rbin_pool),
		K(nr_rbin_alloc),
		K(nr_rbin_file)
		);
#undef K
}

static DECLARE_WAIT_QUEUE_HEAD(event_wait);
static DEFINE_SPINLOCK(lmk_event_lock);
static struct circ_buf event_buffer;
#define MAX_BUFFERED_EVENTS 8
#define MAX_TASKNAME 128

struct lmk_event {
	char taskname[MAX_TASKNAME];
	pid_t pid;
	uid_t uid;
	pid_t group_leader_pid;
	unsigned long min_flt;
	unsigned long maj_flt;
	unsigned long rss_in_pages;
	short oom_score_adj;
	short min_score_adj;
	unsigned long long start_time;
	struct list_head list;
};

void handle_lmk_event(struct task_struct *selected, int selected_tasksize,
		      short min_score_adj)
{
	int head;
	int tail;
	struct lmk_event *events;
	struct lmk_event *event;

	spin_lock(&lmk_event_lock);

	head = event_buffer.head;
	tail = READ_ONCE(event_buffer.tail);

	/* Do not continue to log if no space remains in the buffer. */
	if (CIRC_SPACE(head, tail, MAX_BUFFERED_EVENTS) < 1) {
		spin_unlock(&lmk_event_lock);
		return;
	}

	events = (struct lmk_event *) event_buffer.buf;
	event = &events[head];

	strncpy(event->taskname, selected->comm, MAX_TASKNAME);

	event->pid = selected->pid;
	event->uid = from_kuid_munged(current_user_ns(), task_uid(selected));
	if (selected->group_leader)
		event->group_leader_pid = selected->group_leader->pid;
	else
		event->group_leader_pid = -1;
	event->min_flt = selected->min_flt;
	event->maj_flt = selected->maj_flt;
	event->oom_score_adj = selected->signal->oom_score_adj;
	event->start_time = nsec_to_clock_t(selected->real_start_time);
	event->rss_in_pages = selected_tasksize;
	event->min_score_adj = min_score_adj;

	event_buffer.head = (head + 1) & (MAX_BUFFERED_EVENTS - 1);

	spin_unlock(&lmk_event_lock);

	wake_up_interruptible(&event_wait);
}

static int lmk_event_show(struct seq_file *s, void *unused)
{
	struct lmk_event *events = (struct lmk_event *) event_buffer.buf;
	int head;
	int tail;
	struct lmk_event *event;

	spin_lock(&lmk_event_lock);

	head = event_buffer.head;
	tail = event_buffer.tail;

	if (head == tail) {
		spin_unlock(&lmk_event_lock);
		return -EAGAIN;
	}

	event = &events[tail];

	seq_printf(s, "%lu %lu %lu %lu %lu %lu %hd %hd %llu\n%s\n",
		(unsigned long) event->pid, (unsigned long) event->uid,
		(unsigned long) event->group_leader_pid, event->min_flt,
		event->maj_flt, event->rss_in_pages, event->oom_score_adj,
		event->min_score_adj, event->start_time, event->taskname);

	event_buffer.tail = (tail + 1) & (MAX_BUFFERED_EVENTS - 1);

	spin_unlock(&lmk_event_lock);
	return 0;
}

static unsigned int lmk_event_poll(struct file *file, poll_table *wait)
{
	int ret = 0;

	poll_wait(file, &event_wait, wait);
	spin_lock(&lmk_event_lock);
	if (event_buffer.head != event_buffer.tail)
		ret = POLLIN;
	spin_unlock(&lmk_event_lock);
	return ret;
}

static int lmk_event_open(struct inode *inode, struct file *file)
{
	return single_open(file, lmk_event_show, inode->i_private);
}

static const struct file_operations event_file_ops = {
	.open = lmk_event_open,
	.poll = lmk_event_poll,
	.read = seq_read
};

static void lmk_event_init(void)
{
	struct proc_dir_entry *entry;

	event_buffer.head = 0;
	event_buffer.tail = 0;
	event_buffer.buf = kmalloc(
		sizeof(struct lmk_event) * MAX_BUFFERED_EVENTS, GFP_KERNEL);
	if (!event_buffer.buf)
		return;
	entry = proc_create("lowmemorykiller", 0, NULL, &event_file_ops);
	if (!entry)
		pr_err("error creating kernel lmk event file\n");
}

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_node_page_state(NR_ACTIVE_ANON) +
		global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_ANON) +
		global_node_page_state(NR_INACTIVE_FILE);
}

#if defined(CONFIG_ZSWAP)
extern u64 zswap_pool_pages;
extern atomic_t zswap_stored_pages;
#endif

static int test_task_state(struct task_struct *p, int state)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (t->state & state) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static inline int test_task_lmk_waiting(struct task_struct *p)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (task_lmk_waiting(t)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

#ifdef CONFIG_PROCESS_RECLAIM
static int test_task_exit_state(struct task_struct *p, long flag)
{
	struct task_struct *t = p;

	do {
		task_lock(t);
		if (t->exit_state == flag) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	} while_each_thread(p, t);

	return 0;
}
#endif

static DEFINE_MUTEX(scan_mutex);

static int can_use_cma_pages(gfp_t gfp_mask)
{
	int mtype = gfpflags_to_migratetype(gfp_mask);

	/*
	 * Assumes that all types of movable pages can be
	 * served by cma. Fix this if that changes.
	 */
	if (mtype == MIGRATE_MOVABLE)
		return 1;

	return 0;
}

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file,
					int use_cma_pages)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
					NR_ZONE_INACTIVE_FILE) +
					zone_page_state(zone,
					NR_ZONE_ACTIVE_FILE);
		} else if ((zone_idx < classzone_idx) && other_free) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0)) {
				if (!use_cma_pages) {
					*other_free -= min(
					  zone->lowmem_reserve[classzone_idx] +
					  zone_page_state(
					    zone, NR_FREE_CMA_PAGES),
					  zone_page_state(
					    zone, NR_FREE_PAGES));
				} else {
					*other_free -=
					  zone->lowmem_reserve[classzone_idx];
				}
			} else {
				*other_free -=
					   zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

#ifdef CONFIG_HIGHMEM
static void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
		preferred_zone = zref->zone;

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(
					preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
static void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zoneref *zref;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;
	int use_cma_pages;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	zref = first_zones_zonelist(zonelist, high_zoneidx, NULL);
	preferred_zone = zref->zone;
	classzone_idx = zone_idx(preferred_zone);
	use_cma_pages = can_use_cma_pages(gfp_mask);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   100-1) /
			   100);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file, use_cma_pages);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL, use_cma_pages);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			if (!use_cma_pages) {
				*other_free -= min(
				  preferred_zone->lowmem_reserve[_ZONE]
				  + zone_page_state(
				    preferred_zone, NR_FREE_CMA_PAGES),
				  zone_page_state(
				    preferred_zone, NR_FREE_PAGES));
			} else {
				*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
			}
		} else {
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		}

		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file, use_cma_pages);

		if (!use_cma_pages) {
			*other_free -=
			  zone_page_state(preferred_zone, NR_FREE_CMA_PAGES);
		}

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

static DEFINE_RATELIMIT_STATE(lmk_rs, DEFAULT_RATELIMIT_INTERVAL, 1);

/*
 * Return the percent of memory which gfp_mask is allowed to allocate from.
 * CMA memory is assumed to be a small percent and is not considered.
 * The goal is to apply a margin of minfree over all zones, rather than to
 * each zone individually.
 */
static int get_minfree_scalefactor(gfp_t gfp_mask)
{
	struct zonelist *zonelist = node_zonelist(0, gfp_mask);
	struct zoneref *z;
	struct zone *zone;
	unsigned long nr_usable = 0;

	for_each_zone_zonelist(zone, z, zonelist, gfp_zone(gfp_mask))
		nr_usable += zone->managed_pages;

	return max_t(int, 1, mult_frac(100, nr_usable, totalram_pages));
}

static void mark_lmk_victim(struct task_struct *tsk)
{
	struct mm_struct *mm = tsk->mm;

	if (!cmpxchg(&tsk->signal->oom_mm, NULL, mm)) {
		atomic_inc(&tsk->signal->oom_mm->mm_count);
		set_bit(MMF_OOM_VICTIM, &mm->flags);
	}
}

#ifdef CONFIG_ANDROID_LMK_ADJ_RBTREE
static inline struct task_struct *pick_next_from_adj_tree(struct task_struct *task);
static inline struct task_struct *pick_first_task(void);
static inline struct task_struct *pick_last_task(void);
#endif

static bool avoid_to_kill(uid_t uid)
{
	/* 
	 * uid info
	 * uid == 0 > root
	 * uid == 1001 > radio
	 * uid == 1002 > bluetooth
	 * uid == 1010 > wifi
	 * uid == 1014 > dhcp
	 */
	if (uid == 0 || uid == 1001 || uid == 1002 || uid == 1010 ||
			uid == 1014)
		return 1;
	return 0;
}

static bool protected_apps(char *comm)
{
	if (strcmp(comm, "android.process.acore") == 0 ||
			strcmp(comm, "com.android.systemui") == 0 ||
			strcmp(comm, "com.topjohnwu.magisk") == 0 ||
			strcmp(comm, "com.google.android.gms") == 0 ||
			strcmp(comm, "ch.deletescape.lawnchair.plah") == 0 ||
			strcmp(comm, "com.android.phone") == 0 ||
			strcmp(comm, "com.samsung.android.contacts") == 0 ||
			strcmp(comm, "ndroid.contacts") == 0 ||
			strcmp(comm, "system:ui") == 0)
		return 1;
	return 0;
}

static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	static const struct sched_param sched_zero_prio;
	const struct cred *pcred;
	unsigned int uid = 0;
	unsigned long rem = 0;
	int tasksize;
	int i;
	int ret = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int scale_percent;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	unsigned long nr_cma_free;
#ifdef CONFIG_RBIN
	unsigned long nr_rbin_free, nr_rbin_pool, nr_rbin_alloc, nr_rbin_file;
#endif
	int migratetype;
#if defined(CONFIG_ZSWAP)
	int zswap_stored_pages_temp;
	int swap_rss;
	int selected_swap_rss;
#endif
	int minfree_count_offset = 0;
	int array_count = ARRAY_SIZE(lowmem_per_minfree_count);


	bool lock_required = true;

	other_free = global_page_state(NR_FREE_PAGES) - totalreserve_pages;

	if (global_node_page_state(NR_SHMEM) + global_node_page_state(NR_UNEVICTABLE) + total_swapcache_pages() +
			global_node_page_state(NR_UNEVICTABLE) <
			global_node_page_state(NR_FILE_PAGES))
		other_file = global_node_page_state(NR_FILE_PAGES) -
					global_node_page_state(NR_SHMEM) -
					global_node_page_state(NR_UNEVICTABLE) -
					total_swapcache_pages();
	else
		other_file = 0;

	nr_cma_free = global_page_state(NR_FREE_CMA_PAGES);
	migratetype = gfpflags_to_migratetype(sc->gfp_mask);
	if (!((migratetype == MIGRATE_MOVABLE) &&
		((sc->gfp_mask & GFP_HIGHUSER_MOVABLE) == GFP_HIGHUSER_MOVABLE)))
		other_free -= nr_cma_free;
#ifdef CONFIG_RBIN
	if ((sc->gfp_mask & __GFP_RBIN) != __GFP_RBIN) {
		nr_rbin_free = global_page_state(NR_FREE_RBIN_PAGES);
		nr_rbin_pool = atomic_read(&rbin_pool_pages);
		nr_rbin_alloc = atomic_read(&rbin_allocated_pages);
		nr_rbin_file = totalrbin_pages - nr_rbin_free - nr_rbin_pool
						- nr_rbin_alloc;
		other_free -= nr_rbin_free;
		other_file -= nr_rbin_file;
	}
#endif

	if (!get_nr_swap_pages() && (other_free <= lowmem_minfree[0] >> 1) &&
	    (other_file <= lowmem_minfree[0] >> 1))
		lock_required = false;

	if (likely(lock_required) && !mutex_trylock(&scan_mutex))
		return 0;

	tune_lmk_param(&other_free, &other_file, sc);
	scale_percent = get_minfree_scalefactor(sc->gfp_mask);

	rcu_read_lock();
	tsk = current->group_leader;
	if ((tsk->flags & PF_EXITING) && test_task_flag(tsk, TIF_MEMDIE)) {
		set_tsk_thread_flag(current, TIF_MEMDIE);
		rcu_read_unlock();
		return 0;
	}
	rcu_read_unlock();


	if (!current_is_kswapd() && is_mem_boost_high() &&
			lowmem_direct_minfree_size && lowmem_direct_adj_size) {
		array_size = ARRAY_SIZE(lowmem_direct_adj);
		if (lowmem_direct_adj_size < array_size)
			array_size = lowmem_direct_adj_size;
		if (lowmem_direct_minfree_size < array_size)
			array_size = lowmem_direct_minfree_size;
		for (i = 0; i < array_size; i++) {
			minfree = mult_frac(lowmem_direct_minfree[i], scale_percent, 100) +
			  ((extra_free_kbytes * 1024) / PAGE_SIZE);
			if (other_free + other_file < minfree) {
				min_score_adj = lowmem_direct_adj[i];
				break;
			}
		}
	} else {
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;
		for (i = 0; i < array_size; i++) {
			minfree = mult_frac(lowmem_minfree[i], scale_percent, 100) +
			  ((extra_free_kbytes * 1024) / PAGE_SIZE);
			if (other_free + other_file < minfree) {
				min_score_adj = lowmem_adj[i];
				break;
			}
		}
	}

	ret = adjust_minadj(&min_score_adj);
	if (ret == VMPRESSURE_ADJUST_ENCROACH) {
		minfree_count_offset = array_count-1;
	}

	lowmem_print(3, "lowmem_scan %lu, %x, ofree %d %d, ma %hd\n",
		     sc->nr_to_scan, sc->gfp_mask, other_free,
		     other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		trace_almk_shrink(0, ret, other_free, other_file, 0);
		lowmem_print(5, "lowmem_scan %lu, %x, return 0\n",
			     sc->nr_to_scan, sc->gfp_mask);
		if (lock_required)
			mutex_unlock(&scan_mutex);
		return SHRINK_STOP;
	}

	selected_oom_score_adj = min_score_adj;

	cpu_input_boost_kick_general(250);
	cpu_input_boost_kick_max(500);
	devfreq_boost_kick_max(DEVFREQ_EXYNOS_MIF, 500);

	rcu_read_lock();
#ifdef CONFIG_ANDROID_LMK_ADJ_RBTREE
	for (tsk = pick_first_task();
		tsk != pick_last_task() && tsk != NULL;
		tsk = pick_next_from_adj_tree(tsk)) {
#else
	for_each_process(tsk) {
#endif
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		if (same_thread_group(tsk, current))
			continue;

		if (test_task_flag(tsk, TIF_MEMALLOC))

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		if (oom_reaper) {
			p = find_lock_task_mm(tsk);
			if (!p)
				continue;

			if (test_bit(MMF_OOM_VICTIM, &p->mm->flags)) {
				if (test_bit(MMF_OOM_SKIP, &p->mm->flags)) {
					task_unlock(p);
					continue;
				} else if (time_before_eq(jiffies,
						lowmem_deathpending_timeout)) {
					task_unlock(p);
					rcu_read_unlock();
					if (lock_required)
						mutex_unlock(&scan_mutex);
					if (same_thread_group(current, tsk))
						set_tsk_thread_flag(current,
								    TIF_MEMDIE);
					return 0;
				}
			}
		} else {
			if (time_before_eq(jiffies,
					   lowmem_deathpending_timeout))
				if (test_task_lmk_waiting(tsk)) {
#ifdef CONFIG_PROCESS_RECLAIM
#ifdef PROCESS_RECLAIM_ENABLE_LOG
				ktime_t reclaim_before;
				ktime_t reclaim_after;
				ktime_t reclaim_diff;
				long free_before_kb;
				long free_after_kb;
				long file_before_kb;
				long file_after_kb;
#endif
				bool rcu_locked = true;
				/*
				 if task is ZOMBIE, skip the task
				 */
				if (test_task_exit_state(tsk, EXIT_ZOMBIE))
						continue;

				p = find_lock_task_mm(tsk);
				if (!p)
					continue;
				task_unlock(p);

				/* if task is reclaimed */
				if (test_task_flag(p, TIF_MM_RECLAIMED))
					goto exit_timeout;

				get_task_struct(p);
				set_tsk_thread_flag(p, TIF_MM_RECLAIMED);
					rcu_read_unlock();
					rcu_locked = false;

#ifdef PROCESS_RECLAIM_ENABLE_LOG
				reclaim_before = ktime_get_boottime();
				free_before_kb = global_page_state(NR_FREE_PAGES) *
					(long)(PAGE_SIZE / 1024);
				file_before_kb = global_page_state(NR_FILE_PAGES) *
					(long)(PAGE_SIZE / 1024);
#endif

				if (reclaim_walk_mm(p, "file") < 0)
					clear_tsk_thread_flag(p, TIF_MM_RECLAIMED);

#ifdef PROCESS_RECLAIM_ENABLE_LOG
				reclaim_after = ktime_get_boottime();
				reclaim_diff = ktime_sub(reclaim_after, reclaim_before);

				free_after_kb = global_page_state(NR_FREE_PAGES) *
					(long)(PAGE_SIZE / 1024);
				file_after_kb = global_page_state(NR_FILE_PAGES) *
					(long)(PAGE_SIZE / 1024);

				pr_err("LMK::reclaim_walk_mm() time, %ld, us, " \
						"free inc, %ld, kb, file cache dec, %ld, kb \n",
						(long)ktime_to_ns(reclaim_diff) / 1000,
						free_after_kb - free_before_kb,
						file_after_kb - file_before_kb);
#endif

				put_task_struct(p);
exit_timeout:
				if (rcu_locked)
					rcu_read_unlock();
#else
				rcu_read_unlock();
#endif
					if (lock_required)
						mutex_unlock(&scan_mutex);
					if (same_thread_group(current, tsk))
						set_tsk_thread_flag(current,
								    TIF_MEMDIE);
					return 0;
				}

			p = find_lock_task_mm(tsk);
			if (!p)
				continue;
		}

		if (p->state & TASK_UNINTERRUPTIBLE) {
			task_unlock(p);
			continue;
		}

		oom_score_adj = p->signal->oom_score_adj;
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
#ifdef CONFIG_ANDROID_LMK_ADJ_RBTREE
			break;
#else
			continue;
#endif
		}
		if (fatal_signal_pending(p) ||
				((p->flags & PF_EXITING) &&
					test_tsk_thread_flag(p, TIF_MEMDIE))) {
			lowmem_print(2, "skip slow dying process %d\n", p->pid);
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
#if defined(CONFIG_ZSWAP)
		zswap_stored_pages_temp = atomic_read(&zswap_stored_pages);
		if (zswap_stored_pages_temp) {
			lowmem_print(3, "shown tasksize : %d\n", tasksize);
			swap_rss = (int)zswap_pool_pages
					* get_mm_counter(p->mm, MM_SWAPENTS)
					/ zswap_stored_pages_temp;
			tasksize += swap_rss;
			lowmem_print(3, "real tasksize : %d\n", tasksize);
		} else {
			swap_rss = 0;
		}
#endif
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		if (selected) {
			if (oom_score_adj < selected_oom_score_adj)
#ifdef CONFIG_ANDROID_LMK_ADJ_RBTREE
				break;
#else
				continue;
#endif
			if (oom_score_adj == selected_oom_score_adj &&
			    tasksize <= selected_tasksize)
				continue;
		}
#if defined(CONFIG_ZSWAP)
		selected_swap_rss = swap_rss;
#endif
		pcred = __task_cred(p);
		uid = pcred->uid.val;
		if (avoid_to_kill(uid) || protected_apps(p->comm)){
			if (tasksize * (long)(PAGE_SIZE / 1024) >= 100000){
				selected = p;
				selected_tasksize = tasksize;
				selected_oom_score_adj = oom_score_adj;
				lowmem_print(3, "select protected %d (%s), adj %hd, size %d, to kill\n",
				     	p->pid, p->comm, oom_score_adj, tasksize);
			} else
			lowmem_print(3, "skip protected %d (%s), adj %hd, size %d, to kill\n",
			     	p->pid, p->comm, oom_score_adj, tasksize);
		} else {
			selected = p;
			selected_tasksize = tasksize;
			selected_oom_score_adj = oom_score_adj;
			lowmem_print(3, "select %d (%s), adj %hd, size %d, to kill\n",
			     	p->pid, p->comm, oom_score_adj, tasksize);
		}
	}
	if (selected) {
		long cache_size = other_file * (long)(PAGE_SIZE / 1024);
		long cache_limit = minfree * (long)(PAGE_SIZE / 1024);
		long free = other_free * (long)(PAGE_SIZE / 1024);
#if defined(CONFIG_ZSWAP)
		int orig_tasksize = selected_tasksize - selected_swap_rss;
#endif

		atomic64_set(&lmk_feed, 0);
		if (test_task_lmk_waiting(selected) &&
		    (test_task_state(selected, TASK_UNINTERRUPTIBLE))) {
			lowmem_print(2, "'%s' (%d) is already killed\n",
				     selected->comm,
				     selected->pid);
			rcu_read_unlock();
			if (lock_required)
				mutex_unlock(&scan_mutex);
			return 0;
		}

		task_lock(selected);
		send_sig(SIGKILL, selected, 0);
		sched_setscheduler_nocheck(selected, SCHED_RR, &sched_zero_prio);
		set_cpus_allowed_ptr(selected, cpu_all_mask);
		if (selected->mm) {
			task_set_lmk_waiting(selected);
			if (!test_bit(MMF_OOM_SKIP, &selected->mm->flags) &&
			    oom_reaper) {
				mark_lmk_victim(selected);
				wake_oom_reaper(selected);
			}
		}
		task_unlock(selected);
		trace_lowmemory_kill(selected, cache_size, cache_limit, free);
		lowmem_per_minfree_count[minfree_count_offset]++;
		lowmem_print(1, "Killing '%s' (%d) (tgid %d), adj %hd,\n"
#if defined(CONFIG_ZSWAP)
				 "   to free %ldkB (%ldKB %ldKB) on behalf of '%s' (%d) because\n"
#else
				 "   to free %ldkB on behalf of '%s' (%d) because\n"
#endif
				 "   cache %ldkB is below limit %ldkB for oom_score_adj %hd\n"
				 "   Free memory is %ldkB above reserved\n"
				 "   Free CMA is %ldkB\n"
				 "   GFP mask is %#x(%pGg)\n",
			     selected->comm, selected->pid, selected->tgid,
			     selected_oom_score_adj,
#if defined(CONFIG_ZSWAP)
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     orig_tasksize * (long)(PAGE_SIZE / 1024),
			     selected_swap_rss * (long)(PAGE_SIZE / 1024),
#else
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
#endif
			     current->comm, current->pid,
			     cache_size, cache_limit,
			     min_score_adj,
			     free,
			     nr_cma_free * (long)(PAGE_SIZE / 1024),
			     sc->gfp_mask, &sc->gfp_mask);
		show_mem_extra_call_notifiers();
		show_memory();
		if ((selected_oom_score_adj <= 100) && (__ratelimit(&lmk_rs)))
				dump_tasks(NULL, NULL);

		lowmem_deathpending_timeout = jiffies + HZ;
		rem += selected_tasksize;
		lowmem_lmkcount++;

		get_task_struct(selected);
		rcu_read_unlock();
		/* give the system time to free up the memory */
		msleep_interruptible(20);
		trace_almk_shrink(selected_tasksize, ret,
				  other_free, other_file,
				  selected_oom_score_adj);
	} else {
		trace_almk_shrink(1, ret, other_free, other_file, 0);
		rcu_read_unlock();
		if (other_free < lowmem_minfree[0] &&
		    other_file < lowmem_minfree[0])
			atomic64_set(&lmk_feed, jiffies + HZ);
		else
			atomic64_set(&lmk_feed, 0);

	}

	lowmem_print(4, "lowmem_scan %lu, %x, return %lu\n",
		     sc->nr_to_scan, sc->gfp_mask, rem);
	if (lock_required)
		mutex_unlock(&scan_mutex);

	if (!rem)
		rem = SHRINK_STOP;

	if (selected) {
		handle_lmk_event(selected, selected_tasksize, min_score_adj);
		put_task_struct(selected);
		compact_nodes(false);
	}
	return rem;
}

#ifdef CONFIG_ANDROID_LMK_ADJ_RBTREE
static DEFINE_SPINLOCK(lmk_lock);
static struct rb_root tasks_scoreadj = RB_ROOT;
void add_2_adj_tree(struct task_struct *task)
{
	struct rb_node **link;
	struct rb_node *parent = NULL;
	struct task_struct *task_entry;
	s64 key = task->signal->oom_score_adj;

	/*
	 * Find the right place in the rbtree:
	 */
	spin_lock(&lmk_lock);
	link =  &tasks_scoreadj.rb_node;
	while (*link) {
		parent = *link;
		task_entry = rb_entry(parent, struct task_struct, adj_node);

		if (key < task_entry->signal->oom_score_adj)
			link = &parent->rb_right;
		else
			link = &parent->rb_left;
	}

	rb_link_node(&task->adj_node, parent, link);
	rb_insert_color(&task->adj_node, &tasks_scoreadj);
	spin_unlock(&lmk_lock);
}

void delete_from_adj_tree(struct task_struct *task)
{
	spin_lock(&lmk_lock);
	if (!RB_EMPTY_NODE(&task->adj_node)) {
		rb_erase(&task->adj_node, &tasks_scoreadj);
		RB_CLEAR_NODE(&task->adj_node);
	}
	spin_unlock(&lmk_lock);
}

static struct task_struct *pick_next_from_adj_tree(struct task_struct *task)
{
	struct rb_node *next;

	spin_lock(&lmk_lock);
	next = rb_next(&task->adj_node);
	spin_unlock(&lmk_lock);

	if (!next)
		return NULL;

	return rb_entry(next, struct task_struct, adj_node);
}

static struct task_struct *pick_first_task(void)
{
	struct rb_node *left;

	spin_lock(&lmk_lock);
	left = rb_first(&tasks_scoreadj);
	spin_unlock(&lmk_lock);

	if (!left)
		return NULL;

	return rb_entry(left, struct task_struct, adj_node);
}
static struct task_struct *pick_last_task(void)
{
	struct rb_node *right;

	spin_lock(&lmk_lock);
	right = rb_last(&tasks_scoreadj);
	spin_unlock(&lmk_lock);

	if (!right)
		return NULL;

	return rb_entry(right, struct task_struct, adj_node);
}
#endif

static int lmk_hotplug_callback(struct notifier_block *self,
				unsigned long action, void *arg)
{
	switch (action) {
	case MEM_GOING_OFFLINE:
		if (enable_adaptive_lmk == ADAPTIVE_LMK_ENABLED)
			enable_adaptive_lmk = ADAPTIVE_LMK_WAS_ENABLED;
		break;
	case MEM_OFFLINE:
		if (enable_adaptive_lmk == ADAPTIVE_LMK_WAS_ENABLED)
			enable_adaptive_lmk = ADAPTIVE_LMK_ENABLED;
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = 32
};

#ifdef CONFIG_ANDROID_BG_SCAN_MEM
static int lmk_task_migration_notify(struct notifier_block *nb,
					unsigned long data, void *arg)
{
	struct shrink_control sc = {
		.gfp_mask = GFP_KERNEL,
		.nr_to_scan = 1,
	};

	lowmem_scan(&lowmem_shrinker, &sc);

	return NOTIFY_OK;
}

static struct notifier_block tsk_migration_nb = {
	.notifier_call = lmk_task_migration_notify,
};
#endif

static struct notifier_block lmk_memory_callback_nb = {
	.notifier_call = lmk_hotplug_callback,
	.priority = 0,
};

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	lmk_event_init();
#ifdef CONFIG_ANDROID_BG_SCAN_MEM
	raw_notifier_chain_register(&bgtsk_migration_notifier_head,
					&tsk_migration_nb);
#endif
	vmpressure_notifier_register(&lmk_vmpr_nb);
	if (register_hotmemory_notifier(&lmk_memory_callback_nb))
		lowmem_print(1, "Registering memory hotplug notifier failed\n");
	return 0;
}

static void __exit lowmem_exit(void)
{
		unregister_shrinker(&lowmem_shrinker);
#ifdef CONFIG_ANDROID_BG_SCAN_MEM
	raw_notifier_chain_unregister(&bgtsk_migration_notifier_head,
					&tsk_migration_nb);
#endif
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(short *lowmem_adj, int array_size,
					     int lowmem_adj_size)
{
	int i;
	short oom_adj;
	short oom_score_adj;

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %hd => oom_score_adj %hd\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	int array_size = ARRAY_SIZE(lowmem_adj);

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values(lowmem_adj, array_size,
					 lowmem_adj_size);

	return ret;
}

static int lowmem_direct_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;
	int array_size = ARRAY_SIZE(lowmem_direct_adj);

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values(lowmem_direct_adj, array_size,
					 lowmem_direct_adj_size);

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};

static struct kernel_param_ops lowmem_direct_adj_array_ops = {
	.set = lowmem_direct_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_direct_arr_adj = {
	.max = ARRAY_SIZE(lowmem_direct_adj),
	.num = &lowmem_direct_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_direct_adj[0]),
	.elem = lowmem_direct_adj,
};
#endif

/*
 * not really modular, but the easiest way to keep compat with existing
 * bootargs behaviour is to continue using module_param here.
 */
module_param_named(cost, lowmem_shrinker.seeks, int, 0644);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		.arr = &__param_arr_adj,
		0644);
__MODULE_PARM_TYPE(adj, "array of short");
module_param_cb(direct_adj, &lowmem_direct_adj_array_ops,
		.arr = &__param_direct_arr_adj,
		S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(direct_adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size, 0644);
module_param_array_named(direct_adj, direct_lowmem_adj, short, &lowmem_direct_adj_size,
			 0644);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_array_named(lmk_count, lowmem_per_minfree_count, uint, NULL,
			 S_IRUGO);
module_param_array_named(direct_minfree, lowmem_direct_minfree, uint,
			 &lowmem_direct_minfree_size, 0644);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, 0444);
module_param_named(lmkcount, lowmem_lmkcount, uint, 0444);
module_param_named(lmkd_count, lmkd_count, int, 0644);
module_param_named(lmkd_cricount, lmkd_cricount, int, 0644);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

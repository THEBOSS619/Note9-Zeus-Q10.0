/*
 * Deadline Scheduling Class (SCHED_DEADLINE)
 *
 * Earliest Deadline First (EDF) + Constant Bandwidth Server (CBS).
 *
 * Tasks that periodically executes their instances for less than their
 * runtime won't miss any of their deadlines.
 * Tasks that are not periodic or sporadic or that tries to execute more
 * than their reserved bandwidth will be slowed down (and may potentially
 * miss some of their deadlines), and won't affect any other task.
 *
 * Copyright (C) 2012 Dario Faggioli <raistlin@linux.it>,
 *                    Juri Lelli <juri.lelli@gmail.com>,
 *                    Michael Trimarchi <michael@amarulasolutions.com>,
 *                    Fabio Checconi <fchecconi@gmail.com>
 */
#include "sched.h"

#include <linux/slab.h>
#include "pelt.h"

#include "walt.h"

struct dl_bandwidth def_dl_bandwidth;

static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
	return container_of(dl_se, struct task_struct, dl);
}

static inline struct rq *rq_of_dl_rq(struct dl_rq *dl_rq)
{
	return container_of(dl_rq, struct rq, dl);
}

static inline struct dl_rq *dl_rq_of_se(struct sched_dl_entity *dl_se)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = task_rq(p);

	return &rq->dl;
}

static inline int on_dl_rq(struct sched_dl_entity *dl_se)
{
	return !RB_EMPTY_NODE(&dl_se->rb_node);
}

static void add_average_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	u64 se_bw = dl_se->dl_bw;

	dl_rq->avg_bw += se_bw;
}

static void clear_average_bw(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	u64 se_bw = dl_se->dl_bw;

	dl_rq->avg_bw -= se_bw;
	if (dl_rq->avg_bw < 0) {
		WARN_ON(1);
		dl_rq->avg_bw = 0;
	}
}

static inline
void add_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_held(&(rq_of_dl_rq(dl_rq))->lock);
	dl_rq->running_bw += dl_bw;
	SCHED_WARN_ON(dl_rq->running_bw < old); /* overflow */
	SCHED_WARN_ON(dl_rq->running_bw > dl_rq->this_bw);
}

static inline
void sub_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->running_bw;

	lockdep_assert_held(&(rq_of_dl_rq(dl_rq))->lock);
	dl_rq->running_bw -= dl_bw;
	SCHED_WARN_ON(dl_rq->running_bw > old); /* underflow */
	if (dl_rq->running_bw > old)
		dl_rq->running_bw = 0;
}

static inline
void add_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_held(&(rq_of_dl_rq(dl_rq))->lock);
	dl_rq->this_bw += dl_bw;
	SCHED_WARN_ON(dl_rq->this_bw < old); /* overflow */
}

static inline
void sub_rq_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
	u64 old = dl_rq->this_bw;

	lockdep_assert_held(&(rq_of_dl_rq(dl_rq))->lock);
	dl_rq->this_bw -= dl_bw;
	SCHED_WARN_ON(dl_rq->this_bw > old); /* underflow */
	if (dl_rq->this_bw > old)
		dl_rq->this_bw = 0;
	SCHED_WARN_ON(dl_rq->running_bw > dl_rq->this_bw);
}

void dl_change_utilization(struct task_struct *p, u64 new_bw)
{
	struct rq *rq;

	if (task_on_rq_queued(p))
		return;

	rq = task_rq(p);
	if (p->dl.dl_non_contending) {
		sub_running_bw(p->dl.dl_bw, &rq->dl);
		p->dl.dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be cancelled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
			put_task_struct(p);
	}
	sub_rq_bw(p->dl.dl_bw, &rq->dl);
	add_rq_bw(new_bw, &rq->dl);
}

/*
 * The utilization of a task cannot be immediately removed from
 * the rq active utilization (running_bw) when the task blocks.
 * Instead, we have to wait for the so called "0-lag time".
 *
 * If a task blocks before the "0-lag time", a timer (the inactive
 * timer) is armed, and running_bw is decreased when the timer
 * fires.
 *
 * If the task wakes up again before the inactive timer fires,
 * the timer is cancelled, whereas if the task wakes up after the
 * inactive timer fired (and running_bw has been decreased) the
 * task's utilization has to be added to running_bw again.
 * A flag in the deadline scheduling entity (dl_non_contending)
 * is used to avoid race conditions between the inactive timer handler
 * and task wakeups.
 *
 * The following diagram shows how running_bw is updated. A task is
 * "ACTIVE" when its utilization contributes to running_bw; an
 * "ACTIVE contending" task is in the TASK_RUNNING state, while an
 * "ACTIVE non contending" task is a blocked task for which the "0-lag time"
 * has not passed yet. An "INACTIVE" task is a task for which the "0-lag"
 * time already passed, which does not contribute to running_bw anymore.
 *                              +------------------+
 *             wakeup           |    ACTIVE        |
 *          +------------------>+   contending     |
 *          | add_running_bw    |                  |
 *          |                   +----+------+------+
 *          |                        |      ^
 *          |                dequeue |      |
 * +--------+-------+                |      |
 * |                |   t >= 0-lag   |      | wakeup
 * |    INACTIVE    |<---------------+      |
 * |                | sub_running_bw |      |
 * +--------+-------+                |      |
 *          ^                        |      |
 *          |              t < 0-lag |      |
 *          |                        |      |
 *          |                        V      |
 *          |                   +----+------+------+
 *          | sub_running_bw    |    ACTIVE        |
 *          +-------------------+                  |
 *            inactive timer    |  non contending  |
 *            fired             +------------------+
 *
 * The task_non_contending() function is invoked when a task
 * blocks, and checks if the 0-lag time already passed or
 * not (in the first case, it directly updates running_bw;
 * in the second case, it arms the inactive timer).
 *
 * The task_contending() function is invoked when a task wakes
 * up, and checks if the task is still in the "ACTIVE non contending"
 * state or not (in the second case, it updates running_bw).
 */
static void task_non_contending(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct hrtimer *timer = &dl_se->inactive_timer;
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);
	s64 zerolag_time;

	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
	if (dl_se->dl_runtime == 0)
		return;

	WARN_ON(hrtimer_active(&dl_se->inactive_timer));
	WARN_ON(dl_se->dl_non_contending);

	zerolag_time = dl_se->deadline -
		 div64_long((dl_se->runtime * dl_se->dl_period),
			dl_se->dl_runtime);

	/*
	 * Using relative times instead of the absolute "0-lag time"
	 * allows to simplify the code
	 */
	zerolag_time -= rq_clock(rq);

	/*
	 * If the "0-lag time" already passed, decrease the active
	 * utilization now, instead of starting a timer
	 */
	if (zerolag_time < 0) {
		if (dl_task(p))
			sub_running_bw(dl_se->dl_bw, dl_rq);
		if (!dl_task(p) || p->state == TASK_DEAD) {
			struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

			if (p->state == TASK_DEAD)
				sub_rq_bw(p->dl.dl_bw, &rq->dl);
			raw_spin_lock(&dl_b->lock);
			__dl_clear(dl_b, p->dl.dl_bw);
			__dl_clear_params(p);
			raw_spin_unlock(&dl_b->lock);
		}

		return;
	}

	dl_se->dl_non_contending = 1;
	get_task_struct(p);
	hrtimer_start(timer, ns_to_ktime(zerolag_time), HRTIMER_MODE_REL);
}

static void task_contending(struct sched_dl_entity *dl_se, int flags)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	/*
	 * If this is a non-deadline task that has been boosted,
	 * do nothing
	 */
	if (dl_se->dl_runtime == 0)
		return;

	if (flags & ENQUEUE_MIGRATED)
		add_rq_bw(dl_se->dl_bw, dl_rq);

	if (dl_se->dl_non_contending) {
		dl_se->dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be cancelled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		if (hrtimer_try_to_cancel(&dl_se->inactive_timer) == 1)
			put_task_struct(dl_task_of(dl_se));
	} else {
		/*
		 * Since "dl_non_contending" is not set, the
		 * task's utilization has already been removed from
		 * active utilization (either when the task blocked,
		 * when the "inactive timer" fired).
		 * So, add it back.
		 */
		add_running_bw(dl_se->dl_bw, dl_rq);
	}
}

static inline int is_leftmost(struct task_struct *p, struct dl_rq *dl_rq)
{
	struct sched_dl_entity *dl_se = &p->dl;

	return dl_rq->rb_leftmost == &dl_se->rb_node;
}

void init_dl_bandwidth(struct dl_bandwidth *dl_b, u64 period, u64 runtime)
{
	raw_spin_lock_init(&dl_b->dl_runtime_lock);
	dl_b->dl_period = period;
	dl_b->dl_runtime = runtime;
}

void init_dl_bw(struct dl_bw *dl_b)
{
	raw_spin_lock_init(&dl_b->lock);
	raw_spin_lock(&def_dl_bandwidth.dl_runtime_lock);
	if (global_rt_runtime() == RUNTIME_INF)
		dl_b->bw = -1;
	else
		dl_b->bw = to_ratio(global_rt_period(), global_rt_runtime());
	raw_spin_unlock(&def_dl_bandwidth.dl_runtime_lock);
	dl_b->total_bw = 0;
}

void init_dl_rq(struct dl_rq *dl_rq)
{
	dl_rq->rb_root = RB_ROOT;

#ifdef CONFIG_SMP
	/* zero means no -deadline tasks */
	dl_rq->earliest_dl.curr = dl_rq->earliest_dl.next = 0;

	dl_rq->dl_nr_migratory = 0;
	dl_rq->overloaded = 0;
	dl_rq->pushable_dl_tasks_root = RB_ROOT;
#else
	init_dl_bw(&dl_rq->dl_bw);
#endif

	dl_rq->running_bw = 0;
	dl_rq->this_bw = 0;
	init_dl_rq_bw_ratio(dl_rq);
}

#ifdef CONFIG_SMP

static inline int dl_overloaded(struct rq *rq)
{
	return atomic_read(&rq->rd->dlo_count);
}

static inline void dl_set_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	cpumask_set_cpu(rq->cpu, rq->rd->dlo_mask);
	/*
	 * Must be visible before the overload count is
	 * set (as in sched_rt.c).
	 *
	 * Matched by the barrier in pull_dl_task().
	 */
	smp_wmb();
	atomic_inc(&rq->rd->dlo_count);
}

static inline void dl_clear_overload(struct rq *rq)
{
	if (!rq->online)
		return;

	atomic_dec(&rq->rd->dlo_count);
	cpumask_clear_cpu(rq->cpu, rq->rd->dlo_mask);
}

static void update_dl_migration(struct dl_rq *dl_rq)
{
	if (dl_rq->dl_nr_migratory && dl_rq->dl_nr_running > 1) {
		if (!dl_rq->overloaded) {
			dl_set_overload(rq_of_dl_rq(dl_rq));
			dl_rq->overloaded = 1;
		}
	} else if (dl_rq->overloaded) {
		dl_clear_overload(rq_of_dl_rq(dl_rq));
		dl_rq->overloaded = 0;
	}
}

static void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	struct task_struct *p = dl_task_of(dl_se);

	if (tsk_nr_cpus_allowed(p) > 1)
		dl_rq->dl_nr_migratory++;

	update_dl_migration(dl_rq);
}

static void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	struct task_struct *p = dl_task_of(dl_se);

	if (tsk_nr_cpus_allowed(p) > 1)
		dl_rq->dl_nr_migratory--;

	update_dl_migration(dl_rq);
}

/*
 * The list of pushable -deadline task is not a plist, like in
 * sched_rt.c, it is an rb-tree with tasks ordered by deadline.
 */
static void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;
	struct rb_node **link = &dl_rq->pushable_dl_tasks_root.rb_node;
	struct rb_node *parent = NULL;
	struct task_struct *entry;
	int leftmost = 1;

	BUG_ON(!RB_EMPTY_NODE(&p->pushable_dl_tasks));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct task_struct,
				 pushable_dl_tasks);
		if (dl_entity_preempt(&p->dl, &entry->dl))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost) {
		dl_rq->pushable_dl_tasks_leftmost = &p->pushable_dl_tasks;
		dl_rq->earliest_dl.next = p->dl.deadline;
	}

	rb_link_node(&p->pushable_dl_tasks, parent, link);
	rb_insert_color(&p->pushable_dl_tasks, &dl_rq->pushable_dl_tasks_root);
}

static void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
	struct dl_rq *dl_rq = &rq->dl;

	if (RB_EMPTY_NODE(&p->pushable_dl_tasks))
		return;

	if (dl_rq->pushable_dl_tasks_leftmost == &p->pushable_dl_tasks) {
		struct rb_node *next_node;

		next_node = rb_next(&p->pushable_dl_tasks);
		dl_rq->pushable_dl_tasks_leftmost = next_node;
		if (next_node) {
			dl_rq->earliest_dl.next = rb_entry(next_node,
				struct task_struct, pushable_dl_tasks)->dl.deadline;
		}
	}

	rb_erase(&p->pushable_dl_tasks, &dl_rq->pushable_dl_tasks_root);
	RB_CLEAR_NODE(&p->pushable_dl_tasks);
}

static inline int has_pushable_dl_tasks(struct rq *rq)
{
	return !RB_EMPTY_ROOT(&rq->dl.pushable_dl_tasks_root);
}

static int push_dl_task(struct rq *rq);

static inline bool need_pull_dl_task(struct rq *rq, struct task_struct *prev)
{
	return dl_task(prev);
}

static DEFINE_PER_CPU(struct callback_head, dl_push_head);
static DEFINE_PER_CPU(struct callback_head, dl_pull_head);

static void push_dl_tasks(struct rq *);
static void pull_dl_task(struct rq *);

static inline void queue_push_tasks(struct rq *rq)
{
	if (!has_pushable_dl_tasks(rq))
		return;

	queue_balance_callback(rq, &per_cpu(dl_push_head, rq->cpu), push_dl_tasks);
}

static inline void queue_pull_task(struct rq *rq)
{
	queue_balance_callback(rq, &per_cpu(dl_pull_head, rq->cpu), pull_dl_task);
}

static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq);

static struct rq *dl_task_offline_migration(struct rq *rq, struct task_struct *p)
{
	struct rq *later_rq = NULL;

	later_rq = find_lock_later_rq(p, rq);
	if (!later_rq) {
		int cpu;

		/*
		 * If we cannot preempt any rq, fall back to pick any
		 * online cpu.
		 */
		cpu = cpumask_any_and(cpu_active_mask, tsk_cpus_allowed(p));
		if (cpu >= nr_cpu_ids) {
			/*
			 * Fail to find any suitable cpu.
			 * The task will never come back!
			 */
			BUG_ON(dl_bandwidth_enabled());

			/*
			 * If admission control is disabled we
			 * try a little harder to let the task
			 * run.
			 */
			cpu = cpumask_any(cpu_active_mask);
		}
		later_rq = cpu_rq(cpu);
		double_lock_balance(rq, later_rq);
	}

	set_task_cpu(p, later_rq->cpu);
	double_unlock_balance(later_rq, rq);

	return later_rq;
}

#else

static inline
void enqueue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void dequeue_pushable_dl_task(struct rq *rq, struct task_struct *p)
{
}

static inline
void inc_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

static inline
void dec_dl_migration(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
}

static inline bool need_pull_dl_task(struct rq *rq, struct task_struct *prev)
{
	return false;
}

static inline void pull_dl_task(struct rq *rq)
{
}

static inline void queue_push_tasks(struct rq *rq)
{
}

static inline void queue_pull_task(struct rq *rq)
{
}
#endif /* CONFIG_SMP */

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags);
static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p,
				  int flags);

/*
 * We are being explicitly informed that a new instance is starting,
 * and this means that:
 *  - the absolute deadline of the entity has to be placed at
 *    current time + relative deadline;
 *  - the runtime of the entity has to be set to the maximum value.
 *
 * The capability of specifying such event is useful whenever a -deadline
 * entity wants to (try to!) synchronize its behaviour with the scheduler's
 * one, and to (try to!) reconcile itself with its own scheduling
 * parameters.
 */
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	WARN_ON(dl_se->dl_boosted);
	WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline));

	/*
	 * We are racing with the deadline timer. So, do nothing because
	 * the deadline timer handler will take care of properly recharging
	 * the runtime and postponing the deadline
	 */
	if (dl_se->dl_throttled)
		return;

	/*
	 * We use the regular wall clock time to set deadlines in the
	 * future; in fact, we must consider execution overheads (time
	 * spent on hardirq context, etc.).
	 */
	dl_se->deadline = rq_clock(rq) + dl_se->dl_deadline;
	dl_se->runtime = dl_se->dl_runtime;
}

/*
 * Pure Earliest Deadline First (EDF) scheduling does not deal with the
 * possibility of a entity lasting more than what it declared, and thus
 * exhausting its runtime.
 *
 * Here we are interested in making runtime overrun possible, but we do
 * not want a entity which is misbehaving to affect the scheduling of all
 * other entities.
 * Therefore, a budgeting strategy called Constant Bandwidth Server (CBS)
 * is used, in order to confine each entity within its own bandwidth.
 *
 * This function deals exactly with that, and ensures that when the runtime
 * of a entity is replenished, its deadline is also postponed. That ensures
 * the overrunning entity can't interfere with other entity in the system and
 * can't make them miss their deadlines. Reasons why this kind of overruns
 * could happen are, typically, a entity voluntarily trying to overcome its
 * runtime, or it just underestimated it during sched_setattr().
 */
static void replenish_dl_entity(struct sched_dl_entity *dl_se,
				struct sched_dl_entity *pi_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	BUG_ON(pi_se->dl_runtime <= 0);

	/*
	 * This could be the case for a !-dl task that is boosted.
	 * Just go with full inherited parameters.
	 */
	if (dl_se->dl_deadline == 0) {
		dl_se->deadline = rq_clock(rq) + pi_se->dl_deadline;
		dl_se->runtime = pi_se->dl_runtime;
	}

	if (dl_se->dl_yielded && dl_se->runtime > 0)
		dl_se->runtime = 0;

	/*
	 * We keep moving the deadline away until we get some
	 * available runtime for the entity. This ensures correct
	 * handling of situations where the runtime overrun is
	 * arbitrary large.
	 */
	while (dl_se->runtime <= 0) {
		dl_se->deadline += pi_se->dl_period;
		dl_se->runtime += pi_se->dl_runtime;
	}

	/*
	 * At this point, the deadline really should be "in
	 * the future" with respect to rq->clock. If it's
	 * not, we are, for some reason, lagging too much!
	 * Anyway, after having warn userspace abut that,
	 * we still try to keep the things running by
	 * resetting the deadline and the budget of the
	 * entity.
	 */
	if (dl_time_before(dl_se->deadline, rq_clock(rq))) {
		printk_deferred_once("sched: DL replenish lagged too much\n");
		dl_se->deadline = rq_clock(rq) + pi_se->dl_deadline;
		dl_se->runtime = pi_se->dl_runtime;
	}

	if (dl_se->dl_yielded)
		dl_se->dl_yielded = 0;
	if (dl_se->dl_throttled)
		dl_se->dl_throttled = 0;
}

/*
 * Here we check if --at time t-- an entity (which is probably being
 * [re]activated or, in general, enqueued) can use its remaining runtime
 * and its current deadline _without_ exceeding the bandwidth it is
 * assigned (function returns true if it can't). We are in fact applying
 * one of the CBS rules: when a task wakes up, if the residual runtime
 * over residual deadline fits within the allocated bandwidth, then we
 * can keep the current (absolute) deadline and residual budget without
 * disrupting the schedulability of the system. Otherwise, we should
 * refill the runtime and set the deadline a period in the future,
 * because keeping the current (absolute) deadline of the task would
 * result in breaking guarantees promised to other tasks (refer to
 * Documentation/scheduler/sched-deadline.txt for more informations).
 *
 * This function returns true if:
 *
 *   runtime / (deadline - t) > dl_runtime / dl_deadline ,
 *
 * IOW we can't recycle current parameters.
 *
 * Notice that the bandwidth check is done against the deadline. For
 * task with deadline equal to period this is the same of using
 * dl_period instead of dl_deadline in the equation above.
 */
static bool dl_entity_overflow(struct sched_dl_entity *dl_se,
			       struct sched_dl_entity *pi_se, u64 t)
{
	u64 left, right;

	/*
	 * left and right are the two sides of the equation above,
	 * after a bit of shuffling to use multiplications instead
	 * of divisions.
	 *
	 * Note that none of the time values involved in the two
	 * multiplications are absolute: dl_deadline and dl_runtime
	 * are the relative deadline and the maximum runtime of each
	 * instance, runtime is the runtime left for the last instance
	 * and (deadline - t), since t is rq->clock, is the time left
	 * to the (absolute) deadline. Even if overflowing the u64 type
	 * is very unlikely to occur in both cases, here we scale down
	 * as we want to avoid that risk at all. Scaling down by 10
	 * means that we reduce granularity to 1us. We are fine with it,
	 * since this is only a true/false check and, anyway, thinking
	 * of anything below microseconds resolution is actually fiction
	 * (but still we want to give the user that illusion >;).
	 */
	left = (pi_se->dl_deadline >> DL_SCALE) * (dl_se->runtime >> DL_SCALE);
	right = ((dl_se->deadline - t) >> DL_SCALE) *
		(pi_se->dl_runtime >> DL_SCALE);

	return dl_time_before(right, left);
}

/*
 * Revised wakeup rule [1]: For self-suspending tasks, rather then
 * re-initializing task's runtime and deadline, the revised wakeup
 * rule adjusts the task's runtime to avoid the task to overrun its
 * density.
 *
 * Reasoning: a task may overrun the density if:
 *    runtime / (deadline - t) > dl_runtime / dl_deadline
 *
 * Therefore, runtime can be adjusted to:
 *     runtime = (dl_runtime / dl_deadline) * (deadline - t)
 *
 * In such way that runtime will be equal to the maximum density
 * the task can use without breaking any rule.
 *
 * [1] Luca Abeni, Giuseppe Lipari, and Juri Lelli. 2015. Constant
 * bandwidth server revisited. SIGBED Rev. 11, 4 (January 2015), 19-24.
 */
static void
update_dl_revised_wakeup(struct sched_dl_entity *dl_se, struct rq *rq)
{
	u64 laxity = dl_se->deadline - rq_clock(rq);

	/*
	 * If the task has deadline < period, and the deadline is in the past,
	 * it should already be throttled before this check.
	 *
	 * See update_dl_entity() comments for further details.
	 */
	WARN_ON(dl_time_before(dl_se->deadline, rq_clock(rq)));

	dl_se->runtime = (dl_se->dl_density * laxity) >> 20;
}

/*
 * Regarding the deadline, a task with implicit deadline has a relative
 * deadline == relative period. A task with constrained deadline has a
 * relative deadline <= relative period.
 *
 * We support constrained deadline tasks. However, there are some restrictions
 * applied only for tasks which do not have an implicit deadline. See
 * update_dl_entity() to know more about such restrictions.
 *
 * The dl_is_implicit() returns true if the task has an implicit deadline.
 */
static inline bool dl_is_implicit(struct sched_dl_entity *dl_se)
{
	return dl_se->dl_deadline == dl_se->dl_period;
}

/*
 * When a deadline entity is placed in the runqueue, its runtime and deadline
 * might need to be updated. This is done by a CBS wake up rule. There are two
 * different rules: 1) the original CBS; and 2) the Revisited CBS.
 *
 * When the task is starting a new period, the Original CBS is used. In this
 * case, the runtime is replenished and a new absolute deadline is set.
 *
 * When a task is queued before the begin of the next period, using the
 * remaining runtime and deadline could make the entity to overflow, see
 * dl_entity_overflow() to find more about runtime overflow. When such case
 * is detected, the runtime and deadline need to be updated.
 *
 * If the task has an implicit deadline, i.e., deadline == period, the Original
 * CBS is applied. the runtime is replenished and a new absolute deadline is
 * set, as in the previous cases.
 *
 * However, the Original CBS does not work properly for tasks with
 * deadline < period, which are said to have a constrained deadline. By
 * applying the Original CBS, a constrained deadline task would be able to run
 * runtime/deadline in a period. With deadline < period, the task would
 * overrun the runtime/period allowed bandwidth, breaking the admission test.
 *
 * In order to prevent this misbehave, the Revisited CBS is used for
 * constrained deadline tasks when a runtime overflow is detected. In the
 * Revisited CBS, rather than replenishing & setting a new absolute deadline,
 * the remaining runtime of the task is reduced to avoid runtime overflow.
 * Please refer to the comments update_dl_revised_wakeup() function to find
 * more about the Revised CBS rule.
 */
static void update_dl_entity(struct sched_dl_entity *dl_se,
			     struct sched_dl_entity *pi_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (dl_time_before(dl_se->deadline, rq_clock(rq)) ||
	    dl_entity_overflow(dl_se, pi_se, rq_clock(rq))) {

		if (unlikely(!dl_is_implicit(dl_se) &&
			     !dl_time_before(dl_se->deadline, rq_clock(rq)) &&
			     !dl_se->dl_boosted)){
			update_dl_revised_wakeup(dl_se, rq);
			return;
		}

		dl_se->deadline = rq_clock(rq) + pi_se->dl_deadline;
		dl_se->runtime = pi_se->dl_runtime;
	}
}

static inline u64 dl_next_period(struct sched_dl_entity *dl_se)
{
	return dl_se->deadline - dl_se->dl_deadline + dl_se->dl_period;
}

/*
 * If the entity depleted all its runtime, and if we want it to sleep
 * while waiting for some new execution time to become available, we
 * set the bandwidth replenishment timer to the replenishment instant
 * and try to activate it.
 *
 * Notice that it is important for the caller to know if the timer
 * actually started or not (i.e., the replenishment instant is in
 * the future or in the past).
 */
static int start_dl_timer(struct task_struct *p)
{
	struct sched_dl_entity *dl_se = &p->dl;
	struct hrtimer *timer = &dl_se->dl_timer;
	struct rq *rq = task_rq(p);
	ktime_t now, act;
	s64 delta;

	lockdep_assert_held(&rq->lock);

	/*
	 * We want the timer to fire at the deadline, but considering
	 * that it is actually coming from rq->clock and not from
	 * hrtimer's time base reading.
	 */
	act = ns_to_ktime(dl_next_period(dl_se));
	now = hrtimer_cb_get_time(timer);
	delta = ktime_to_ns(now) - rq_clock(rq);
	act = ktime_add_ns(act, delta);

	/*
	 * If the expiry time already passed, e.g., because the value
	 * chosen as the deadline is too small, don't even try to
	 * start the timer in the past!
	 */
	if (ktime_us_delta(act, now) < 0)
		return 0;

	/*
	 * !enqueued will guarantee another callback; even if one is already in
	 * progress. This ensures a balanced {get,put}_task_struct().
	 *
	 * The race against __run_timer() clearing the enqueued state is
	 * harmless because we're holding task_rq()->lock, therefore the timer
	 * expiring after we've done the check will wait on its task_rq_lock()
	 * and observe our state.
	 */
	if (!hrtimer_is_queued(timer)) {
		get_task_struct(p);
		hrtimer_start(timer, act, HRTIMER_MODE_ABS);
	}

	return 1;
}

/*
 * This is the bandwidth enforcement timer callback. If here, we know
 * a task is not on its dl_rq, since the fact that the timer was running
 * means the task is throttled and needs a runtime replenishment.
 *
 * However, what we actually do depends on the fact the task is active,
 * (it is on its rq) or has been removed from there by a call to
 * dequeue_task_dl(). In the former case we must issue the runtime
 * replenishment and add the task back to the dl_rq; in the latter, we just
 * do nothing but clearing dl_throttled, so that runtime and deadline
 * updating (and the queueing back to dl_rq) will be done by the
 * next call to enqueue_task_dl().
 */
static enum hrtimer_restart dl_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     dl_timer);
	struct task_struct *p = dl_task_of(dl_se);
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	/*
	 * The task might have changed its scheduling policy to something
	 * different than SCHED_DEADLINE (through switched_fromd_dl()).
	 */
	if (!dl_task(p))
		goto unlock;

	/*
	 * The task might have been boosted by someone else and might be in the
	 * boosting/deboosting path, its not throttled.
	 */
	if (dl_se->dl_boosted)
		goto unlock;

	/*
	 * Spurious timer due to start_dl_timer() race; or we already received
	 * a replenishment from rt_mutex_setprio().
	 */
	if (!dl_se->dl_throttled)
		goto unlock;

	sched_clock_tick();
	update_rq_clock(rq);

	/*
	 * If the throttle happened during sched-out; like:
	 *
	 *   schedule()
	 *     deactivate_task()
	 *       dequeue_task_dl()
	 *         update_curr_dl()
	 *           start_dl_timer()
	 *         __dequeue_task_dl()
	 *     prev->on_rq = 0;
	 *
	 * We can be both throttled and !queued. Replenish the counter
	 * but do not enqueue -- wait for our wakeup to do that.
	 */
	if (!task_on_rq_queued(p)) {
		replenish_dl_entity(dl_se, dl_se);
		goto unlock;
	}

#ifdef CONFIG_SMP
	if (unlikely(!rq->online)) {
		/*
		 * If the runqueue is no longer available, migrate the
		 * task elsewhere. This necessarily changes rq.
		 */
		lockdep_unpin_lock(&rq->lock, rf.cookie);
		rq = dl_task_offline_migration(rq, p);
		rf.cookie = lockdep_pin_lock(&rq->lock);
		update_rq_clock(rq);

		/*
		 * Now that the task has been migrated to the new RQ and we
		 * have that locked, proceed as normal and enqueue the task
		 * there.
		 */
	}
#endif

	enqueue_task_dl(rq, p, ENQUEUE_REPLENISH);
	if (dl_task(rq->curr))
		check_preempt_curr_dl(rq, p, 0);
	else
		resched_curr(rq);

#ifdef CONFIG_SMP
	/*
	 * Queueing this task back might have overloaded rq, check if we need
	 * to kick someone away.
	 */
	if (has_pushable_dl_tasks(rq)) {
		/*
		 * Nothing relies on rq->lock after this, so its safe to drop
		 * rq->lock.
		 */
		rq_unpin_lock(rq, &rf);
		push_dl_task(rq);
		rq_repin_lock(rq, &rf);
	}
#endif

unlock:
	task_rq_unlock(rq, p, &rf);

	/*
	 * This can free the task_struct, including this hrtimer, do not touch
	 * anything related to that after this.
	 */
	put_task_struct(p);

	return HRTIMER_NORESTART;
}

void init_dl_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->dl_timer;

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = dl_task_timer;
}

/*
 * During the activation, CBS checks if it can reuse the current task's
 * runtime and period. If the deadline of the task is in the past, CBS
 * cannot use the runtime, and so it replenishes the task. This rule
 * works fine for implicit deadline tasks (deadline == period), and the
 * CBS was designed for implicit deadline tasks. However, a task with
 * constrained deadline (deadine < period) might be awakened after the
 * deadline, but before the next period. In this case, replenishing the
 * task would allow it to run for runtime / deadline. As in this case
 * deadline < period, CBS enables a task to run for more than the
 * runtime / period. In a very loaded system, this can cause a domino
 * effect, making other tasks miss their deadlines.
 *
 * To avoid this problem, in the activation of a constrained deadline
 * task after the deadline but before the next period, throttle the
 * task and set the replenishing timer to the begin of the next period,
 * unless it is boosted.
 */
static inline void dl_check_constrained_dl(struct sched_dl_entity *dl_se)
{
	struct task_struct *p = dl_task_of(dl_se);
	struct rq *rq = rq_of_dl_rq(dl_rq_of_se(dl_se));

	if (dl_time_before(dl_se->deadline, rq_clock(rq)) &&
	    dl_time_before(rq_clock(rq), dl_next_period(dl_se))) {
		if (unlikely(dl_se->dl_boosted || !start_dl_timer(p)))
			return;
		dl_se->dl_throttled = 1;
		if (dl_se->runtime > 0)
			dl_se->runtime = 0;
	}
}

static
int dl_runtime_exceeded(struct sched_dl_entity *dl_se)
{
	return (dl_se->runtime <= 0);
}

extern bool sched_rt_bandwidth_account(struct rt_rq *rt_rq);

/*
 * This function implements the GRUB accounting rule:
 * according to the GRUB reclaiming algorithm, the runtime is
 * not decreased as "dq = -dt", but as "dq = -Uact dt", where
 * Uact is the (per-runqueue) active utilization.
 * Since rq->dl.running_bw contains Uact * 2^BW_SHIFT, the result
 * has to be shifted right by BW_SHIFT.
 * To reclaim only a fraction Umax of the CPU time, the
 * runtime accounting rule is modified as
 * "dq = -Uact / Umax dt"; since rq->dl.bw_ratio contains
 * 2^RATIO_SHIFT / Umax, delta is multiplied by bw_ratio and shifted
 * right by RATIO_SHIFT.
 * Since delta is a 64 bit variable, to have an overflow its value
 * should be larger than 2^(64 - 20 - 8), which is more than 64 seconds.
 * So, overflow is not an issue here.
 */
u64 grub_reclaim(u64 delta, struct rq *rq)
{
	delta *= rq->dl.running_bw;
	delta *= rq->dl.bw_ratio;
	delta >>= BW_SHIFT + RATIO_SHIFT;

	return delta;
}

/*
 * Update the current task's runtime statistics (provided it is still
 * a -deadline task and has not been removed from the dl_rq).
 */
static void update_curr_dl(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	struct sched_dl_entity *dl_se = &curr->dl;
	u64 delta_exec, now;

	if (!dl_task(curr) || !on_dl_rq(dl_se))
		return;

	/*
	 * Consumed budget is computed considering the time as
	 * observed by schedulable tasks (excluding time spent
	 * in hardirq context, etc.). Deadlines are instead
	 * computed using hard walltime. This seems to be the more
	 * natural solution, but the full ramifications of this
	 * approach need further study.
	 */
	now = rq_clock_task(rq);
	delta_exec = now - curr->se.exec_start;
	if (unlikely((s64)delta_exec <= 0)) {
		if (unlikely(dl_se->dl_yielded))
			goto throttle;
		return;
	}

	/* kick cpufreq (see the comment in kernel/sched/sched.h). */
	cpufreq_update_util(rq, SCHED_CPUFREQ_DL);

	schedstat_set(curr->se.statistics.exec_max,
		      max(curr->se.statistics.exec_max, delta_exec));

	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);

	curr->se.exec_start = now;
	cpuacct_charge(curr, delta_exec);

	sched_rt_avg_update(rq, delta_exec);

	if (unlikely(dl_se->flags & SCHED_FLAG_RECLAIM))
		delta_exec = grub_reclaim(delta_exec, rq);
	dl_se->runtime -= delta_exec;

throttle:
	if (dl_runtime_exceeded(dl_se) || dl_se->dl_yielded) {
		dl_se->dl_throttled = 1;
		__dequeue_task_dl(rq, curr, 0);
		if (unlikely(dl_se->dl_boosted || !start_dl_timer(curr)))
			enqueue_task_dl(rq, curr, ENQUEUE_REPLENISH);

		if (!is_leftmost(curr, &rq->dl))
			resched_curr(rq);
	}

	/*
	 * Because -- for now -- we share the rt bandwidth, we need to
	 * account our runtime there too, otherwise actual rt tasks
	 * would be able to exceed the shared quota.
	 *
	 * Account to the root rt group for now.
	 *
	 * The solution we're working towards is having the RT groups scheduled
	 * using deadline servers -- however there's a few nasties to figure
	 * out before that can happen.
	 */
	if (rt_bandwidth_enabled()) {
		struct rt_rq *rt_rq = &rq->rt;

		raw_spin_lock(&rt_rq->rt_runtime_lock);
		/*
		 * We'll let actual RT tasks worry about the overflow here, we
		 * have our own CBS to keep us inline; only account when RT
		 * bandwidth is relevant.
		 */
		if (sched_rt_bandwidth_account(rt_rq))
			rt_rq->rt_time += delta_exec;
		raw_spin_unlock(&rt_rq->rt_runtime_lock);
	}
}

static enum hrtimer_restart inactive_task_timer(struct hrtimer *timer)
{
	struct sched_dl_entity *dl_se = container_of(timer,
						     struct sched_dl_entity,
						     inactive_timer);
	struct task_struct *p = dl_task_of(dl_se);
	struct rq_flags rf;
	struct rq *rq;

	rq = task_rq_lock(p, &rf);

	if (!dl_task(p) || p->state == TASK_DEAD) {
		struct dl_bw *dl_b = dl_bw_of(task_cpu(p));

		if (p->state == TASK_DEAD && dl_se->dl_non_contending) {
			sub_running_bw(p->dl.dl_bw, dl_rq_of_se(&p->dl));
			sub_rq_bw(p->dl.dl_bw, dl_rq_of_se(&p->dl));
			dl_se->dl_non_contending = 0;
		}

		raw_spin_lock(&dl_b->lock);
		__dl_clear(dl_b, p->dl.dl_bw);
		raw_spin_unlock(&dl_b->lock);
		__dl_clear_params(p);

		goto unlock;
	}
	if (dl_se->dl_non_contending == 0)
		goto unlock;

	sched_clock_tick();
	update_rq_clock(rq);

	sub_running_bw(dl_se->dl_bw, &rq->dl);
	dl_se->dl_non_contending = 0;
unlock:
	task_rq_unlock(rq, p, &rf);
	put_task_struct(p);

	return HRTIMER_NORESTART;
}

void init_dl_inactive_task_timer(struct sched_dl_entity *dl_se)
{
	struct hrtimer *timer = &dl_se->inactive_timer;

	hrtimer_init(timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	timer->function = inactive_task_timer;
}

#ifdef CONFIG_SMP

static void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	if (dl_rq->earliest_dl.curr == 0 ||
	    dl_time_before(deadline, dl_rq->earliest_dl.curr)) {
		dl_rq->earliest_dl.curr = deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, deadline);
	}
}

static void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline)
{
	struct rq *rq = rq_of_dl_rq(dl_rq);

	/*
	 * Since we may have removed our earliest (and/or next earliest)
	 * task we must recompute them.
	 */
	if (!dl_rq->dl_nr_running) {
		dl_rq->earliest_dl.curr = 0;
		dl_rq->earliest_dl.next = 0;
		cpudl_clear(&rq->rd->cpudl, rq->cpu);
	} else {
		struct rb_node *leftmost = dl_rq->rb_leftmost;
		struct sched_dl_entity *entry;

		entry = rb_entry(leftmost, struct sched_dl_entity, rb_node);
		dl_rq->earliest_dl.curr = entry->deadline;
		cpudl_set(&rq->rd->cpudl, rq->cpu, entry->deadline);
	}
}

#else

static inline void inc_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}
static inline void dec_dl_deadline(struct dl_rq *dl_rq, u64 deadline) {}

#endif /* CONFIG_SMP */

static inline
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;
	u64 deadline = dl_se->deadline;

	WARN_ON(!dl_prio(prio));
	dl_rq->dl_nr_running++;
	add_nr_running(rq_of_dl_rq(dl_rq), 1);
	walt_inc_cumulative_runnable_avg(rq_of_dl_rq(dl_rq), dl_task_of(dl_se));

	inc_dl_deadline(dl_rq, deadline);
	inc_dl_migration(dl_se, dl_rq);
}

static inline
void dec_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
	int prio = dl_task_of(dl_se)->prio;

	WARN_ON(!dl_prio(prio));
	WARN_ON(!dl_rq->dl_nr_running);
	dl_rq->dl_nr_running--;
	sub_nr_running(rq_of_dl_rq(dl_rq), 1);
	walt_dec_cumulative_runnable_avg(rq_of_dl_rq(dl_rq), dl_task_of(dl_se));

	dec_dl_deadline(dl_rq, dl_se->deadline);
	dec_dl_migration(dl_se, dl_rq);
}

static void __enqueue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
	struct rb_node **link = &dl_rq->rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct sched_dl_entity *entry;
	int leftmost = 1;

	BUG_ON(!RB_EMPTY_NODE(&dl_se->rb_node));

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_dl_entity, rb_node);
		if (dl_time_before(dl_se->deadline, entry->deadline))
			link = &parent->rb_left;
		else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		dl_rq->rb_leftmost = &dl_se->rb_node;

	rb_link_node(&dl_se->rb_node, parent, link);
	rb_insert_color(&dl_se->rb_node, &dl_rq->rb_root);

	inc_dl_tasks(dl_se, dl_rq);
}

static void __dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	struct dl_rq *dl_rq = dl_rq_of_se(dl_se);

	if (RB_EMPTY_NODE(&dl_se->rb_node))
		return;

	if (dl_rq->rb_leftmost == &dl_se->rb_node) {
		struct rb_node *next_node;

		next_node = rb_next(&dl_se->rb_node);
		dl_rq->rb_leftmost = next_node;
	}

	rb_erase(&dl_se->rb_node, &dl_rq->rb_root);
	RB_CLEAR_NODE(&dl_se->rb_node);

	dec_dl_tasks(dl_se, dl_rq);
}

static void
enqueue_dl_entity(struct sched_dl_entity *dl_se,
		  struct sched_dl_entity *pi_se, int flags)
{
	BUG_ON(on_dl_rq(dl_se));

	/*
	 * If this is a wakeup or a new instance, the scheduling
	 * parameters of the task might need updating. Otherwise,
	 * we want a replenishment of its runtime.
	 */
	if (flags & ENQUEUE_WAKEUP) {
		task_contending(dl_se, flags);
		update_dl_entity(dl_se, pi_se);
	} else if (flags & ENQUEUE_REPLENISH) {
		replenish_dl_entity(dl_se, pi_se);
	}

	__enqueue_dl_entity(dl_se);
}

static void dequeue_dl_entity(struct sched_dl_entity *dl_se)
{
	__dequeue_dl_entity(dl_se);
}

static void enqueue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *pi_task = rt_mutex_get_top_task(p);
	struct sched_dl_entity *pi_se = &p->dl;

	/*
	 * Use the scheduling parameters of the top pi-waiter
	 * task if we have one and its (absolute) deadline is
	 * smaller than our one... OTW we keep our runtime and
	 * deadline.
	 */
	if (pi_task && p->dl.dl_boosted && dl_prio(pi_task->normal_prio)) {
		pi_se = &pi_task->dl;
	} else if (!dl_prio(p->normal_prio)) {
		/*
		 * Special case in which we have a !SCHED_DEADLINE task
		 * that is going to be deboosted, but exceedes its
		 * runtime while doing so. No point in replenishing
		 * it, as it's going to return back to its original
		 * scheduling class after this.
		 */
		BUG_ON(!p->dl.dl_boosted || flags != ENQUEUE_REPLENISH);
		return;
	}

	/*
	 * Check if a constrained deadline task was activated
	 * after the deadline but before the next period.
	 * If that is the case, the task will be throttled and
	 * the replenishment timer will be set to the next period.
	 */
	if (!p->dl.dl_throttled && !dl_is_implicit(&p->dl))
		dl_check_constrained_dl(&p->dl);

	if (p->on_rq == TASK_ON_RQ_MIGRATING || flags & ENQUEUE_RESTORE) {
		add_rq_bw(p->dl.dl_bw, &rq->dl);
		add_running_bw(p->dl.dl_bw, &rq->dl);
	}

	/*
	 * If p is throttled, we do not enqueue it. In fact, if it exhausted
	 * its budget it needs a replenishment and, since it now is on
	 * its rq, the bandwidth timer callback (which clearly has not
	 * run yet) will take care of this.
	 * However, the active utilization does not depend on the fact
	 * that the task is on the runqueue or not (but depends on the
	 * task's state - in GRUB parlance, "inactive" vs "active contending").
	 * In other words, even if a task is throttled its utilization must
	 * be counted in the active utilization; hence, we need to call
	 * add_running_bw().
	 */
	if (p->dl.dl_throttled && !(flags & ENQUEUE_REPLENISH)) {
		if (flags & ENQUEUE_WAKEUP)
			task_contending(&p->dl, flags);

		return;
	}

	enqueue_dl_entity(&p->dl, pi_se, flags);

	if (!task_current(rq, p) && tsk_nr_cpus_allowed(p) > 1)
		enqueue_pushable_dl_task(rq, p);
}

static void __dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	dequeue_dl_entity(&p->dl);
	dequeue_pushable_dl_task(rq, p);
}

static void dequeue_task_dl(struct rq *rq, struct task_struct *p, int flags)
{
	update_curr_dl(rq);
	__dequeue_task_dl(rq, p, flags);

	if (p->on_rq == TASK_ON_RQ_MIGRATING || flags & DEQUEUE_SAVE) {
		sub_running_bw(p->dl.dl_bw, &rq->dl);
		sub_rq_bw(p->dl.dl_bw, &rq->dl);
	}

	/*
	 * This check allows to start the inactive timer (or to immediately
	 * decrease the active utilization, if needed) in two cases:
	 * when the task blocks and when it is terminating
	 * (p->state == TASK_DEAD). We can handle the two cases in the same
	 * way, because from GRUB's point of view the same thing is happening
	 * (the task moves from "active contending" to "active non contending"
	 * or "inactive")
	 */
	if (flags & DEQUEUE_SLEEP)
		task_non_contending(p);
}

/*
 * Yield task semantic for -deadline tasks is:
 *
 *   get off from the CPU until our next instance, with
 *   a new runtime. This is of little use now, since we
 *   don't have a bandwidth reclaiming mechanism. Anyway,
 *   bandwidth reclaiming is planned for the future, and
 *   yield_task_dl will indicate that some spare budget
 *   is available for other task instances to use it.
 */
static void yield_task_dl(struct rq *rq)
{
	/*
	 * We make the task go to sleep until its current deadline by
	 * forcing its runtime to zero. This way, update_curr_dl() stops
	 * it and the bandwidth timer will wake it up and will give it
	 * new scheduling parameters (thanks to dl_yielded=1).
	 */
	rq->curr->dl.dl_yielded = 1;

	update_rq_clock(rq);
	update_curr_dl(rq);
	/*
	 * Tell update_rq_clock() that we've just updated,
	 * so we don't do microscopic update in schedule()
	 * and double the fastpath cost.
	 */
	rq_clock_skip_update(rq);
}

#ifdef CONFIG_SMP

static int find_later_rq(struct task_struct *task);

static int
select_task_rq_dl(struct task_struct *p, int cpu, int sd_flag, int flags,
		  int sibling_count_hint)
{
	struct task_struct *curr;
	struct rq *rq;

	if (sd_flag != SD_BALANCE_WAKE)
		goto out;

	rq = cpu_rq(cpu);

	rcu_read_lock();
	curr = READ_ONCE(rq->curr); /* unlocked access */

	/*
	 * If we are dealing with a -deadline task, we must
	 * decide where to wake it up.
	 * If it has a later deadline and the current task
	 * on this rq can't move (provided the waking task
	 * can!) we prefer to send it somewhere else. On the
	 * other hand, if it has a shorter deadline, we
	 * try to make it stay here, it might be important.
	 */
	if (unlikely(dl_task(curr)) &&
	    (tsk_nr_cpus_allowed(curr) < 2 ||
	     !dl_entity_preempt(&p->dl, &curr->dl)) &&
	    (tsk_nr_cpus_allowed(p) > 1)) {
		int target = find_later_rq(p);

		if (target != -1 &&
				(dl_time_before(p->dl.deadline,
					cpu_rq(target)->dl.earliest_dl.curr) ||
				(cpu_rq(target)->dl.dl_nr_running == 0)))
			cpu = target;
	}
	rcu_read_unlock();

out:
	return cpu;
}

static void migrate_task_rq_dl(struct task_struct *p)
{
	struct rq *rq;

	if (p->state != TASK_WAKING)
		return;

	rq = task_rq(p);
	/*
	 * Since p->state == TASK_WAKING, set_task_cpu() has been called
	 * from try_to_wake_up(). Hence, p->pi_lock is locked, but
	 * rq->lock is not... So, lock it
	 */
	raw_spin_lock(&rq->lock);
	if (p->dl.dl_non_contending) {
		sub_running_bw(p->dl.dl_bw, &rq->dl);
		p->dl.dl_non_contending = 0;
		/*
		 * If the timer handler is currently running and the
		 * timer cannot be cancelled, inactive_task_timer()
		 * will see that dl_not_contending is not set, and
		 * will not touch the rq's active utilization,
		 * so we are still safe.
		 */
		if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
			put_task_struct(p);
	}
	sub_rq_bw(p->dl.dl_bw, &rq->dl);
	raw_spin_unlock(&rq->lock);
}

static void check_preempt_equal_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * Current can't be migrated, useless to reschedule,
	 * let's hope p can move out.
	 */
	if (tsk_nr_cpus_allowed(rq->curr) == 1 ||
	    cpudl_find(&rq->rd->cpudl, rq->curr, NULL) == -1)
		return;

	/*
	 * p is migratable, so let's not schedule it and
	 * see if it is pushed or pulled somewhere else.
	 */
	if (tsk_nr_cpus_allowed(p) != 1 &&
	    cpudl_find(&rq->rd->cpudl, p, NULL) != -1)
		return;

	resched_curr(rq);
}

#endif /* CONFIG_SMP */

/*
 * Only called when both the current and waking task are -deadline
 * tasks.
 */
static void check_preempt_curr_dl(struct rq *rq, struct task_struct *p,
				  int flags)
{
	if (dl_entity_preempt(&p->dl, &rq->curr->dl)) {
		resched_curr(rq);
		return;
	}

#ifdef CONFIG_SMP
	/*
	 * In the unlikely case current and p have the same deadline
	 * let us try to decide what's the best thing to do...
	 */
	if ((p->dl.deadline == rq->curr->dl.deadline) &&
	    !test_tsk_need_resched(rq->curr))
		check_preempt_equal_dl(rq, p);
#endif /* CONFIG_SMP */
}

#ifdef CONFIG_SCHED_HRTICK
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
	hrtick_start(rq, p->dl.runtime);
}
#else /* !CONFIG_SCHED_HRTICK */
static void start_hrtick_dl(struct rq *rq, struct task_struct *p)
{
}
#endif

static struct sched_dl_entity *pick_next_dl_entity(struct rq *rq,
						   struct dl_rq *dl_rq)
{
	struct rb_node *left = dl_rq->rb_leftmost;

	if (!left)
		return NULL;

	return rb_entry(left, struct sched_dl_entity, rb_node);
}

struct task_struct *
pick_next_task_dl(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	struct sched_dl_entity *dl_se;
	struct task_struct *p;
	struct dl_rq *dl_rq;

	dl_rq = &rq->dl;

	if (need_pull_dl_task(rq, prev)) {
		/*
		 * This is OK, because current is on_cpu, which avoids it being
		 * picked for load-balance and preemption/IRQs are still
		 * disabled avoiding further scheduler activity on it and we're
		 * being very careful to re-start the picking loop.
		 */
		rq_unpin_lock(rq, rf);
		pull_dl_task(rq);
		rq_repin_lock(rq, rf);
		/*
		 * pull_dl_task() can drop (and re-acquire) rq->lock; this
		 * means a stop task can slip in, in which case we need to
		 * re-start task selection.
		 */
		if (rq->stop && task_on_rq_queued(rq->stop))
			return RETRY_TASK;
	}

	/*
	 * When prev is DL, we may throttle it in put_prev_task().
	 * So, we update time before we check for dl_nr_running.
	 */
	if (prev->sched_class == &dl_sched_class)
		update_curr_dl(rq);

	if (unlikely(!dl_rq->dl_nr_running))
		return NULL;

	put_prev_task(rq, prev);

	dl_se = pick_next_dl_entity(rq, dl_rq);
	BUG_ON(!dl_se);

	p = dl_task_of(dl_se);
	p->se.exec_start = rq_clock_task(rq);

	/* Running task will never be pushed. */
       dequeue_pushable_dl_task(rq, p);

	if (hrtick_enabled(rq))
		start_hrtick_dl(rq, p);

	queue_push_tasks(rq);

	if (rq->curr->sched_class != &dl_sched_class)
		update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 0);

	return p;
}

static void put_prev_task_dl(struct rq *rq, struct task_struct *p)
{
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_task(rq), rq, 1);

	if (on_dl_rq(&p->dl) && tsk_nr_cpus_allowed(p) > 1)
		enqueue_pushable_dl_task(rq, p);
}

static void task_tick_dl(struct rq *rq, struct task_struct *p, int queued)
{
	update_curr_dl(rq);

	update_dl_rq_load_avg(rq_clock_pelt(rq), rq, 1);
	/*
	 * Even when we have runtime, update_curr_dl() might have resulted in us
	 * not being the leftmost task anymore. In that case NEED_RESCHED will
	 * be set and schedule() will start a new hrtick for the next task.
	 */
	if (hrtick_enabled(rq) && queued && p->dl.runtime > 0 &&
	    is_leftmost(p, &rq->dl))
		start_hrtick_dl(rq, p);
}

static void task_fork_dl(struct task_struct *p)
{
	/*
	 * SCHED_DEADLINE tasks cannot fork and this is achieved through
	 * sched_fork()
	 */
}

static void set_curr_task_dl(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	p->se.exec_start = rq_clock_task(rq);

	/* You can't push away the running task */
	dequeue_pushable_dl_task(rq, p);
}

#ifdef CONFIG_SMP

/* Only try algorithms three times */
#define DL_MAX_TRIES 3

static int pick_dl_task(struct rq *rq, struct task_struct *p, int cpu)
{
	if (!task_running(rq, p) &&
	    cpumask_test_cpu(cpu, tsk_cpus_allowed(p)))
		return 1;
	return 0;
}

/*
 * Return the earliest pushable rq's task, which is suitable to be executed
 * on the CPU, NULL otherwise:
 */
static struct task_struct *pick_earliest_pushable_dl_task(struct rq *rq, int cpu)
{
	struct rb_node *next_node = rq->dl.pushable_dl_tasks_leftmost;
	struct task_struct *p = NULL;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

next_node:
	if (next_node) {
		p = rb_entry(next_node, struct task_struct, pushable_dl_tasks);

		if (pick_dl_task(rq, p, cpu))
			return p;

		next_node = rb_next(next_node);
		goto next_node;
	}

	return NULL;
}

static DEFINE_PER_CPU(cpumask_var_t, local_cpu_mask_dl);

static int find_later_rq(struct task_struct *task)
{
	struct sched_domain *sd;
	struct cpumask *later_mask = this_cpu_cpumask_var_ptr(local_cpu_mask_dl);
	int this_cpu = smp_processor_id();
	int best_cpu, cpu = task_cpu(task);

	/* Make sure the mask is initialized first */
	if (unlikely(!later_mask))
		return -1;

	if (tsk_nr_cpus_allowed(task) == 1)
		return -1;

	/*
	 * We have to consider system topology and task affinity
	 * first, then we can look for a suitable cpu.
	 */
	best_cpu = cpudl_find(&task_rq(task)->rd->cpudl,
			task, later_mask);
	if (best_cpu == -1)
		return -1;

	/*
	 * If we are here, some target has been found,
	 * the most suitable of which is cached in best_cpu.
	 * This is, among the runqueues where the current tasks
	 * have later deadlines than the task's one, the rq
	 * with the latest possible one.
	 *
	 * Now we check how well this matches with task's
	 * affinity and system topology.
	 *
	 * The last cpu where the task run is our first
	 * guess, since it is most likely cache-hot there.
	 */
	if (cpumask_test_cpu(cpu, later_mask))
		return cpu;
	/*
	 * Check if this_cpu is to be skipped (i.e., it is
	 * not in the mask) or not.
	 */
	if (!cpumask_test_cpu(this_cpu, later_mask))
		this_cpu = -1;

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		if (sd->flags & SD_WAKE_AFFINE) {

			/*
			 * If possible, preempting this_cpu is
			 * cheaper than migrating.
			 */
			if (this_cpu != -1 &&
			    cpumask_test_cpu(this_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return this_cpu;
			}

			/*
			 * Last chance: if best_cpu is valid and is
			 * in the mask, that becomes our choice.
			 */
			if (best_cpu < nr_cpu_ids &&
			    cpumask_test_cpu(best_cpu, sched_domain_span(sd))) {
				rcu_read_unlock();
				return best_cpu;
			}
		}
	}
	rcu_read_unlock();

	/*
	 * At this point, all our guesses failed, we just return
	 * 'something', and let the caller sort the things out.
	 */
	if (this_cpu != -1)
		return this_cpu;

	cpu = cpumask_any(later_mask);
	if (cpu < nr_cpu_ids)
		return cpu;

	return -1;
}

/* Locks the rq it finds */
static struct rq *find_lock_later_rq(struct task_struct *task, struct rq *rq)
{
	struct rq *later_rq = NULL;
	int tries;
	int cpu;

	for (tries = 0; tries < DL_MAX_TRIES; tries++) {
		cpu = find_later_rq(task);

		if ((cpu == -1) || (cpu == rq->cpu))
			break;

		later_rq = cpu_rq(cpu);

		if (later_rq->dl.dl_nr_running &&
		    !dl_time_before(task->dl.deadline,
					later_rq->dl.earliest_dl.curr)) {
			/*
			 * Target rq has tasks of equal or earlier deadline,
			 * retrying does not release any lock and is unlikely
			 * to yield a different result.
			 */
			later_rq = NULL;
			break;
		}

		/* Retry if something changed. */
		if (double_lock_balance(rq, later_rq)) {
			if (unlikely(task_rq(task) != rq ||
				     !cpumask_test_cpu(later_rq->cpu,
						       tsk_cpus_allowed(task)) ||
				     task_running(rq, task) ||
				     !dl_task(task) ||
				     !task_on_rq_queued(task))) {
				double_unlock_balance(rq, later_rq);
				later_rq = NULL;
				break;
			}
		}

		/*
		 * If the rq we found has no -deadline task, or
		 * its earliest one has a later deadline than our
		 * task, the rq is a good one.
		 */
		if (!later_rq->dl.dl_nr_running ||
		    dl_time_before(task->dl.deadline,
				   later_rq->dl.earliest_dl.curr))
			break;

		/* Otherwise we try again. */
		double_unlock_balance(rq, later_rq);
		later_rq = NULL;
	}

	return later_rq;
}

static struct task_struct *pick_next_pushable_dl_task(struct rq *rq)
{
	struct task_struct *p;

	if (!has_pushable_dl_tasks(rq))
		return NULL;

	p = rb_entry(rq->dl.pushable_dl_tasks_leftmost,
		     struct task_struct, pushable_dl_tasks);

	BUG_ON(rq->cpu != task_cpu(p));
	BUG_ON(task_current(rq, p));
	BUG_ON(tsk_nr_cpus_allowed(p) <= 1);

	BUG_ON(!task_on_rq_queued(p));
	BUG_ON(!dl_task(p));

	return p;
}

/*
 * See if the non running -deadline tasks on this rq
 * can be sent to some other CPU where they can preempt
 * and start executing.
 */
static int push_dl_task(struct rq *rq)
{
	struct task_struct *next_task;
	struct rq *later_rq;
	int ret = 0;

	if (!rq->dl.overloaded)
		return 0;

	next_task = pick_next_pushable_dl_task(rq);
	if (!next_task)
		return 0;

retry:
	if (unlikely(next_task == rq->curr)) {
		WARN_ON(1);
		return 0;
	}

	/*
	 * If next_task preempts rq->curr, and rq->curr
	 * can move away, it makes sense to just reschedule
	 * without going further in pushing next_task.
	 */
	if (dl_task(rq->curr) &&
	    dl_time_before(next_task->dl.deadline, rq->curr->dl.deadline) &&
	    tsk_nr_cpus_allowed(rq->curr) > 1) {
		resched_curr(rq);
		return 0;
	}

	/* We might release rq lock */
	get_task_struct(next_task);

	/* Will lock the rq it'll find */
	later_rq = find_lock_later_rq(next_task, rq);
	if (!later_rq) {
		struct task_struct *task;

		/*
		 * We must check all this again, since
		 * find_lock_later_rq releases rq->lock and it is
		 * then possible that next_task has migrated.
		 */
		task = pick_next_pushable_dl_task(rq);
		if (task_cpu(next_task) == rq->cpu && task == next_task) {
			/*
			 * The task is still there. We don't try
			 * again, some other cpu will pull it when ready.
			 */
			goto out;
		}

		if (!task)
			/* No more tasks */
			goto out;

		put_task_struct(next_task);
		next_task = task;
		goto retry;
	}

	deactivate_task(rq, next_task, 0);
	next_task->on_rq = TASK_ON_RQ_MIGRATING;
	sub_running_bw(next_task->dl.dl_bw, &rq->dl);
	clear_average_bw(&next_task->dl, &rq->dl);
	sub_rq_bw(next_task->dl.dl_bw, &rq->dl);
	set_task_cpu(next_task, later_rq->cpu);
	add_rq_bw(next_task->dl.dl_bw, &later_rq->dl);
	add_running_bw(next_task->dl.dl_bw, &later_rq->dl);
	next_task->on_rq = TASK_ON_RQ_QUEUED;
	add_average_bw(&next_task->dl, &later_rq->dl);
	activate_task(later_rq, next_task, 0);
	ret = 1;

	resched_curr(later_rq);

	double_unlock_balance(rq, later_rq);

out:
	put_task_struct(next_task);

	return ret;
}

static void push_dl_tasks(struct rq *rq)
{
	/* push_dl_task() will return true if it moved a -deadline task */
	while (push_dl_task(rq))
		;
}

static void pull_dl_task(struct rq *this_rq)
{
	int this_cpu = this_rq->cpu, cpu;
	struct task_struct *p;
	bool resched = false;
	struct rq *src_rq;
	u64 dmin = LONG_MAX;

	if (likely(!dl_overloaded(this_rq)))
		return;

	/*
	 * Match the barrier from dl_set_overloaded; this guarantees that if we
	 * see overloaded we must also see the dlo_mask bit.
	 */
	smp_rmb();

	for_each_cpu(cpu, this_rq->rd->dlo_mask) {
		if (this_cpu == cpu)
			continue;

		src_rq = cpu_rq(cpu);

		/*
		 * It looks racy, abd it is! However, as in sched_rt.c,
		 * we are fine with this.
		 */
		if (this_rq->dl.dl_nr_running &&
		    dl_time_before(this_rq->dl.earliest_dl.curr,
				   src_rq->dl.earliest_dl.next))
			continue;

		/* Might drop this_rq->lock */
		double_lock_balance(this_rq, src_rq);

		/*
		 * If there are no more pullable tasks on the
		 * rq, we're done with it.
		 */
		if (src_rq->dl.dl_nr_running <= 1)
			goto skip;

		p = pick_earliest_pushable_dl_task(src_rq, this_cpu);

		/*
		 * We found a task to be pulled if:
		 *  - it preempts our current (if there's one),
		 *  - it will preempt the last one we pulled (if any).
		 */
		if (p && dl_time_before(p->dl.deadline, dmin) &&
		    (!this_rq->dl.dl_nr_running ||
		     dl_time_before(p->dl.deadline,
				    this_rq->dl.earliest_dl.curr))) {
			WARN_ON(p == src_rq->curr);
			WARN_ON(!task_on_rq_queued(p));

			/*
			 * Then we pull iff p has actually an earlier
			 * deadline than the current task of its runqueue.
			 */
			if (dl_time_before(p->dl.deadline,
					   src_rq->curr->dl.deadline))
				goto skip;

			resched = true;

			deactivate_task(src_rq, p, 0);
			p->on_rq = TASK_ON_RQ_MIGRATING;
			clear_average_bw(&p->dl, &src_rq->dl);
			sub_running_bw(p->dl.dl_bw, &src_rq->dl);
			sub_rq_bw(p->dl.dl_bw, &src_rq->dl);
			set_task_cpu(p, this_cpu);
			add_rq_bw(p->dl.dl_bw, &this_rq->dl);
			add_running_bw(p->dl.dl_bw, &this_rq->dl);
			p->on_rq = TASK_ON_RQ_QUEUED;
			add_average_bw(&p->dl, &this_rq->dl);
			activate_task(this_rq, p, 0);
			dmin = p->dl.deadline;

			/* Is there any other task even earlier? */
		}
skip:
		double_unlock_balance(this_rq, src_rq);
	}

	if (resched)
		resched_curr(this_rq);
}

/*
 * Since the task is not running and a reschedule is not going to happen
 * anytime soon on its runqueue, we try pushing it away now.
 */
static void task_woken_dl(struct rq *rq, struct task_struct *p)
{
	if (!task_running(rq, p) &&
	    !test_tsk_need_resched(rq->curr) &&
	    tsk_nr_cpus_allowed(p) > 1 &&
	    dl_task(rq->curr) &&
	    (tsk_nr_cpus_allowed(rq->curr) < 2 ||
	     !dl_entity_preempt(&p->dl, &rq->curr->dl))) {
		push_dl_tasks(rq);
	}
}

static void set_cpus_allowed_dl(struct task_struct *p,
				const struct cpumask *new_mask)
{
	struct root_domain *src_rd;
	struct rq *rq;

	BUG_ON(!dl_task(p));

	rq = task_rq(p);
	src_rd = rq->rd;
	/*
	 * Migrating a SCHED_DEADLINE task between exclusive
	 * cpusets (different root_domains) entails a bandwidth
	 * update. We already made space for us in the destination
	 * domain (see cpuset_can_attach()).
	 */
	if (!cpumask_intersects(src_rd->span, new_mask)) {
		struct dl_bw *src_dl_b;

		src_dl_b = dl_bw_of(cpu_of(rq));
		/*
		 * We now free resources of the root_domain we are migrating
		 * off. In the worst case, sched_setattr() may temporary fail
		 * until we complete the update.
		 */
		raw_spin_lock(&src_dl_b->lock);
		__dl_clear(src_dl_b, p->dl.dl_bw);
		raw_spin_unlock(&src_dl_b->lock);
	}

	set_cpus_allowed_common(p, new_mask);
}

/* Assumes rq->lock is held */
static void rq_online_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_set_overload(rq);

	cpudl_set_freecpu(&rq->rd->cpudl, rq->cpu);
	if (rq->dl.dl_nr_running > 0)
		cpudl_set(&rq->rd->cpudl, rq->cpu, rq->dl.earliest_dl.curr);
}

/* Assumes rq->lock is held */
static void rq_offline_dl(struct rq *rq)
{
	if (rq->dl.overloaded)
		dl_clear_overload(rq);

	cpudl_clear(&rq->rd->cpudl, rq->cpu);
	cpudl_clear_freecpu(&rq->rd->cpudl, rq->cpu);
}

void __init init_sched_dl_class(void)
{
	unsigned int i;

	for_each_possible_cpu(i)
		zalloc_cpumask_var_node(&per_cpu(local_cpu_mask_dl, i),
					GFP_KERNEL, cpu_to_node(i));
}

#endif /* CONFIG_SMP */

static void switched_from_dl(struct rq *rq, struct task_struct *p)
{
	/*
	 * task_non_contending() can start the "inactive timer" (if the 0-lag
	 * time is in the future). If the task switches back to dl before
	 * the "inactive timer" fires, it can continue to consume its current
	 * runtime using its current deadline. If it stays outside of
	 * SCHED_DEADLINE until the 0-lag time passes, inactive_task_timer()
	 * will reset the task parameters.
	 */
	if (task_on_rq_queued(p) && p->dl.dl_runtime)
		task_non_contending(p);

	if (!task_on_rq_queued(p))
		sub_rq_bw(p->dl.dl_bw, &rq->dl);

	/*
	 * We cannot use inactive_task_timer() to invoke sub_running_bw()
	 * at the 0-lag time, because the task could have been migrated
	 * while SCHED_OTHER in the meanwhile.
	 */
	if (p->dl.dl_non_contending)
		p->dl.dl_non_contending = 0;

	clear_average_bw(&p->dl, &rq->dl);

	/*
	 * Since this might be the only -deadline task on the rq,
	 * this is the right place to try to pull some other one
	 * from an overloaded cpu, if any.
	 */
	if (!task_on_rq_queued(p) || rq->dl.dl_nr_running)
		return;

	queue_pull_task(rq);
}

/*
 * When switching to -deadline, we may overload the rq, then
 * we try to push someone off, if possible.
 */
static void switched_to_dl(struct rq *rq, struct task_struct *p)
{
	if (hrtimer_try_to_cancel(&p->dl.inactive_timer) == 1)
		put_task_struct(p);

	/* If p is not queued we will update its parameters at next wakeup. */
	if (!task_on_rq_queued(p)) {
		add_rq_bw(p->dl.dl_bw, &rq->dl);

		return;
	}
	/*
	 * If p is boosted we already updated its params in
	 * rt_mutex_setprio()->enqueue_task(..., ENQUEUE_REPLENISH),
	 * p's deadline being now already after rq_clock(rq).
	 */
	if (dl_time_before(p->dl.deadline, rq_clock(rq))) {
		setup_new_dl_entity(&p->dl);
		add_average_bw(&p->dl, dl_rq_of_se(&p->dl));
	}

	if (rq->curr != p) {
#ifdef CONFIG_SMP
		if (tsk_nr_cpus_allowed(p) > 1 && rq->dl.overloaded)
			queue_push_tasks(rq);
#endif
		if (dl_task(rq->curr))
			check_preempt_curr_dl(rq, p, 0);
		else
			resched_curr(rq);
	}
}

/*
 * If the scheduling parameters of a -deadline task changed,
 * a push or pull operation might be needed.
 */
static void prio_changed_dl(struct rq *rq, struct task_struct *p,
			    int oldprio)
{
	if (task_on_rq_queued(p) || rq->curr == p) {
#ifdef CONFIG_SMP
		/*
		 * This might be too much, but unfortunately
		 * we don't have the old deadline value, and
		 * we can't argue if the task is increasing
		 * or lowering its prio, so...
		 */
		if (!rq->dl.overloaded)
			queue_pull_task(rq);

		/*
		 * If we now have a earlier deadline task than p,
		 * then reschedule, provided p is still on this
		 * runqueue.
		 */
		if (dl_time_before(rq->dl.earliest_dl.curr, p->dl.deadline))
			resched_curr(rq);
#else
		/*
		 * Again, we don't know if p has a earlier
		 * or later deadline, so let's blindly set a
		 * (maybe not needed) rescheduling point.
		 */
		resched_curr(rq);
#endif /* CONFIG_SMP */
	}
}

const struct sched_class dl_sched_class = {
	.next			= &rt_sched_class,
	.enqueue_task		= enqueue_task_dl,
	.dequeue_task		= dequeue_task_dl,
	.yield_task		= yield_task_dl,

	.check_preempt_curr	= check_preempt_curr_dl,

	.pick_next_task		= pick_next_task_dl,
	.put_prev_task		= put_prev_task_dl,

#ifdef CONFIG_SMP
	.select_task_rq		= select_task_rq_dl,
	.migrate_task_rq	= migrate_task_rq_dl,
	.set_cpus_allowed       = set_cpus_allowed_dl,
	.rq_online              = rq_online_dl,
	.rq_offline             = rq_offline_dl,
	.task_woken		= task_woken_dl,
#endif

	.set_curr_task		= set_curr_task_dl,
	.task_tick		= task_tick_dl,
	.task_fork              = task_fork_dl,

	.prio_changed           = prio_changed_dl,
	.switched_from		= switched_from_dl,
	.switched_to		= switched_to_dl,

	.update_curr		= update_curr_dl,
};

#ifdef CONFIG_SCHED_DEBUG
extern void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq);

void print_dl_stats(struct seq_file *m, int cpu)
{
	print_dl_rq(m, cpu, &cpu_rq(cpu)->dl);
}
#endif /* CONFIG_SCHED_DEBUG */

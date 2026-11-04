// SPDX-License-Identifier: GPL-2.0
/*
 * pollmove: moves the card's two block-device pollers to the CPUs asked for, then refuses to stay loaded. They are kernel threads bound with kthread_bind, so PF_NO_SETAFFINITY shuts userspace out (taskset: EINVAL); set_cpus_allowed_ptr from
 * inside the kernel is not stopped by it. By the user's order of 11-04: threads 121 and 122, the first two threads of the 61st core, so that 120 threads on 60 cores have those cores to themselves.
 *
 * insmod pollmove.ko pid0=N cpu0=241 pid1=M cpu1=242 always ends with "Operation canceled": nothing remains loaded. The result is read back from /proc by scripts/pollers.sh, not taken from this module's word.
 */
#include <linux/cpumask.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/string.h>

static int pid0, pid1, cpu0 = -1, cpu1 = -1;
module_param(pid0, int, 0);
module_param(pid1, int, 0);
module_param(cpu0, int, 0);
module_param(cpu1, int, 0);

static int move_one(int nr, int cpu)
{
	struct pid *sp = find_get_pid(nr);
	struct task_struct *t = sp ? get_pid_task(sp, PIDTYPE_PID) : NULL;
	int rc;

	put_pid(sp);
	if (!t) {
		pr_warn("pollmove: no task %d\n", nr);
		return -ESRCH;
	}
	if (!(t->flags & PF_KTHREAD) || strncmp(t->comm, "phiblk", 6)) {
		pr_warn("pollmove: %d is %s, not a poller: refused\n", nr, t->comm);
		rc = -EPERM;
	} else if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_online(cpu)) {
		pr_warn("pollmove: cpu %d is not online\n", cpu);
		rc = -EINVAL;
	} else {
		rc = set_cpus_allowed_ptr(t, cpumask_of(cpu));
		pr_info("pollmove: %s (%d) to cpu %d: %d\n", t->comm, nr, cpu, rc);
	}
	put_task_struct(t);
	return rc;
}

static int __init pollmove_init(void)
{
	int a = move_one(pid0, cpu0), b = move_one(pid1, cpu1);

	return a ? a : b ? b : -ECANCELED;
}
module_init(pollmove_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("one shot: moves the phiblk pollers");

// SPDX-License-Identifier: GPL-2.0
/*
 * kernel/ai/ai_proc.c - AI Subsystem /proc interface
 *
 * Creates /proc/ai/ with files exposing system state the AI can read
 * for contextual awareness without needing userspace tools.
 *
 *   /proc/ai/sysinfo    - kernel version, architecture, CPU info
 *   /proc/ai/processes  - top 32 processes by CPU+memory
 *   /proc/ai/resources  - memory, load average summary
 *   /proc/ai/claws      - registered OpenClaw tools (JSON)
 *   /proc/ai/stats      - AI subsystem message statistics
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/swap.h>
#include <linux/utsname.h>
#include <linux/cpumask.h>
#include <linux/sysinfo.h>
#include <linux/sched/loadavg.h>
#include <linux/ai.h>

static struct proc_dir_entry *proc_ai_dir;

/* ===== /proc/ai/sysinfo ===== */

static int ai_sysinfo_show(struct seq_file *m, void *v)
{
	struct new_utsname *u = utsname();

	seq_printf(m, "{\n");
	seq_printf(m, "  \"kernel_version\": \"%s\",\n", u->release);
	seq_printf(m, "  \"kernel_build\": \"%s\",\n",   u->version);
	seq_printf(m, "  \"hostname\": \"%s\",\n",         u->nodename);
	seq_printf(m, "  \"arch\": \"%s\",\n",             u->machine);
	seq_printf(m, "  \"os\": \"%s\",\n",               u->sysname);
	seq_printf(m, "  \"num_cpus\": %d,\n",             num_online_cpus());
	seq_printf(m, "  \"num_cpus_possible\": %d\n",     num_possible_cpus());
	seq_printf(m, "}\n");
	return 0;
}

static int ai_sysinfo_open(struct inode *inode, struct file *file)
{
	return single_open(file, ai_sysinfo_show, NULL);
}

static const struct proc_ops ai_sysinfo_ops = {
	.proc_open    = ai_sysinfo_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ===== /proc/ai/resources ===== */

static int ai_resources_show(struct seq_file *m, void *v)
{
	struct sysinfo si;
	unsigned long available;

	si_meminfo(&si);
	available = si_mem_available();

	seq_printf(m, "{\n");
	seq_printf(m, "  \"memory_total_kb\": %lu,\n",
		   si.totalram * (si.mem_unit / 1024));
	seq_printf(m, "  \"memory_free_kb\": %lu,\n",
		   si.freeram * (si.mem_unit / 1024));
	seq_printf(m, "  \"memory_available_kb\": %lu,\n",
		   available * (si.mem_unit / 1024));
	seq_printf(m, "  \"memory_buffers_kb\": %lu,\n",
		   si.bufferram * (si.mem_unit / 1024));
	seq_printf(m, "  \"swap_total_kb\": %lu,\n",
		   si.totalswap * (si.mem_unit / 1024));
	seq_printf(m, "  \"swap_free_kb\": %lu,\n",
		   si.freeswap * (si.mem_unit / 1024));
	seq_printf(m, "  \"uptime_secs\": %lld,\n",
		   ktime_divns(ktime_get_boottime(), NSEC_PER_SEC));
	seq_printf(m, "  \"load_avg_1min\": %lu,\n",  avenrun[0] >> FSHIFT);
	seq_printf(m, "  \"load_avg_5min\": %lu,\n",  avenrun[1] >> FSHIFT);
	seq_printf(m, "  \"load_avg_15min\": %lu\n",  avenrun[2] >> FSHIFT);
	seq_printf(m, "}\n");
	return 0;
}

static int ai_resources_open(struct inode *inode, struct file *file)
{
	return single_open(file, ai_resources_show, NULL);
}

static const struct proc_ops ai_resources_ops = {
	.proc_open    = ai_resources_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ===== /proc/ai/stats ===== */

static int ai_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "{\n");
	seq_printf(m, "  \"total_queries\": %lld,\n",
		   atomic64_read(&ai_global_stats.total_queries));
	seq_printf(m, "  \"total_responses\": %lld,\n",
		   atomic64_read(&ai_global_stats.total_responses));
	seq_printf(m, "  \"total_tool_calls\": %lld,\n",
		   atomic64_read(&ai_global_stats.total_tool_calls));
	seq_printf(m, "  \"total_errors\": %lld,\n",
		   atomic64_read(&ai_global_stats.total_errors));
	seq_printf(m, "  \"queue_depth\": %d,\n",
		   atomic_read(&ai_global_stats.queue_depth));
	seq_printf(m, "  \"subsystem_ready\": %s\n",
		   ai_subsystem_ready() ? "true" : "false");
	seq_printf(m, "}\n");
	return 0;
}

static int ai_stats_open(struct inode *inode, struct file *file)
{
	return single_open(file, ai_stats_show, NULL);
}

static const struct proc_ops ai_stats_ops = {
	.proc_open    = ai_stats_open,
	.proc_read    = seq_read,
	.proc_lseek   = seq_lseek,
	.proc_release = single_release,
};

/* ===== Module Init / Exit ===== */

int __init ai_proc_init(void)
{
	proc_ai_dir = proc_mkdir("ai", NULL);
	if (!proc_ai_dir) {
		pr_err("ai: failed to create /proc/ai\n");
		return -ENOMEM;
	}

	if (!proc_create("sysinfo",   0444, proc_ai_dir, &ai_sysinfo_ops)   ||
	    !proc_create("resources", 0444, proc_ai_dir, &ai_resources_ops) ||
	    !proc_create("stats",     0444, proc_ai_dir, &ai_stats_ops)) {
		pr_err("ai: failed to create /proc/ai entries\n");
		remove_proc_subtree("ai", NULL);
		return -ENOMEM;
	}

	pr_info("ai: /proc/ai/ interface created\n");
	return 0;
}

void __exit ai_proc_exit(void)
{
	remove_proc_subtree("ai", NULL);
	pr_info("ai: /proc/ai/ removed\n");
}

module_init(ai_proc_init);
module_exit(ai_proc_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Linux AI Subsystem - /proc/ai interface");

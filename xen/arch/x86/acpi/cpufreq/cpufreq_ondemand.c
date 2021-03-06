/*
 *  xen/arch/x86/acpi/cpufreq/cpufreq_ondemand.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *             Feb 2008 Liu Jinsong <jinsong.liu@intel.com>
 *             Porting cpufreq_ondemand.c from Liunx 2.6.23 to Xen hypervisor 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <xen/types.h>
#include <xen/percpu.h>
#include <xen/cpumask.h>
#include <xen/types.h>
#include <xen/sched.h>
#include <xen/timer.h>
#include <asm/config.h>
#include <acpi/cpufreq/cpufreq.h>

#define DEF_FREQUENCY_UP_THRESHOLD              (80)

#define MIN_DBS_INTERVAL                        (MICROSECS(100))
#define MIN_SAMPLING_MILLISECS                  (20)
#define MIN_STAT_SAMPLING_RATE                   \
    (MIN_SAMPLING_MILLISECS * MILLISECS(1))
#define DEF_SAMPLING_RATE_LATENCY_MULTIPLIER    (1000)
#define TRANSITION_LATENCY_LIMIT                (10 * 1000 )

static uint64_t def_sampling_rate;

/* Sampling types */
enum {DBS_NORMAL_SAMPLE, DBS_SUB_SAMPLE};

static DEFINE_PER_CPU(struct cpu_dbs_info_s, cpu_dbs_info);

static unsigned int dbs_enable;    /* number of CPUs using this policy */

static struct dbs_tuners {
    uint64_t     sampling_rate;
    unsigned int up_threshold;
    unsigned int ignore_nice;
    unsigned int powersave_bias;
} dbs_tuners_ins = {
    .up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
    .ignore_nice = 0,
    .powersave_bias = 0,
};

static struct timer dbs_timer[NR_CPUS];

uint64_t get_cpu_idle_time(unsigned int cpu)
{
    uint64_t idle_ns;
    struct vcpu *v;

    if ((v = idle_vcpu[cpu]) == NULL)
        return 0;

    idle_ns = v->runstate.time[RUNSTATE_running];
    if (v->is_running)
        idle_ns += NOW() - v->runstate.state_entry_time;

    return idle_ns;
}

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
    unsigned int load = 0;
    uint64_t cur_ns, idle_ns, total_ns;

    struct cpufreq_policy *policy;
    unsigned int j;

    if (!this_dbs_info->enable)
        return;

    policy = this_dbs_info->cur_policy;

    if (unlikely(policy->resume)) {
        __cpufreq_driver_target(policy, policy->max,CPUFREQ_RELATION_H);
        return;
    }

    cur_ns = NOW();
    total_ns = cur_ns - this_dbs_info->prev_cpu_wall;
    this_dbs_info->prev_cpu_wall = NOW();

    if (total_ns < MIN_DBS_INTERVAL)
        return;

    /* Get Idle Time */
    idle_ns = UINT_MAX;
    for_each_cpu_mask(j, policy->cpus) {
        uint64_t total_idle_ns;
        unsigned int tmp_idle_ns;
        struct cpu_dbs_info_s *j_dbs_info;

        j_dbs_info = &per_cpu(cpu_dbs_info, j);
        total_idle_ns = get_cpu_idle_time(j);
        tmp_idle_ns = total_idle_ns - j_dbs_info->prev_cpu_idle;
        j_dbs_info->prev_cpu_idle = total_idle_ns;

        if (tmp_idle_ns < idle_ns)
            idle_ns = tmp_idle_ns;
    }

    if (likely(total_ns > idle_ns))
        load = (100 * (total_ns - idle_ns)) / total_ns;

    /* Check for frequency increase */
    if (load > dbs_tuners_ins.up_threshold) {
        /* if we are already at full speed then break out early */
        if (policy->cur == policy->max)
            return;
        __cpufreq_driver_target(policy, policy->max,CPUFREQ_RELATION_H);
        return;
    }

    /* Check for frequency decrease */
    /* if we cannot reduce the frequency anymore, break out early */
    if (policy->cur == policy->min)
        return;

    /*
     * The optimal frequency is the frequency that is the lowest that
     * can support the current CPU usage without triggering the up
     * policy. To be safe, we focus 10 points under the threshold.
     */
    if (load < (dbs_tuners_ins.up_threshold - 10)) {
        unsigned int freq_next, freq_cur;

        freq_cur = __cpufreq_driver_getavg(policy);
        if (!freq_cur)
            freq_cur = policy->cur;

        freq_next = (freq_cur * load) / (dbs_tuners_ins.up_threshold - 10);

        __cpufreq_driver_target(policy, freq_next, CPUFREQ_RELATION_L);
    }
}

static void do_dbs_timer(void *dbs)
{
    struct cpu_dbs_info_s *dbs_info = (struct cpu_dbs_info_s *)dbs;

    if (!dbs_info->enable)
        return;

    dbs_check_cpu(dbs_info);

    set_timer(&dbs_timer[dbs_info->cpu], NOW()+dbs_tuners_ins.sampling_rate);
}

static void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
    dbs_info->enable = 1;

    init_timer(&dbs_timer[dbs_info->cpu], do_dbs_timer, 
        (void *)dbs_info, dbs_info->cpu);

    set_timer(&dbs_timer[dbs_info->cpu], NOW()+dbs_tuners_ins.sampling_rate);
}

static void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
    dbs_info->enable = 0;
    stop_timer(&dbs_timer[dbs_info->cpu]);
}

int cpufreq_governor_dbs(struct cpufreq_policy *policy, unsigned int event)
{
    unsigned int cpu = policy->cpu;
    struct cpu_dbs_info_s *this_dbs_info;
    unsigned int j;

    this_dbs_info = &per_cpu(cpu_dbs_info, cpu);

    switch (event) {
    case CPUFREQ_GOV_START:
        if ((!cpu_online(cpu)) || (!policy->cur))
            return -EINVAL;

        if (policy->cpuinfo.transition_latency >
            (TRANSITION_LATENCY_LIMIT * 1000)) {
            printk(KERN_WARNING "ondemand governor failed to load "
                "due to too long transition latency\n");
            return -EINVAL;
        }
        if (this_dbs_info->enable)
            /* Already enabled */
            break;

        dbs_enable++;

        for_each_cpu_mask(j, policy->cpus) {
            struct cpu_dbs_info_s *j_dbs_info;
            j_dbs_info = &per_cpu(cpu_dbs_info, j);
            j_dbs_info->cur_policy = policy;

            j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j);
            j_dbs_info->prev_cpu_wall = NOW();
        }
        this_dbs_info->cpu = cpu;
        /*
         * Start the timerschedule work, when this governor
         * is used for first time
         */
        if (dbs_enable == 1) {
            def_sampling_rate = policy->cpuinfo.transition_latency *
                DEF_SAMPLING_RATE_LATENCY_MULTIPLIER;

            if (def_sampling_rate < MIN_STAT_SAMPLING_RATE)
                def_sampling_rate = MIN_STAT_SAMPLING_RATE;

            dbs_tuners_ins.sampling_rate = def_sampling_rate;
        }
        dbs_timer_init(this_dbs_info);

        break;

    case CPUFREQ_GOV_STOP:
        dbs_timer_exit(this_dbs_info);
        dbs_enable--;

        break;

    case CPUFREQ_GOV_LIMITS:
        if (policy->max < this_dbs_info->cur_policy->cur)
            __cpufreq_driver_target(this_dbs_info->cur_policy,
                policy->max, CPUFREQ_RELATION_H);
        else if (policy->min > this_dbs_info->cur_policy->cur)
            __cpufreq_driver_target(this_dbs_info->cur_policy,
                policy->min, CPUFREQ_RELATION_L);
        break;
    }
    return 0;
} 

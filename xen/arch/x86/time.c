/******************************************************************************
 * arch/x86/time.c
 * 
 * Per-CPU time calibration and management.
 * 
 * Copyright (c) 2002-2005, K A Fraser
 * 
 * Portions from Linux are:
 * Copyright (c) 1991, 1992, 1995  Linus Torvalds
 */

#include <xen/config.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/sched.h>
#include <xen/lib.h>
#include <xen/config.h>
#include <xen/init.h>
#include <xen/time.h>
#include <xen/timer.h>
#include <xen/smp.h>
#include <xen/irq.h>
#include <xen/softirq.h>
#include <asm/io.h>
#include <asm/msr.h>
#include <asm/mpspec.h>
#include <asm/processor.h>
#include <asm/fixmap.h>
#include <asm/mc146818rtc.h>
#include <asm/div64.h>
#include <asm/hpet.h>
#include <io_ports.h>

/* opt_clocksource: Force clocksource to one of: pit, hpet, cyclone, acpi. */
static char opt_clocksource[10];
string_param("clocksource", opt_clocksource);

unsigned long cpu_khz;  /* CPU clock frequency in kHz. */
DEFINE_SPINLOCK(rtc_lock);
unsigned long pit0_ticks;
static u32 wc_sec, wc_nsec; /* UTC time at last 'time update'. */
static DEFINE_SPINLOCK(wc_lock);

struct time_scale {
    int shift;
    u32 mul_frac;
};

struct cpu_time {
    u64 local_tsc_stamp;
    s_time_t stime_local_stamp;
    s_time_t stime_master_stamp;
    struct time_scale tsc_scale;
};

struct platform_timesource {
    char *id;
    char *name;
    u64 frequency;
    u64 (*read_counter)(void);
    int (*init)(struct platform_timesource *);
    void (*resume)(struct platform_timesource *);
    int counter_bits;
};

static DEFINE_PER_CPU(struct cpu_time, cpu_time);

/* Calibrate all CPUs to platform timer every EPOCH. */
#define EPOCH MILLISECS(1000)
static struct timer calibration_timer;

/* TSC is invariant on C state entry? */
static bool_t tsc_invariant;

/*
 * We simulate a 32-bit platform timer from the 16-bit PIT ch2 counter.
 * Otherwise overflow happens too quickly (~50ms) for us to guarantee that
 * softirq handling will happen in time.
 * 
 * The pit_lock protects the 16- and 32-bit stamp fields as well as the 
 */
static DEFINE_SPINLOCK(pit_lock);
static u16 pit_stamp16;
static u32 pit_stamp32;
static int using_pit;

/*
 * 32-bit division of integer dividend and integer divisor yielding
 * 32-bit fractional quotient.
 */
static inline u32 div_frac(u32 dividend, u32 divisor)
{
    u32 quotient, remainder;
    ASSERT(dividend < divisor);
    asm ( 
        "divl %4"
        : "=a" (quotient), "=d" (remainder)
        : "0" (0), "1" (dividend), "r" (divisor) );
    return quotient;
}

/*
 * 32-bit multiplication of multiplicand and fractional multiplier
 * yielding 32-bit product (radix point at same position as in multiplicand).
 */
static inline u32 mul_frac(u32 multiplicand, u32 multiplier)
{
    u32 product_int, product_frac;
    asm (
        "mul %3"
        : "=a" (product_frac), "=d" (product_int)
        : "0" (multiplicand), "r" (multiplier) );
    return product_int;
}

/*
 * Scale a 64-bit delta by scaling and multiplying by a 32-bit fraction,
 * yielding a 64-bit result.
 */
static inline u64 scale_delta(u64 delta, struct time_scale *scale)
{
    u64 product;
#ifdef CONFIG_X86_32
    u32 tmp1, tmp2;
#endif

    if ( scale->shift < 0 )
        delta >>= -scale->shift;
    else
        delta <<= scale->shift;

#ifdef CONFIG_X86_32
    asm (
        "mul  %5       ; "
        "mov  %4,%%eax ; "
        "mov  %%edx,%4 ; "
        "mul  %5       ; "
        "xor  %5,%5    ; "
        "add  %4,%%eax ; "
        "adc  %5,%%edx ; "
        : "=A" (product), "=r" (tmp1), "=r" (tmp2)
        : "a" ((u32)delta), "1" ((u32)(delta >> 32)), "2" (scale->mul_frac) );
#else
    asm (
        "mul %%rdx ; shrd $32,%%rdx,%%rax"
        : "=a" (product) : "0" (delta), "d" ((u64)scale->mul_frac) );
#endif

    return product;
}

/* Compute the reciprocal of the given time_scale. */
static inline struct time_scale scale_reciprocal(struct time_scale scale)
{
    struct time_scale reciprocal;
    u32 dividend;

    dividend = 0x80000000u;
    reciprocal.shift = 1 - scale.shift;
    while ( unlikely(dividend >= scale.mul_frac) )
    {
        dividend >>= 1;
        reciprocal.shift++;
    }

    asm (
        "divl %4"
        : "=a" (reciprocal.mul_frac), "=d" (dividend)
        : "0" (0), "1" (dividend), "r" (scale.mul_frac) );

    return reciprocal;
}

/*
 * cpu_mask that denotes the CPUs that needs timer interrupt coming in as
 * IPIs in place of local APIC timers
 */
extern int xen_cpuidle;
static cpumask_t pit_broadcast_mask;

static void smp_send_timer_broadcast_ipi(void)
{
    int cpu = smp_processor_id();
    cpumask_t mask;

    cpus_and(mask, cpu_online_map, pit_broadcast_mask);

    if ( cpu_isset(cpu, mask) )
    {
        cpu_clear(cpu, mask);
        raise_softirq(TIMER_SOFTIRQ);
    }

    if ( !cpus_empty(mask) )
    {
        cpumask_raise_softirq(mask, TIMER_SOFTIRQ);
    }
}

static void timer_interrupt(int irq, void *dev_id, struct cpu_user_regs *regs)
{
    ASSERT(local_irq_is_enabled());

    if ( hpet_legacy_irq_tick() )
        return;

    /* Only for start-of-day interruopt tests in io_apic.c. */
    (*(volatile unsigned long *)&pit0_ticks)++;

    /* Rough hack to allow accurate timers to sort-of-work with no APIC. */
    if ( !cpu_has_apic )
        raise_softirq(TIMER_SOFTIRQ);

    if ( xen_cpuidle )
        smp_send_timer_broadcast_ipi();

    /* Emulate a 32-bit PIT counter. */
    if ( using_pit )
    {
        u16 count;

        spin_lock_irq(&pit_lock);

        outb(0x80, PIT_MODE);
        count  = inb(PIT_CH2);
        count |= inb(PIT_CH2) << 8;

        pit_stamp32 += (u16)(pit_stamp16 - count);
        pit_stamp16 = count;

        spin_unlock_irq(&pit_lock);
    }
}

static struct irqaction irq0 = { timer_interrupt, "timer", NULL };

/* ------ Calibrate the TSC ------- 
 * Return processor ticks per second / CALIBRATE_FRAC.
 */

#define CLOCK_TICK_RATE 1193182 /* system crystal frequency (Hz) */
#define CALIBRATE_FRAC  20      /* calibrate over 50ms */
#define CALIBRATE_LATCH ((CLOCK_TICK_RATE+(CALIBRATE_FRAC/2))/CALIBRATE_FRAC)

static u64 init_pit_and_calibrate_tsc(void)
{
    u64 start, end;
    unsigned long count;

    /* Set PIT channel 0 to HZ Hz. */
#define LATCH (((CLOCK_TICK_RATE)+(HZ/2))/HZ)
    outb_p(0x34, PIT_MODE);        /* binary, mode 2, LSB/MSB, ch 0 */
    outb_p(LATCH & 0xff, PIT_CH0); /* LSB */
    outb(LATCH >> 8, PIT_CH0);     /* MSB */

    /* Set the Gate high, disable speaker */
    outb((inb(0x61) & ~0x02) | 0x01, 0x61);

    /*
     * Now let's take care of CTC channel 2
     *
     * Set the Gate high, program CTC channel 2 for mode 0, (interrupt on
     * terminal count mode), binary count, load 5 * LATCH count, (LSB and MSB)
     * to begin countdown.
     */
    outb(0xb0, PIT_MODE);           /* binary, mode 0, LSB/MSB, Ch 2 */
    outb(CALIBRATE_LATCH & 0xff, PIT_CH2); /* LSB of count */
    outb(CALIBRATE_LATCH >> 8, PIT_CH2);   /* MSB of count */

    rdtscll(start);
    for ( count = 0; (inb(0x61) & 0x20) == 0; count++ )
        continue;
    rdtscll(end);

    /* Error if the CTC doesn't behave itself. */
    if ( count == 0 )
        return 0;

    return ((end - start) * (u64)CALIBRATE_FRAC);
}

static void set_time_scale(struct time_scale *ts, u64 ticks_per_sec)
{
    u64 tps64 = ticks_per_sec;
    u32 tps32;
    int shift = 0;

    ASSERT(tps64 != 0);

    while ( tps64 > (MILLISECS(1000)*2) )
    {
        tps64 >>= 1;
        shift--;
    }

    tps32 = (u32)tps64;
    while ( tps32 <= (u32)MILLISECS(1000) )
    {
        tps32 <<= 1;
        shift++;
    }

    ts->mul_frac = div_frac(MILLISECS(1000), tps32);
    ts->shift    = shift;
}

static atomic_t tsc_calibrate_gang = ATOMIC_INIT(0);
static unsigned int tsc_calibrate_status = 0;

void calibrate_tsc_bp(void)
{
    while ( atomic_read(&tsc_calibrate_gang) != (num_booting_cpus() - 1) )
        mb();

    outb(CALIBRATE_LATCH & 0xff, PIT_CH2);
    outb(CALIBRATE_LATCH >> 8, PIT_CH2);

    tsc_calibrate_status = 1;
    wmb();

    while ( (inb(0x61) & 0x20) == 0 )
        continue;

    tsc_calibrate_status = 2;
    wmb();

    while ( atomic_read(&tsc_calibrate_gang) != 0 )
        mb();
}

void calibrate_tsc_ap(void)
{
    u64 t1, t2, ticks_per_sec;

    atomic_inc(&tsc_calibrate_gang);

    while ( tsc_calibrate_status < 1 )
        mb();

    rdtscll(t1);

    while ( tsc_calibrate_status < 2 )
        mb();

    rdtscll(t2);

    ticks_per_sec = (t2 - t1) * (u64)CALIBRATE_FRAC;
    set_time_scale(&this_cpu(cpu_time).tsc_scale, ticks_per_sec);

    atomic_dec(&tsc_calibrate_gang);
}

static char *freq_string(u64 freq)
{
    static char s[20];
    unsigned int x, y;
    y = (unsigned int)do_div(freq, 1000000) / 1000;
    x = (unsigned int)freq;
    snprintf(s, sizeof(s), "%u.%03uMHz", x, y);
    return s;
}

/************************************************************
 * PLATFORM TIMER 1: PROGRAMMABLE INTERVAL TIMER (LEGACY PIT)
 */

static u64 read_pit_count(void)
{
    u16 count16;
    u32 count32;
    unsigned long flags;

    spin_lock_irqsave(&pit_lock, flags);

    outb(0x80, PIT_MODE);
    count16  = inb(PIT_CH2);
    count16 |= inb(PIT_CH2) << 8;

    count32 = pit_stamp32 + (u16)(pit_stamp16 - count16);

    spin_unlock_irqrestore(&pit_lock, flags);

    return count32;
}

static int init_pit(struct platform_timesource *pts)
{
    using_pit = 1;
    return 1;
}

static struct platform_timesource plt_pit =
{
    .id = "pit",
    .name = "PIT",
    .frequency = CLOCK_TICK_RATE,
    .read_counter = read_pit_count,
    .counter_bits = 32,
    .init = init_pit
};

/************************************************************
 * PLATFORM TIMER 2: HIGH PRECISION EVENT TIMER (HPET)
 */

static u64 read_hpet_count(void)
{
    return hpet_read32(HPET_COUNTER);
}

static int init_hpet(struct platform_timesource *pts)
{
    u64 hpet_rate = hpet_setup();

    if ( hpet_rate == 0 )
        return 0;

    pts->frequency = hpet_rate;
    return 1;
}

static void resume_hpet(struct platform_timesource *pts)
{
    u64 hpet_rate = hpet_setup();

    BUG_ON(hpet_rate == 0);
    pts->frequency = hpet_rate;
}

static struct platform_timesource plt_hpet =
{
    .id = "hpet",
    .name = "HPET",
    .read_counter = read_hpet_count,
    .counter_bits = 32,
    .init = init_hpet,
    .resume = resume_hpet
};

/************************************************************
 * PLATFORM TIMER 3: IBM 'CYCLONE' TIMER
 */

int use_cyclone;

/*
 * Although the counter is read via a 64-bit register, I believe it is actually
 * a 40-bit counter. Since this will wrap, I read only the low 32 bits and
 * periodically fold into a 64-bit software counter, just as for PIT and HPET.
 */
#define CYCLONE_CBAR_ADDR   0xFEB00CD0
#define CYCLONE_PMCC_OFFSET 0x51A0
#define CYCLONE_MPMC_OFFSET 0x51D0
#define CYCLONE_MPCS_OFFSET 0x51A8
#define CYCLONE_TIMER_FREQ  100000000

/* Cyclone MPMC0 register. */
static volatile u32 *cyclone_timer;

static u64 read_cyclone_count(void)
{
    return *cyclone_timer;
}

static volatile u32 *map_cyclone_reg(unsigned long regaddr)
{
    unsigned long pageaddr = regaddr &  PAGE_MASK;
    unsigned long offset   = regaddr & ~PAGE_MASK;
    set_fixmap_nocache(FIX_CYCLONE_TIMER, pageaddr);
    return (volatile u32 *)(fix_to_virt(FIX_CYCLONE_TIMER) + offset);
}

static int init_cyclone(struct platform_timesource *pts)
{
    u32 base;
    
    if ( !use_cyclone )
        return 0;

    /* Find base address. */
    base = *(map_cyclone_reg(CYCLONE_CBAR_ADDR));
    if ( base == 0 )
    {
        printk(KERN_ERR "Cyclone: Could not find valid CBAR value.\n");
        return 0;
    }

    /* Enable timer and map the counter register. */
    *(map_cyclone_reg(base + CYCLONE_PMCC_OFFSET)) = 1;
    *(map_cyclone_reg(base + CYCLONE_MPCS_OFFSET)) = 1;
    cyclone_timer = map_cyclone_reg(base + CYCLONE_MPMC_OFFSET);
    return 1;
}

static struct platform_timesource plt_cyclone =
{
    .id = "cyclone",
    .name = "IBM Cyclone",
    .frequency = CYCLONE_TIMER_FREQ,
    .read_counter = read_cyclone_count,
    .counter_bits = 32,
    .init = init_cyclone
};

/************************************************************
 * PLATFORM TIMER 4: ACPI PM TIMER
 */

u32 pmtmr_ioport;

/* ACPI PM timer ticks at 3.579545 MHz. */
#define ACPI_PM_FREQUENCY 3579545

static u64 read_pmtimer_count(void)
{
    return inl(pmtmr_ioport);
}

static int init_pmtimer(struct platform_timesource *pts)
{
    if ( pmtmr_ioport == 0 )
        return 0;

    return 1;
}

static struct platform_timesource plt_pmtimer =
{
    .id = "acpi",
    .name = "ACPI PM Timer",
    .frequency = ACPI_PM_FREQUENCY,
    .read_counter = read_pmtimer_count,
    .counter_bits = 24,
    .init = init_pmtimer
};

/************************************************************
 * GENERIC PLATFORM TIMER INFRASTRUCTURE
 */

static struct platform_timesource plt_src; /* details of chosen timesource  */
static u64 plt_mask;             /* hardware-width mask                     */
static u64 plt_overflow_period;  /* ns between calls to plt_overflow()      */
static struct time_scale plt_scale; /* scale: platform counter -> nanosecs  */

/* Protected by platform_timer_lock. */
static DEFINE_SPINLOCK(platform_timer_lock);
static s_time_t stime_platform_stamp; /* System time at below platform time */
static u64 platform_timer_stamp;      /* Platform time at above system time */
static u64 plt_stamp64;          /* 64-bit platform counter stamp           */
static u64 plt_stamp;            /* hardware-width platform counter stamp   */
static struct timer plt_overflow_timer;

static void plt_overflow(void *unused)
{
    u64 count;

    spin_lock_irq(&platform_timer_lock);
    count = plt_src.read_counter();
    plt_stamp64 += (count - plt_stamp) & plt_mask;
    plt_stamp = count;
    spin_unlock_irq(&platform_timer_lock);

    set_timer(&plt_overflow_timer, NOW() + plt_overflow_period);
}

static s_time_t __read_platform_stime(u64 platform_time)
{
    u64 diff = platform_time - platform_timer_stamp;
    ASSERT(spin_is_locked(&platform_timer_lock));
    return (stime_platform_stamp + scale_delta(diff, &plt_scale));
}

static s_time_t read_platform_stime(void)
{
    u64 count;
    s_time_t stime;

    ASSERT(!local_irq_is_enabled());

    spin_lock(&platform_timer_lock);
    count = plt_stamp64 + ((plt_src.read_counter() - plt_stamp) & plt_mask);
    stime = __read_platform_stime(count);
    spin_unlock(&platform_timer_lock);

    return stime;
}

static void platform_time_calibration(void)
{
    u64 count;
    s_time_t stamp;

    spin_lock_irq(&platform_timer_lock);
    count = plt_stamp64 + ((plt_src.read_counter() - plt_stamp) & plt_mask);
    stamp = __read_platform_stime(count);
    stime_platform_stamp = stamp;
    platform_timer_stamp = count;
    spin_unlock_irq(&platform_timer_lock);
}

static void resume_platform_timer(void)
{
    /* Timer source can be reset when backing from S3 to S0 */
    if ( plt_src.resume )
        plt_src.resume(&plt_src);

    plt_stamp64 = platform_timer_stamp;
    plt_stamp = plt_src.read_counter();
}

static void init_platform_timer(void)
{
    static struct platform_timesource * const plt_timers[] = {
        &plt_cyclone, &plt_hpet, &plt_pmtimer, &plt_pit
    };

    struct platform_timesource *pts = NULL;
    int i, rc = -1;

    if ( opt_clocksource[0] != '\0' )
    {
        for ( i = 0; i < ARRAY_SIZE(plt_timers); i++ )
        {
            pts = plt_timers[i];
            if ( !strcmp(opt_clocksource, pts->id) )
            {
                rc = pts->init(pts);
                break;
            }
        }

        if ( rc <= 0 )
            printk("WARNING: %s clocksource '%s'.\n",
                   (rc == 0) ? "Could not initialise" : "Unrecognised",
                   opt_clocksource);
    }

    if ( rc <= 0 )
    {
        for ( i = 0; i < ARRAY_SIZE(plt_timers); i++ )
        {
            pts = plt_timers[i];
            if ( (rc = pts->init(pts)) > 0 )
                break;
        }
    }

    BUG_ON(rc <= 0);

    plt_mask = (u64)~0ull >> (64 - pts->counter_bits);

    set_time_scale(&plt_scale, pts->frequency);

    plt_overflow_period = scale_delta(
        1ull << (pts->counter_bits-1), &plt_scale);
    init_timer(&plt_overflow_timer, plt_overflow, NULL, 0);
    plt_src = *pts;
    plt_overflow(NULL);

    platform_timer_stamp = plt_stamp64;

    printk("Platform timer is %s %s\n",
           freq_string(pts->frequency), pts->name);
}

void cstate_restore_tsc(void)
{
    struct cpu_time *t = &this_cpu(cpu_time);
    struct time_scale sys_to_tsc = scale_reciprocal(t->tsc_scale);
    s_time_t stime_delta;
    u64 tsc_delta;

    if ( tsc_invariant )
        return;

    stime_delta = read_platform_stime() - t->stime_master_stamp;
    if ( stime_delta < 0 )
        stime_delta = 0;

    tsc_delta = scale_delta(stime_delta, &sys_to_tsc);

    wrmsrl(MSR_IA32_TSC, t->local_tsc_stamp + tsc_delta);
}

/***************************************************************************
 * CMOS Timer functions
 ***************************************************************************/

/* Converts Gregorian date to seconds since 1970-01-01 00:00:00.
 * Assumes input in normal date format, i.e. 1980-12-31 23:59:59
 * => year=1980, mon=12, day=31, hour=23, min=59, sec=59.
 *
 * [For the Julian calendar (which was used in Russia before 1917,
 * Britain & colonies before 1752, anywhere else before 1582,
 * and is still in use by some communities) leave out the
 * -year/100+year/400 terms, and add 10.]
 *
 * This algorithm was first published by Gauss (I think).
 *
 * WARNING: this function will overflow on 2106-02-07 06:28:16 on
 * machines were long is 32-bit! (However, as time_t is signed, we
 * will already get problems at other places on 2038-01-19 03:14:08)
 */
unsigned long
mktime (unsigned int year, unsigned int mon,
        unsigned int day, unsigned int hour,
        unsigned int min, unsigned int sec)
{
    /* 1..12 -> 11,12,1..10: put Feb last since it has a leap day. */
    if ( 0 >= (int) (mon -= 2) )
    {
        mon += 12;
        year -= 1;
    }

    return ((((unsigned long)(year/4 - year/100 + year/400 + 367*mon/12 + day)+
              year*365 - 719499
        )*24 + hour /* now have hours */
        )*60 + min  /* now have minutes */
        )*60 + sec; /* finally seconds */
}

static unsigned long __get_cmos_time(void)
{
    unsigned int year, mon, day, hour, min, sec;

    sec  = CMOS_READ(RTC_SECONDS);
    min  = CMOS_READ(RTC_MINUTES);
    hour = CMOS_READ(RTC_HOURS);
    day  = CMOS_READ(RTC_DAY_OF_MONTH);
    mon  = CMOS_READ(RTC_MONTH);
    year = CMOS_READ(RTC_YEAR);
    
    if ( !(CMOS_READ(RTC_CONTROL) & RTC_DM_BINARY) || RTC_ALWAYS_BCD )
    {
        BCD_TO_BIN(sec);
        BCD_TO_BIN(min);
        BCD_TO_BIN(hour);
        BCD_TO_BIN(day);
        BCD_TO_BIN(mon);
        BCD_TO_BIN(year);
    }

    if ( (year += 1900) < 1970 )
        year += 100;

    return mktime(year, mon, day, hour, min, sec);
}

static unsigned long get_cmos_time(void)
{
    unsigned long res, flags;
    int i;

    spin_lock_irqsave(&rtc_lock, flags);

    /* read RTC exactly on falling edge of update flag */
    for ( i = 0 ; i < 1000000 ; i++ ) /* may take up to 1 second... */
        if ( (CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP) )
            break;
    for ( i = 0 ; i < 1000000 ; i++ ) /* must try at least 2.228 ms */
        if ( !(CMOS_READ(RTC_FREQ_SELECT) & RTC_UIP) )
            break;

    res = __get_cmos_time();

    spin_unlock_irqrestore(&rtc_lock, flags);
    return res;
}

/***************************************************************************
 * System Time
 ***************************************************************************/

s_time_t get_s_time(void)
{
    struct cpu_time *t = &this_cpu(cpu_time);
    u64 tsc, delta;
    s_time_t now;

    rdtscll(tsc);
    delta = tsc - t->local_tsc_stamp;
    now = t->stime_local_stamp + scale_delta(delta, &t->tsc_scale);

    return now;
}

static inline void version_update_begin(u32 *version)
{
    /* Explicitly OR with 1 just in case version number gets out of sync. */
    *version = (*version + 1) | 1;
    wmb();
}

static inline void version_update_end(u32 *version)
{
    wmb();
    (*version)++;
}

void update_vcpu_system_time(struct vcpu *v)
{
    struct cpu_time       *t;
    struct vcpu_time_info *u;

    if ( v->vcpu_info == NULL )
        return;

    t = &this_cpu(cpu_time);
    u = &vcpu_info(v, time);

    if ( u->tsc_timestamp == t->local_tsc_stamp )
        return;

    version_update_begin(&u->version);

    u->tsc_timestamp     = t->local_tsc_stamp;
    u->system_time       = t->stime_local_stamp;
    u->tsc_to_system_mul = t->tsc_scale.mul_frac;
    u->tsc_shift         = (s8)t->tsc_scale.shift;

    version_update_end(&u->version);
}

void update_domain_wallclock_time(struct domain *d)
{
    spin_lock(&wc_lock);
    version_update_begin(&shared_info(d, wc_version));
    shared_info(d, wc_sec)  = wc_sec + d->time_offset_seconds;
    shared_info(d, wc_nsec) = wc_nsec;
    version_update_end(&shared_info(d, wc_version));
    spin_unlock(&wc_lock);
}

void domain_set_time_offset(struct domain *d, int32_t time_offset_seconds)
{
    d->time_offset_seconds = time_offset_seconds;
    if ( is_hvm_domain(d) )
        rtc_update_clock(d);
}

int cpu_frequency_change(u64 freq)
{
    struct cpu_time *t = &this_cpu(cpu_time);
    u64 curr_tsc;

    /* Sanity check: CPU frequency allegedly dropping below 1MHz? */
    if ( freq < 1000000u )
    {
        gdprintk(XENLOG_WARNING, "Rejecting CPU frequency change "
                 "to %"PRIu64" Hz.\n", freq);
        return -EINVAL;
    }

    local_irq_disable();
    /* Platform time /first/, as we may be delayed by platform_timer_lock. */
    t->stime_master_stamp = read_platform_stime();
    /* TSC-extrapolated time may be bogus after frequency change. */
    /*t->stime_local_stamp = get_s_time();*/
    t->stime_local_stamp = t->stime_master_stamp;
    rdtscll(curr_tsc);
    t->local_tsc_stamp = curr_tsc;
    set_time_scale(&t->tsc_scale, freq);
    local_irq_enable();

    update_vcpu_system_time(current);

    /* A full epoch should pass before we check for deviation. */
    if ( smp_processor_id() == 0 )
    {
        set_timer(&calibration_timer, NOW() + EPOCH);
        platform_time_calibration();
    }

    return 0;
}

/* Set clock to <secs,usecs> after 00:00:00 UTC, 1 January, 1970. */
void do_settime(unsigned long secs, unsigned long nsecs, u64 system_time_base)
{
    u64 x;
    u32 y, _wc_sec, _wc_nsec;
    struct domain *d;

    x = (secs * 1000000000ULL) + (u64)nsecs - system_time_base;
    y = do_div(x, 1000000000);

    spin_lock(&wc_lock);
    wc_sec  = _wc_sec  = (u32)x;
    wc_nsec = _wc_nsec = (u32)y;
    spin_unlock(&wc_lock);

    rcu_read_lock(&domlist_read_lock);
    for_each_domain ( d )
        update_domain_wallclock_time(d);
    rcu_read_unlock(&domlist_read_lock);
}

/* Per-CPU communication between rendezvous IRQ and softirq handler. */
struct cpu_calibration {
    u64 local_tsc_stamp;
    s_time_t stime_local_stamp;
    s_time_t stime_master_stamp;
};
static DEFINE_PER_CPU(struct cpu_calibration, cpu_calibration);

/* Softirq handler for per-CPU time calibration. */
static void local_time_calibration(void)
{
    struct cpu_time *t = &this_cpu(cpu_time);
    struct cpu_calibration *c = &this_cpu(cpu_calibration);

    /*
     * System timestamps, extrapolated from local and master oscillators,
     * taken during this calibration and the previous calibration.
     */
    s_time_t prev_local_stime, curr_local_stime;
    s_time_t prev_master_stime, curr_master_stime;

    /* TSC timestamps taken during this calibration and prev calibration. */
    u64 prev_tsc, curr_tsc;

    /*
     * System time and TSC ticks elapsed during the previous calibration
     * 'epoch'. These values are down-shifted to fit in 32 bits.
     */
    u64 stime_elapsed64, tsc_elapsed64;
    u32 stime_elapsed32, tsc_elapsed32;

    /* The accumulated error in the local estimate. */
    u64 local_stime_err;

    /* Error correction to slow down a fast local clock. */
    u32 error_factor = 0;

    /* Calculated TSC shift to ensure 32-bit scale multiplier. */
    int tsc_shift = 0;

    /* The overall calibration scale multiplier. */
    u32 calibration_mul_frac;

    prev_tsc          = t->local_tsc_stamp;
    prev_local_stime  = t->stime_local_stamp;
    prev_master_stime = t->stime_master_stamp;

    /* Disabling IRQs ensures we atomically read cpu_calibration struct. */
    local_irq_disable();
    curr_tsc          = c->local_tsc_stamp;
    curr_local_stime  = c->stime_local_stamp;
    curr_master_stime = c->stime_master_stamp;
    local_irq_enable();

#if 0
    printk("PRE%d: tsc=%"PRIu64" stime=%"PRIu64" master=%"PRIu64"\n",
           smp_processor_id(), prev_tsc, prev_local_stime, prev_master_stime);
    printk("CUR%d: tsc=%"PRIu64" stime=%"PRIu64" master=%"PRIu64
           " -> %"PRId64"\n",
           smp_processor_id(), curr_tsc, curr_local_stime, curr_master_stime,
           curr_master_stime - curr_local_stime);
#endif

    /* Local time warps forward if it lags behind master time. */
    if ( curr_local_stime < curr_master_stime )
        curr_local_stime = curr_master_stime;

    stime_elapsed64 = curr_master_stime - prev_master_stime;
    tsc_elapsed64   = curr_tsc - prev_tsc;

    /*
     * Weirdness can happen if we lose sync with the platform timer.
     * We could be smarter here: resync platform timer with local timer?
     */
    if ( ((s64)stime_elapsed64 < (EPOCH / 2)) )
        goto out;

    /*
     * Calculate error-correction factor. This only slows down a fast local
     * clock (slow clocks are warped forwards). The scale factor is clamped
     * to >= 0.5.
     */
    if ( curr_local_stime != curr_master_stime )
    {
        local_stime_err = curr_local_stime - curr_master_stime;
        if ( local_stime_err > EPOCH )
            local_stime_err = EPOCH;
        error_factor = div_frac(EPOCH, EPOCH + (u32)local_stime_err);
    }

    /*
     * We require 0 < stime_elapsed < 2^31.
     * This allows us to binary shift a 32-bit tsc_elapsed such that:
     * stime_elapsed < tsc_elapsed <= 2*stime_elapsed
     */
    while ( ((u32)stime_elapsed64 != stime_elapsed64) ||
            ((s32)stime_elapsed64 < 0) )
    {
        stime_elapsed64 >>= 1;
        tsc_elapsed64   >>= 1;
    }

    /* stime_master_diff now fits in a 32-bit word. */
    stime_elapsed32 = (u32)stime_elapsed64;

    /* tsc_elapsed <= 2*stime_elapsed */
    while ( tsc_elapsed64 > (stime_elapsed32 * 2) )
    {
        tsc_elapsed64 >>= 1;
        tsc_shift--;
    }

    /* Local difference must now fit in 32 bits. */
    ASSERT((u32)tsc_elapsed64 == tsc_elapsed64);
    tsc_elapsed32 = (u32)tsc_elapsed64;

    /* tsc_elapsed > stime_elapsed */
    ASSERT(tsc_elapsed32 != 0);
    while ( tsc_elapsed32 <= stime_elapsed32 )
    {
        tsc_elapsed32 <<= 1;
        tsc_shift++;
    }

    calibration_mul_frac = div_frac(stime_elapsed32, tsc_elapsed32);
    if ( error_factor != 0 )
        calibration_mul_frac = mul_frac(calibration_mul_frac, error_factor);

#if 0
    printk("---%d: %08x %08x %d\n", smp_processor_id(),
           error_factor, calibration_mul_frac, tsc_shift);
#endif

    /* Record new timestamp information, atomically w.r.t. interrupts. */
    local_irq_disable();
    t->tsc_scale.mul_frac = calibration_mul_frac;
    t->tsc_scale.shift    = tsc_shift;
    t->local_tsc_stamp    = curr_tsc;
    t->stime_local_stamp  = curr_local_stime;
    t->stime_master_stamp = curr_master_stime;
    local_irq_enable();

    update_vcpu_system_time(current);

 out:
    if ( smp_processor_id() == 0 )
    {
        set_timer(&calibration_timer, NOW() + EPOCH);
        platform_time_calibration();
    }
}

/*
 * Rendezvous for all CPUs in IRQ context.
 * Master CPU snapshots the platform timer.
 * All CPUS snapshot their local TSC and extrapolation of system time.
 */
struct calibration_rendezvous {
    cpumask_t cpu_calibration_map;
    atomic_t nr_cpus;
    s_time_t master_stime;
};

static void time_calibration_rendezvous(void *_r)
{
    struct cpu_calibration *c = &this_cpu(cpu_calibration);
    struct calibration_rendezvous *r = _r;
    unsigned int total_cpus = cpus_weight(r->cpu_calibration_map);

    if ( smp_processor_id() == 0 )
    {
        while ( atomic_read(&r->nr_cpus) != (total_cpus - 1) )
            cpu_relax();
        r->master_stime = read_platform_stime();
        mb(); /* write r->master_stime /then/ signal */
        atomic_inc(&r->nr_cpus);
    }
    else
    {
        atomic_inc(&r->nr_cpus);
        while ( atomic_read(&r->nr_cpus) != total_cpus )
            cpu_relax();
        mb(); /* receive signal /then/ read r->master_stime */
    }

    rdtscll(c->local_tsc_stamp);
    c->stime_local_stamp = get_s_time();
    c->stime_master_stamp = r->master_stime;

    raise_softirq(TIME_CALIBRATE_SOFTIRQ);
}

static void time_calibration(void *unused)
{
    struct calibration_rendezvous r = {
        .cpu_calibration_map = cpu_online_map,
        .nr_cpus = ATOMIC_INIT(0)
    };

    /* @wait=1 because we must wait for all cpus before freeing @r. */
    on_selected_cpus(r.cpu_calibration_map,
                     time_calibration_rendezvous, &r, 0, 1);
}

void init_percpu_time(void)
{
    struct cpu_time *t = &this_cpu(cpu_time);
    unsigned long flags;
    s_time_t now;

    local_irq_save(flags);
    rdtscll(t->local_tsc_stamp);
    now = !plt_src.read_counter ? 0 : read_platform_stime();
    local_irq_restore(flags);

    t->stime_master_stamp = now;
    t->stime_local_stamp  = now;

    if ( smp_processor_id() == 0 )
    {
        init_timer(&calibration_timer, time_calibration, NULL, 0);
        set_timer(&calibration_timer, NOW() + EPOCH);
    }
}

/* Late init function (after all CPUs are booted). */
int __init init_xen_time(void)
{
    /* check if TSC is invariant during deep C state
       this is a new feature introduced by Nehalem*/
    if ( cpuid_edx(0x80000007) & (1u<<8) )
        tsc_invariant = 1;

    open_softirq(TIME_CALIBRATE_SOFTIRQ, local_time_calibration);

    init_percpu_time();

    stime_platform_stamp = 0;
    init_platform_timer();

    do_settime(get_cmos_time(), 0, NOW());

    return 0;
}


/* Early init function. */
void __init early_time_init(void)
{
    u64 tmp = init_pit_and_calibrate_tsc();

    set_time_scale(&this_cpu(cpu_time).tsc_scale, tmp);

    do_div(tmp, 1000);
    cpu_khz = (unsigned long)tmp;
    printk("Detected %lu.%03lu MHz processor.\n", 
           cpu_khz / 1000, cpu_khz % 1000);

    setup_irq(0, &irq0);
}

/* force_hpet_broadcast: if true, force using hpet_broadcast to fix lapic stop
   issue for deep C state with pit disabled */
static int force_hpet_broadcast;
boolean_param("hpetbroadcast", force_hpet_broadcast);

/* keep pit enabled for pit_broadcast working while cpuidle enabled */
static int disable_pit_irq(void)
{
    if ( using_pit || !cpu_has_apic || (xen_cpuidle && !force_hpet_broadcast) )
        return 0;

    /*
     * If we do not rely on PIT CH0 then we can use HPET for one-shot timer 
     * emulation when entering deep C states.
     * XXX dom0 may rely on RTC interrupt delivery, so only enable
     * hpet_broadcast if force_hpet_broadcast.
     */
    if ( xen_cpuidle && force_hpet_broadcast )
    {
        hpet_broadcast_init();
        if ( !hpet_broadcast_is_available() )
        {
            printk("HPET broadcast init failed, turn to PIT broadcast.\n");
            return 0;
        }
    }

    /* Disable PIT CH0 timer interrupt. */
    outb_p(0x30, PIT_MODE);
    outb_p(0, PIT_CH0);
    outb_p(0, PIT_CH0);

    return 0;
}
__initcall(disable_pit_irq);

void pit_broadcast_enter(void)
{
    cpu_set(smp_processor_id(), pit_broadcast_mask);
}

void pit_broadcast_exit(void)
{
    int cpu = smp_processor_id();

    if ( cpu_test_and_clear(cpu, pit_broadcast_mask) )
        reprogram_timer(per_cpu(timer_deadline, cpu));
}

int pit_broadcast_is_available(void)
{
    return xen_cpuidle;
}

void send_timer_event(struct vcpu *v)
{
    send_guest_vcpu_virq(v, VIRQ_TIMER);
}

/* Return secs after 00:00:00 localtime, 1 January, 1970. */
unsigned long get_localtime(struct domain *d)
{
    return wc_sec + (wc_nsec + NOW()) / 1000000000ULL 
        + d->time_offset_seconds;
}

/* "cmos_utc_offset" is the difference between UTC time and CMOS time. */
static long cmos_utc_offset; /* in seconds */

int time_suspend(void)
{
    if ( smp_processor_id() == 0 )
    {
        cmos_utc_offset = -get_cmos_time();
        cmos_utc_offset += (wc_sec + (wc_nsec + NOW()) / 1000000000ULL);
        kill_timer(&calibration_timer);

        /* Sync platform timer stamps. */
        platform_time_calibration();
    }

    /* Better to cancel calibration timer for accuracy. */
    clear_bit(TIME_CALIBRATE_SOFTIRQ, &softirq_pending(smp_processor_id()));

    return 0;
}

int time_resume(void)
{
    /*u64 tmp = */init_pit_and_calibrate_tsc();

    /* Disable this while calibrate_tsc_ap() also is skipped. */
    /*set_time_scale(&this_cpu(cpu_time).tsc_scale, tmp);*/

    resume_platform_timer();

    disable_pit_irq();

    init_percpu_time();

    do_settime(get_cmos_time() + cmos_utc_offset, 0, NOW());

    if ( !is_idle_vcpu(current) )
        update_vcpu_system_time(current);

    return 0;
}

int dom0_pit_access(struct ioreq *ioreq)
{
    /* Is Xen using Channel 2? Then disallow direct dom0 access. */
    if ( using_pit )
        return 0;

    switch ( ioreq->addr )
    {
    case PIT_CH2:
        if ( ioreq->dir == IOREQ_READ )
            ioreq->data = inb(PIT_CH2);
        else
            outb(ioreq->data, PIT_CH2);
        return 1;

    case PIT_MODE:
        if ( ioreq->dir == IOREQ_READ )
            return 0; /* urk! */
        switch ( ioreq->data & 0xc0 )
        {
        case 0xc0: /* Read Back */
            if ( ioreq->data & 0x08 )    /* Select Channel 2? */
                outb(ioreq->data & 0xf8, PIT_MODE);
            if ( !(ioreq->data & 0x06) ) /* Select Channel 0/1? */
                return 1; /* no - we're done */
            /* Filter Channel 2 and reserved bit 0. */
            ioreq->data &= ~0x09;
            return 0; /* emulate ch0/1 readback */
        case 0x80: /* Select Counter 2 */
            outb(ioreq->data, PIT_MODE);
            return 1;
        }

    case 0x61:
        if ( ioreq->dir == IOREQ_READ )
            ioreq->data = inb(0x61);
        else
            outb((inb(0x61) & ~3) | (ioreq->data & 3), 0x61);
        return 1;
    }

    return 0;
}

struct tm wallclock_time(void)
{
    uint64_t seconds;

    if ( !wc_sec )
        return (struct tm) { 0 };

    seconds = NOW() + (wc_sec * 1000000000ull) + wc_nsec;
    do_div(seconds, 1000000000);
    return gmtime(seconds);
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

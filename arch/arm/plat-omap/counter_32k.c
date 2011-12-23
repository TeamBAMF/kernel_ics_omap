/*
 * OMAP 32ksynctimer/counter_32k-related code
 *
 * Copyright (C) 2009 Texas Instruments
 * Copyright (C) 2010 Nokia Corporation
 * Tony Lindgren <tony@atomide.com>
 * Added OMAP4 support - Santosh Shilimkar <santosh.shilimkar@ti.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * NOTE: This timer is not the same timer as the old OMAP1 MPU timer.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/sched.h>

#include <asm/sched_clock.h>

#include <plat/common.h>
#include <plat/board.h>

#include <plat/clock.h>


/*
 * 32KHz clocksource ... always available, on pretty most chips except
 * OMAP 730 and 1510.  Other timers could be used as clocksources, with
 * higher resolution in free-running counter modes (e.g. 12 MHz xtal),
 * but systems won't necessarily want to spend resources that way.
 */

#define OMAP16XX_TIMER_32K_SYNCHRONIZED		0xfffbc410

#include <linux/clocksource.h>

/*
 * offset_32k holds the init time counter value. It is then subtracted
 * from every counter read to achieve a counter that counts time from the
 * kernel boot (needed for sched_clock()).
 */
static u32 offset_32k __read_mostly;

#ifdef CONFIG_ARCH_OMAP16XX
static cycle_t notrace omap16xx_32k_read(struct clocksource *cs)
{
	return omap_readl(OMAP16XX_TIMER_32K_SYNCHRONIZED) - offset_32k;
}
#else
#define omap16xx_32k_read	NULL
#endif

#ifdef CONFIG_SOC_OMAP2420
static cycle_t notrace omap2420_32k_read(struct clocksource *cs)
{
	return omap_readl(OMAP2420_32KSYNCT_BASE + 0x10) - offset_32k;
}
#else
#define omap2420_32k_read	NULL
#endif

#ifdef CONFIG_SOC_OMAP2430
static cycle_t notrace omap2430_32k_read(struct clocksource *cs)
{
	return omap_readl(OMAP2430_32KSYNCT_BASE + 0x10) - offset_32k;
}
#else
#define omap2430_32k_read	NULL
#endif

#ifdef CONFIG_ARCH_OMAP3
static cycle_t notrace omap34xx_32k_read(struct clocksource *cs)
{
	return omap_readl(OMAP3430_32KSYNCT_BASE + 0x10) - offset_32k;
}
#else
#define omap34xx_32k_read	NULL
#endif

#ifdef CONFIG_ARCH_OMAP4
static cycle_t notrace omap44xx_32k_read(struct clocksource *cs)
{
	return omap_readl(OMAP4430_32KSYNCT_BASE + 0x10) - offset_32k;
}
#else
#define omap44xx_32k_read	NULL
#endif

/*
 * Kernel assumes that sched_clock can be called early but may not have
 * things ready yet.
 */
static cycle_t notrace omap_32k_read_dummy(struct clocksource *cs)
{
	return 0;
}

static struct clocksource clocksource_32k = {
	.name		= "32k_counter",
	.rating		= 250,
	.read		= omap_32k_read_dummy,
	.mask		= CLOCKSOURCE_MASK(32),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

/*
 * Returns current time from boot in nsecs. It's OK for this to wrap
 * around for now, as it's just a relative time stamp.
 */
static DEFINE_CLOCK_DATA(cd);

/*
 * Constants generated by clocks_calc_mult_shift(m, s, 32768, NSEC_PER_SEC, 60).
 * This gives a resolution of about 30us and a wrap period of about 36hrs.
 */
#define SC_MULT		4000000000u
#define SC_SHIFT	17

static inline unsigned long long notrace _omap_32k_sched_clock(void)
{
	u32 cyc = clocksource_32k.read(&clocksource_32k);
	return cyc_to_fixed_sched_clock(&cd, cyc, (u32)~0, SC_MULT, SC_SHIFT);
}

#ifndef CONFIG_OMAP_MPU_TIMER
unsigned long long notrace sched_clock(void)
{
	return _omap_32k_sched_clock();
}
#else
unsigned long long notrace omap_32k_sched_clock(void)
{
	return _omap_32k_sched_clock();
}
#endif

static void notrace omap_update_sched_clock(void)
{
	u32 cyc = clocksource_32k.read(&clocksource_32k);
	update_sched_clock(&cd, cyc, (u32)~0);
}

/**
 * read_persistent_clock -  Return time from a persistent clock.
 *
 * Reads the time from a source which isn't disabled during PM, the
 * 32k sync timer.  Convert the cycles elapsed since last read into
 * nsecs and adds to a monotonically increasing timespec.
 */
static struct timespec persistent_ts;
static cycles_t cycles;
static DEFINE_SPINLOCK(read_persistent_clock_lock);
void read_persistent_clock(struct timespec *ts)
{
	unsigned long long nsecs;
	cycles_t last_cycles;
	unsigned long flags;

	spin_lock_irqsave(&read_persistent_clock_lock, flags);

	last_cycles = cycles;
	cycles = clocksource_32k.read(&clocksource_32k);

	nsecs = clocksource_cyc2ns(cycles - last_cycles,
				   clocksource_32k.mult, clocksource_32k.shift);

	timespec_add_ns(&persistent_ts, nsecs);

	*ts = persistent_ts;

	spin_unlock_irqrestore(&read_persistent_clock_lock, flags);
}

int __init omap_init_clocksource_32k(void)
{
	static char err[] __initdata = KERN_ERR
			"%s: can't register clocksource!\n";

	if (cpu_is_omap16xx() || cpu_class_is_omap2()) {
		struct clk *sync_32k_ick;

		if (cpu_is_omap16xx())
			clocksource_32k.read = omap16xx_32k_read;
		else if (cpu_is_omap2420())
			clocksource_32k.read = omap2420_32k_read;
		else if (cpu_is_omap2430())
			clocksource_32k.read = omap2430_32k_read;
		else if (cpu_is_omap34xx())
			clocksource_32k.read = omap34xx_32k_read;
		else if (cpu_is_omap44xx())
			clocksource_32k.read = omap44xx_32k_read;
		else
			return -ENODEV;

		sync_32k_ick = clk_get(NULL, "omap_32ksync_ick");
		if (!IS_ERR(sync_32k_ick))
			clk_enable(sync_32k_ick);

		offset_32k = clocksource_32k.read(&clocksource_32k);

		if (clocksource_register_hz(&clocksource_32k, 32768))
			printk(err, clocksource_32k.name);

		init_fixed_sched_clock(&cd, omap_update_sched_clock, 32,
				       32768, SC_MULT, SC_SHIFT);
	}
	return 0;
}

/*
 * arch/sh/kernel/cpu/shmobile/pm.c
 *
 * Power management support code for SuperH Mobile
 *
 *  Copyright (C) 2009 Magnus Damm
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/suspend.h>
#include <asm/suspend.h>
#include <asm/uaccess.h>

/*
 * Notifier lists for pre/post sleep notification
 */
ATOMIC_NOTIFIER_HEAD(sh_mobile_pre_sleep_notifier_list);
ATOMIC_NOTIFIER_HEAD(sh_mobile_post_sleep_notifier_list);

/*
 * Sleep modes available on SuperH Mobile:
 *
 * Sleep mode is just plain "sleep" instruction
 * Sleep Self-Refresh mode is above plus RAM put in Self-Refresh
 * Standby Self-Refresh mode is above plus stopped clocks
 */
#define SUSP_MODE_SLEEP		(SUSP_SH_SLEEP)
#define SUSP_MODE_SLEEP_SF	(SUSP_SH_SLEEP | SUSP_SH_SF)
#define SUSP_MODE_STANDBY_SF	(SUSP_SH_STANDBY | SUSP_SH_SF)

/*
 * The following modes are not there yet:
 *
 * R-standby mode is unsupported, but will be added in the future
 * U-standby mode is low priority since it needs bootloader hacks
 */

#define ILRAM_BASE 0xe5200000

void sh_mobile_call_standby(unsigned long mode)
{
	void *onchip_mem = (void *)ILRAM_BASE;
	struct sh_sleep_data *sdp = onchip_mem;
	void (*standby_onchip_mem)(unsigned long, unsigned long);

	/* code located directly after data structure */
	standby_onchip_mem = (void *)(sdp + 1);

	atomic_notifier_call_chain(&sh_mobile_pre_sleep_notifier_list,
				   mode, NULL);

	/* Let assembly snippet in on-chip memory handle the rest */
	standby_onchip_mem(mode, ILRAM_BASE);

	atomic_notifier_call_chain(&sh_mobile_post_sleep_notifier_list,
				   mode, NULL);
}

extern char sh_mobile_sleep_enter_start;
extern char sh_mobile_sleep_enter_end;

extern char sh_mobile_sleep_resume_start;
extern char sh_mobile_sleep_resume_end;

unsigned long sh_mobile_sleep_supported = SUSP_SH_SLEEP;

void sh_mobile_register_self_refresh(unsigned long flags,
				     void *pre_start, void *pre_end,
				     void *post_start, void *post_end)
{
	void *onchip_mem = (void *)ILRAM_BASE;
	void *vp;
	struct sh_sleep_data *sdp;
	int n;

	/* part 0: data area */
	sdp = onchip_mem;
	sdp->addr.stbcr = 0xa4150020; /* STBCR */
	vp = sdp + 1;

	/* part 1: common code to enter sleep mode */
	n = &sh_mobile_sleep_enter_end - &sh_mobile_sleep_enter_start;
	memcpy(vp, &sh_mobile_sleep_enter_start, n);
	vp += roundup(n, 4);

	/* part 2: board specific code to enter self-refresh mode */
	n = pre_end - pre_start;
	memcpy(vp, pre_start, n);
	sdp->sf_pre = (unsigned long)vp;
	vp += roundup(n, 4);

	/* part 3: board specific code to resume from self-refresh mode */
	n = post_end - post_start;
	memcpy(vp, post_start, n);
	sdp->sf_post = (unsigned long)vp;
	vp += roundup(n, 4);

	/* part 4: common code to resume from sleep mode */
	WARN_ON(vp > (onchip_mem + 0x600));
	vp = onchip_mem + 0x600; /* located at interrupt vector */
	n = &sh_mobile_sleep_resume_end - &sh_mobile_sleep_resume_start;
	memcpy(vp, &sh_mobile_sleep_resume_start, n);

	sh_mobile_sleep_supported |= flags;
}

static int sh_pm_enter(suspend_state_t state)
{
	if (!(sh_mobile_sleep_supported & SUSP_MODE_STANDBY_SF))
		return -ENXIO;

	local_irq_disable();
	set_bl_bit();
	sh_mobile_call_standby(SUSP_MODE_STANDBY_SF);
	local_irq_disable();
	clear_bl_bit();
	return 0;
}

static struct platform_suspend_ops sh_pm_ops = {
	.enter          = sh_pm_enter,
	.valid          = suspend_valid_only_mem,
};

static int __init sh_pm_init(void)
{
	suspend_set_ops(&sh_pm_ops);
	sh_mobile_setup_cpuidle();
	return 0;
}

late_initcall(sh_pm_init);

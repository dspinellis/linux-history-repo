/* linux/arch/arm/mach-s3c2410/mach-jive.c
 *
 * Copyright 2007 Simtec Electronics
 *	Ben Dooks <ben@simtec.co.uk>
 *
 * http://armlinux.simtec.co.uk/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/init.h>
#include <linux/sysdev.h>
#include <linux/serial_core.h>
#include <linux/platform_device.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include <asm/plat-s3c/regs-serial.h>
#include <asm/plat-s3c/nand.h>

#include <asm/arch/regs-power.h>
#include <asm/arch/regs-gpio.h>
#include <asm/arch/regs-mem.h>
#include <asm/arch/regs-lcd.h>

#include <asm/mach-types.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/mtd/partitions.h>

#include <asm/plat-s3c24xx/clock.h>
#include <asm/plat-s3c24xx/devs.h>
#include <asm/plat-s3c24xx/cpu.h>
#include <asm/plat-s3c24xx/pm.h>
#include <asm/plat-s3c24xx/udc.h>

static struct map_desc jive_iodesc[] __initdata = {
};

#define UCON S3C2410_UCON_DEFAULT
#define ULCON S3C2410_LCON_CS8 | S3C2410_LCON_PNONE
#define UFCON S3C2410_UFCON_RXTRIG8 | S3C2410_UFCON_FIFOMODE

static struct s3c2410_uartcfg jive_uartcfgs[] = {
	[0] = {
		.hwport	     = 0,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[1] = {
		.hwport	     = 1,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[2] = {
		.hwport	     = 2,
		.flags	     = 0,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	}
};

/* Jive flash assignment
 *
 * 0x00000000-0x00028000 : uboot
 * 0x00028000-0x0002c000 : uboot env
 * 0x0002c000-0x00030000 : spare
 * 0x00030000-0x00200000 : zimage A
 * 0x00200000-0x01600000 : cramfs A
 * 0x01600000-0x017d0000 : zimage B
 * 0x017d0000-0x02bd0000 : cramfs B
 * 0x02bd0000-0x03fd0000 : yaffs
 */
static struct mtd_partition jive_imageA_nand_part[] = {

#ifdef CONFIG_MACH_JIVE_SHOW_BOOTLOADER
	/* Don't allow access to the bootloader from linux */
	{
		.name           = "uboot",
		.offset         = 0,
		.size           = (160 * SZ_1K),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	},

	/* spare */
        {
                .name           = "spare",
                .offset         = (176 * SZ_1K),
                .size           = (16 * SZ_1K),
        },
#endif

	/* booted images */
        {
		.name		= "kernel (ro)",
		.offset		= (192 * SZ_1K),
		.size		= (SZ_2M) - (192 * SZ_1K),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
        }, {
                .name           = "root (ro)",
                .offset         = (SZ_2M),
                .size           = (20 * SZ_1M),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
        },

	/* yaffs */
	{
		.name		= "yaffs",
		.offset		= (44 * SZ_1M),
		.size		= (20 * SZ_1M),
	},

	/* bootloader environment */
	{
                .name		= "env",
		.offset		= (160 * SZ_1K),
		.size		= (16 * SZ_1K),
	},

	/* upgrade images */
        {
		.name		= "zimage",
		.offset		= (22 * SZ_1M),
		.size		= (2 * SZ_1M) - (192 * SZ_1K),
        }, {
		.name		= "cramfs",
		.offset		= (24 * SZ_1M) - (192*SZ_1K),
		.size		= (20 * SZ_1M),
        },
};

static struct mtd_partition jive_imageB_nand_part[] = {

#ifdef CONFIG_MACH_JIVE_SHOW_BOOTLOADER
	/* Don't allow access to the bootloader from linux */
	{
		.name           = "uboot",
		.offset         = 0,
		.size           = (160 * SZ_1K),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	},

	/* spare */
        {
                .name           = "spare",
                .offset         = (176 * SZ_1K),
                .size           = (16 * SZ_1K),
        },
#endif

	/* booted images */
        {
		.name           = "kernel (ro)",
		.offset         = (22 * SZ_1M),
		.size           = (2 * SZ_1M) - (192 * SZ_1K),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
        },
	{
		.name		= "root (ro)",
		.offset		= (24 * SZ_1M) - (192 * SZ_1K),
                .size		= (20 * SZ_1M),
		.mask_flags	= MTD_WRITEABLE, /* force read-only */
	},

	/* yaffs */
	{
		.name		= "yaffs",
		.offset		= (44 * SZ_1M),
		.size		= (20 * SZ_1M),
        },

	/* bootloader environment */
	{
		.name		= "env",
		.offset		= (160 * SZ_1K),
		.size		= (16 * SZ_1K),
	},

	/* upgrade images */
	{
		.name		= "zimage",
		.offset		= (192 * SZ_1K),
		.size		= (2 * SZ_1M) - (192 * SZ_1K),
        }, {
		.name		= "cramfs",
		.offset		= (2 * SZ_1M),
		.size		= (20 * SZ_1M),
        },
};

static struct s3c2410_nand_set jive_nand_sets[] = {
	[0] = {
		.name           = "flash",
		.nr_chips       = 1,
		.nr_partitions  = ARRAY_SIZE(jive_imageA_nand_part),
		.partitions     = jive_imageA_nand_part,
	},
};

static struct s3c2410_platform_nand jive_nand_info = {
	/* set taken from osiris nand timings, possibly still conservative */
	.tacls		= 30,
	.twrph0		= 55,
	.twrph1		= 40,
	.sets		= jive_nand_sets,
	.nr_sets	= ARRAY_SIZE(jive_nand_sets),
};

static int __init jive_mtdset(char *options)
{
	struct s3c2410_nand_set *nand = &jive_nand_sets[0];
	unsigned long set;

	if (options == NULL || options[0] == '\0')
		return 0;

	if (strict_strtoul(options, 10, &set)) {
		printk(KERN_ERR "failed to parse mtdset=%s\n", options);
		return 0;
	}

	switch (set) {
	case 1:
		nand->nr_partitions = ARRAY_SIZE(jive_imageB_nand_part);
		nand->partitions = jive_imageB_nand_part;
	case 0:
		/* this is already setup in the nand info */
		break;
	default:
		printk(KERN_ERR "Unknown mtd set %ld specified,"
		       "using default.", set);
	}

	return 0;
}

/* parse the mtdset= option given to the kernel command line */
__setup("mtdset=", jive_mtdset);

static struct platform_device *jive_devices[] __initdata = {
	&s3c_device_usb,
	&s3c_device_rtc,
	&s3c_device_wdt,
	&s3c_device_i2c,
	&s3c_device_nand,
	&s3c_device_usbgadget,
};

static struct s3c2410_udc_mach_info jive_udc_cfg __initdata = {
	.vbus_pin	= S3C2410_GPG1,		/* detect is on GPG1 */
};

/* Jive power management device */

#ifdef CONFIG_PM
static int jive_pm_suspend(struct sys_device *sd, pm_message_t state)
{
	/* Write the magic value u-boot uses to check for resume into
	 * the INFORM0 register, and ensure INFORM1 is set to the
	 * correct address to resume from. */

	__raw_writel(0x2BED, S3C2412_INFORM0);
	__raw_writel(virt_to_phys(s3c2410_cpu_resume), S3C2412_INFORM1);

	return 0;
}

static int jive_pm_resume(struct sys_device *sd)
{
	__raw_writel(0x0, S3C2412_INFORM0);
	return 0;
}

#else
#define jive_pm_suspend NULL
#define jive_pm_resume NULL
#endif

static struct sysdev_class jive_pm_sysclass = {
	.name		= "jive-pm",
	.suspend	= jive_pm_suspend,
	.resume		= jive_pm_resume,
};

static struct sys_device jive_pm_sysdev = {
	.cls		= &jive_pm_sysclass,
};

static void __init jive_map_io(void)
{
	s3c24xx_init_io(jive_iodesc, ARRAY_SIZE(jive_iodesc));
	s3c24xx_init_clocks(12000000);
	s3c24xx_init_uarts(jive_uartcfgs, ARRAY_SIZE(jive_uartcfgs));
}

static void __init jive_machine_init(void)
{
	/* register system devices for managing low level suspend */

	sysdev_class_register(&jive_pm_sysclass);
	sysdev_register(&jive_pm_sysdev);

	s3c2410_pm_init();

	s3c_device_nand.dev.platform_data = &jive_nand_info;

	/* Turn off suspend on both USB ports, and switch the
	 * selectable USB port to USB device mode. */

	s3c2410_modify_misccr(S3C2410_MISCCR_USBHOST |
			      S3C2410_MISCCR_USBSUSPND0 |
			      S3C2410_MISCCR_USBSUSPND1, 0x0);

	s3c24xx_udc_set_platdata(&jive_udc_cfg);

	platform_add_devices(jive_devices, ARRAY_SIZE(jive_devices));
}

MACHINE_START(JIVE, "JIVE")
	/* Maintainer: Ben Dooks <ben@fluff.org> */
	.phys_io	= S3C2410_PA_UART,
	.io_pg_offst	= (((u32)S3C24XX_VA_UART) >> 18) & 0xfffc,
	.boot_params	= S3C2410_SDRAM_PA + 0x100,

	.init_irq	= s3c24xx_init_irq,
	.map_io		= jive_map_io,
	.init_machine	= jive_machine_init,
	.timer		= &s3c24xx_timer,
MACHINE_END

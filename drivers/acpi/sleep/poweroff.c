/*
 * poweroff.c - ACPI handler for powering off the system.
 *
 * AKA S5, but it is independent of whether or not the kernel supports
 * any other sleep support in the system.
 *
 * Copyright (c) 2005 Alexey Starikovskiy <alexey.y.starikovskiy@intel.com>
 *
 * This file is released under the GPLv2.
 */

#include <linux/pm.h>
#include <linux/init.h>
#include <acpi/acpi_bus.h>
#include <linux/sched.h>
#include <linux/sysdev.h>
#include <asm/io.h>
#include "sleep.h"

int acpi_sleep_prepare(u32 acpi_state)
{
	/* Flag to do not allow second time invocation for S5 state */
	static int shutdown_prepared = 0;
#ifdef CONFIG_ACPI_SLEEP
	/* do we have a wakeup address for S2 and S3? */
	/* Here, we support only S4BIOS, those we set the wakeup address */
	/* S4OS is only supported for now via swsusp.. */
	if (acpi_state == ACPI_STATE_S3 || acpi_state == ACPI_STATE_S4) {
		if (!acpi_wakeup_address) {
			return -EFAULT;
		}
		acpi_set_firmware_waking_vector((acpi_physical_address)
						virt_to_phys((void *)
							     acpi_wakeup_address));

	}
	ACPI_FLUSH_CPU_CACHE();
	acpi_enable_wakeup_device_prep(acpi_state);
#endif
	if (acpi_state == ACPI_STATE_S5) {
		/* Check if we were already called */
		if (shutdown_prepared)
			return 0;
		acpi_wakeup_gpe_poweroff_prepare();
		shutdown_prepared = 1;
	}
	acpi_enter_sleep_state_prep(acpi_state);
	return 0;
}

void acpi_power_off(void)
{
	printk("%s called\n", __FUNCTION__);
	acpi_sleep_prepare(ACPI_STATE_S5);
	local_irq_disable();
	/* Some SMP machines only can poweroff in boot CPU */
	set_cpus_allowed(current, cpumask_of_cpu(0));
	acpi_enter_sleep_state(ACPI_STATE_S5);
}

#ifdef CONFIG_PM

static int acpi_shutdown(struct sys_device *x)
{
	return acpi_sleep_prepare(ACPI_STATE_S5);
}

static struct sysdev_class acpi_sysclass = {
	set_kset_name("acpi"),
	.shutdown = acpi_shutdown
};

static struct sys_device device_acpi = {
	.id = 0,
	.cls = &acpi_sysclass,
};

#endif

static int acpi_poweroff_init(void)
{
	if (!acpi_disabled) {
		u8 type_a, type_b;
		acpi_status status;

		status =
		    acpi_get_sleep_type_data(ACPI_STATE_S5, &type_a, &type_b);
		if (ACPI_SUCCESS(status)) {
			pm_power_off = acpi_power_off;
#ifdef CONFIG_PM
			{
				int error;
				error = sysdev_class_register(&acpi_sysclass);
				if (!error)
					error = sysdev_register(&device_acpi);
				return error;
			}
#endif
		}
	}
	return 0;
}

late_initcall(acpi_poweroff_init);

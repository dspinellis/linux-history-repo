/*
 * Header for code common to all DaVinci machines.
 *
 * Author: Kevin Hilman, MontaVista Software, Inc. <source@mvista.com>
 *
 * 2007 (c) MontaVista Software, Inc. This file is licensed under
 * the terms of the GNU General Public License version 2. This program
 * is licensed "as is" without any warranty of any kind, whether express
 * or implied.
 */

#ifndef __ARCH_ARM_MACH_DAVINCI_COMMON_H
#define __ARCH_ARM_MACH_DAVINCI_COMMON_H

struct sys_timer;

extern struct sys_timer davinci_timer;

extern void davinci_irq_init(void);
extern void davinci_map_common_io(void);

/* parameters describe VBUS sourcing for host mode */
extern void setup_usb(unsigned mA, unsigned potpgt_msec);

/* parameters describe VBUS sourcing for host mode */
extern void setup_usb(unsigned mA, unsigned potpgt_msec);

#endif /* __ARCH_ARM_MACH_DAVINCI_COMMON_H */

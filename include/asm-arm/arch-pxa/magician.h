/*
 * GPIO and IRQ definitions for HTC Magician PDA phones
 *
 * Copyright (c) 2007 Philipp Zabel
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _MAGICIAN_H_
#define _MAGICIAN_H_

#include <asm/arch/pxa-regs.h>

/*
 * PXA GPIOs
 */

#define GPIO0_MAGICIAN_KEY_POWER		0
#define GPIO9_MAGICIAN_UNKNOWN			9
#define GPIO10_MAGICIAN_GSM_IRQ			10
#define GPIO11_MAGICIAN_GSM_OUT1		11
#define GPIO13_MAGICIAN_CPLD_IRQ		13
#define GPIO18_MAGICIAN_UNKNOWN			18
#define GPIO22_MAGICIAN_VIBRA_EN		22
#define GPIO26_MAGICIAN_GSM_POWER		26
#define GPIO27_MAGICIAN_USBC_PUEN		27
#define GPIO30_MAGICIAN_nCHARGE_EN		30
#define GPIO37_MAGICIAN_KEY_HANGUP		37
#define GPIO38_MAGICIAN_KEY_CONTACTS		38
#define GPIO40_MAGICIAN_GSM_OUT2		40
#define GPIO48_MAGICIAN_UNKNOWN			48
#define GPIO56_MAGICIAN_UNKNOWN			56
#define GPIO57_MAGICIAN_CAM_RESET		57
#define GPIO83_MAGICIAN_nIR_EN			83
#define GPIO86_MAGICIAN_GSM_RESET		86
#define GPIO87_MAGICIAN_GSM_SELECT		87
#define GPIO90_MAGICIAN_KEY_CALENDAR		90
#define GPIO91_MAGICIAN_KEY_CAMERA		91
#define GPIO93_MAGICIAN_KEY_UP			93
#define GPIO94_MAGICIAN_KEY_DOWN		94
#define GPIO95_MAGICIAN_KEY_LEFT		95
#define GPIO96_MAGICIAN_KEY_RIGHT		96
#define GPIO97_MAGICIAN_KEY_ENTER		97
#define GPIO98_MAGICIAN_KEY_RECORD		98
#define GPIO99_MAGICIAN_HEADPHONE_IN		99
#define GPIO100_MAGICIAN_KEY_VOL_UP		100
#define GPIO101_MAGICIAN_KEY_VOL_DOWN 		101
#define GPIO102_MAGICIAN_KEY_PHONE		102
#define GPIO103_MAGICIAN_LED_KP			103
#define GPIO104_MAGICIAN_LCD_POWER_1 		104
#define GPIO105_MAGICIAN_LCD_POWER_2		105
#define GPIO106_MAGICIAN_LCD_POWER_3		106
#define GPIO107_MAGICIAN_DS1WM_IRQ		107
#define GPIO108_MAGICIAN_GSM_READY		108
#define GPIO114_MAGICIAN_UNKNOWN		114
#define GPIO115_MAGICIAN_nPEN_IRQ		115
#define GPIO116_MAGICIAN_nCAM_EN		116
#define GPIO119_MAGICIAN_UNKNOWN		119
#define GPIO120_MAGICIAN_UNKNOWN		120

/*
 * PXA GPIO alternate function mode & direction
 */

#define GPIO0_MAGICIAN_KEY_POWER_MD		(0 | GPIO_IN)
#define GPIO9_MAGICIAN_UNKNOWN_MD		(9 | GPIO_IN)
#define GPIO10_MAGICIAN_GSM_IRQ_MD		(10 | GPIO_IN)
#define GPIO11_MAGICIAN_GSM_OUT1_MD		(11 | GPIO_OUT)
#define GPIO13_MAGICIAN_CPLD_IRQ_MD		(13 | GPIO_IN)
#define GPIO18_MAGICIAN_UNKNOWN_MD		(18 | GPIO_OUT)
#define GPIO22_MAGICIAN_VIBRA_EN_MD		(22 | GPIO_OUT)
#define GPIO26_MAGICIAN_GSM_POWER_MD		(26 | GPIO_OUT)
#define GPIO27_MAGICIAN_USBC_PUEN_MD		(27 | GPIO_OUT)
#define GPIO30_MAGICIAN_nCHARGE_EN_MD		(30 | GPIO_OUT)
#define GPIO37_MAGICIAN_KEY_HANGUP_MD		(37 | GPIO_OUT)
#define GPIO38_MAGICIAN_KEY_CONTACTS_MD		(38 | GPIO_OUT)
#define GPIO40_MAGICIAN_GSM_OUT2_MD		(40 | GPIO_OUT)
#define GPIO48_MAGICIAN_UNKNOWN_MD		(48 | GPIO_OUT)
#define GPIO56_MAGICIAN_UNKNOWN_MD		(56 | GPIO_OUT)
#define GPIO57_MAGICIAN_CAM_RESET_MD		(57 | GPIO_OUT)
#define GPIO83_MAGICIAN_nIR_EN_MD		(83 | GPIO_OUT)
#define GPIO86_MAGICIAN_GSM_RESET_MD		(86 | GPIO_OUT)
#define GPIO87_MAGICIAN_GSM_SELECT_MD		(87 | GPIO_OUT)
#define GPIO90_MAGICIAN_KEY_CALENDAR_MD		(90 | GPIO_OUT)
#define GPIO91_MAGICIAN_KEY_CAMERA_MD		(91 | GPIO_OUT)
#define GPIO93_MAGICIAN_KEY_UP_MD		(93 | GPIO_IN)
#define GPIO94_MAGICIAN_KEY_DOWN_MD		(94 | GPIO_IN)
#define GPIO95_MAGICIAN_KEY_LEFT_MD		(95 | GPIO_IN)
#define GPIO96_MAGICIAN_KEY_RIGHT_MD		(96 | GPIO_IN)
#define GPIO97_MAGICIAN_KEY_ENTER_MD		(97 | GPIO_IN)
#define GPIO98_MAGICIAN_KEY_RECORD_MD		(98 | GPIO_IN)
#define GPIO99_MAGICIAN_HEADPHONE_IN_MD		(99 | GPIO_IN)
#define GPIO100_MAGICIAN_KEY_VOL_UP_MD		(100 | GPIO_IN)
#define GPIO101_MAGICIAN_KEY_VOL_DOWN_MD 	(101 | GPIO_IN)
#define GPIO102_MAGICIAN_KEY_PHONE_MD		(102 | GPIO_IN)
#define GPIO103_MAGICIAN_LED_KP_MD		(103 | GPIO_OUT)
#define GPIO104_MAGICIAN_LCD_POWER_1_MD 	(104 | GPIO_OUT)
#define GPIO105_MAGICIAN_LCD_POWER_2_MD		(105 | GPIO_OUT)
#define GPIO106_MAGICIAN_LCD_POWER_3_MD		(106 | GPIO_OUT)
#define GPIO107_MAGICIAN_DS1WM_IRQ_MD		(107 | GPIO_IN)
#define GPIO108_MAGICIAN_GSM_READY_MD		(108 | GPIO_IN)
#define GPIO114_MAGICIAN_UNKNOWN_MD		(114 | GPIO_OUT)
#define GPIO115_MAGICIAN_nPEN_IRQ_MD		(115 | GPIO_IN)
#define GPIO116_MAGICIAN_nCAM_EN_MD		(116 | GPIO_OUT)
#define GPIO119_MAGICIAN_UNKNOWN_MD		(119 | GPIO_OUT)
#define GPIO120_MAGICIAN_UNKNOWN_MD		(120 | GPIO_OUT)

#endif /* _MAGICIAN_H_ */

/*
 * linux/drivers/video/fonts.c -- `Soft' font definitions
 *
 *    Created 1995 by Geert Uytterhoeven
 *    Rewritten 1998 by Martin Mares <mj@ucw.cz>
 *
 *	2001 - Documented with DocBook
 *	- Brad Douglas <brad@neruo.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#if defined(__mc68000__) || defined(CONFIG_APUS)
#include <asm/setup.h>
#endif
#include <linux/font.h>

#define NO_FONTS

static struct font_desc *fonts[] = {
#ifdef CONFIG_FONT_8x8
#undef NO_FONTS
    &font_vga_8x8,
#endif
#ifdef CONFIG_FONT_8x16
#undef NO_FONTS
    &font_vga_8x16,
#endif
#ifdef CONFIG_FONT_6x11
#undef NO_FONTS
    &font_vga_6x11,
#endif
#ifdef CONFIG_FONT_SUN8x16
#undef NO_FONTS
    &font_sun_8x16,
#endif
#ifdef CONFIG_FONT_SUN12x22
#undef NO_FONTS
    &font_sun_12x22,
#endif
#ifdef CONFIG_FONT_ACORN_8x8
#undef NO_FONTS
    &font_acorn_8x8,
#endif
#ifdef CONFIG_FONT_PEARL_8x8
#undef NO_FONTS
    &font_pearl_8x8,
#endif
#ifdef CONFIG_FONT_MINI_4x6
#undef NO_FONTS
    &font_mini_4x6,
#endif
};

#define num_fonts (sizeof(fonts)/sizeof(*fonts))

#ifdef NO_FONTS
#error No fonts configured.
#endif


/**
 *	find_font - find a font
 *	@name: string name of a font
 *
 *	Find a specified font with string name @name.
 *
 *	Returns %NULL if no font found, or a pointer to the
 *	specified font.
 *
 */

struct font_desc *find_font(char *name)
{
   unsigned int i;

   for (i = 0; i < num_fonts; i++)
      if (!strcmp(fonts[i]->name, name))
	  return fonts[i];
   return NULL;
}


/**
 *	get_default_font - get default font
 *	@xres: screen size of X
 *	@yres: screen size of Y
 *
 *	Get the default font for a specified screen size.
 *	Dimensions are in pixels.
 *
 *	Returns %NULL if no font is found, or a pointer to the
 *	chosen font.
 *
 */

struct font_desc *get_default_font(int xres, int yres)
{
    int i, c, cc;
    struct font_desc *f, *g;

    g = NULL;
    cc = -10000;
    for(i=0; i<num_fonts; i++) {
	f = fonts[i];
	c = f->pref;
#if defined(__mc68000__) || defined(CONFIG_APUS)
#ifdef CONFIG_FONT_PEARL_8x8
	if (MACH_IS_AMIGA && f->idx == PEARL8x8_IDX)
	    c = 100;
#endif
#ifdef CONFIG_FONT_6x11
	if (MACH_IS_MAC && xres < 640 && f->idx == VGA6x11_IDX)
	    c = 100;
#endif
#endif
	if ((yres < 400) == (f->height <= 8))
	    c += 1000;
	if (c > cc) {
	    cc = c;
	    g = f;
	}
    }
    return g;
}

EXPORT_SYMBOL(fonts);
EXPORT_SYMBOL(find_font);
EXPORT_SYMBOL(get_default_font);

MODULE_AUTHOR("James Simmons <jsimmons@users.sf.net>");
MODULE_DESCRIPTION("Console Fonts");
MODULE_LICENSE("GPL");

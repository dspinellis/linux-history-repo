/*
    saa7127 interface functions
    Copyright (C) 2004-2007  Hans Verkuil <hverkuil@xs4all.nl>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

void ivtv_set_wss(struct ivtv *itv, int enabled, int mode);
void ivtv_set_cc(struct ivtv *itv, int mode, u8 cc1, u8 cc2, u8 cc3, u8 cc4);
void ivtv_set_vps(struct ivtv *itv, int enabled, u8 vps1, u8 vps2, u8 vps3,
		  u8 vps4, u8 vps5);
void ivtv_encoder_enable(struct ivtv *itv, int enabled);
void ivtv_video_set_io(struct ivtv *itv);

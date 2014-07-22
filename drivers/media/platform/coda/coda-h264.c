/*
 * Coda multi-standard codec IP - H.264 helper functions
 *
 * Copyright (C) 2012 Vista Silicon S.L.
 *    Javier Martin, <javier.martin@vista-silicon.com>
 *    Xavier Duret
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/videodev2.h>
#include <coda.h>

static const u8 coda_filler_size[8] = { 0, 7, 14, 13, 12, 11, 10, 9 };

static const u8 *coda_find_nal_header(const u8 *buf, const u8 *end)
{
	u32 val = 0xffffffff;

	do {
		val = val << 8 | *buf++;
		if (buf >= end)
			return NULL;
	} while (val != 0x00000001);

	return buf;
}

int coda_sps_parse_profile(struct coda_ctx *ctx, struct vb2_buffer *vb)
{
	const u8 *buf = vb2_plane_vaddr(vb, 0);
	const u8 *end = buf + vb2_get_plane_payload(vb, 0);

	/* Find SPS header */
	do {
		buf = coda_find_nal_header(buf, end);
		if (!buf)
			return -EINVAL;
	} while ((*buf++ & 0x1f) != 0x7);

	ctx->params.h264_profile_idc = buf[0];
	ctx->params.h264_level_idc = buf[2];

	return 0;
}

int coda_h264_filler_nal(int size, char *p)
{
	if (size < 6)
		return -EINVAL;

	p[0] = 0x00;
	p[1] = 0x00;
	p[2] = 0x00;
	p[3] = 0x01;
	p[4] = 0x0c;
	memset(p + 5, 0xff, size - 6);
	/* Add rbsp stop bit and trailing at the end */
	p[size - 1] = 0x80;

	return 0;
}

int coda_h264_padding(int size, char *p)
{
	int nal_size;
	int diff;

	diff = size - (size & ~0x7);
	if (diff == 0)
		return 0;

	nal_size = coda_filler_size[diff];
	coda_h264_filler_nal(nal_size, p);

	return nal_size;
}

int coda_h264_profile(int profile_idc)
{
	switch (profile_idc) {
	case 66: return V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE;
	case 77: return V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
	case 88: return V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED;
	case 100: return V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
	default: return -EINVAL;
	}
}

int coda_h264_level(int level_idc)
{
	switch (level_idc) {
	case 10: return V4L2_MPEG_VIDEO_H264_LEVEL_1_0;
	case 9:  return V4L2_MPEG_VIDEO_H264_LEVEL_1B;
	case 11: return V4L2_MPEG_VIDEO_H264_LEVEL_1_1;
	case 12: return V4L2_MPEG_VIDEO_H264_LEVEL_1_2;
	case 13: return V4L2_MPEG_VIDEO_H264_LEVEL_1_3;
	case 20: return V4L2_MPEG_VIDEO_H264_LEVEL_2_0;
	case 21: return V4L2_MPEG_VIDEO_H264_LEVEL_2_1;
	case 22: return V4L2_MPEG_VIDEO_H264_LEVEL_2_2;
	case 30: return V4L2_MPEG_VIDEO_H264_LEVEL_3_0;
	case 31: return V4L2_MPEG_VIDEO_H264_LEVEL_3_1;
	case 32: return V4L2_MPEG_VIDEO_H264_LEVEL_3_2;
	case 40: return V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
	case 41: return V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
	default: return -EINVAL;
	}
}

struct rbsp {
	char *buf;
	int pos;
};

static inline int rbsp_read_bit(struct rbsp *rbsp)
{
	int shift = 7 - (rbsp->pos % 8);
	int ofs = rbsp->pos++ / 8;

	return (rbsp->buf[ofs] >> shift) & 1;
}

static inline void rbsp_write_bit(struct rbsp *rbsp, int bit)
{
	int shift = 7 - (rbsp->pos % 8);
	int ofs = rbsp->pos++ / 8;

	rbsp->buf[ofs] &= ~BIT(shift);
	rbsp->buf[ofs] |= bit << shift;
}

static inline int rbsp_read_bits(struct rbsp *rbsp, int num)
{
	int i, ret = 0;

	for (i = 0; i < num; i++)
		ret |= rbsp_read_bit(rbsp) << (num - i - 1);

	return ret;
}

static void rbsp_write_bits(struct rbsp *rbsp, int num, int value)
{
	while (num--)
		rbsp_write_bit(rbsp, (value >> num) & 1);
}

static int rbsp_read_uev(struct rbsp *rbsp)
{
	int leading_zero_bits = 0;
	int tmp = 0;

	while (!rbsp_read_bit(rbsp))
		leading_zero_bits++;

	if (leading_zero_bits > 0)
		tmp = rbsp_read_bits(rbsp, leading_zero_bits);

	return (1 << leading_zero_bits) - 1 + tmp;
}

static void rbsp_write_uev(struct rbsp *rbsp, int value)
{
	int tmp = value + 1;
	int leading_zero_bits = fls(tmp) - 1;
	int i;

	for (i = 0; i < leading_zero_bits; i++)
		rbsp_write_bit(rbsp, 0);

	rbsp_write_bits(rbsp, leading_zero_bits + 1, tmp);
}

static int rbsp_read_sev(struct rbsp *rbsp)
{
	int tmp = rbsp_read_uev(rbsp);

	if (tmp & 1)
		return (tmp + 1) / 2;
	else
		return -(tmp / 2);
}

void coda_sps_fixup(struct coda_ctx *ctx, int width, int height, char *buf,
		    int *size)
{
	int profile_idc, level_idc, seq_parameter_set_id;
	int /* log2_max_frame_num_minus4, */ pic_order_cnt_type;
	int max_num_ref_frames, gaps_in_frame_num_value_allowed_flag;
	int pic_width_in_mbs_minus1, pic_height_in_map_units_minus1;
	int frame_mbs_only_flag, direct_8x8_inference_flag, frame_cropping_flag;
	int vui_parameters_present_flag, crop_right, crop_bottom;
	struct rbsp sps;
	int pos;

	sps.buf = buf + 5; /* Skip NAL header */
	sps.pos = 0;

	profile_idc = rbsp_read_bits(&sps, 8);
	sps.pos += 8; /* Skip constraint_set[0-5]_flag, reserved_zero_2bits */
	level_idc = rbsp_read_bits(&sps, 8);
	seq_parameter_set_id = rbsp_read_uev(&sps);

	if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
	    profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
	    profile_idc == 86 || profile_idc == 118 || profile_idc == 128) {
		dev_err(ctx->fh.vdev->dev_parent,
			"%s: Handling profile_idc %d not implemented\n",
			__func__, profile_idc);
		return;
	}

	/* log2_max_frame_num_minus4 = */ rbsp_read_uev(&sps);
	pic_order_cnt_type = rbsp_read_uev(&sps);

	if (pic_order_cnt_type == 0) {
		/* int log2_max_pic_order_cnt_lsb_minus4 = */ rbsp_read_uev(&sps);
	} else if (pic_order_cnt_type == 1) {
		int i, num_ref_frames_in_pic_order_cnt_cycle;

		/* int delta_pic_order_always_zero_flag = */ rbsp_read_bit(&sps);
		/* int offset_for_non_ref_pic = */ rbsp_read_sev(&sps);
		/* int offset_for_top_to_bottom_field = */ rbsp_read_sev(&sps);

		num_ref_frames_in_pic_order_cnt_cycle = rbsp_read_uev(&sps);
		for (i = 0; i < num_ref_frames_in_pic_order_cnt_cycle; i++) {
			/* int offset_for_ref_frame = */ rbsp_read_sev(&sps);
		}
	}

	max_num_ref_frames = rbsp_read_uev(&sps);

	gaps_in_frame_num_value_allowed_flag = rbsp_read_bit(&sps);
	pic_width_in_mbs_minus1 = rbsp_read_uev(&sps);
	pic_height_in_map_units_minus1 = rbsp_read_uev(&sps);
	frame_mbs_only_flag = rbsp_read_bit(&sps);
	if (!frame_mbs_only_flag) {
		/* int mb_adaptive_frame_field_flag = */ rbsp_read_bit(&sps);
	}
	direct_8x8_inference_flag = rbsp_read_bit(&sps);

	pos = sps.pos;
	frame_cropping_flag = rbsp_read_bit(&sps);
	if (frame_cropping_flag) {
		int crop_left, crop_right, crop_top, crop_bottom;
		crop_left = rbsp_read_uev(&sps);
		crop_right = rbsp_read_uev(&sps);
		crop_top = rbsp_read_uev(&sps);
		crop_bottom = rbsp_read_uev(&sps);
	}
	vui_parameters_present_flag = rbsp_read_bit(&sps);
	if (vui_parameters_present_flag) {
		dev_err(ctx->fh.vdev->dev_parent,
			"%s: Handling vui_parameters not implemented\n",
			__func__);
		return;
	}

	crop_right = round_up(width, 16) - width;
	crop_bottom = round_up(height, 16) - height;
	crop_right /= 2;
	if (frame_mbs_only_flag)
		crop_bottom /= 2;
	else
		crop_bottom /= 4;

	sps.pos = pos;
	frame_cropping_flag = 1;
	rbsp_write_bit(&sps, frame_cropping_flag);
	rbsp_write_uev(&sps, 0); /* crop_left */
	rbsp_write_uev(&sps, crop_right);
	rbsp_write_uev(&sps, 0); /* crop_top */
	rbsp_write_uev(&sps, crop_bottom);
	rbsp_write_bit(&sps, vui_parameters_present_flag);
	rbsp_write_bit(&sps, 1);
	*size = 5 + DIV_ROUND_UP(sps.pos, 8);
}

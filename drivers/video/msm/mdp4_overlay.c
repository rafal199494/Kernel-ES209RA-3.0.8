vim/* Copyright (c) 2009-2010, Code Aurora Forum. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Code Aurora Forum nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * Alternatively, provided that this notice is retained in full, this software
 * may be relicensed by the recipient under the terms of the GNU General Public
 * License version 2 ("GPL") and only version 2, in which case the provisions of
 * the GPL apply INSTEAD OF those given above.  If the recipient relicenses the
 * software under the GPL, then the identification text in the MODULE_LICENSE
 * macro must be changed to reflect "GPLv2" instead of "Dual BSD/GPL".  Once a
 * recipient changes the license terms to the GPL, subsequent recipients shall
 * not relicense under alternate licensing terms, including the BSD or dual
 * BSD/GPL terms.  In addition, the following license statement immediately
 * below and between the words START and END shall also then apply when this
 * software is relicensed under the GPL:
 *
 * START
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 and only version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * END
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/hrtimer.h>
#include <linux/clk.h>
#include <mach/hardware.h>
#include <mach/iommu_domains.h>
#include <mach/iommu.h>
#include <linux/io.h>
#include <linux/debugfs.h>
#include <linux/fb.h>
#include <linux/msm_mdp.h>
#include <linux/file.h>
#include <linux/android_pmem.h>
#include <linux/major.h>
#include <asm/system.h>
#include <asm/mach-types.h>
#include <linux/semaphore.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/msm_kgsl.h>
#include "mdp.h"
#include "msm_fb.h"
#include "mdp4.h"

struct mdp4_overlay_ctrl {
	struct mdp4_overlay_pipe plist[MDP4_MAX_OVERLAY_PIPE];
	struct mdp4_overlay_pipe *stage[MDP4_MAX_MIXER][MDP4_MAX_STAGE];
} mdp4_overlay_db = {
	.plist = {
		{
			.pipe_type = OVERLAY_TYPE_RGB,
			.pipe_num = OVERLAY_PIPE_RGB1,
			.pipe_ndx = 1,
		},
		{
			.pipe_type = OVERLAY_TYPE_RGB,
			.pipe_num = OVERLAY_PIPE_RGB2,
			.pipe_ndx = 2,
		},
		{
			.pipe_type = OVERLAY_TYPE_VG,
			.pipe_num = OVERLAY_PIPE_VG1,
			.pipe_ndx = 3,
		},
		{
			.pipe_type = OVERLAY_TYPE_VG,
			.pipe_num = OVERLAY_PIPE_VG2,
			.pipe_ndx = 4,
		}
	}
};

static struct mdp4_overlay_ctrl *ctrl = &mdp4_overlay_db;
static struct ion_client *display_iclient;

void mdp4_overlay_iommu_unmap_freelist(int mixer)
{
	int i;
	struct ion_handle *ihdl;
	struct iommu_free_list *flist;

	mutex_lock(&iommu_mutex);
	flist = &ctrl->iommu_free[mixer];
	if (flist->total == 0) {
		mutex_unlock(&iommu_mutex);
		return;
	}
	for (i = 0; i < IOMMU_FREE_LIST_MAX; i++) {
		ihdl = flist->ihdl[i];
		if (ihdl == NULL)
			continue;
		pr_debug("%s: mixer=%d i=%d ihdl=0x%p\n", __func__,
					mixer, i, ihdl);
		ion_unmap_iommu(display_iclient, ihdl, DISPLAY_READ_DOMAIN,
							GEN_POOL);
		mdp4_stat.iommu_unmap++;
		pr_debug("%s: map=%d unmap=%d drop=%d\n", __func__,
			(int)mdp4_stat.iommu_map, (int)mdp4_stat.iommu_unmap,
				(int)mdp4_stat.iommu_drop);
		ion_free(display_iclient, ihdl);
		flist->ihdl[i] = NULL;
	}

	flist->fndx = 0;
	flist->total = 0;
	mutex_unlock(&iommu_mutex);
}

void mdp4_overlay_iommu_2freelist(int mixer, struct ion_handle *ihdl)
{
	struct iommu_free_list *flist;

	flist = &ctrl->iommu_free[mixer];
	if (flist->fndx >= IOMMU_FREE_LIST_MAX) {
		pr_err("%s: Error, mixer=%d iommu fndx=%d\n",
				__func__, mixer, flist->fndx);
		mdp4_stat.iommu_drop++;
		return;
	}

	pr_debug("%s: add mixer=%d fndx=%d ihdl=0x%p\n", __func__,
				mixer, flist->fndx, ihdl);

	flist->total++;
	flist->ihdl[flist->fndx++] = ihdl;
}

void mdp4_overlay_iommu_pipe_free(int ndx, int all)
{
	struct mdp4_overlay_pipe *pipe;
	struct mdp4_iommu_pipe_info *iom;
	int plane, mixer;

	pipe = mdp4_overlay_ndx2pipe(ndx);
	if (pipe == NULL)
		return;

	mutex_lock(&iommu_mutex);
	mixer = pipe->mixer_num;
	iom = &pipe->iommu;
	pr_debug("%s: mixer=%d ndx=%d all=%d\n", __func__,
				mixer, pipe->pipe_ndx, all);
	for (plane = 0; plane < MDP4_MAX_PLANE; plane++) {
		if (iom->prev_ihdl[plane]) {
			mdp4_overlay_iommu_2freelist(mixer,
					iom->prev_ihdl[plane]);
			iom->prev_ihdl[plane] = NULL;
		}
		if (all && iom->ihdl[plane]) {
			mdp4_overlay_iommu_2freelist(mixer, iom->ihdl[plane]);
			iom->ihdl[plane] = NULL;
		}
	}
	mutex_unlock(&iommu_mutex);
}

int mdp4_overlay_iommu_map_buf(int mem_id,
	struct mdp4_overlay_pipe *pipe, unsigned int plane,
	unsigned long *start, unsigned long *len,
	struct ion_handle **srcp_ihdl)
{
	struct mdp4_iommu_pipe_info *iom;

	if (!display_iclient)
		return -EINVAL;

	*srcp_ihdl = ion_import_dma_buf(display_iclient, mem_id);
	if (IS_ERR_OR_NULL(*srcp_ihdl)) {
		pr_err("ion_import_dma_buf() failed\n");
		return PTR_ERR(*srcp_ihdl);
	}
	pr_debug("%s(): ion_hdl %p, mixer %u, pipe %u, plane %u\n",
		 __func__, *srcp_ihdl, pipe->mixer_num, pipe->pipe_ndx, plane);
	if (ion_map_iommu(display_iclient, *srcp_ihdl,
		DISPLAY_READ_DOMAIN, GEN_POOL, SZ_4K, 0, start,
		len, 0, ION_IOMMU_UNMAP_DELAYED)) {
		ion_free(display_iclient, *srcp_ihdl);
		pr_err("ion_map_iommu() failed\n");
		return -EINVAL;
	}

	mutex_lock(&iommu_mutex);
	iom = &pipe->iommu;
	if (iom->prev_ihdl[plane]) {
		mdp4_overlay_iommu_2freelist(pipe->mixer_num,
						iom->prev_ihdl[plane]);
		mdp4_stat.iommu_drop++;
		pr_err("%s: dropped, ndx=%d plane=%d\n", __func__,
						pipe->pipe_ndx, plane);
	}
	iom->prev_ihdl[plane] = iom->ihdl[plane];
	iom->ihdl[plane] = *srcp_ihdl;
	mdp4_stat.iommu_map++;

	pr_debug("%s: ndx=%d plane=%d prev=0x%p cur=0x%p start=0x%lx len=%lx\n",
		 __func__, pipe->pipe_ndx, plane, iom->prev_ihdl[plane],
			iom->ihdl[plane], *start, *len);
	mutex_unlock(&iommu_mutex);
	return 0;
}

static struct mdp4_iommu_pipe_info mdp_iommu[MDP4_MIXER_MAX][OVERLAY_PIPE_MAX];

void mdp4_iommu_unmap(struct mdp4_overlay_pipe *pipe)
{
	struct mdp4_iommu_pipe_info *iom_pipe_info;
	unsigned char i, j;

	if (!display_iclient)
		return;

	for (j = 0; j < OVERLAY_PIPE_MAX; j++) {
		iom_pipe_info = &mdp_iommu[pipe->mixer_num][j];
		for (i = 0; i < MDP4_MAX_PLANE; i++) {
			if (iom_pipe_info->prev_ihdl[i]) {
				pr_debug("%s(): mixer %u, pipe %u, plane %u, "
					"prev_ihdl %p\n", __func__,
					pipe->mixer_num, j + 1, i,
					iom_pipe_info->prev_ihdl[i]);
				ion_unmap_iommu(display_iclient,
					iom_pipe_info->prev_ihdl[i],
					DISPLAY_READ_DOMAIN, GEN_POOL);
				ion_free(display_iclient,
					iom_pipe_info->prev_ihdl[i]);
				iom_pipe_info->prev_ihdl[i] = NULL;
			}

			if (iom_pipe_info->mark_unmap) {
				if (iom_pipe_info->ihdl[i]) {
					pr_debug("%s(): MARK, mixer %u, pipe %u, plane %u, "
						"ihdl %p\n", __func__,
						pipe->mixer_num, j + 1, i,
						iom_pipe_info->ihdl[i]);
					ion_unmap_iommu(display_iclient,
						iom_pipe_info->ihdl[i],
						DISPLAY_READ_DOMAIN, GEN_POOL);
					ion_free(display_iclient,
						iom_pipe_info->ihdl[i]);
					iom_pipe_info->ihdl[i] = NULL;
				}
			}
		}
		iom_pipe_info->mark_unmap = 0;
	}
}
void mdp4_overlay_dmae_cfg(struct msm_fb_data_type *mfd, int lcdc)
{
	uint32	dmae_cfg_reg;

#ifdef DMAE_DEFLAGER
	dmae_cfg_reg = DMA_DEFLKR_EN;
#else
	dmae_cfg_reg = 0;
#endif
	if (mfd->fb_imgType == MDP_BGR_565)
		dmae_cfg_reg |= DMA_PACK_PATTERN_BGR;
	else
		dmae_cfg_reg |= DMA_PACK_PATTERN_RGB;


	if (mfd->panel_info.bpp == 18) {
		dmae_cfg_reg |= DMA_DSTC0G_6BITS |	/* 666 18BPP */
		    DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;
	} else if (mfd->panel_info.bpp == 16) {
		dmae_cfg_reg |= DMA_DSTC0G_6BITS |	/* 565 16BPP */
		    DMA_DSTC1B_5BITS | DMA_DSTC2R_5BITS;
	} else {
		dmae_cfg_reg |= DMA_DSTC0G_8BITS |	/* 888 16BPP */
		    DMA_DSTC1B_8BITS | DMA_DSTC2R_8BITS;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* dma2 config register */
	MDP_OUTP(MDP_BASE + 0xb0000, dmae_cfg_reg);
	MDP_OUTP(MDP_BASE + 0xb0070, 0xff0000);
	MDP_OUTP(MDP_BASE + 0xb0074, 0xff0000);
	MDP_OUTP(MDP_BASE + 0xb0078, 0xff0000);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_dmae_xy(struct mdp4_overlay_pipe *pipe)
{

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* dma_p source */
	MDP_OUTP(MDP_BASE + 0xb0004,
			(pipe->src_height << 16 | pipe->src_width));
	MDP_OUTP(MDP_BASE + 0xb0008, pipe->srcp0_addr);
	MDP_OUTP(MDP_BASE + 0xb000c, pipe->srcp0_ystride);

	/* dma_p dest */
	MDP_OUTP(MDP_BASE + 0xb0010, (pipe->dst_y << 16 | pipe->dst_x));

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_dmap_cfg(struct msm_fb_data_type *mfd, int lcdc)
{
	uint32	dma2_cfg_reg;

	dma2_cfg_reg = DMA_DITHER_EN;

	if (mfd->fb_imgType == MDP_BGR_565)
		dma2_cfg_reg |= DMA_PACK_PATTERN_BGR;
	else
		dma2_cfg_reg |= DMA_PACK_PATTERN_RGB;


	if (mfd->panel_info.bpp == 18) {
		dma2_cfg_reg |= DMA_DSTC0G_6BITS |	/* 666 18BPP */
		    DMA_DSTC1B_6BITS | DMA_DSTC2R_6BITS;
	} else if (mfd->panel_info.bpp == 16) {
		dma2_cfg_reg |= DMA_DSTC0G_6BITS |	/* 565 16BPP */
		    DMA_DSTC1B_5BITS | DMA_DSTC2R_5BITS;
	} else {
		dma2_cfg_reg |= DMA_DSTC0G_8BITS |	/* 888 16BPP */
		    DMA_DSTC1B_8BITS | DMA_DSTC2R_8BITS;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	if (lcdc)
		dma2_cfg_reg |= DMA_PACK_ALIGN_MSB;

	/* dma2 config register */
	MDP_OUTP(MDP_BASE + 0x90000, dma2_cfg_reg);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_dmap_xy(struct mdp4_overlay_pipe *pipe)
{
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* dma_p source */
	MDP_OUTP(MDP_BASE + 0x90004,
			(pipe->src_height << 16 | pipe->src_width));
	MDP_OUTP(MDP_BASE + 0x90008, pipe->srcp0_addr);
	MDP_OUTP(MDP_BASE + 0x9000c, pipe->srcp0_ystride);

	/* dma_p dest */
	MDP_OUTP(MDP_BASE + 0x90010, (pipe->dst_y << 16 | pipe->dst_x));

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

#define MDP4_VG_PHASE_STEP_DEFAULT	0x20000000
#define MDP4_VG_PHASE_STEP_SHIFT	29

static int mdp4_leading_0(uint32 num)
{
	uint32 bit = 0x80000000;
	int i;

	for (i = 0; i < 32; i++) {
		if (bit & num)
			return i;
		bit >>= 1;
	}

	return i;
}

static uint32 mdp4_scale_phase_step(int f_num, uint32 src, uint32 dst)
{
	uint32 val;
	int	n;

	n = mdp4_leading_0(src);
	if (n > f_num)
		n = f_num;
	val = src << n;	/* maximum to reduce lose of resolution */
	val /= dst;
	if (n < f_num) {
		n = f_num - n;
		val <<= n;
	}

	return val;
}

static void mdp4_scale_setup(struct mdp4_overlay_pipe *pipe)
{

	pipe->phasex_step = MDP4_VG_PHASE_STEP_DEFAULT;
	pipe->phasey_step = MDP4_VG_PHASE_STEP_DEFAULT;

	if (pipe->dst_h && pipe->src_h != pipe->dst_h) {
		if (pipe->dst_h >= pipe->src_h * 8)	/* too much */
			return;
		pipe->op_mode |= MDP4_OP_SCALEY_EN;

		if (pipe->pipe_type == OVERLAY_TYPE_VG) {
			if (pipe->dst_h <= (pipe->src_h / 4))
				pipe->op_mode |= MDP4_OP_SCALEY_MN_PHASE;
			else
				pipe->op_mode |= MDP4_OP_SCALEY_FIR;
		}

		pipe->phasey_step = mdp4_scale_phase_step(29,
					pipe->src_h, pipe->dst_h);
	}

	if (pipe->dst_w && pipe->src_w != pipe->dst_w) {
		if (pipe->dst_w >= pipe->src_w * 8)	/* too much */
			return;
		pipe->op_mode |= MDP4_OP_SCALEX_EN;

		if (pipe->pipe_type == OVERLAY_TYPE_VG) {
			if (pipe->dst_w <= (pipe->src_w / 4))
				pipe->op_mode |= MDP4_OP_SCALEX_MN_PHASE;
			else
				pipe->op_mode |= MDP4_OP_SCALEX_FIR;
		}

		pipe->phasex_step = mdp4_scale_phase_step(29,
					pipe->src_w, pipe->dst_w);
	}
}

void mdp4_overlay_rgb_setup(struct mdp4_overlay_pipe *pipe)
{
	char *rgb_base;
	uint32 src_size, src_xy, dst_size, dst_xy;
	uint32 format, pattern;

	rgb_base = MDP_BASE + MDP4_RGB_BASE;
	rgb_base += (MDP4_RGB_OFF * pipe->pipe_num);

	src_size = ((pipe->src_h << 16) | pipe->src_w);
	src_xy = ((pipe->src_y << 16) | pipe->src_x);
	dst_size = ((pipe->dst_h << 16) | pipe->dst_w);
	dst_xy = ((pipe->dst_y << 16) | pipe->dst_x);

	format = mdp4_overlay_format(pipe);
	pattern = mdp4_overlay_unpack_pattern(pipe);

#ifdef MDP4_IGC_LUT_ENABLE
	pipe->op_mode |= MDP4_OP_IGC_LUT_EN;
#endif

	mdp4_scale_setup(pipe);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	outpdw(rgb_base + 0x0000, src_size);	/* MDP_RGB_SRC_SIZE */
	outpdw(rgb_base + 0x0004, src_xy);	/* MDP_RGB_SRC_XY */
	outpdw(rgb_base + 0x0008, dst_size);	/* MDP_RGB_DST_SIZE */
	outpdw(rgb_base + 0x000c, dst_xy);	/* MDP_RGB_DST_XY */

	outpdw(rgb_base + 0x0010, pipe->srcp0_addr);
	outpdw(rgb_base + 0x0040, pipe->srcp0_ystride);

	outpdw(rgb_base + 0x0050, format);/* MDP_RGB_SRC_FORMAT */
	outpdw(rgb_base + 0x0054, pattern);/* MDP_RGB_SRC_UNPACK_PATTERN */
	outpdw(rgb_base + 0x0058, pipe->op_mode);/* MDP_RGB_OP_MODE */
	outpdw(rgb_base + 0x005c, pipe->phasex_step);
	outpdw(rgb_base + 0x0060, pipe->phasey_step);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_vg_setup(struct mdp4_overlay_pipe *pipe)
{
	char *vg_base;
	uint32 frame_size, src_size, src_xy, dst_size, dst_xy;
	uint32 format, pattern;

	vg_base = MDP_BASE + MDP4_VIDEO_BASE;
	vg_base += (MDP4_VIDEO_OFF * pipe->pipe_num);

	frame_size = ((pipe->src_height << 16) | pipe->src_width);
	src_size = ((pipe->src_h << 16) | pipe->src_w);
	src_xy = ((pipe->src_y << 16) | pipe->src_x);
	dst_size = ((pipe->dst_h << 16) | pipe->dst_w);
	dst_xy = ((pipe->dst_y << 16) | pipe->dst_x);

	format = mdp4_overlay_format(pipe);
	pattern = mdp4_overlay_unpack_pattern(pipe);

#ifdef MDP4_IGC_LUT_ENABLE
	pipe->op_mode |= (MDP4_OP_CSC_EN | MDP4_OP_SRC_DATA_YCBCR |
				MDP4_OP_IGC_LUT_EN);
#else
	pipe->op_mode |= (MDP4_OP_CSC_EN | MDP4_OP_SRC_DATA_YCBCR);
#endif

	mdp4_scale_setup(pipe);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	outpdw(vg_base + 0x0000, src_size);	/* MDP_RGB_SRC_SIZE */
	outpdw(vg_base + 0x0004, src_xy);	/* MDP_RGB_SRC_XY */
	outpdw(vg_base + 0x0008, dst_size);	/* MDP_RGB_DST_SIZE */
	outpdw(vg_base + 0x000c, dst_xy);	/* MDP_RGB_DST_XY */
	outpdw(vg_base + 0x0048, frame_size);	/* TILE frame size */

	/* luma component plane */
	outpdw(vg_base + 0x0010, pipe->srcp0_addr);

	/* chroma component plane */
	outpdw(vg_base + 0x0014, pipe->srcp1_addr);

	outpdw(vg_base + 0x0040,
			pipe->srcp1_ystride << 16 | pipe->srcp0_ystride);

	outpdw(vg_base + 0x0050, format);	/* MDP_RGB_SRC_FORMAT */
	outpdw(vg_base + 0x0054, pattern);	/* MDP_RGB_SRC_UNPACK_PATTERN */
	outpdw(vg_base + 0x0058, pipe->op_mode);/* MDP_RGB_OP_MODE */
	outpdw(vg_base + 0x005c, pipe->phasex_step);
	outpdw(vg_base + 0x0060, pipe->phasey_step);

	if (pipe->op_mode & MDP4_OP_DITHER_EN) {
		outpdw(vg_base + 0x0068,
			pipe->r_bit << 4 | pipe->b_bit << 2 | pipe->g_bit);
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

int mdp4_overlay_format2type(uint32 format)
{
	switch (format) {
	case MDP_RGB_565:
	case MDP_RGB_888:
	case MDP_BGR_565:
	case MDP_XRGB_8888:
	case MDP_ARGB_8888:
	case MDP_RGBA_8888:
	case MDP_BGRA_8888:
	case MDP_RGBX_8888:
		return OVERLAY_TYPE_RGB;
	case MDP_YCRYCB_H2V1:
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CBCR_H2V1:
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CBCR_H2V2_TILE:
	case MDP_Y_CRCB_H2V2_TILE:
		return OVERLAY_TYPE_VG;
	default:
		return -ERANGE;
	}

}

#define C3_ALPHA	3	/* alpha */
#define C2_R_Cr		2	/* R/Cr */
#define C1_B_Cb		1	/* B/Cb */
#define C0_G_Y		0	/* G/luma */

int mdp4_overlay_format2pipe(struct mdp4_overlay_pipe *pipe)
{
	switch (pipe->src_format) {
	case MDP_RGB_565:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 1;	/* R, 5 bits */
		pipe->b_bit = 1;	/* B, 5 bits */
		pipe->g_bit = 2;	/* G, 6 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_RGB_888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C1_B_Cb;	
		pipe->element1 = C0_G_Y;	
		pipe->element0 = C2_R_Cr;	
		pipe->bpp = 3;	
		break;
	case MDP_BGR_565:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;
		pipe->r_bit = 1;	/* R, 5 bits */
		pipe->b_bit = 1;	/* B, 5 bits */
		pipe->g_bit = 2;	/* G, 6 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 2;
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_XRGB_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C1_B_Cb;	
		pipe->element2 = C0_G_Y;	
		pipe->element1 = C2_R_Cr;	
		pipe->element0 = C3_ALPHA;	
		pipe->bpp = 4;		
		break;
	case MDP_ARGB_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C1_B_Cb;	
		pipe->element2 = C0_G_Y;	
		pipe->element1 = C2_R_Cr;	
		pipe->element0 = C3_ALPHA;	
		pipe->bpp = 4;		
		break;
	case MDP_RGBA_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_RGBX_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C1_B_Cb;	/* B */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C2_R_Cr;	/* R */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_BGRA_8888:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 3;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 1;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C3_ALPHA;	/* alpha */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 4;		/* 4 bpp */
		break;
	case MDP_YCRYCB_H2V1:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_INTERLEAVED;
		pipe->a_bit = 0;	/* alpha, 4 bits */
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 3;
		pipe->element3 = C0_G_Y;	/* G */
		pipe->element2 = C2_R_Cr;	/* R */
		pipe->element1 = C0_G_Y;	/* G */
		pipe->element0 = C1_B_Cb;	/* B */
		pipe->bpp = 2;		/* 2 bpp */
		pipe->chroma_sample = MDP4_CHROMA_H2V1;
		break;
	case MDP_Y_CRCB_H2V1:
	case MDP_Y_CBCR_H2V1:
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CBCR_H2V2:
		pipe->frame_format = MDP4_FRAME_FORMAT_LINEAR;
		pipe->fetch_plane = OVERLAY_PLANE_PSEUDO_PLANAR;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 1;		/* 2 */
		pipe->element3 = C0_G_Y;	/* not used */
		pipe->element2 = C0_G_Y;	/* not used */
		if (pipe->src_format == MDP_Y_CRCB_H2V1) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_H2V1;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V1) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_H2V1;
		} else if (pipe->src_format == MDP_Y_CRCB_H2V2) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_420;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V2) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_420;
		}
		pipe->bpp = 2;	/* 2 bpp */
		break;
	case MDP_Y_CBCR_H2V2_TILE:
	case MDP_Y_CRCB_H2V2_TILE:
		pipe->frame_format = MDP4_FRAME_FORMAT_VIDEO_SUPERTILE;
		pipe->fetch_plane = OVERLAY_PLANE_PSEUDO_PLANAR;
		pipe->a_bit = 0;
		pipe->r_bit = 3;	/* R, 8 bits */
		pipe->b_bit = 3;	/* B, 8 bits */
		pipe->g_bit = 3;	/* G, 8 bits */
		pipe->alpha_enable = 0;
		pipe->unpack_tight = 1;
		pipe->unpack_align_msb = 0;
		pipe->unpack_count = 1;		/* 2 */
		pipe->element3 = C0_G_Y;	/* not used */
		pipe->element2 = C0_G_Y;	/* not used */
		if (pipe->src_format == MDP_Y_CRCB_H2V2_TILE) {
			pipe->element1 = C2_R_Cr;	/* R */
			pipe->element0 = C1_B_Cb;	/* B */
			pipe->chroma_sample = MDP4_CHROMA_420;
		} else if (pipe->src_format == MDP_Y_CBCR_H2V2_TILE) {
			pipe->element1 = C1_B_Cb;	/* B */
			pipe->element0 = C2_R_Cr;	/* R */
			pipe->chroma_sample = MDP4_CHROMA_420;
		}
		pipe->bpp = 2;	/* 2 bpp */
		break;
	default:
		/* not likely */
		return -ERANGE;
	}

	return 0;
}

/*
 * color_key_convert: output with 12 bits color key
 */
static uint32 color_key_convert(int start, int num, uint32 color)
{
	uint32 data;

	data = (color >> start) & ((1 << num) - 1);

	/* convert to 8 bits */
	if (num == 5)
		data = ((data << 3) | (data >> 2));
	else if (num == 6)
		data = ((data << 2) | (data >> 4));

	/* convert 8 bits to 12 bits */
	data = (data << 4) | (data >> 4);

	return data;
}

void transp_color_key(int format, uint32 transp,
			uint32 *c0, uint32 *c1, uint32 *c2)
{
	int b_start, g_start, r_start;
	int b_num, g_num, r_num;

	switch (format) {
	case MDP_RGB_565:
		b_start = 0;
		g_start = 5;
		r_start = 11;
		r_num = 5;
		g_num = 6;
		b_num = 5;
		break;
	case MDP_RGB_888:
	case MDP_XRGB_8888:
	case MDP_ARGB_8888:
	case MDP_BGRA_8888:
		b_start = 0;
		g_start = 8;
		r_start = 16;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_RGBA_8888:
	case MDP_RGBX_8888:
		b_start = 16;
		g_start = 8;
		r_start = 0;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_BGR_565:
		b_start = 11;
		g_start = 5;
		r_start = 0;
		r_num = 5;
		g_num = 6;
		b_num = 5;
		break;
	case MDP_Y_CBCR_H2V2:
	case MDP_Y_CBCR_H2V1:
		b_start = 8;
		g_start = 16;
		r_start = 0;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	case MDP_Y_CRCB_H2V2:
	case MDP_Y_CRCB_H2V1:
		b_start = 0;
		g_start = 16;
		r_start = 8;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	default:
		b_start = 0;
		g_start = 8;
		r_start = 16;
		r_num = 8;
		g_num = 8;
		b_num = 8;
		break;
	}

	*c0 = color_key_convert(g_start, g_num, transp);
	*c1 = color_key_convert(b_start, b_num, transp);
	*c2 = color_key_convert(r_start, r_num, transp);
}

uint32 mdp4_overlay_format(struct mdp4_overlay_pipe *pipe)
{
	uint32	format;

	format = 0;

	if (pipe->solid_fill)
		format |= MDP4_FORMAT_SOLID_FILL;

	if (pipe->unpack_align_msb)
		format |= MDP4_FORMAT_UNPACK_ALIGN_MSB;

	if (pipe->unpack_tight)
		format |= MDP4_FORMAT_UNPACK_TIGHT;

	if (pipe->alpha_enable)
		format |= MDP4_FORMAT_ALPHA_ENABLE;

	format |= (pipe->unpack_count << 13);
	format |= ((pipe->bpp - 1) << 9);
	format |= (pipe->a_bit << 6);
	format |= (pipe->r_bit << 4);
	format |= (pipe->b_bit << 2);
	format |= pipe->g_bit;

	format |= (pipe->frame_format << 29);

	if (pipe->fetch_plane == OVERLAY_PLANE_PSEUDO_PLANAR) {
		/* video/graphic */
		format |= (pipe->fetch_plane << 19);
		format |= (pipe->chroma_site << 28);
		format |= (pipe->chroma_sample << 26);
	}

	return format;
}

uint32 mdp4_overlay_unpack_pattern(struct mdp4_overlay_pipe *pipe)
{
	return (pipe->element3 << 24) | (pipe->element2 << 16) |
			(pipe->element1 << 8) | pipe->element0;
}

void mdp4_overlayproc_cfg(struct mdp4_overlay_pipe *pipe)
{
	uint32 data;
	char *overlay_base;

	if (pipe->mixer_num == MDP4_MIXER1)
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC1_BASE;/* 0x18000 */
	else
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* MDP_OVERLAYPROC_CFG */
	outpdw(overlay_base + 0x0004, 0x01); /* directout */
	data = pipe->src_height;
	data <<= 16;
	data |= pipe->src_width;
	outpdw(overlay_base + 0x0008, data); /* ROI, height + width */
	outpdw(overlay_base + 0x000c, pipe->srcp0_addr);
	outpdw(overlay_base + 0x0010, pipe->srcp0_ystride);

#ifdef MDP4_IGC_LUT_ENABLE
	outpdw(overlay_base + 0x0014, 0x4);	/* GC_LUT_EN, 888 */
#endif
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

int mdp4_overlay_pipe_staged(int mixer)
{
	uint32 data, mask, i;
	int p1, p2;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	data = inpdw(MDP_BASE + 0x10100);
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
	p1 = 0;
	p2 = 0;
	for (i = 0; i < 8; i++) {
		mask = data & 0x0f;
		if (mask) {
			if (mask <= 4)
				p1++;
			else
				p2++;
		}
		data >>= 4;
	}

	if (mixer)
		return p2;
	else
		return p1;
}

int mdp4_mixer_info(int mixer_num, struct mdp_mixer_info *info)
{

	int ndx, cnt;
	struct mdp4_overlay_pipe *pipe;

	if (mixer_num > MDP4_MIXER_MAX)
		return -ENODEV;

	cnt = 0;
	ndx = 1; /* ndx 0 if not used */

	for ( ; ndx < MDP4_MIXER_STAGE_MAX; ndx++) {
		pipe = ctrl->stage[mixer_num][ndx];
		if (pipe == NULL)
			continue;
		info->z_order = pipe->mixer_stage - MDP4_MIXER_STAGE0;
		info->ptype = pipe->pipe_type;
		info->pnum = pipe->pipe_num;
		info->pndx = pipe->pipe_ndx;
		info->mixer_num = pipe->mixer_num;
		info++;
		cnt++;
	}
	return cnt;
}

void mdp4_mixer_stage_up(struct mdp4_overlay_pipe *pipe)
{
	uint32 data, mask, snum, stage, mixer;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	stage = pipe->mixer_stage;
	mixer = pipe->mixer_num;

	/* MDP_LAYERMIXER_IN_CFG, shard by both mixer 0 and 1  */
	data = inpdw(MDP_BASE + 0x10100);

	if (mixer == MDP4_MIXER1)
		stage += 8;

	if (pipe->pipe_type == OVERLAY_TYPE_VG) {/* VG1 and VG2 */
		snum = 0;
		snum += (4 * pipe->pipe_num);
	} else {
		snum = 8;
		snum += (4 * pipe->pipe_num);	/* RGB1 and RGB2 */
	}

	mask = 0x0f;
	mask <<= snum;
	stage <<= snum;
	data &= ~mask;	/* clear old bits */

	data |= stage;

	outpdw(MDP_BASE + 0x10100, data); /* MDP_LAYERMIXER_IN_CFG */

	data = inpdw(MDP_BASE + 0x10100);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	ctrl->stage[pipe->mixer_num][pipe->mixer_stage] = pipe;	/* keep it */
}

void mdp4_mixer_stage_down(struct mdp4_overlay_pipe *pipe)
{
	uint32 data, mask, snum, stage, mixer;

	stage = pipe->mixer_stage;
	mixer = pipe->mixer_num;

	if (pipe != ctrl->stage[mixer][stage])	/* not runing */
		return;

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	/* MDP_LAYERMIXER_IN_CFG, shard by both mixer 0 and 1  */
	data = inpdw(MDP_BASE + 0x10100);

	if (mixer == MDP4_MIXER1)
		stage += 8;

	if (pipe->pipe_type == OVERLAY_TYPE_VG) {/* VG1 and VG2 */
		snum = 0;
		snum += (4 * pipe->pipe_num);
	} else {
		snum = 8;
		snum += (4 * pipe->pipe_num);	/* RGB1 and RGB2 */
	}

	mask = 0x0f;
	mask <<= snum;
	data &= ~mask;	/* clear old bits */

	outpdw(MDP_BASE + 0x10100, data); /* MDP_LAYERMIXER_IN_CFG */

	data = inpdw(MDP_BASE + 0x10100);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	ctrl->stage[pipe->mixer_num][pipe->mixer_stage] = NULL;	/* clear it */
}

void mdp4_mixer_blend_setup(struct mdp4_overlay_pipe *pipe)
{
	struct mdp4_overlay_pipe *bg_pipe;
	unsigned char *overlay_base;
	uint32 c0, c1, c2, blend_op;
	int off;

	if (pipe->mixer_num) 	/* mixer number, /dev/fb0, /dev/fb1 */
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC1_BASE;/* 0x18000 */
	else
		overlay_base = MDP_BASE + MDP4_OVERLAYPROC0_BASE;/* 0x10000 */

	/* stage 0 to stage 2 */
	off = 0x20 * (pipe->mixer_stage - MDP4_MIXER_STAGE0);

	bg_pipe = mdp4_overlay_stage_pipe(pipe->mixer_num,
					MDP4_MIXER_STAGE_BASE);
	if (bg_pipe == NULL) {
		printk(KERN_INFO "%s: Error: no bg_pipe\n", __func__);
		return;
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);

	blend_op = 0;

	if (bg_pipe->alpha_enable && pipe->alpha_enable) {
		/* both pipe are ARGB */
		blend_op |= (MDP4_BLEND_BG_ALPHA_BG_PIXEL |
				MDP4_BLEND_FG_ALPHA_FG_PIXEL);
	} else if (bg_pipe->alpha_enable && pipe->alpha_enable == 0) {
		blend_op = (MDP4_BLEND_BG_ALPHA_BG_PIXEL |
				MDP4_BLEND_FG_ALPHA_BG_PIXEL |
				MDP4_BLEND_FG_INV_ALPHA);
	} else if (bg_pipe->alpha_enable == 0 && pipe->alpha_enable) {
		blend_op = (MDP4_BLEND_FG_ALPHA_FG_PIXEL |
				MDP4_BLEND_BG_ALPHA_FG_PIXEL |
				MDP4_BLEND_BG_INV_ALPHA);
	} else if (bg_pipe->alpha_enable == 0 && pipe->alpha_enable == 0) {
		/* not ARGB on both pipe */
		blend_op |= (MDP4_BLEND_FG_ALPHA_FG_CONST |
					MDP4_BLEND_BG_ALPHA_BG_CONST);
		if (pipe->is_fg) {
			outpdw(overlay_base + off + 0x108, pipe->alpha);
			outpdw(overlay_base + off + 0x10c, 0xff - pipe->alpha);
		} else {
			outpdw(overlay_base + off + 0x108, 0xff - pipe->alpha);
			outpdw(overlay_base + off + 0x10c, pipe->alpha);
		}

		if (pipe->transp != MDP_TRANSP_NOP) {
			if (pipe->is_fg) {
				transp_color_key(pipe->src_format, pipe->transp,
						&c0, &c1, &c2);
				/* Fg blocked */
				blend_op |= MDP4_BLEND_FG_TRANSP_EN;
				/* lower limit */
				outpdw(overlay_base + off + 0x110,
						(c1 << 16 | c0));/* low */
				outpdw(overlay_base + off + 0x114, c2);/* low */
				/* upper limit */
				outpdw(overlay_base + off + 0x118,
						(c1 << 16 | c0));
				outpdw(overlay_base + off + 0x11c, c2);
			} else {
				transp_color_key(bg_pipe->src_format,
					pipe->transp, &c0, &c1, &c2);
				/* bg blocked */
				blend_op |= MDP4_BLEND_BG_TRANSP_EN;
				/* lower limit */
				outpdw(overlay_base + 0x180,
						(c1 << 16 | c0));/* low */
				outpdw(overlay_base + 0x184, c2);/* low */
				/* upper limit */
				outpdw(overlay_base + 0x188,
						(c1 << 16 | c0));/* high */
				outpdw(overlay_base + 0x18c, c2);/* high */
			}
		}
	}
	outpdw(overlay_base + off + 0x104, blend_op);

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

void mdp4_overlay_reg_flush(struct mdp4_overlay_pipe *pipe, int all)
{
	uint32 bits = 0;

	if (pipe->mixer_num == MDP4_MIXER1)
		bits |= 0x02;
	else
		bits |= 0x01;

	if (all) {
		if (pipe->pipe_type == OVERLAY_TYPE_RGB) {
			if (pipe->pipe_num == OVERLAY_PIPE_RGB2)
				bits |= 0x20;
			else
				bits |= 0x10;
		} else {
			if (pipe->pipe_num == OVERLAY_PIPE_VG2)
				bits |= 0x08;
			else
				bits |= 0x04;
		}
	}

	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	outpdw(MDP_BASE + 0x18000, bits);	/* MDP_OVERLAY_REG_FLUSH */
	mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
}

struct mdp4_overlay_pipe *mdp4_overlay_stage_pipe(int mixer, int stage)
{
	return ctrl->stage[mixer][stage];
}

struct mdp4_overlay_pipe *mdp4_overlay_ndx2pipe(int ndx)
{
	struct mdp4_overlay_pipe *pipe;

	if (ndx <= 0 || ndx > MDP4_MAX_OVERLAY_PIPE)
		return NULL;

	pipe = &ctrl->plist[ndx - 1];	/* ndx start from 1 */

	if (pipe->pipe_used == 0)
		return NULL;

	return pipe;
}

struct mdp4_overlay_pipe *mdp4_overlay_pipe_alloc(int ptype)
{
	int i;
	struct mdp4_overlay_pipe *pipe;

	pipe = &ctrl->plist[0];
	for (i = 0; i < MDP4_MAX_OVERLAY_PIPE; i++) {
		if (pipe->pipe_type == ptype && pipe->pipe_used == 0) {
			pipe->pipe_used++;
			init_completion(&pipe->comp);
	printk(KERN_INFO "mdp4_overlay_pipe_alloc: pipe=%x ndx=%d\n",
					(int)pipe, pipe->pipe_ndx);
			return pipe;
		}
		pipe++;
	}

	printk(KERN_INFO "mdp4_overlay_pipe_alloc: ptype=%d FAILED\n",
							ptype);

	return NULL;
}


void mdp4_overlay_pipe_free(struct mdp4_overlay_pipe *pipe)
{
	uint32 ptype, num, ndx;

	printk(KERN_INFO "mdp4_overlay_pipe_free: pipe=%x ndx=%d\n",
					(int)pipe, pipe->pipe_ndx);
	ptype = pipe->pipe_type;
	num = pipe->pipe_num;
	ndx = pipe->pipe_ndx;

	memset(pipe, 0, sizeof(*pipe));

	pipe->pipe_type = ptype;
	pipe->pipe_num = num;
	pipe->pipe_ndx = ndx;
}

int mdp4_overlay_req_check(uint32 id, uint32 z_order, uint32 mixer)
{
	struct mdp4_overlay_pipe *pipe;

	pipe = ctrl->stage[mixer][z_order];

	if (pipe == NULL)
		return 0;

	if (pipe->pipe_ndx == id)	/* same req, recycle */
		return 0;

	return -EPERM;
}

static int mdp4_overlay_req2pipe(struct mdp_overlay *req, int mixer,
			struct mdp4_overlay_pipe **ppipe)
{
	struct mdp4_overlay_pipe *pipe;
	int ret, ptype;

	if (mixer >= MDP4_MAX_MIXER) {
		printk(KERN_ERR "mpd_overlay_req2pipe: mixer out of range!\n");
		return -ERANGE;
	}

	if (req->z_order < 0 || req->z_order > 2) {
		printk(KERN_ERR "mpd_overlay_req2pipe: z_order=%d out of range!\n",
				req->z_order);
		return -ERANGE;
	}

	if (req->src_rect.h == 0 || req->src_rect.w == 0) {
		printk(KERN_ERR "mpd_overlay_req2pipe: src img of zero size!\n");
		return -EINVAL;
	}


	if (req->dst_rect.h > (req->src_rect.h * 8))	/* too much */
		return -ERANGE;

	if (req->src_rect.h > (req->dst_rect.h * 8))	/* too little */
		return -ERANGE;

	if (req->dst_rect.w > (req->src_rect.w * 8))	/* too much */
		return -ERANGE;

	if (req->src_rect.w > (req->dst_rect.w * 8))	/* too little */
		return -ERANGE;

	/*  non integer down saceling ratio  smaller than 1/4
	 *  is not supportted
	 */
	if (req->src_rect.h > (req->dst_rect.h * 4)) {
		if (req->src_rect.h % req->dst_rect.h)	/* need integer */
			return -ERANGE;
	}

	if (req->src_rect.w > (req->dst_rect.w * 4)) {
		if (req->src_rect.w % req->dst_rect.w)	/* need integer */
			return -ERANGE;
	}

	ret = mdp4_overlay_req_check(req->id, req->z_order, mixer);
	if (ret < 0)
		return ret;

	ptype = mdp4_overlay_format2type(req->src.format);
	if (ptype < 0)
		return ptype;

	/* no down scale on rgb pipe */
	if (ptype == OVERLAY_TYPE_RGB) {
		if (req->src_rect.h > req->dst_rect.h)
			return -ERANGE;
		if (req->src_rect.w > req->dst_rect.w)
			return -ERANGE;
	}

	if (req->id == MSMFB_NEW_REQUEST)  /* new request */
		pipe = mdp4_overlay_pipe_alloc(ptype);
	else
		pipe = mdp4_overlay_ndx2pipe(req->id);

	if (pipe == NULL)
		return -ENOMEM;

	pipe->src_format = req->src.format;
	ret = mdp4_overlay_format2pipe(pipe);

	if (ret < 0)
		return ret;

	/*
	 * base layer == 1, reserved for frame buffer
	 * zorder 0 == stage 0 == 2
	 * zorder 1 == stage 1 == 3
	 * zorder 2 == stage 2 == 4
	 */
	if (req->id == MSMFB_NEW_REQUEST) {  /* new request */
		pipe->mixer_num = mixer;
		pipe->mixer_stage = req->z_order + MDP4_MIXER_STAGE0;
		printk(KERN_INFO "mpd4_overlay_req2pipe: zorder=%d pipe_num=%d\n",
				req->z_order, pipe->pipe_num);
	}

	pipe->src_width = req->src.width & 0x07ff;	/* source img width */
	pipe->src_height = req->src.height & 0x07ff;	/* source img height */
	pipe->src_h = req->src_rect.h & 0x07ff;
	pipe->src_w = req->src_rect.w & 0x07ff;
	pipe->src_y = req->src_rect.y & 0x07ff;
	pipe->src_x = req->src_rect.x & 0x07ff;
	pipe->dst_h = req->dst_rect.h & 0x07ff;
	pipe->dst_w = req->dst_rect.w & 0x07ff;
	pipe->dst_y = req->dst_rect.y & 0x07ff;
	pipe->dst_x = req->dst_rect.x & 0x07ff;

	if (req->flags & MDP_FLIP_LR)
		pipe->op_mode |= MDP4_OP_FLIP_LR;

	if (req->flags & MDP_FLIP_UD)
		pipe->op_mode |= MDP4_OP_FLIP_UD;

	if (req->flags & MDP_DITHER)
		pipe->op_mode |= MDP4_OP_DITHER_EN;

	if (req->flags & MDP_DEINTERLACE)
		pipe->op_mode |= MDP4_OP_DEINT_ODD_REF;

	pipe->is_fg = req->is_fg;/* control alpha and color key */

	pipe->alpha = req->alpha & 0x0ff;

	pipe->transp = req->transp_mask;

	*ppipe = pipe;

	return 0;
}

static int get_img(struct msmfb_data *img, struct fb_info *info,
	unsigned long *start, unsigned long *len, struct file **pp_file)
{
	int put_needed, ret = 0;
	struct file *file;
#ifdef CONFIG_ANDROID_PMEM
	unsigned long vstart;
#endif

#ifdef CONFIG_MSM_MULTIMEDIA_USE_ION
	return mdp4_overlay_iommu_map_buf(img->memory_id, pipe, plane,
		start, len, srcp_ihdl);
#endif

#ifdef CONFIG_ANDROID_PMEM
	if (!get_pmem_file(img->memory_id, start, &vstart, len, pp_file))
		return 0;
#endif
	file = fget_light(img->memory_id, &put_needed);
	if (file == NULL)
		return -1;

	if (MAJOR(file->f_dentry->d_inode->i_rdev) == FB_MAJOR) {
		*start = info->fix.smem_start;
		*len = info->fix.smem_len;
		*pp_file = file;
	} else {
		ret = -1;
		fput_light(file, put_needed);
	}
	return ret;
}
int mdp4_overlay_get(struct fb_info *info, struct mdp_overlay *req)
{
	struct mdp4_overlay_pipe *pipe;

	pipe = mdp4_overlay_ndx2pipe(req->id);
	if (pipe == NULL)
		return -ENODEV;

	*req = pipe->req_data;

	return 0;
}

static int mdp4_pull_mode(int mixer)
{
	uint32 lcdc;
	int off;

	if (mixer == MDP4_MIXER1) /* DTV */
		off = 0xd0000;
	else			/* LCDC */
		off = 0xc0000;

	lcdc = inpdw(MDP_BASE + off);
	lcdc &= 0x01;

	return lcdc;
}

int mdp4_overlay_set(struct fb_info *info, struct mdp_overlay *req)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	int ret, mixer;
	struct mdp4_overlay_pipe *pipe;
	int pull;

	if (mfd == NULL)
		return -ENODEV;

	if (req->src.format == MDP_FB_FORMAT)
		req->src.format = mfd->fb_imgType;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	mixer = mfd->panel_info.pdest;	/* DISPLAY_1 or DISPLAY_2 */

	ret = mdp4_overlay_req2pipe(req, mixer, &pipe);
	if (ret < 0) {
		mutex_unlock(&mfd->dma->ov_mutex);
		return ret;
	}

	pull = mdp4_pull_mode(mixer);

	if (pull == 0) { /* mddi */
		/* MDP cmd block enable */
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
	}

	/* return id back to user */
	req->id = pipe->pipe_ndx;	/* pipe_ndx start from 1 */
	pipe->req_data = *req;		/* keep original req */

	mdp4_vg_qseed_init(pipe->mixer_num);

	if (!IS_ERR_OR_NULL(mfd->iclient)) {
		pr_debug("pipe->flags 0x%x\n", pipe->flags);
		if (pipe->flags & MDP_SECURE_OVERLAY_SESSION) {
			mfd->mem_hid &= ~BIT(ION_IOMMU_HEAP_ID);
			mfd->mem_hid |= ION_SECURE;
		} else {
			mfd->mem_hid |= BIT(ION_IOMMU_HEAP_ID);
			mfd->mem_hid &= ~ION_SECURE;
		}
	}

	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

int mdp4_overlay_unset(struct fb_info *info, int ndx)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct mdp4_overlay_pipe *pipe;
	int pull;

	if (mfd == NULL)
		return -ENODEV;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	pipe = mdp4_overlay_ndx2pipe(ndx);

	if (pipe == NULL) {
		mutex_unlock(&mfd->dma->ov_mutex);
		return -ENODEV;
	}

	pull = mdp4_pull_mode(pipe->mixer_num);

	mdp4_mixer_stage_down(pipe);

	if (pull) /* LCDC or DTV mode */
		mdp4_overlay_reg_flush(pipe, 0);
	else  {	/* mddi */
		/* MDP cmd block disable */
		mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);
		mdp4_mddi_overlay_restore();
	}

	mdp4_overlay_pipe_free(pipe);

	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

struct tile_desc {
	uint32 width;  /* tile's width */
	uint32 height; /* tile's height */
	uint32 row_tile_w; /* tiles per row's width */
	uint32 row_tile_h; /* tiles per row's height */
};

void tile_samsung(struct tile_desc *tp)
{
	/*
	 * each row of samsung tile consists of two tiles in height
	 * and two tiles in width which means width should align to
	 * 64 x 2 bytes and height should align to 32 x 2 bytes.
	 * video decoder generate two tiles in width and one tile
	 * in height which ends up height align to 32 X 1 bytes.
	 */
	tp->width = 64;		/* 64 bytes */
	tp->row_tile_w = 2;	/* 2 tiles per row's width */
	tp->height = 32;	/* 32 bytes */
	tp->row_tile_h = 1;	/* 1 tiles per row's height */
}

uint32 tile_mem_size(struct mdp4_overlay_pipe *pipe, struct tile_desc *tp)
{
	uint32 tile_w, tile_h;
	uint32 row_num_w, row_num_h;


	tile_w = tp->width * tp->row_tile_w;
	tile_h = tp->height * tp->row_tile_h;

	row_num_w = (pipe->src_width + tile_w - 1) / tile_w;
	row_num_h = (pipe->src_height + tile_h - 1) / tile_h;

	return row_num_w * row_num_h * tile_w * tile_h;
}

int mdp4_overlay_play(struct fb_info *info, struct msmfb_overlay_data *req,
		struct file **pp_src_file)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct msmfb_data *img;
	struct mdp4_overlay_pipe *pipe;
	ulong start, addr;
	ulong len = 0;
	struct file *p_src_file = 0;
	struct ion_handle *srcp0_ihdl = NULL;
	struct ion_handle *srcp1_ihdl = NULL, *srcp2_ihdl = NULL;
	int pull;

	if (mfd == NULL)
		return -ENODEV;

	pipe = mdp4_overlay_ndx2pipe(req->id);
	if (pipe == NULL)
		return -ENODEV;

	if (mutex_lock_interruptible(&mfd->dma->ov_mutex))
		return -EINTR;

	img = &req->data;
	get_img(img, info, &start, &len, &p_src_file);
	if (len == 0) {
		mutex_unlock(&mfd->dma->ov_mutex);
		printk(KERN_ERR "mdp_overlay_play: could not retrieve"
				       " image from memory\n");
		return -1;
	}
	*pp_src_file = p_src_file;

	addr = start + img->offset;
	pipe->srcp0_addr = addr;
	pipe->srcp0_ystride = pipe->src_width * pipe->bpp;

	if (pipe->fetch_plane == OVERLAY_PLANE_PSEUDO_PLANAR) {
		if (pipe->frame_format == MDP4_FRAME_FORMAT_VIDEO_SUPERTILE) {
			struct tile_desc tile;

			tile_samsung(&tile);
			pipe->srcp1_addr = addr + tile_mem_size(pipe, &tile);
		} else
			pipe->srcp1_addr = addr +
					pipe->src_width * pipe->src_height;

		pipe->srcp0_ystride = pipe->src_width;
		pipe->srcp1_ystride = pipe->src_width;
	}

	if (pipe->pipe_type == OVERLAY_TYPE_VG)
		mdp4_overlay_vg_setup(pipe);	/* video/graphic pipe */
	else
		mdp4_overlay_rgb_setup(pipe);	/* rgb pipe */

	mdp4_mixer_blend_setup(pipe);
	mdp4_mixer_stage_up(pipe);

	pull = mdp4_pull_mode(pipe->mixer_num);

	if (pull) {	/* LCDC or DTV mode */
		mdp4_overlay_reg_flush(pipe, 1);
		if (pipe->mixer_stage != MDP4_MIXER_STAGE_BASE) { /* done */
			mutex_unlock(&mfd->dma->ov_mutex);
			return 0;
		}
	} else { 	/* MDDI mode */
		if (!mfd->dma->busy && mfd->panel_power_on)
			mdp4_mddi_overlay_kickoff(mfd, pipe);
	}

	mutex_unlock(&mfd->dma->ov_mutex);

	return 0;
}

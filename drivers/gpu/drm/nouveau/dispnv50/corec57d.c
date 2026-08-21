/*
 * Copyright 2018 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "core.h"
#include "head.h"

#include <nvif/class.h>
#include <nvif/pushc37b.h>
#include <nvif/if0010.h>

#include <nvhw/class/clc57d.h>
#include <nvhw/class/clc573.h>
#include <nvhw/class/clc37dswspare.h>

int
corec57d_caps_init(struct nouveau_drm *drm, struct nv50_disp *disp)
{
	u32 syscap;
	int i, ret;

	ret = corec37d_caps_init(drm, disp);
	if (ret)
		return ret;

	/* Two taps support up to 2x downscaling, while the five-tap limit is
	 * either 2x or 4x. Read the per-tap line-store widths as well, since
	 * they bound vertical filtering independently of the scale ratio. On
	 * GB20x these offsets belong to tiles, so head i uses tile i's caps.
	 */
	syscap = nvif_rd32(&disp->caps, NVC573_SYS_CAP);
	for (i = 0; i < NVC573_SYS_CAP_HEAD_EXISTS__SIZE_1; i++) {
		struct nv50_scaler_caps *scaler;
		u32 capa, capc, capd;

		if (!NVDEF_TEST(syscap, NVC573, SYS_CAP, HEAD_EXISTS, i, ==, YES))
			continue;

		capa = nvif_rd32(&disp->caps, NVC573_POSTCOMP_HEAD_HDR_CAPA(i));
		if (!NVDEF_TEST(capa, NVC573, POSTCOMP_HEAD_HDR_CAPA,
				SCLR_PRESENT, ==, TRUE))
			continue;

		capc = nvif_rd32(&disp->caps, NVC573_POSTCOMP_HEAD_HDR_CAPC(i));
		capd = nvif_rd32(&disp->caps, NVC573_POSTCOMP_HEAD_HDR_CAPD(i));

		scaler = &disp->scaler[i];
		scaler->taps2.max_h = 0x800;
		scaler->taps2.max_v = 0x800;
		scaler->taps2.max_pixels =
			NVVAL_GET(capd, NVC573, POSTCOMP_HEAD_HDR_CAPD,
				  VSCLR_MAX_PIXELS_2TAP);
		scaler->taps5.max_h =
			NVDEF_TEST(capc, NVC573, POSTCOMP_HEAD_HDR_CAPC,
				   SCLR_HS_MAX_SCALE_FACTOR, ==, 4X) ?
				0x1000 : 0x800;
		scaler->taps5.max_v =
			NVDEF_TEST(capc, NVC573, POSTCOMP_HEAD_HDR_CAPC,
				   SCLR_VS_MAX_SCALE_FACTOR, ==, 4X) ?
				0x1000 : 0x800;
		scaler->taps5.max_pixels =
			NVVAL_GET(capd, NVC573, POSTCOMP_HEAD_HDR_CAPD,
				  VSCLR_MAX_PIXELS_5TAP);

		NV_DEBUG(drm, "disp: head-%d scaler: 5-tap %ux max %u px, 2-tap %u px\n",
			 i, scaler->taps5.max_v >> 10, scaler->taps5.max_pixels,
			 scaler->taps2.max_pixels);
	}

	nouveau_display(drm->dev)->scaler_limits = true;
	nouveau_display(drm->dev)->max_viewport =
		disp->disp->object.oclass >= GB202_DISP ? 16384 : 8192;

	return 0;
}

/* Five-tap coefficients from OpenRM's scalerTaps5Coeff, with 16 phases per
 * ratio (1x, 2x, 4x) and four weights per phase. Hardware derives the center
 * weight so all five taps sum to one. Phases 0 and +/-16 are symmetric, so row
 * 0 packs phase 0's (c0, c1) followed by phase +/-16's (c0, c1). Hardware
 * selects the phase sign.
 */
const s16 corec57d_taps5_coeff[3][16][4] = {
	{ {   0,   0, -16, 144 }, {  0,  -5,   5,   0 }, {  0,  -9,  11,   0 },
	  {  -1, -12,  18,  -1 }, { -1, -15,  25,  -1 }, { -1, -18,  33,  -2 },
	  {  -2, -20,  42,  -3 }, { -2, -21,  51,  -3 }, { -3, -22,  60,  -5 },
	  {  -3, -22,  70,  -6 }, { -4, -22,  81,  -7 }, { -4, -22,  91,  -9 },
	  {  -5, -21, 102, -10 }, { -5, -20, 113, -12 }, { -5, -19, 125, -13 },
	  {  -6, -18, 136, -15 } },
	{ {   3,  60,  20, 108 }, {  3,  57,  63,   4 }, {  2,  54,  66,   4 },
	  {   2,  51,  69,   5 }, {  2,  48,  72,   6 }, {  1,  45,  75,   7 },
	  {   1,  43,  78,   7 }, {  1,  40,  81,   8 }, {  1,  37,  84,   9 },
	  {   0,  35,  88,  10 }, {  0,  33,  91,  12 }, {  0,  30,  94,  13 },
	  {   0,  28,  97,  14 }, {  0,  26,  99,  16 }, {  0,  24, 102,  17 },
	  {   0,  22, 105,  19 } },
	{ {   4,  62,  23, 105 }, {  4,  59,  64,   5 }, {  3,  56,  67,   6 },
	  {   3,  53,  70,   7 }, {  2,  51,  73,   8 }, {  2,  48,  76,   8 },
	  {   2,  45,  79,   9 }, {  1,  43,  81,  10 }, {  1,  40,  84,  12 },
	  {   1,  38,  87,  13 }, {  1,  36,  90,  14 }, {  0,  34,  92,  15 },
	  {   0,  31,  95,  17 }, {  0,  29,  97,  18 }, {  0,  27, 100,  20 },
	  {   0,  25, 102,  22 } },
};

int
corec57d_mclk_war(struct nv50_core *core, int head, bool disable)
{
	struct nvif_push *push = &core->chan.push;
	int ret;

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	/* A mid-frame memory clock switch can corrupt scanout without a
	 * primary window on Turing, so disable the mid-frame/DWCF watermarks.
	 * This write leaves VPLL_REF at NO_PREF because nouveau never selects
	 * the QSYNC reference.
	 */
	PUSH_MTHD(push, NVC57D, HEAD_SET_SW_SPARE_A(head),
		  disable ?
		  NVDEF(NVC37D, HEAD_SET_SW_SPARE_A, DISABLE_MID_FRAME_AND_DWCF_WATERMARK, TRUE) :
		  NVDEF(NVC37D, HEAD_SET_SW_SPARE_A, DISABLE_MID_FRAME_AND_DWCF_WATERMARK, FALSE));
	return 0;
}

int
corec57d_wndw_usage_bounds(struct nv50_core *core, int wndw, u8 formats,
			   u16 fetch)
{
	struct nvif_push *push = &core->chan.push;
	int ret;

	ret = PUSH_WAIT(push, 6);
	if (ret)
		return ret;

	/* Keep LUTs allowed and input scaling at 1:1 as in OpenRM's default
	 * window bounds, while format and fetch bounds follow plane usage.
	 */
	PUSH_MTHD(push, NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(wndw),
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED1BPP,
			!!(formats & NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP)) |
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED2BPP,
			!!(formats & NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP)) |
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED4BPP,
			!!(formats & NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP)) |
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED8BPP,
			!!(formats & NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP)) |
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, YUV_PACKED422,
			!!(formats & NVIF_DISP_IMP_FORMAT_YUV_PACKED_422)));

	PUSH_MTHD(push, NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS(wndw),
		  NVVAL(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS,
			MAX_PIXELS_FETCHED_PER_LINE, fetch) |
		  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, ILUT_ALLOWED, TRUE) |
		  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, TMO_LUT_ALLOWED, TRUE) |
		  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
		  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE));

	PUSH_MTHD(push, NVC57D, WINDOW_SET_MAX_INPUT_SCALE_FACTOR(wndw),
		  NVVAL(NVC57D, WINDOW_SET_MAX_INPUT_SCALE_FACTOR, HORIZONTAL, 0x400) |
		  NVVAL(NVC57D, WINDOW_SET_MAX_INPUT_SCALE_FACTOR, VERTICAL, 0x400));
	return 0;
}

static int
corec57d_init(struct nv50_core *core)
{
	struct nvif_push *push = &core->chan.push;
	unsigned long head_mask = core->disp->disp->head_mask;
	const u32 windows = 8; /*XXX*/
	int ret, i;

	if ((ret = PUSH_WAIT(push, 2 + windows * 5)))
		return ret;

	PUSH_MTHD(push, NVC57D, SET_CONTEXT_DMA_NOTIFIER, core->chan.sync.handle);

	for (i = 0; i < windows; i++) {
		PUSH_MTHD(push, NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i),
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED1BPP, TRUE) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED2BPP, TRUE) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED4BPP, TRUE) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED8BPP, TRUE),

					WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(i), 0x00000000);

		PUSH_MTHD(push, NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS(i),
			  NVVAL(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, MAX_PIXELS_FETCHED_PER_LINE, 0x7fff) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, ILUT_ALLOWED, TRUE) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, TMO_LUT_ALLOWED, TRUE) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
			  NVDEF(NVC57D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE));
	}

	/* Initialize each head's coefficients before a modeset can select five taps. */
	for_each_set_bit(i, &head_mask, 8) {
		int idx;

		ret = PUSH_WAIT(push, 3 * 16 * 4 * 2);
		if (ret)
			return ret;

		/* The index encodes ratio << 6 | phase << 2 | coefficient. */
		for (idx = 0; idx < 3 * 16 * 4; idx++) {
			const s16 coeff =
				corec57d_taps5_coeff[idx >> 6][(idx >> 2) & 15][idx & 3];

			PUSH_MTHD(push, NVC57D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE(i),
				  NVVAL(NVC57D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE, DATA, coeff) |
				  NVVAL(NVC57D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE, INDEX, idx));
		}
	}

	core->assign_windows = true;
	return PUSH_KICK(push);
}

static const struct nv50_core_func
corec57d = {
	.init = corec57d_init,
	.ntfy_init = corec37d_ntfy_init,
	.caps_init = corec57d_caps_init,
	.caps_class = GV100_DISP_CAPS,
	.ntfy_wait_done = corec37d_ntfy_wait_done,
	.update = corec37d_update,
	.wndw.owner = corec37d_wndw_owner,
	.wndw.usage_bounds = corec57d_wndw_usage_bounds,
	.head = &headc57d,
	.sor = &sorc37d,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.crc = &crcc57d,
#endif
};

/* Turing shares the C57D implementation with later GPUs, but only its table
 * gets the mclk workaround like in OpenRM's C5 HAL.
 */
static const struct nv50_core_func
coretu102 = {
	.init = corec57d_init,
	.ntfy_init = corec37d_ntfy_init,
	.caps_init = corec57d_caps_init,
	.caps_class = GV100_DISP_CAPS,
	.ntfy_wait_done = corec37d_ntfy_wait_done,
	.update = corec37d_update,
	.wndw.owner = corec37d_wndw_owner,
	.wndw.usage_bounds = corec57d_wndw_usage_bounds,
	.mclk_war = corec57d_mclk_war,
	.head = &headc57d,
	.sor = &sorc37d,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.crc = &crcc57d,
#endif
};

int
coretu102_new(struct nouveau_drm *drm, s32 oclass, struct nv50_core **pcore)
{
	return core507d_new_(&coretu102, drm, oclass, pcore);
}

int
corec57d_new(struct nouveau_drm *drm, s32 oclass, struct nv50_core **pcore)
{
	return core507d_new_(&corec57d, drm, oclass, pcore);
}

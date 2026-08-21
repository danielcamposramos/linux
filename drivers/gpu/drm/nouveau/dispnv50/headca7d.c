/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include "head.h"
#include "atom.h"
#include "core.h"

#include <nvif/pushc97b.h>

#include <nvhw/class/clca7d.h>

static int
headca7d_display_id(struct nv50_head *head, u32 display_id)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_DISPLAY_ID(i, 0), display_id);

	return 0;
}

static int
headca7d_or(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	u8 depth;
	int ret;

	switch (asyh->or.depth) {
	case 6:
		depth = NVCA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_30_444;
		break;
	case 5:
		depth = NVCA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_24_444;
		break;
	case 2:
		depth = NVCA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_18_444;
		break;
	case 0:
		depth = NVCA7D_HEAD_SET_CONTROL_OUTPUT_RESOURCE_PIXEL_DEPTH_BPP_24_444;
		break;
	default:
		WARN_ON(1);
		return -EINVAL;
	}

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE(i),
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, CRC_MODE, asyh->or.crc_raster) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, HSYNC_POLARITY, asyh->or.nhsync) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, VSYNC_POLARITY, asyh->or.nvsync) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, PIXEL_DEPTH, depth) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, COLOR_SPACE_OVERRIDE, DISABLE) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_RESOURCE, EXT_PACKET_WIN, NONE));

	return 0;
}

static int
headca7d_procamp(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_PROCAMP(i),
		  NVDEF(NVCA7D, HEAD_SET_PROCAMP, COLOR_SPACE, RGB) |
		  NVDEF(NVCA7D, HEAD_SET_PROCAMP, CHROMA_LPF, DISABLE) |
		  NVDEF(NVCA7D, HEAD_SET_PROCAMP, DYNAMIC_RANGE, VESA));

	return 0;
}

static int
headca7d_dither(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_DITHER_CONTROL(i),
		  NVVAL(NVCA7D, HEAD_SET_DITHER_CONTROL, ENABLE, asyh->dither.enable) |
		  NVVAL(NVCA7D, HEAD_SET_DITHER_CONTROL, BITS, asyh->dither.bits) |
		  NVDEF(NVCA7D, HEAD_SET_DITHER_CONTROL, OFFSET_ENABLE, DISABLE) |
		  NVVAL(NVCA7D, HEAD_SET_DITHER_CONTROL, MODE, asyh->dither.mode) |
		  NVVAL(NVCA7D, HEAD_SET_DITHER_CONTROL, PHASE, 0));

	return 0;
}

static int
headca7d_curs_clr(struct nv50_head *head)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 4);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_CONTROL_CURSOR(i),
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR, ENABLE, DISABLE) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR, FORMAT, A8R8G8B8));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR(i, 0),
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR, ENABLE, DISABLE));

	return 0;
}

static int
headca7d_curs_set(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const u32 curs_hi = upper_32_bits(asyh->curs.offset);
	const u32 curs_lo = lower_32_bits(asyh->curs.offset);
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 7);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_SURFACE_ADDRESS_HI_CURSOR(i, 0), curs_hi);

	PUSH_MTHD(push, NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR(i, 0),
		  NVVAL(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR, ADDRESS_LO, curs_lo >> 4) |
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR, TARGET, PHYSICAL_NVM) |
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_CURSOR, ENABLE, ENABLE));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_CONTROL_CURSOR(i),
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR, ENABLE, ENABLE) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_CURSOR, FORMAT, asyh->curs.format) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_CURSOR, SIZE, asyh->curs.layout) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_CURSOR, HOT_SPOT_X, 0) |
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_CURSOR, HOT_SPOT_Y, 0),

				HEAD_SET_CONTROL_CURSOR_COMPOSITION(i),
		  NVVAL(NVCA7D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, K1, 0xff) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, CURSOR_COLOR_FACTOR_SELECT,
								     K1) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, VIEWPORT_COLOR_FACTOR_SELECT,
								     NEG_K1_TIMES_SRC) |
		  NVDEF(NVCA7D, HEAD_SET_CONTROL_CURSOR_COMPOSITION, MODE, BLEND));

	return 0;
}

static int
headca7d_olut_clr(struct nv50_head *head)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_OLUT(i),
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_OLUT, ENABLE, DISABLE));

	return 0;
}

static int
headca7d_olut_set(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const u32 olut_hi = upper_32_bits(asyh->olut.offset);
	const u32 olut_lo = lower_32_bits(asyh->olut.offset);
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 6);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_SURFACE_ADDRESS_HI_OLUT(i), olut_hi,

				HEAD_SET_SURFACE_ADDRESS_LO_OLUT(i),
		  NVVAL(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_OLUT, ADDRESS_LO, olut_lo >> 4) |
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_OLUT, TARGET, PHYSICAL_NVM) |
		  NVDEF(NVCA7D, HEAD_SET_SURFACE_ADDRESS_LO_OLUT, ENABLE, ENABLE));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_OLUT_CONTROL(i),
		  NVVAL(NVCA7D, HEAD_SET_OLUT_CONTROL, INTERPOLATE, asyh->olut.output_mode) |
		  NVDEF(NVCA7D, HEAD_SET_OLUT_CONTROL, MIRROR, DISABLE) |
		  NVVAL(NVCA7D, HEAD_SET_OLUT_CONTROL, MODE, asyh->olut.mode) |
		  NVVAL(NVCA7D, HEAD_SET_OLUT_CONTROL, SIZE, asyh->olut.size),

				HEAD_SET_OLUT_FP_NORM_SCALE(i), 0xffffffff);

	return 0;
}

static int
headca7d_mode(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	struct nv50_head_mode *m = &asyh->mode;
	const u64 hz = (u64)m->clock * 1000;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 14);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_RASTER_SIZE(i),
		  NVVAL(NVCA7D, HEAD_SET_RASTER_SIZE, WIDTH, m->h.active) |
		  NVVAL(NVCA7D, HEAD_SET_RASTER_SIZE, HEIGHT, m->v.active),

				HEAD_SET_RASTER_SYNC_END(i),
		  NVVAL(NVCA7D, HEAD_SET_RASTER_SYNC_END, X, m->h.synce) |
		  NVVAL(NVCA7D, HEAD_SET_RASTER_SYNC_END, Y, m->v.synce),

				HEAD_SET_RASTER_BLANK_END(i),
		  NVVAL(NVCA7D, HEAD_SET_RASTER_BLANK_END, X, m->h.blanke) |
		  NVVAL(NVCA7D, HEAD_SET_RASTER_BLANK_END, Y, m->v.blanke),

				HEAD_SET_RASTER_BLANK_START(i),
		  NVVAL(NVCA7D, HEAD_SET_RASTER_BLANK_START, X, m->h.blanks) |
		  NVVAL(NVCA7D, HEAD_SET_RASTER_BLANK_START, Y, m->v.blanks));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_CONTROL(i),
		  NVDEF(NVCA7D, HEAD_SET_CONTROL, STRUCTURE, PROGRESSIVE));

	/* The FREQUENCY methods carry only 31 HERTZ bits; the upper bits
	 * of anything past 2.147GHz live in the HI methods
	 * (EvoSetRasterParams9()). Truncating would scan out at pclk modulo 2^31.
	 */
	PUSH_MTHD(push, NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY(i),
		  NVVAL(NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY, HERTZ,
			(u32)(hz & 0x7fffffff)));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(i),
		  NVVAL(NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX, HERTZ,
			(u32)(hz & 0x7fffffff)));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI(i),
		  NVVAL(NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI, HERTZ,
			(u32)(hz >> 31)),

				HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI_MAX(i),
		  NVVAL(NVCA7D, HEAD_SET_PIXEL_CLOCK_FREQUENCY_HI_MAX, HERTZ,
			(u32)(hz >> 31)));

	return 0;
}

static int
headca7d_view(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	const bool scale = asyh->view.iW != asyh->view.oW ||
			   asyh->view.iH != asyh->view.oH;
	int ret;

	ret = PUSH_WAIT(push, 18);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_VIEWPORT_SIZE_IN(i),
		  NVVAL(NVCA7D, HEAD_SET_VIEWPORT_SIZE_IN, WIDTH, asyh->view.iW) |
		  NVVAL(NVCA7D, HEAD_SET_VIEWPORT_SIZE_IN, HEIGHT, asyh->view.iH));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_VIEWPORT_SIZE_OUT(i),
		  NVVAL(NVCA7D, HEAD_SET_VIEWPORT_SIZE_OUT, WIDTH, asyh->view.oW) |
		  NVVAL(NVCA7D, HEAD_SET_VIEWPORT_SIZE_OUT, HEIGHT, asyh->view.oH));
	/* Firmware may leave nonzero viewport offsets. Clear them to match
	 * the centered viewport used by validation.
	 */
	PUSH_MTHD(push, NVCA7D, HEAD_SET_VIEWPORT_POINT_IN(i), 0x00000000);
	PUSH_MTHD(push, NVCA7D, HEAD_SET_VIEWPORT_POINT_OUT_ADJUST(i), 0x00000000);

	/* Program the taps and bounds with the viewport so hardware uses
	 * the configuration that passed validation.
	 */
	PUSH_MTHD(push, NVCA7D, HEAD_SET_CONTROL_OUTPUT_SCALER(i),
		  (asyh->view.vtaps == 5 ?
		   NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_SCALER, VERTICAL_TAPS, TAPS_5) :
		   NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_SCALER, VERTICAL_TAPS, TAPS_2)) |
		  (asyh->view.htaps == 5 ?
		   NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_SCALER, HORIZONTAL_TAPS, TAPS_5) :
		   NVDEF(NVCA7D, HEAD_SET_CONTROL_OUTPUT_SCALER, HORIZONTAL_TAPS, TAPS_2)));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR(i),
		  NVVAL(NVCA7D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, HORIZONTAL, asyh->view.max_h) |
		  NVVAL(NVCA7D, HEAD_SET_MAX_OUTPUT_SCALE_FACTOR, VERTICAL, asyh->view.max_v));

	PUSH_MTHD(push, NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS(i),
		  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, CURSOR, USAGE_W256_H256) |
		  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OLUT_ALLOWED, TRUE) |
		  (asyh->view.vtaps == 5 ?
		   NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_SCALER_TAPS, TAPS_5) :
		   NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_SCALER_TAPS, TAPS_2)) |
		  (asyh->view.upscale_v ?
		   NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, TRUE) :
		   NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE)));

	/* The postcomp scaler needs fixed-point samples between RGB2ITP and
	 * ITP2RGB. Enable their FVLUTs while scaling, otherwise FP16 bit patterns
	 * are interpreted as fixed point and corrupt the output.
	 */
	PUSH_MTHD(push, NVCA7D, HEAD_SET_RGB2ITP_CONTROL(i), scale ?
		  NVDEF(NVCA7D, HEAD_SET_RGB2ITP_CONTROL, ENABLE_FVLUT, ENABLE) |
		  NVDEF(NVCA7D, HEAD_SET_RGB2ITP_CONTROL, FVLUT_INTERPOLATE, ENABLE) : 0);
	PUSH_MTHD(push, NVCA7D, HEAD_SET_ITP2RGB_CONTROL(i), scale ?
		  NVDEF(NVCA7D, HEAD_SET_ITP2RGB_CONTROL, ENABLE_FVLUT, ENABLE) |
		  NVDEF(NVCA7D, HEAD_SET_ITP2RGB_CONTROL, FVLUT_INTERPOLATE, ENABLE) : 0);
	return 0;
}

static int
headca7d_mtc_clr(struct nv50_head *head)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const int i = head->base.index;
	int ret;

	ret = PUSH_WAIT(push, 6);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(i), 0x00000000);
	PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(i * 2), 0x00000000);
	PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(i * 2 + 1), 0x00000000);

	return 0;
}

/* Use NVKMS's non-DSC tile split: round up the width of each tile except
 * the last, which takes the remainder so the slices cover hactive exactly.
 */
static int
headca7d_mtc_set(struct nv50_head *head, struct nv50_head_atom *asyh)
{
	struct nvif_push *push = &head->disp->core->chan.push;
	const u32 hactive = asyh->mode.h.blanks - asyh->mode.h.blanke;
	const int i = head->base.index;
	unsigned long tiles_mask;
	u32 start = 0, idx = 0, tiles, tile;
	int ret;

	/* An empty mask indicates that atomic check did not provide a required
	 * assignment. Warn and fall back to the hardware identity mapping.
	 */
	if (WARN_ON_ONCE(!asyh->mtc.tiles_mask)) {
		asyh->mtc.tiles_mask = BIT(i);
		asyh->mtc.phywins_mask[0] = BIT(i * 2);
		asyh->mtc.phywins_mask[1] = BIT(i * 2 + 1);
	}

	tiles_mask = asyh->mtc.tiles_mask;
	tiles = hweight8(asyh->mtc.tiles_mask);

	ret = PUSH_WAIT(push, 6 + tiles * 2);
	if (ret)
		return ret;

	PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(i), asyh->mtc.tiles_mask);
	PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(i * 2),
		  asyh->mtc.phywins_mask[0]);
	PUSH_MTHD(push, NVCA7D, WINDOW_SET_PHYSICAL(i * 2 + 1),
		  asyh->mtc.phywins_mask[1]);

	for_each_set_bit(tile, &tiles_mask, 8) {
		const u32 width = (idx < tiles - 1) ?
			DIV_ROUND_UP(hactive, tiles) : hactive - start;

		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(tile),
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, START, start) |
			  NVVAL(NVCA7D, TILE_SET_TILE_SIZE, WIDTH, width));

		start += width;
		idx++;
	}

	return 0;
}

const struct nv50_head_func
headca7d = {
	.view = headca7d_view,
	.mode = headca7d_mode,
	.olut = headc57d_olut,
	.ilut_check = head907d_ilut_check,
	.olut_identity = true,
	.olut_size = 1024,
	.olut_set = headca7d_olut_set,
	.olut_clr = headca7d_olut_clr,
	.curs_layout = head917d_curs_layout,
	.curs_format = headc37d_curs_format,
	.curs_set = headca7d_curs_set,
	.curs_clr = headca7d_curs_clr,
	.dither = headca7d_dither,
	.procamp = headca7d_procamp,
	.or = headca7d_or,
	.static_wndw_map = headc37d_static_wndw_map,
	.display_id = headca7d_display_id,
	.mtc_set = headca7d_mtc_set,
	.mtc_clr = headca7d_mtc_clr,
};

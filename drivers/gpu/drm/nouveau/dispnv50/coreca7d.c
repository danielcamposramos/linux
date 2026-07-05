/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include "core.h"
#include "head.h"

#include <nvif/class.h>
#include <nvif/pushc97b.h>

#include <nvhw/class/clca7d.h>
#include <nvhw/class/clca73.h>
#include <nvhw/drf.h>

#include <nouveau_bo.h>

static int
coreca7d_caps_init(struct nouveau_drm *drm, struct nv50_disp *disp)
{
	u32 syscapc, capb, capf;
	int i, ret;

	ret = corec57d_caps_init(drm, disp);
	if (ret)
		return ret;

	/* TYPE_0 tiles retain both the output scaler and vertical filter, so
	 * record them separately from tiles that lack either feature.
	 */
	syscapc = nvif_rd32(&disp->caps, NVCA73_SYS_CAPC);
	capb = nvif_rd32(&disp->caps, NVCA73_SYS_CAPB);
	capf = nvif_rd32(&disp->caps, NVCA73_IHUB_COMMON_CAPF);

	for (i = 0; i < NVCA73_SYS_CAPC_TILE_EXISTS__SIZE_1; i++) {
		u32 capa;

		if (!NVDEF_TEST(syscapc, NVCA73, SYS_CAPC, TILE_EXISTS, i, ==, YES))
			continue;

		disp->tile.tiles |= BIT(i);
		disp->tile.nr_tiles++;

		if (NVDEF_TEST(syscapc, NVCA73, SYS_CAPC,
			       TILE_SUPPORT_MULTI_TILE, i, ==, YES))
			disp->tile.multi_tiles |= BIT(i);

		capa = nvif_rd32(&disp->caps, NVCA73_POSTCOMP_HDR_CAPA(i));
		if (NVDEF_TEST(capa, NVCA73, POSTCOMP_HDR_CAPA, SCLR_PRESENT, ==, TRUE) &&
		    NVDEF_TEST(capa, NVCA73, POSTCOMP_HDR_CAPA, VFILTER_PRESENT, ==, TRUE))
			disp->tile.type0_tiles |= BIT(i);
	}

	/* GB20x has equal numbers of logical and physical windows, so derive
	 * phywin availability from the window-exists bits. Keep the mask to
	 * avoid requiring contiguous IDs.
	 */
	for (i = 0; i < NVCA73_SYS_CAPB_WINDOW_EXISTS__SIZE_1; i++) {
		if (!NVDEF_TEST(capb, NVCA73, SYS_CAPB, WINDOW_EXISTS, i, ==, YES))
			continue;
		disp->tile.phywins |= BIT(i);
		disp->tile.nr_phywins++;
	}
	for (i = 0; i < NVCA73_IHUB_COMMON_CAPF_PHYWIN_SUPPORT_MULTI_TILE__SIZE_1; i++) {
		if (NVDEF_TEST(capf, NVCA73, IHUB_COMMON_CAPF,
			       PHYWIN_SUPPORT_MULTI_TILE, i, ==, YES))
			disp->tile.multi_phywins |= BIT(i);
	}

	NV_DEBUG(drm, "disp: tiles %02x (multi %02x, type0 %02x), phywins %08x (multi %08x)\n",
		 disp->tile.tiles, disp->tile.multi_tiles,
		 disp->tile.type0_tiles, disp->tile.phywins,
		 disp->tile.multi_phywins);
	return 0;
}

static int
coreca7d_update(struct nv50_core *core, u32 *interlock, bool ntfy)
{
	const u64 ntfy_addr = core->disp->sync->offset + NV50_DISP_CORE_NTFY;
	const u32 ntfy_hi = upper_32_bits(ntfy_addr);
	const u32 ntfy_lo = lower_32_bits(ntfy_addr);
	struct nvif_push *push = &core->chan.push;
	int ret;

	ret = PUSH_WAIT(push, 5 + (ntfy ? 5 + 2 : 0));
	if (ret)
		return ret;

	if (ntfy) {
		PUSH_MTHD(push, NVCA7D, SET_SURFACE_ADDRESS_HI_NOTIFIER, ntfy_hi,

					SET_SURFACE_ADDRESS_LO_NOTIFIER,
			  NVVAL(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, ADDRESS_LO, ntfy_lo >> 4) |
			  NVDEF(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, TARGET, PHYSICAL_NVM) |
			  NVDEF(NVCA7D, SET_SURFACE_ADDRESS_LO_NOTIFIER, ENABLE, ENABLE));

		PUSH_MTHD(push, NVCA7D, SET_NOTIFIER_CONTROL,
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, MODE, WRITE) |
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, NOTIFY, ENABLE));
	}

	PUSH_MTHD(push, NVCA7D, SET_INTERLOCK_FLAGS, interlock[NV50_DISP_INTERLOCK_CURS],
				SET_WINDOW_INTERLOCK_FLAGS, interlock[NV50_DISP_INTERLOCK_WNDW]);

	PUSH_MTHD(push, NVCA7D, UPDATE,
		  NVDEF(NVCA7D, UPDATE, RELEASE_ELV, TRUE) |
		  NVDEF(NVCA7D, UPDATE, SPECIAL_HANDLING, NONE) |
		  NVDEF(NVCA7D, UPDATE, INHIBIT_INTERRUPTS, FALSE));

	if (ntfy) {
		PUSH_MTHD(push, NVCA7D, SET_NOTIFIER_CONTROL,
			  NVDEF(NVCA7D, SET_NOTIFIER_CONTROL, NOTIFY, DISABLE));
	}

	return PUSH_KICK(push);
}

static int
coreca7d_init(struct nv50_core *core)
{
	struct nvif_push *push = &core->chan.push;
	unsigned long head_mask = core->disp->disp->head_mask;
	const u32 windows = 8, heads = 4;
	int ret, i;

	ret = PUSH_WAIT(push, windows * 6 + heads * 6);
	if (ret)
		return ret;

	for (i = 0; i < windows; i++) {
		PUSH_MTHD(push, NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i),
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED1BPP, TRUE) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED2BPP, TRUE) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED4BPP, TRUE) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS, RGB_PACKED8BPP, TRUE),

					WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(i), 0x00000000);

		PUSH_MTHD(push, NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS(i),
			  NVVAL(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, MAX_PIXELS_FETCHED_PER_LINE, 0x7fff) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, ILUT_ALLOWED, TRUE) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, INPUT_SCALER_TAPS, TAPS_2) |
			  NVDEF(NVCA7D, WINDOW_SET_WINDOW_USAGE_BOUNDS, UPSCALING_ALLOWED, FALSE),

					WINDOW_SET_PHYSICAL(i), BIT(i));
	}

	for (i = 0; i < heads; i++) {
		PUSH_MTHD(push, NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS(i),
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, CURSOR, USAGE_W256_H256) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OLUT_ALLOWED, TRUE) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, OUTPUT_SCALER_TAPS, TAPS_2) |
			  NVDEF(NVCA7D, HEAD_SET_HEAD_USAGE_BOUNDS, UPSCALING_ALLOWED, TRUE));

		PUSH_MTHD(push, NVCA7D, HEAD_SET_TILE_MASK(i), BIT(i));

		PUSH_MTHD(push, NVCA7D, TILE_SET_TILE_SIZE(i), 0);
	}

	/* CA7D uses the same five-tap coefficient table as C57D. */
	for_each_set_bit(i, &head_mask, 8) {
		int idx;

		ret = PUSH_WAIT(push, 3 * 16 * 4 * 2);
		if (ret)
			return ret;

		/* The index encodes ratio << 6 | phase << 2 | coefficient. */
		for (idx = 0; idx < 3 * 16 * 4; idx++) {
			const s16 coeff =
				corec57d_taps5_coeff[idx >> 6][(idx >> 2) & 15][idx & 3];

			PUSH_MTHD(push, NVCA7D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE(i),
				  NVVAL(NVCA7D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE, DATA, coeff) |
				  NVVAL(NVCA7D, HEAD_SET_OUTPUT_SCALER_COEFF_VALUE, INDEX, idx));
		}
	}

	core->assign_windows = true;
	return PUSH_KICK(push);
}

static const struct nv50_core_func
coreca7d = {
	.init = coreca7d_init,
	.ntfy_init = corec37d_ntfy_init,
	.caps_init = coreca7d_caps_init,
	.caps_class = GB202_DISP_CAPS,
	.ntfy_wait_done = corec37d_ntfy_wait_done,
	.update = coreca7d_update,
	.wndw.owner = corec37d_wndw_owner,
	.head = &headca7d,
	.sor = &sorc37d,
#if IS_ENABLED(CONFIG_DEBUG_FS)
	.crc = &crcca7d,
#endif
};

int
coreca7d_new(struct nouveau_drm *drm, s32 oclass, struct nv50_core **pcore)
{
	return core507d_new_(&coreca7d, drm, oclass, pcore);
}

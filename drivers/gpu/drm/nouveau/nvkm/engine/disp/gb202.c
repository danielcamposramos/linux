// SPDX-License-Identifier: MIT
/*
 * Copyright 2026 Valve Corp.
 */
#include "priv.h"
#include "head.h"
#include "ior.h"
#include "outp.h"

#include <linux/math64.h>

#include <subdev/timer.h>

/* GB20x (NVD5.0) reorganised the SF HDMI packet units. The AVI unit is
 * unchanged from GV100, but the legacy VSI unit is gone. Vendor infoframes
 * are sent through the shared generic infoframe units instead. Register
 * layout per NVIDIA's clc971.h/clca71.h, programming sequence per
 * nvhdmipkt_C971.c:programAdvancedInfoframeC971().
 */
static void
gb202_sor_hdmi_infoframe_vsi(struct nvkm_ior *ior, int head, void *data, u32 size)
{
	struct nvkm_device *device = ior->disp->engine.subdev.device;
	const u32 hoff = head * 0x400;
	/* Generic infoframe unit 1, the slot NVIDIA's driver uses for the VSI. */
	const u32 ctrl = 0x6f0138 + hoff;
	u8 buf[36] = {};
	int i;

	/* Disable the unit and wait for it to go idle. */
	nvkm_mask(device, ctrl, 0x00000001, 0x00000000);
	if (nvkm_msec(device, 2000,
		if (!(nvkm_rd32(device, ctrl) & 0x00400000))
			break;
	) < 0)
		return;

	if (!size)
		return;

	/* Clear SENT status, and point the data port at unit 1's slot. */
	nvkm_mask(device, ctrl, 0x00800000, 0x00800000);
	nvkm_wr32(device, 0x6f03f0 + hoff, 0x00000001);

	/* The data port takes the raw packet, except that a zero is inserted
	 * in HB3 after the three header bytes. A slot is 9 dwords (HB0-3 plus
	 * up to 32 payload bytes). An HDMI infoframe carries at most PB0-27,
	 * so the tail stays zero, and we always write the whole slot.
	 */
	size = min_t(u32, size, 31);
	memcpy(buf, data, min_t(u32, size, 3));
	if (size > 3)
		memcpy(&buf[4], (u8 *)data + 3, size - 3);

	for (i = 0; i < 36; i += 4) {
		nvkm_wr32(device, 0x6f03f4 + hoff, buf[i + 0] | buf[i + 1] << 8 |
						   buf[i + 2] << 16 |
						   (u32)buf[i + 3] << 24);
	}

	/* No flip ID or scanline matching. */
	nvkm_wr32(device, 0x6f013c + hoff, 0x00000000);

	/* ENABLE | RUN_MODE=ALWAYS | LOC=VBLANK | OFFSET=1 | SIZE=0. */
	nvkm_wr32(device, ctrl, 0x00000041);

	/* Audio priority low (the init value). */
	nvkm_wr32(device, 0x6f03f8 + hoff, 0x00000002);
}

/* NVD5.0 uses GCP slot 1. Mask SB0-SB2 to preserve SB1_CTRL (bit 24),
 * which defaults to hardware generation of the deep color fields.
 */
static void
gb202_sor_hdmi_gcp(struct nvkm_ior *sor, int head, bool enable)
{
	struct nvkm_device *device = sor->disp->engine.subdev.device;
	const u32 hdmi = head * 0x400;

	nvkm_mask(device, 0x6f0040 + hdmi, 0x00000001, 0x00000000);
	nvkm_mask(device, 0x6f004c + hdmi, 0x00ffffff,
		  (sor->asy.outp->hdmi_gcp_sb1 << 8) | (!enable ? 0x00000001 : 0x00000010));
	nvkm_mask(device, 0x6f0040 + hdmi, 0x00000001, 0x00000001);
}

/* Same core-channel state mirror as gv100_head_state() (assembly at 0x680000,
 * armed at +0x8000, per-head method offsets unchanged), but NVD5.0 spaces
 * heads 0x800 apart (see NVCA7D_HEAD_SET_*(a) in clca7d.h).
 */
static void
gb202_head_state(struct nvkm_head *head, struct nvkm_head_state *state)
{
	struct nvkm_device *device = head->disp->engine.subdev.device;
	const u32 hoff = (state == &head->arm) * 0x8000 + head->id * 0x800;
	const u32 aoff = hoff - head->id * 0x800;
	u32 data;

	data = nvkm_rd32(device, 0x682064 + hoff);
	state->vtotal = (data & 0xffff0000) >> 16;
	state->htotal = (data & 0x0000ffff);
	data = nvkm_rd32(device, 0x682068 + hoff);
	state->vsynce = (data & 0xffff0000) >> 16;
	state->hsynce = (data & 0x0000ffff);
	data = nvkm_rd32(device, 0x68206c + hoff);
	state->vblanke = (data & 0xffff0000) >> 16;
	state->hblanke = (data & 0x0000ffff);
	data = nvkm_rd32(device, 0x682070 + hoff);
	state->vblanks = (data & 0xffff0000) >> 16;
	state->hblanks = (data & 0x0000ffff);
	/* The low method holds clock bits 30:0 and ADJ1000DIV1001 at bit 31.
	 * Read the four bits in SET_PIXEL_CLOCK_FREQUENCY_HI as well to recover
	 * the full 35 bit value from headca7d_mode().
	 */
	data = nvkm_rd32(device, 0x68200c + hoff);
	state->hz = data & 0x7fffffff;
	state->hz |= (u64)(nvkm_rd32(device, 0x6820c0 + hoff) & 0x0000000f) << 31;
	/* ADJ1000DIV1001 scales the programmed rate by 1000/1001. */
	if (data & 0x80000000)
		state->hz = div_u64(state->hz * 1000, 1001);

	/* STRUCTURE is 1:0, PROGRESSIVE is zero. */
	state->interlace = (nvkm_rd32(device, 0x682008 + hoff) & 0x00000003) != 0;

	/* Both scaler tap fields encode TAPS_2 as 1 and TAPS_5 as 4. */
	data = nvkm_rd32(device, 0x68204c + hoff);
	state->view.iW = (data & 0x00007fff);
	state->view.iH = (data & 0x7fff0000) >> 16;
	data = nvkm_rd32(device, 0x682058 + hoff);
	state->view.oW = (data & 0x00007fff);
	state->view.oH = (data & 0x7fff0000) >> 16;
	data = nvkm_rd32(device, 0x682014 + hoff);
	state->view.vtaps = (data & 0x00000007) >= 4 ? 5 : 2;
	state->view.htaps = ((data & 0x00000070) >> 4) >= 4 ? 5 : 2;

	data = nvkm_rd32(device, 0x682004 + hoff);
	state->or.nhsync = (data & 0x00000004) != 0;
	state->or.nvsync = (data & 0x00000008) != 0;
	switch ((data & 0x000000f0) >> 4) {
	case 10: state->or.depth = 18; break; /* BPP_18_444NP */
	case 9: state->or.depth = 16; break;
	case 7: state->or.depth = 36; break;
	case 5: state->or.depth = 30; break;
	case 4: state->or.depth = 24; break;
	case 1: state->or.depth = 18; break;
	default:
		state->or.depth = 18;
		/* Takeover may query unused heads, which have no valid depth when
		 * the raster is unset.
		 */
		WARN_ON(state->htotal && state->vtotal);
		break;
	}
	/* Use the matching mirror for ownership too, so tile and window
	 * assignments describe the same armed or assembly state as the raster.
	 */
	state->mtc.tiles = nvkm_rd32(device, 0x682060 + hoff) & 0x000000ff;
	state->mtc.phywins[0] = nvkm_rd32(device, 0x681014 + (head->id * 2) * 0x80 + aoff);
	state->mtc.phywins[1] = nvkm_rd32(device, 0x681014 + (head->id * 2 + 1) * 0x80 + aoff);
}

/* NVD5.0 (GB20x and later) moved the RM head-timing interrupt enable to
 * the low-latency vector's EN1 block. The event latch is unchanged.
 */
static void
gb202_head_vblank_put(struct nvkm_head *head)
{
	struct nvkm_device *device = head->disp->engine.subdev.device;

	nvkm_mask(device, 0x611ef0 + (head->id * 4), 0x00000002, 0x00000000);
}

static void
gb202_head_vblank_get(struct nvkm_head *head)
{
	struct nvkm_device *device = head->disp->engine.subdev.device;

	nvkm_wr32(device, 0x611800 + (head->id * 4), 0x00000002);
	nvkm_mask(device, 0x611ef0 + (head->id * 4), 0x00000002, 0x00000002);
}

static irqreturn_t
gb202_disp_intr(struct nvkm_inth *inth)
{
	struct nvkm_disp *disp = container_of(inth, typeof(*disp), engine.subdev.inth);
	irqreturn_t ret = tu102_disp_intr(inth);

	/* The FE interrupt vectors are message-based on NVD5.0. Re-arm the
	 * low-latency vector so it fires again for any event that latched
	 * while we were servicing.
	 */
	nvkm_wr32(disp->engine.subdev.device, 0x611f34, 0x00000001);
	return ret;
}

static const struct nvkm_head_func
gb202_gsp_head = {
	.state = gb202_head_state,
	.rgpos = gv100_head_rgpos,
	.vblank_get = gb202_head_vblank_get,
	.vblank_put = gb202_head_vblank_put,
};

/* GB20x is GSP-only. This table supplies the register programming the
 * GSP-RM display path needs from the chip.
 */
static const struct nvkm_disp_func
gb202_gsp_disp = {
	.uevent = &gv100_disp_chan_uevent,
	.ramht_size = 0x2000,
	/* Head timing arrives on the dedicated low-latency vector. */
	.gsp.intr = gb202_disp_intr,
	.gsp.intr_low_latency = true,
	.gsp.head = &gb202_gsp_head,
	.gsp.hdmi_gcp = gb202_sor_hdmi_gcp,
	/* The legacy AVI unit is unchanged on GB20x. */
	.gsp.hdmi_infoframe_avi = gv100_sor_hdmi_infoframe_avi,
	.gsp.hdmi_infoframe_vsi = gb202_sor_hdmi_infoframe_vsi,
};

int
gb202_disp_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	       struct nvkm_disp **pdisp)
{
	return r535_disp_new(&gb202_gsp_disp, device, type, inst, pdisp);
}

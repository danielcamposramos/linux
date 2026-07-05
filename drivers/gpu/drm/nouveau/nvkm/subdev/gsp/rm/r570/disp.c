/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#include <rm/rm.h>

#include <engine/disp.h>
#include <engine/disp/ior.h>
#include <engine/disp/outp.h>

#include "nvhw/drf.h"

#include "nvrm/disp.h"

static int
r570_dmac_alloc(struct nvkm_disp *disp, u32 oclass, int inst, u32 put_offset,
		struct nvkm_gsp_object *dmac)
{
	NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS *args;

	args = nvkm_gsp_rm_alloc_get(&disp->rm.object, (oclass << 16) | inst, oclass,
				     sizeof(*args), dmac);
	if (IS_ERR(args))
		return PTR_ERR(args);

	args->channelInstance = inst;
	args->offset = put_offset;
	args->subDeviceId = BIT(0);

	return nvkm_gsp_rm_alloc_wr(dmac, args);
}

static int
r570_disp_chan_set_pushbuf(struct nvkm_disp *disp, s32 oclass, int inst, struct nvkm_memory *memory)
{
	struct nvkm_gsp *gsp = disp->rm.objcom.client->gsp;
	NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&gsp->internal.device.subdevice,
				    NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,
				    sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	if (memory) {
		switch (nvkm_memory_target(memory)) {
		case NVKM_MEM_TARGET_NCOH:
			ctrl->addressSpace = ADDR_SYSMEM;
			ctrl->cacheSnoop = 0;
			ctrl->pbTargetAperture = PHYS_PCI;
			break;
		case NVKM_MEM_TARGET_HOST:
			ctrl->addressSpace = ADDR_SYSMEM;
			ctrl->cacheSnoop = 1;
			ctrl->pbTargetAperture = PHYS_PCI_COHERENT;
			break;
		case NVKM_MEM_TARGET_VRAM:
			ctrl->addressSpace = ADDR_FBMEM;
			ctrl->pbTargetAperture = PHYS_NVM;
			break;
		default:
			WARN_ON(1);
			return -EINVAL;
		}

		ctrl->physicalAddr = nvkm_memory_addr(memory);
		ctrl->limit = nvkm_memory_size(memory) - 1;
	}

	ctrl->hclass = oclass;
	ctrl->channelInstance = inst;
	ctrl->valid = ((oclass & 0xff) != 0x7a) ? 1 : 0;
	ctrl->subDeviceId = BIT(0);

	return nvkm_gsp_rm_ctrl_wr(&gsp->internal.device.subdevice, ctrl);
}

static int
r570_dp_vcpi(struct nvkm_ior *sor, int head, u8 slot, u8 slot_nr, u16 pbn, u16 aligned_pbn)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;
	ctrl->sorIndex = sor->id;
	ctrl->dpLink = sor->asy.link == 2;
	ctrl->bEnableOverride = 1;
	ctrl->bMST = 1;
	ctrl->hBlankSym = 0;
	ctrl->vBlankSym = 0;
	ctrl->colorFormat = 0;
	ctrl->bEnableTwoHeadOneOr = 0;
	ctrl->singleHeadMultistreamMode = 0;
	ctrl->MST.slotStart = slot;
	ctrl->MST.slotEnd = slot + slot_nr - 1;
	ctrl->MST.PBN = pbn;
	ctrl->MST.Timeslice = aligned_pbn;
	ctrl->MST.sendACT = 0;
	ctrl->MST.singleHeadMSTPipeline = 0;
	ctrl->MST.bEnableAudioOverRightPanel = 0;
	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r570_dp_sst(struct nvkm_ior *sor, int head, bool ef,
	    u32 watermark, u32 hblanksym, u32 vblanksym)
{
	struct nvkm_disp *disp = sor->disp;
	NV0073_CTRL_CMD_DP_CONFIG_STREAM_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_STREAM, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;
	ctrl->sorIndex = sor->id;
	ctrl->dpLink = sor->asy.link == 2;
	ctrl->bEnableOverride = 1;
	ctrl->bMST = 0;
	ctrl->hBlankSym = hblanksym;
	ctrl->vBlankSym = vblanksym;
	ctrl->colorFormat = 0;
	ctrl->bEnableTwoHeadOneOr = 0;
	ctrl->SST.bEnhancedFraming = ef;
	ctrl->SST.tuSize = 64;
	ctrl->SST.waterMark = watermark;
	ctrl->SST.bEnableAudioOverRightPanel = 0;
	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r570_dp_set_indexed_link_rates(struct nvkm_outp *outp)
{
	NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES_PARAMS *ctrl;
	struct nvkm_disp *disp = outp->disp;

	if (WARN_ON(outp->dp.rates > ARRAY_SIZE(ctrl->linkRateTbl)))
		return -EINVAL;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_CONFIG_INDEXED_LINK_RATES, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(outp->index);
	for (int i = 0; i < outp->dp.rates; i++)
		ctrl->linkRateTbl[outp->dp.rate[i].dpcd] = outp->dp.rate[i].rate * 10 / 200;

	return nvkm_gsp_rm_ctrl_wr(&disp->rm.objcom, ctrl);
}

static int
r570_dp_get_caps(struct nvkm_disp *disp, int *plink_bw, bool *pmst, bool *pwm)
{
	NV0073_CTRL_CMD_DP_GET_CAPS_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_DP_GET_CAPS, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->sorIndex = ~0;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	switch (NVVAL_GET(ctrl->maxLinkRate, NV0073_CTRL_CMD, DP_GET_CAPS, MAX_LINK_RATE)) {
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_1_62:
		*plink_bw = 0x06;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_2_70:
		*plink_bw = 0x0a;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_5_40:
		*plink_bw = 0x14;
		break;
	case NV0073_CTRL_CMD_DP_GET_CAPS_MAX_LINK_RATE_8_10:
		*plink_bw = 0x1e;
		break;
	default:
		*plink_bw = 0x00;
		break;
	}

	*pmst = ctrl->bIsMultistreamSupported;
	*pwm = ctrl->bHasIncreasedWatermarkLimits;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

/* Validate the supplied head configuration with RM and extract per-head
 * tile requirements. Windows follow nouveau's fixed two-per-head assignment
 * with format bounds supplied by the caller.
 */
static int
r570_disp_imp_check_locked(struct nvkm_disp *disp, u8 num_heads, bool tiled,
			   const struct nvkm_disp_imp_head *heads,
			   struct nvkm_disp_imp_result *result)
{
	NVC372_CTRL_IS_MODE_POSSIBLE_PARAMS *ctrl;
	int ret = 0, i, w;

	memset(result, 0, sizeof(*result));

	if (num_heads > ARRAY_SIZE(result->head))
		return -EINVAL;

	/* An empty configuration does not need resources or tiling assignment, so
	 * return possible without an RM query like NVKMS does.
	 */
	if (!num_heads) {
		result->possible = true;
		return 0;
	}

	for (i = 0; i < num_heads; i++) {
		if (heads[i].index >= ARRAY_SIZE(result->head))
			return -EINVAL;
	}

	if (!disp->rm.c372.client) {
		ret = nvkm_gsp_rm_alloc(&disp->rm.device.object, NVKM_RM_DISP_SW,
					NVC372_DISPLAY_SW, 0, &disp->rm.c372);
		if (ret) {
			memset(&disp->rm.c372, 0, sizeof(disp->rm.c372));
			return ret;
		}
	}

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.c372,
				    NVC372_CTRL_CMD_IS_MODE_POSSIBLE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	memset(ctrl, 0, sizeof(*ctrl));

	for (i = 0; i < num_heads; i++) {
		const struct nvkm_disp_imp_head *head = &heads[i];
		NVC372_CTRL_IMP_HEAD *imph = &ctrl->head[ctrl->numHeads++];
		const u16 hactive = head->hblanks - head->hblanke;
		const u16 vactive = head->vblanks - head->vblanke;
		/* Aspect scaling can round the output height above the active raster,
		 * so clamp it here to keep the overscan subtraction from underflowing.
		 */
		const u16 idle_h = min_t(u16, head->out_h, vactive);
		u32 leading, overscan;

		imph->headIndex = head->index;
		imph->maxPixelClkKHz = head->pclk_khz;
		imph->rasterSize.width = head->htotal;
		imph->rasterSize.height = head->vtotal;
		imph->rasterBlankStart.X = head->hblanks;
		imph->rasterBlankStart.Y = head->vblanks;
		imph->rasterBlankEnd.X = head->hblanke;
		imph->rasterBlankEnd.Y = head->vblanke;

		imph->control.masterLockMode = NV_DISP_LOCK_MODE_NO_LOCK;
		imph->control.masterLockPin = NV_DISP_LOCK_PIN_UNSPECIFIED;
		imph->control.slaveLockMode = NV_DISP_LOCK_MODE_NO_LOCK;
		imph->control.slaveLockPin = NV_DISP_LOCK_PIN_UNSPECIFIED;

		/* Use the caller's validated ratios and assigned taps so IMP models the
		 * scaler configuration that will be programmed. Downscale factors are
		 * input/output ratios multiplied by 0x400.
		 */
		imph->maxDownscaleFactorH = head->out_w && head->in_w > head->out_w ?
			DIV_ROUND_UP(head->in_w * 0x400, head->out_w) : 0x400;
		imph->maxDownscaleFactorV = head->out_h && head->in_h > head->out_h ?
			DIV_ROUND_UP(head->in_h * 0x400, head->out_h) : 0x400;
		imph->outputScalerVerticalTaps = head->vtaps ? head->vtaps : 2;
		imph->bUpscalingAllowedV = head->out_h > head->in_h;

		/* Include the overscan borders in the frame-idle counts, matching
		 * nvComputeMinFrameIdle(). nouveau centers the output viewport, so its
		 * yAdjust term is zero.
		 */
		overscan = vactive / 2 - idle_h / 2;
		leading = head->vblanke + overscan + 1;
		if (leading < 2 || leading + idle_h > head->vtotal) {
			/* Vsync and back porch each need a line, and the trailing count must
			 * not underflow. Invalid timings make the mode impossible without
			 * constituting an RM control error.
			 */
			nvkm_gsp_rm_ctrl_done(&disp->rm.c372, ctrl);
			return 0;
		}
		imph->minFrameIdle.leadingRasterLines = leading;
		imph->minFrameIdle.trailingRasterLines = head->vtotal -
			(leading + idle_h);

		imph->lut = NVC372_CTRL_IMP_LUT_USAGE_1025;
		imph->cursorSize32p = 256 / 32;

		imph->bEnableDsc = head->dsc_enable;
		/* Pass the slice mask on all GPUs, as NVKMS does. On tiled GPUs the
		 * control definition also requires target bpp and slice width, so fill
		 * both even though NVKMS leaves target bpp unset.
		 */
		if (head->dsc_enable) {
			u32 min_slices = ffs(head->dsc_slice_mask);

			if (tiled && !min_slices) {
				/* Tiled DSC requires at least one allowed slice count. */
				nvkm_gsp_rm_ctrl_done(&disp->rm.c372, ctrl);
				return 0;
			}
			imph->possibleDscSliceCountMask = head->dsc_slice_mask;
			if (tiled) {
				imph->dscTargetBppX16 = head->dsc_bpp_x16;
				imph->maxDscSliceWidth = min_slices < hactive ?
					DIV_ROUND_UP(hactive, min_slices) : hactive;
			}
		}

		/* A nonzero mask constrains IMP to the caller's assignment, so
		 * IMP must fail if those tiles are insufficient. Zero lets IMP
		 * choose the tile count.
		 */
		imph->tileMask = head->tile_mask;

		for (w = 0; w < 2; w++) {
			NVC372_CTRL_IMP_WINDOW *impw;

			/* Omit windows with no allowed formats. */
			if (!head->wndw_formats[w])
				continue;

			impw = &ctrl->window[ctrl->numWindows++];
			impw->windowIndex = head->index * 2 + w;
			impw->owningHead = head->index;
			/* Window scaling is fixed at 1:1 with two taps, so only
			 * the format bound comes from the caller. The fetch
			 * bound follows the input width.
			 */
			impw->formatUsageBound = head->wndw_formats[w];
			impw->maxPixelsFetchedPerLine =
				(((head->in_w + 14) * 0x400 + 1023) >> 10) + 8;
			impw->maxDownscaleFactorH = 0x400;
			impw->maxDownscaleFactorV = 0x400;
			impw->inputScalerVerticalTaps = 2;
			impw->bUpscalingAllowedV = false;
			impw->lut = NVC372_CTRL_IMP_LUT_USAGE_1025;
			impw->tmoLut = NVC372_CTRL_IMP_LUT_USAGE_1025;
		}
	}

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.c372, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.c372, ctrl);
		return ret;
	}

	/* Tiled GPUs need an assignment to implement a possible mode, while
	 * earlier GPUs report feasibility through bIsPossible alone.
	 */
	result->possible = ctrl->bIsPossible && (!tiled || ctrl->numTilingAssignments);

	/* The first assignment gives the required tiles. Later assignments only
	 * reduce dispclk, so use the first like NVKMS. Each tileList entry
	 * indexes ctrl->head and supplies that head's DSC slice count.
	 */
	if (result->possible) {
		const u32 tiles = min_t(u32, ctrl->tilingAssignments[0].numTiles,
					ARRAY_SIZE(ctrl->tileList));

		for (i = 0; i < tiles; i++) {
			const NVC372_TILE_ENTRY *entry = &ctrl->tileList[i];
			u8 head;

			if (entry->head >= ctrl->numHeads) {
				result->possible = false;
				break;
			}

			head = ctrl->head[entry->head].headIndex;
			if (head >= ARRAY_SIZE(result->head)) {
				result->possible = false;
				break;
			}

			/* Entries for a head should agree on the DSC slice count, so keep the
			 * first entry's value like NVKMS.
			 */
			if (!result->head[head].required_tiles) {
				result->head[head].dsc_slices =
					entry->headDscSlices;
			}
			result->head[head].required_tiles++;
		}

		/* Each active head requires at least one tile. */
		for (i = 0; i < num_heads && tiled; i++) {
			if (!result->head[heads[i].index].required_tiles)
				result->possible = false;
		}
	}

	nvkm_gsp_rm_ctrl_done(&disp->rm.c372, ctrl);
	return 0;
}

static int
r570_disp_imp_check(struct nvkm_disp *disp, u8 num_heads, bool tiled,
		    const struct nvkm_disp_imp_head *heads,
		    struct nvkm_disp_imp_result *result)
{
	int ret;

	mutex_lock(&disp->rm.imp_mutex);
	ret = r570_disp_imp_check_locked(disp, num_heads, tiled, heads, result);
	mutex_unlock(&disp->rm.imp_mutex);
	return ret;
}

static int
r570_bl_ctrl(struct nvkm_disp *disp, unsigned display_id, bool set, int *pval)
{
	u32 cmd = set ? NV0073_CTRL_CMD_SPECIFIC_SET_BACKLIGHT_BRIGHTNESS :
			NV0073_CTRL_CMD_SPECIFIC_GET_BACKLIGHT_BRIGHTNESS;
	NV0073_CTRL_SPECIFIC_BACKLIGHT_BRIGHTNESS_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom, cmd, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->displayId = BIT(display_id);
	ctrl->brightness = *pval;
	ctrl->brightnessType = NV0073_CTRL_SPECIFIC_BACKLIGHT_BRIGHTNESS_TYPE_PERCENT100;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret)
		return ret;

	*pval = ctrl->brightness;

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r570_disp_get_active(struct nvkm_disp *disp, unsigned head, u32 *displayid)
{
	NV0073_CTRL_SYSTEM_GET_ACTIVE_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->head = head;

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret) {
		nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
		return ret;
	}

	*displayid = ctrl->displayId;
	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}
static int
r570_disp_get_connect_state(struct nvkm_disp *disp, unsigned display_id)
{
	NV0073_CTRL_SYSTEM_GET_CONNECT_STATE_PARAMS *ctrl;
	int ret;

	ctrl = nvkm_gsp_rm_ctrl_get(&disp->rm.objcom,
				    NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->subDeviceInstance = 0;
	ctrl->displayMask = BIT(display_id);

	ret = nvkm_gsp_rm_ctrl_push(&disp->rm.objcom, &ctrl, sizeof(*ctrl));
	if (ret == 0 && (ctrl->displayMask & BIT(display_id)))
		ret = 1;

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return ret;
}

static int
r570_disp_get_supported(struct nvkm_disp *disp, unsigned long *pmask)
{
	NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS *ctrl;

	ctrl = nvkm_gsp_rm_ctrl_rd(&disp->rm.objcom,
				   NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	*pmask = ctrl->displayMask;

	nvkm_gsp_rm_ctrl_done(&disp->rm.objcom, ctrl);
	return 0;
}

static int
r570_disp_get_static_info(struct nvkm_disp *disp)
{
	NV2080_CTRL_INTERNAL_DISPLAY_GET_STATIC_INFO_PARAMS *ctrl;
	struct nvkm_gsp *gsp = disp->engine.subdev.device->gsp;

	ctrl = nvkm_gsp_rm_ctrl_rd(&gsp->internal.device.subdevice,
				   NV2080_CTRL_CMD_INTERNAL_DISPLAY_GET_STATIC_INFO,
				   sizeof(*ctrl));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	disp->wndw.mask = ctrl->windowPresentMask;
	disp->wndw.nr = fls(disp->wndw.mask);

	nvkm_gsp_rm_ctrl_done(&gsp->internal.device.subdevice, ctrl);

	/* Create the IMP object here to avoid RM object allocation during normal
	 * atomic checks. The query path retries if allocation fails or fini frees
	 * the object, as happens across suspend/resume.
	 */
	if (nvkm_gsp_rm_alloc(&disp->rm.device.object, NVKM_RM_DISP_SW,
			      NVC372_DISPLAY_SW, 0, &disp->rm.c372))
		memset(&disp->rm.c372, 0, sizeof(disp->rm.c372));

	return 0;
}

const struct nvkm_rm_api_disp
r570_disp = {
	.get_static_info = r570_disp_get_static_info,
	.imp_check = r570_disp_imp_check,
	.get_supported = r570_disp_get_supported,
	.get_connect_state = r570_disp_get_connect_state,
	.get_active = r570_disp_get_active,
	.bl_ctrl = r570_bl_ctrl,
	.dp = {
		.get_caps = r570_dp_get_caps,
		.set_indexed_link_rates = r570_dp_set_indexed_link_rates,
		.sst = r570_dp_sst,
		.vcpi = r570_dp_vcpi,
	},
	.chan = {
		.set_pushbuf = r570_disp_chan_set_pushbuf,
		.dmac_alloc = r570_dmac_alloc,
	},
};

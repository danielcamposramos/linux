/*
 * Copyright 2011 Red Hat Inc.
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
 *
 * Authors: Ben Skeggs
 */
#include "disp.h"
#include "atom.h"
#include "core.h"
#include "head.h"
#include "wndw.h"
#include "handles.h"

#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/string_choices.h>
#include <linux/dma-mapping.h>
#include <linux/hdmi.h>
#include <linux/component.h>
#include <linux/iopoll.h>
#include <linux/math64.h>

#include <drm/display/drm_dp_helper.h>
#include <drm/display/drm_hdmi_helper.h>
#include <drm/display/drm_scdc_helper.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_edid.h>
#include <drm/drm_eld.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fixed.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <nvif/push507c.h>

#include <nvif/class.h>
#include <nvif/cl0002.h>
#include <nvif/event.h>
#include <nvif/if0010.h>
#include <nvif/if0012.h>
#include <nvif/if0014.h>
#include <nvif/timer.h>

#include <nvhw/class/cl507c.h>
#include <nvhw/class/cl507d.h>
#include <nvhw/class/cl837d.h>
#include <nvhw/class/cl887d.h>
#include <nvhw/class/cl907d.h>
#include <nvhw/class/cl917d.h>

#include "nouveau_drv.h"
#include "nouveau_dma.h"
#include "nouveau_gem.h"
#include "nouveau_connector.h"
#include "nouveau_encoder.h"
#include "nouveau_fence.h"
#include "nv50_display.h"

/******************************************************************************
 * EVO channel
 *****************************************************************************/

static int
nv50_chan_create(struct nvif_device *device, struct nvif_object *disp,
		 const s32 *oclass, u8 head, void *data, u32 size,
		 struct nv50_chan *chan)
{
	struct nvif_sclass *sclass;
	int ret, i, n;

	chan->device = device;

	ret = n = nvif_object_sclass_get(disp, &sclass);
	if (ret < 0)
		return ret;

	while (oclass[0]) {
		for (i = 0; i < n; i++) {
			if (sclass[i].oclass == oclass[0]) {
				ret = nvif_object_ctor(disp, "kmsChan", 0,
						       oclass[0], data, size,
						       &chan->user);
				if (ret == 0) {
					ret = nvif_object_map(&chan->user, NULL, 0);
					if (ret)
						nvif_object_dtor(&chan->user);
				}
				nvif_object_sclass_put(&sclass);
				return ret;
			}
		}
		oclass++;
	}

	nvif_object_sclass_put(&sclass);
	return -ENOSYS;
}

static void
nv50_chan_destroy(struct nv50_chan *chan)
{
	nvif_object_dtor(&chan->user);
}

/******************************************************************************
 * DMA EVO channel
 *****************************************************************************/

void
nv50_dmac_destroy(struct nv50_dmac *dmac)
{
	nvif_object_dtor(&dmac->vram);
	nvif_object_dtor(&dmac->sync);

	nv50_chan_destroy(&dmac->base);

	nvif_mem_dtor(&dmac->push.mem);
}

static void
nv50_dmac_kick(struct nvif_push *push)
{
	struct nv50_dmac *dmac = container_of(push, typeof(*dmac), push);

	dmac->cur = push->cur - (u32 __iomem *)dmac->push.mem.object.map.ptr;
	if (dmac->put != dmac->cur) {
		/* Push buffer fetches are not coherent with BAR1, we need to ensure
		 * writes have been flushed right through to VRAM before writing PUT.
		 */
		if (dmac->push.mem.type & NVIF_MEM_VRAM) {
			struct nvif_device *device = dmac->base.device;
			nvif_wr32(&device->object, 0x070000, 0x00000001);
			nvif_msec(device, 2000,
				if (!(nvif_rd32(&device->object, 0x070000) & 0x00000002))
					break;
			);
		}

		NVIF_WV32(&dmac->base.user, NV507C, PUT, PTR, dmac->cur);
		dmac->put = dmac->cur;
	}

	push->bgn = push->cur;
}

static int
nv50_dmac_free(struct nv50_dmac *dmac)
{
	u32 get = NVIF_RV32(&dmac->base.user, NV507C, GET, PTR);
	if (get > dmac->cur) /* NVIDIA stay 5 away from GET, do the same. */
		return get - dmac->cur - 5;
	return dmac->max - dmac->cur;
}

static int
nv50_dmac_wind(struct nv50_dmac *dmac)
{
	/* Wait for GET to depart from the beginning of the push buffer to
	 * prevent writing PUT == GET, which would be ignored by HW.
	 */
	u32 get = NVIF_RV32(&dmac->base.user, NV507C, GET, PTR);
	if (get == 0) {
		/* Corner-case, HW idle, but non-committed work pending. */
		if (dmac->put == 0)
			nv50_dmac_kick(&dmac->push);

		if (nvif_msec(dmac->base.device, 2000,
			if (NVIF_TV32(&dmac->base.user, NV507C, GET, PTR, >, 0))
				break;
		) < 0)
			return -ETIMEDOUT;
	}

	PUSH_RSVD(&dmac->push, PUSH_JUMP(&dmac->push, 0));
	dmac->cur = 0;
	return 0;
}

static int
nv50_dmac_wait(struct nvif_push *push, u32 size)
{
	struct nv50_dmac *dmac = container_of(push, typeof(*dmac), push);
	int free;

	if (WARN_ON(size > dmac->max))
		return -EINVAL;

	dmac->cur = push->cur - (u32 __iomem *)dmac->push.mem.object.map.ptr;
	if (dmac->cur + size >= dmac->max) {
		int ret = nv50_dmac_wind(dmac);
		if (ret)
			return ret;

		push->cur = dmac->push.mem.object.map.ptr;
		push->cur = push->cur + dmac->cur;
		nv50_dmac_kick(push);
	}

	if (nvif_msec(dmac->base.device, 2000,
		if ((free = nv50_dmac_free(dmac)) >= size)
			break;
	) < 0) {
		WARN_ON(1);
		return -ETIMEDOUT;
	}

	push->bgn = dmac->push.mem.object.map.ptr;
	push->bgn = push->bgn + dmac->cur;
	push->cur = push->bgn;
	push->end = push->cur + free;
	return 0;
}

MODULE_PARM_DESC(kms_vram_pushbuf, "Place EVO/NVD push buffers in VRAM (default: auto)");
static int nv50_dmac_vram_pushbuf = -1;
module_param_named(kms_vram_pushbuf, nv50_dmac_vram_pushbuf, int, 0400);

MODULE_PARM_DESC(imp_check, "Validate display configurations through GSP's IMP "
		 "(1 = enforce (default), 0 = skip, like OpenRM's "
		 "NVKMS_MODE_VALIDATION_NO_EXTENDED_GPU_CAPABILITIES_CHECK. "
		 "Ignored where the IMP reply configures the hardware, e.g. GB20x+)");
static int nouveau_imp_check = 1;
module_param_named(imp_check, nouveau_imp_check, int, 0400);

int
nv50_dmac_create(struct nouveau_drm *drm,
		 const s32 *oclass, u8 head, void *data, u32 size, s64 syncbuf,
		 struct nv50_dmac *dmac)
{
	struct nvif_device *device = &drm->device;
	struct nvif_object *disp = &drm->display->disp.object;
	struct nvif_disp_chan_v0 *args = data;
	u8 type = NVIF_MEM_COHERENT;
	int ret;

	/* Pascal added support for 47-bit physical addresses, but some
	 * parts of EVO still only accept 40-bit PAs.
	 *
	 * To avoid issues on systems with large amounts of RAM, and on
	 * systems where an IOMMU maps pages at a high address, we need
	 * to allocate push buffers in VRAM instead.
	 *
	 * This appears to match NVIDIA's behaviour on Pascal.
	 */
	if ((nv50_dmac_vram_pushbuf > 0) ||
	    (nv50_dmac_vram_pushbuf < 0 && device->info.family == NV_DEVICE_INFO_V0_PASCAL))
		type |= NVIF_MEM_VRAM;

	ret = nvif_mem_ctor_map(&drm->mmu, "kmsChanPush", type, 0x1000, &dmac->push.mem);
	if (ret)
		return ret;

	dmac->push.wait = nv50_dmac_wait;
	dmac->push.kick = nv50_dmac_kick;
	dmac->push.bgn = dmac->push.mem.object.map.ptr;
	dmac->push.cur = dmac->push.bgn;
	dmac->push.end = dmac->push.bgn;
	dmac->max = 0x1000/4 - 1;

	/* EVO channels are affected by a HW bug where the last 12 DWORDs
	 * of the push buffer aren't able to be used safely.
	 */
	if (disp->oclass < GV100_DISP)
		dmac->max -= 12;

	args->pushbuf = nvif_handle(&dmac->push.mem.object);

	ret = nv50_chan_create(device, disp, oclass, head, data, size,
			       &dmac->base);
	if (ret)
		return ret;

	if (syncbuf < 0)
		return 0;

	/* No CTXDMAs on Blackwell. */
	if (disp->oclass >= GB202_DISP) {
		/* "handle != NULL_HANDLE" is used to determine enable status
		 * in a number of places, so fill in some fake object handles.
		 */
		dmac->sync.handle = NV50_DISP_HANDLE_SYNCBUF;
		dmac->vram.handle = NV50_DISP_HANDLE_VRAM;
		return 0;
	}

	ret = nvif_object_ctor(&dmac->base.user, "kmsSyncCtxDma", NV50_DISP_HANDLE_SYNCBUF,
			       NV_DMA_IN_MEMORY,
			       &(struct nv_dma_v0) {
					.target = NV_DMA_V0_TARGET_VRAM,
					.access = NV_DMA_V0_ACCESS_RDWR,
					.start = syncbuf + 0x0000,
					.limit = syncbuf + 0x0fff,
			       }, sizeof(struct nv_dma_v0),
			       &dmac->sync);
	if (ret)
		return ret;

	ret = nvif_object_ctor(&dmac->base.user, "kmsVramCtxDma", NV50_DISP_HANDLE_VRAM,
			       NV_DMA_IN_MEMORY,
			       &(struct nv_dma_v0) {
					.target = NV_DMA_V0_TARGET_VRAM,
					.access = NV_DMA_V0_ACCESS_RDWR,
					.start = 0,
					.limit = device->info.ram_user - 1,
			       }, sizeof(struct nv_dma_v0),
			       &dmac->vram);
	if (ret)
		return ret;

	return ret;
}

/* Mode checks and client restore commits can run from the poll worker. Since
 * runtime suspend waits for that worker, resuming here would deadlock. The
 * device is already runtime active during polling, so take the reference
 * without resuming.
 */
static int
nv50_disp_pm_get(struct drm_device *dev)
{
	int ret;

	if (drm_kms_helper_is_poll_worker()) {
		pm_runtime_get_noresume(dev->dev);
		return 0;
	}

	ret = pm_runtime_get_sync(dev->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_autosuspend(dev->dev);
		return ret;
	}
	return 0;
}

/******************************************************************************
 * Output path helpers
 *****************************************************************************/
static void
nv50_outp_dump_caps(struct nouveau_drm *drm,
		    struct nouveau_encoder *outp)
{
	NV_DEBUG(drm, "%s caps: dp_interlace=%d\n",
		 outp->base.base.name, outp->caps.dp_interlace);
}

static int
nv50_outp_atomic_check_view(struct drm_encoder *encoder,
			    struct drm_crtc_state *crtc_state,
			    struct drm_connector_state *conn_state,
			    struct drm_display_mode *native_mode)
{
	struct drm_display_mode *adjusted_mode = &crtc_state->adjusted_mode;
	struct drm_display_mode *mode = &crtc_state->mode;
	struct drm_connector *connector = conn_state->connector;
	struct nouveau_conn_atom *asyc = nouveau_conn_atom(conn_state);
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);

	NV_ATOMIC(drm, "%s atomic_check\n", encoder->name);
	asyc->scaler.full = false;
	if (!native_mode)
		return 0;

	if (asyc->scaler.mode == DRM_MODE_SCALE_NONE) {
		switch (connector->connector_type) {
		case DRM_MODE_CONNECTOR_LVDS:
		case DRM_MODE_CONNECTOR_eDP:
			/* Don't force scaler for EDID modes with
			 * same size as the native one (e.g. different
			 * refresh rate)
			 */
			if (mode->hdisplay == native_mode->hdisplay &&
			    mode->vdisplay == native_mode->vdisplay &&
			    mode->type & DRM_MODE_TYPE_DRIVER)
				break;
			mode = native_mode;
			asyc->scaler.full = true;
			break;
		default:
			break;
		}
	} else {
		mode = native_mode;
	}

	if (!drm_mode_equal(adjusted_mode, mode)) {
		drm_mode_copy(adjusted_mode, mode);
		crtc_state->mode_changed = true;
	}

	return 0;
}

/* Frame-packed 3D doubles the pixel clock, so TMDS calculations need
 * the raster timings programmed by the head.
 */
static void
nv50_hdmi_hw_mode(const struct drm_display_mode *mode,
		  struct drm_display_mode *hw)
{
	*hw = *mode;
	drm_mode_set_crtcinfo(hw, CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);
}

/* Choose the highest depth up to bpc supported by both ends. Enable
 * 16 and 12 bpc on Turing+ and 10 bpc on Ampere+. DVI remains at 8 bpc.
 * 16 bpc (48 bpp, HDMI DC_48) is untested: no 48-bit sink at hand.
 */
static u8
nv50_hdmi_fix_bpc(struct nv50_disp *disp, const struct drm_display_info *info,
		  u8 bpc)
{
	const u32 oclass = disp->disp->object.oclass;

	if (!info->is_hdmi)
		return 8;
	if (bpc >= 16 && oclass >= TU102_DISP &&
	    (info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_48))
		return 16;
	if (bpc >= 12 && oclass >= TU102_DISP &&
	    (info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_36))
		return 12;
	if (bpc >= 10 && oclass >= GA102_DISP &&
	    (info->edid_hdmi_rgb444_dc_modes & DRM_EDID_HDMI_DC_30))
		return 10;
	return 8;
}

/* Return the next supported depth, or zero if none is lower. */
static u8
nv50_hdmi_lower_bpc(struct nv50_disp *disp, const struct drm_display_info *info,
		    u8 bpc)
{
	return bpc > 8 ? nv50_hdmi_fix_bpc(disp, info, bpc - 2) : 0;
}

static int
nv50_outp_atomic_fix_depth(struct drm_encoder *encoder, struct drm_crtc_state *crtc_state,
			   struct drm_connector *connector)
{
	struct nv50_head_atom *asyh = nv50_head_atom(crtc_state);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nv50_disp *disp = nv50_disp(encoder->dev);
	struct drm_display_mode *mode = &asyh->state.adjusted_mode;
	const struct drm_display_info *info = &connector->display_info;
	unsigned int max_rate, mode_rate;
	u8 min_bpc;
	const struct drm_edid *edid = nouveau_connector(connector)->drm_edid;

	switch (nv_encoder->dcb->type) {
	case DCB_OUTPUT_DP:
		max_rate = nv_encoder->dp.link_nr * nv_encoder->dp.link_bw;
		/* PIOR training uses 6 bpc regardless of output depth. */
		if (nv_encoder->dcb->location != DCB_LOC_ON_CHIP)
			min_bpc = 6;
		else
			min_bpc = nouveau_dp_min_bpc(drm_edid_raw(edid));

		/* DP depth selection is limited to 10 bpc. */
		asyh->or.bpc = clamp_t(u8, asyh->or.bpc, min_bpc, 10);

		/* Try lower depths to fit the link bandwidth. */
		while (asyh->or.bpc > min_bpc) {
			mode_rate = DIV_ROUND_UP(mode->clock * asyh->or.bpc * 3, 8);
			if (mode_rate <= max_rate)
				break;

			asyh->or.bpc -= 2;
		}

		/* Even minimum bpc may exceed the link budget. */
		mode_rate = DIV_ROUND_UP(mode->clock * asyh->or.bpc * 3, 8);
		if (mode_rate > max_rate)
			return -EINVAL;
		break;
	case DCB_OUTPUT_TMDS:
		/* The sink's deep color capability does not guarantee that the
		 * selected mode fits the TMDS link.
		 */
		asyh->or.bpc = nv50_hdmi_fix_bpc(disp, info, asyh->or.bpc);
		if (asyh->or.bpc > 8) {
			const unsigned int tmds_max =
				nouveau_connector_tmds_link_bandwidth(connector);
			struct drm_display_mode hw;

			nv50_hdmi_hw_mode(mode, &hw);
			while (asyh->or.bpc > 8 &&
			       !nouveau_hdmi_tmds_possible(tmds_max, hw.crtc_clock,
							   asyh->or.bpc))
				asyh->or.bpc = nv50_hdmi_lower_bpc(disp, info,
								   asyh->or.bpc);
		}
		break;
	default:
		break;
	}

	return 0;
}

static int
nv50_outp_atomic_check(struct drm_encoder *encoder,
		       struct drm_crtc_state *crtc_state,
		       struct drm_connector_state *conn_state)
{
	struct drm_connector *connector = conn_state->connector;
	struct nouveau_connector *nv_connector = nouveau_connector(connector);
	struct nv50_head_atom *asyh = nv50_head_atom(crtc_state);
	int ret;

	ret = nv50_outp_atomic_check_view(encoder, crtc_state, conn_state,
					  nv_connector->native_mode);
	if (ret)
		return ret;

	if (crtc_state->mode_changed || crtc_state->connectors_changed) {
		asyh->or.bpc = connector->display_info.bpc;
		/* The property accepts odd values, but wire depths are even.
		 * Round the cap down so the fallback encoding cannot exceed
		 * the depth used for bandwidth validation.
		 */
		if (conn_state->max_requested_bpc)
			asyh->or.bpc = min_t(u8, asyh->or.bpc,
					     conn_state->max_requested_bpc & ~1);
	}

	/* Check the selected depth against the link limits. */
	ret = nv50_outp_atomic_fix_depth(encoder, crtc_state, connector);
	if (ret)
		return ret;

	return 0;
}

struct nouveau_connector *
nv50_outp_get_new_connector(struct drm_atomic_commit *state, struct nouveau_encoder *outp)
{
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	struct drm_encoder *encoder = to_drm_encoder(outp);
	int i;

	for_each_new_connector_in_state(state, connector, connector_state, i) {
		if (connector_state->best_encoder == encoder)
			return nouveau_connector(connector);
	}

	return NULL;
}

struct nouveau_connector *
nv50_outp_get_old_connector(struct drm_atomic_commit *state, struct nouveau_encoder *outp)
{
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	struct drm_encoder *encoder = to_drm_encoder(outp);
	int i;

	for_each_old_connector_in_state(state, connector, connector_state, i) {
		if (connector_state->best_encoder == encoder)
			return nouveau_connector(connector);
	}

	return NULL;
}

static struct nouveau_crtc *
nv50_outp_get_new_crtc(const struct drm_atomic_commit *state, const struct nouveau_encoder *outp)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	const u32 mask = drm_encoder_mask(&outp->base.base);
	int i;

	for_each_new_crtc_in_state(state, crtc, crtc_state, i) {
		if (crtc_state->encoder_mask & mask)
			return nouveau_crtc(crtc);
	}

	return NULL;
}

/******************************************************************************
 * DAC
 *****************************************************************************/
static void
nv50_dac_atomic_disable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nv50_core *core = nv50_disp(encoder->dev)->core;
	const u32 ctrl = NVDEF(NV507D, DAC_SET_CONTROL, OWNER, NONE);

	core->func->dac->ctrl(core, nv_encoder->outp.or.id, ctrl, NULL);
	nv_encoder->crtc = NULL;
}

static void
nv50_dac_atomic_enable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_crtc *nv_crtc = nv50_outp_get_new_crtc(state, nv_encoder);
	struct nv50_head_atom *asyh =
		nv50_head_atom(drm_atomic_get_new_crtc_state(state, &nv_crtc->base));
	struct nv50_core *core = nv50_disp(encoder->dev)->core;
	u32 ctrl = 0;

	switch (nv_crtc->index) {
	case 0: ctrl |= NVDEF(NV507D, DAC_SET_CONTROL, OWNER, HEAD0); break;
	case 1: ctrl |= NVDEF(NV507D, DAC_SET_CONTROL, OWNER, HEAD1); break;
	case 2: ctrl |= NVDEF(NV907D, DAC_SET_CONTROL, OWNER_MASK, HEAD2); break;
	case 3: ctrl |= NVDEF(NV907D, DAC_SET_CONTROL, OWNER_MASK, HEAD3); break;
	default:
		WARN_ON(1);
		break;
	}

	ctrl |= NVDEF(NV507D, DAC_SET_CONTROL, PROTOCOL, RGB_CRT);

	if (!nvif_outp_acquired(&nv_encoder->outp))
		nvif_outp_acquire_dac(&nv_encoder->outp);

	core->func->dac->ctrl(core, nv_encoder->outp.or.id, ctrl, asyh);
	asyh->or.depth = 0;

	nv_encoder->crtc = &nv_crtc->base;
}

static enum drm_connector_status
nv50_dac_detect(struct drm_encoder *encoder, struct drm_connector *connector)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	u32 loadval;
	int ret;

	loadval = nouveau_drm(encoder->dev)->vbios.dactestval;
	if (loadval == 0)
		loadval = 340;

	ret = nvif_outp_load_detect(&nv_encoder->outp, loadval);
	if (ret <= 0)
		return connector_status_disconnected;

	return connector_status_connected;
}

static const struct drm_encoder_helper_funcs
nv50_dac_help = {
	.atomic_check = nv50_outp_atomic_check,
	.atomic_enable = nv50_dac_atomic_enable,
	.atomic_disable = nv50_dac_atomic_disable,
	.detect = nv50_dac_detect
};

static void
nv50_dac_destroy(struct drm_encoder *encoder)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);

	nvif_outp_dtor(&nv_encoder->outp);

	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static const struct drm_encoder_funcs
nv50_dac_func = {
	.destroy = nv50_dac_destroy,
};

static int
nv50_dac_create(struct nouveau_encoder *nv_encoder)
{
	struct drm_connector *connector = &nv_encoder->conn->base;
	struct nouveau_drm *drm = nouveau_drm(connector->dev);
	struct nvkm_i2c *i2c = nvxx_i2c(drm);
	struct nvkm_i2c_bus *bus;
	struct drm_encoder *encoder;
	struct dcb_output *dcbe = nv_encoder->dcb;
	int type = DRM_MODE_ENCODER_DAC;

	bus = nvkm_i2c_bus_find(i2c, dcbe->i2c_index);
	if (bus)
		nv_encoder->i2c = &bus->i2c;

	encoder = to_drm_encoder(nv_encoder);
	drm_encoder_init(connector->dev, encoder, &nv50_dac_func, type,
			 "dac-%04x-%04x", dcbe->hasht, dcbe->hashm);
	drm_encoder_helper_add(encoder, &nv50_dac_help);

	drm_connector_attach_encoder(connector, encoder);
	return 0;
}

/*
 * audio component binding for ELD notification
 */
static void
nv50_audio_component_eld_notify(struct drm_audio_component *acomp, int port,
				int dev_id)
{
	if (acomp && acomp->audio_ops && acomp->audio_ops->pin_eld_notify)
		acomp->audio_ops->pin_eld_notify(acomp->audio_ops->audio_ptr,
						 port, dev_id);
}

static int
nv50_audio_component_get_eld(struct device *kdev, int port, int dev_id,
			     bool *enabled, unsigned char *buf, int max_bytes)
{
	struct nouveau_drm *drm = dev_get_drvdata(kdev);
	struct drm_encoder *encoder;
	struct nouveau_encoder *nv_encoder;
	struct nouveau_crtc *nv_crtc;
	int ret = 0;

	*enabled = false;

	mutex_lock(&drm->audio.lock);

	drm_for_each_encoder(encoder, drm->dev) {
		struct nouveau_connector *nv_connector = NULL;

		if (encoder->encoder_type == DRM_MODE_ENCODER_DPMST)
			continue; /* TODO */

		nv_encoder = nouveau_encoder(encoder);
		nv_connector = nv_encoder->conn;
		nv_crtc = nouveau_crtc(nv_encoder->crtc);

		if (!nv_crtc || nv_encoder->outp.or.id != port || nv_crtc->index != dev_id)
			continue;

		*enabled = nv_encoder->audio.enabled;
		if (*enabled) {
			ret = drm_eld_size(nv_connector->base.eld);
			memcpy(buf, nv_connector->base.eld,
			       min(max_bytes, ret));
		}
		break;
	}

	mutex_unlock(&drm->audio.lock);

	return ret;
}

static const struct drm_audio_component_ops nv50_audio_component_ops = {
	.get_eld = nv50_audio_component_get_eld,
};

static int
nv50_audio_component_bind(struct device *kdev, struct device *hda_kdev,
			  void *data)
{
	struct nouveau_drm *drm = dev_get_drvdata(kdev);
	struct drm_audio_component *acomp = data;

	if (WARN_ON(!device_link_add(hda_kdev, kdev, DL_FLAG_STATELESS)))
		return -ENOMEM;

	drm_modeset_lock_all(drm->dev);
	acomp->ops = &nv50_audio_component_ops;
	acomp->dev = kdev;
	drm->audio.component = acomp;
	drm_modeset_unlock_all(drm->dev);
	return 0;
}

static void
nv50_audio_component_unbind(struct device *kdev, struct device *hda_kdev,
			    void *data)
{
	struct nouveau_drm *drm = dev_get_drvdata(kdev);
	struct drm_audio_component *acomp = data;

	drm_modeset_lock_all(drm->dev);
	drm->audio.component = NULL;
	acomp->ops = NULL;
	acomp->dev = NULL;
	drm_modeset_unlock_all(drm->dev);
}

static const struct component_ops nv50_audio_component_bind_ops = {
	.bind   = nv50_audio_component_bind,
	.unbind = nv50_audio_component_unbind,
};

static void
nv50_audio_component_init(struct nouveau_drm *drm)
{
	if (component_add(drm->dev->dev, &nv50_audio_component_bind_ops))
		return;

	drm->audio.component_registered = true;
	mutex_init(&drm->audio.lock);
}

static void
nv50_audio_component_fini(struct nouveau_drm *drm)
{
	if (!drm->audio.component_registered)
		return;

	component_del(drm->dev->dev, &nv50_audio_component_bind_ops);
	drm->audio.component_registered = false;
	mutex_destroy(&drm->audio.lock);
}

/******************************************************************************
 * Audio
 *****************************************************************************/
static bool
nv50_audio_supported(struct drm_encoder *encoder)
{
	struct nv50_disp *disp = nv50_disp(encoder->dev);

	if (disp->disp->object.oclass <= GT200_DISP ||
	    disp->disp->object.oclass == GT206_DISP)
		return false;

	if (encoder->encoder_type != DRM_MODE_ENCODER_DPMST) {
		struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);

		switch (nv_encoder->dcb->type) {
		case DCB_OUTPUT_TMDS:
		case DCB_OUTPUT_DP:
			break;
		default:
			return false;
		}
	}

	return true;
}

static void
nv50_audio_disable(struct drm_encoder *encoder, struct nouveau_crtc *nv_crtc)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nvif_outp *outp = &nv_encoder->outp;

	if (!nv50_audio_supported(encoder))
		return;

	mutex_lock(&drm->audio.lock);
	if (nv_encoder->audio.enabled) {
		nv_encoder->audio.enabled = false;
		nvif_outp_hda_eld(&nv_encoder->outp, nv_crtc->index, NULL, 0);
	}
	mutex_unlock(&drm->audio.lock);

	nv50_audio_component_eld_notify(drm->audio.component, outp->or.id, nv_crtc->index);
}

static void
nv50_audio_enable(struct drm_encoder *encoder, struct nouveau_crtc *nv_crtc,
		  struct nouveau_connector *nv_connector, struct drm_atomic_commit *state,
		  struct drm_display_mode *mode)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nvif_outp *outp = &nv_encoder->outp;

	if (!nv50_audio_supported(encoder) || !nv_connector->base.display_info.has_audio)
		return;

	mutex_lock(&drm->audio.lock);

	nvif_outp_hda_eld(&nv_encoder->outp, nv_crtc->index, nv_connector->base.eld,
			  drm_eld_size(nv_connector->base.eld));
	nv_encoder->audio.enabled = true;

	mutex_unlock(&drm->audio.lock);

	nv50_audio_component_eld_notify(drm->audio.component, outp->or.id, nv_crtc->index);
}

/******************************************************************************
 * IMP
 *****************************************************************************/

/* Probe modes with one head so existing displays do not constrain the
 * advertised mode list. Atomic check validates the combined configuration.
 */
enum drm_mode_status
nv50_imp_mode_valid(struct drm_connector *connector,
		    const struct drm_display_mode *mode)
{
	struct drm_device *dev = connector->dev;
	struct nv50_disp *disp = nv50_disp(dev);
	struct nvif_disp_imp_check_v0 imp = {};
	struct nvif_disp_imp_check_head_v0 *imph = &imp.head[0];
	struct drm_display_mode adjusted;
	struct nv50_head_mode m = {};
	int probe_head, ret;

	/* Reject interlaced modes even when IMP is bypassed, matching the
	 * atomic check.
	 */
	if (mode->flags & DRM_MODE_FLAG_INTERLACE)
		return MODE_NO_INTERLACE;

	/* Tiled modesets depend on IMP for allocation, so keep their probe
	 * checks enabled even with imp_check=0.
	 */
	if (!nouveau_imp_check && !disp->tile.nr_tiles)
		return MODE_OK;

	/* Firmware without the IMP API. */
	if (disp->imp_unsupported)
		return MODE_OK;

	ret = nv50_disp_pm_get(dev);
	if (ret)
		goto fail;

	/* Convert the mode to the raster atomic check would program. */
	adjusted = *mode;
	nv50_head_mode_from_drm(&adjusted, &m);

	/* Use the first present head rather than assuming head 0 exists. */
	probe_head = disp->disp->head_mask ? ffs(disp->disp->head_mask) - 1 : 0;

	imp.num_heads = 1;
	imp.tiled = disp->tile.nr_tiles > 0;
	imph->index = probe_head;
	/* Probe the unscaled viewport with two filter taps. */
	imph->vtaps = 2;
	/* A zero mask lets IMP choose tiles for this head in isolation. Atomic
	 * check handles contention with other heads.
	 */
	imph->tile_mask = 0;
	imph->pclk_khz = m.clock;
	imph->htotal = m.h.active;
	imph->vtotal = m.v.active;
	imph->hblanks = m.h.blanks;
	imph->hblanke = m.h.blanke;
	imph->vblanks = m.v.blanks;
	imph->vblanke = m.v.blanke;
	/* Use the converted viewport so stereo frame packing matches the
	 * raster sent to IMP.
	 */
	imph->in_w = adjusted.crtc_hdisplay;
	imph->in_h = adjusted.crtc_vdisplay;
	imph->out_w = adjusted.crtc_hdisplay;
	imph->out_h = adjusted.crtc_vdisplay;

	/* Probe with only primary RGB formats up to 32 bpp so modes that
	 * require compositor fallback remain available. The modeset check will
	 * account for other windows and active heads.
	 */
	imph->wndw_formats[0] = NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP |
				NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP |
				NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP;

	ret = nvif_disp_imp_check(disp->disp, &imp);
	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);
	if (ret == -ENODEV) {
		disp->imp_unsupported = true;
		return MODE_OK;
	}
	if (ret)
		goto fail;

	return imp.possible ? MODE_OK : MODE_BAD;

fail:
	/* An unexpected query or runtime PM error must not admit an unchecked
	 * mode.
	 */
	drm_warn_once(dev, "IMP mode check failed (%d), rejecting\n", ret);
	return MODE_ERROR;
}

/******************************************************************************
 * HDMI
 *****************************************************************************/

static void
nv50_hdmi_enable(struct drm_encoder *encoder, struct nouveau_crtc *nv_crtc,
		 struct nouveau_connector *nv_connector, struct drm_atomic_commit *state,
		 struct drm_display_mode *mode, bool hda)
{
	struct nouveau_drm *drm = nouveau_drm(encoder->dev);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct drm_hdmi_info *hdmi = &nv_connector->base.display_info.hdmi;
	const struct nv50_head_atom *asyh =
		nv50_head_atom(drm_atomic_get_new_crtc_state(state, &nv_crtc->base));
	union hdmi_infoframe infoframe = { 0 };
	const u8 rekey = 56; /* binary driver, and tegra, constant */
	struct drm_display_mode hw;
	u32 max_ac_packet, tmds_clock;
	u8 gcp_sb1 = 0;
	DEFINE_RAW_FLEX(struct nvif_outp_infoframe_v0, args, data, 17);
	const u8 data_len = __member_size(args->data);
	int ret, size;

	/* The TMDS character rate is pixel clock * bpc / 8. Use that rate
	 * for SCDC clock-ratio/scrambling setup and the firmware clock.
	 */
	nv50_hdmi_hw_mode(mode, &hw);
	tmds_clock = hw.crtc_clock * asyh->or.bpc / 8;

	/* At 12 bpc, GCP SB1 carries CD=6 and a packing phase selected by the
	 * parity of the back porch plus active width. At 10 bpc it carries
	 * CD=5 and the phase within the four-pixel group (HDMI 1.4b 6.5.3).
	 */
	/* At 16 bpc, CD=7; a 48-bpp group holds one pixel, so the packing
	 * phase stays 0.
	 */
	if (asyh->or.bpc == 16)
		gcp_sb1 = 0x07;
	else if (asyh->or.bpc == 12)
		gcp_sb1 = 0x06 | ((hw.crtc_htotal - hw.crtc_hsync_end +
				   hw.crtc_hdisplay) & 1 ? 0x10 : 0x20);
	else if (asyh->or.bpc == 10)
		gcp_sb1 = 0x05 | (((hw.crtc_htotal - hw.crtc_hsync_end +
				    hw.crtc_hdisplay) & 3) << 4);

	max_ac_packet  = mode->htotal - mode->hdisplay;
	max_ac_packet -= rekey;
	max_ac_packet -= 18; /* constant from tegra */
	max_ac_packet /= 32;

	if (nv_encoder->i2c && hdmi->scdc.scrambling.supported) {
		const bool high_tmds_clock_ratio = tmds_clock > 340000;
		u8 scdc;

		ret = drm_scdc_readb(nv_encoder->i2c, SCDC_TMDS_CONFIG, &scdc);
		if (ret < 0) {
			NV_ERROR(drm, "Failure to read SCDC_TMDS_CONFIG: %d\n", ret);
			return;
		}

		scdc &= ~(SCDC_TMDS_BIT_CLOCK_RATIO_BY_40 | SCDC_SCRAMBLING_ENABLE);
		if (high_tmds_clock_ratio || hdmi->scdc.scrambling.low_rates)
			scdc |= SCDC_SCRAMBLING_ENABLE;
		if (high_tmds_clock_ratio)
			scdc |= SCDC_TMDS_BIT_CLOCK_RATIO_BY_40;

		ret = drm_scdc_writeb(nv_encoder->i2c, SCDC_TMDS_CONFIG, scdc);
		if (ret < 0)
			NV_ERROR(drm, "Failure to write SCDC_TMDS_CONFIG = 0x%02x: %d\n",
				 scdc, ret);
	}

	ret = nvif_outp_hdmi(&nv_encoder->outp, nv_crtc->index, true, max_ac_packet, rekey,
			     tmds_clock, hdmi->scdc.supported, hdmi->scdc.scrambling.supported,
			     hdmi->scdc.scrambling.low_rates, gcp_sb1);
	if (ret)
		return;

	/* AVI InfoFrame. */
	args->version = 0;
	args->head = nv_crtc->index;

	if (!drm_hdmi_avi_infoframe_from_display_mode(&infoframe.avi, &nv_connector->base, mode)) {
		const struct drm_connector_state *conn_state =
			drm_atomic_get_new_connector_state(state, &nv_connector->base);

		/* the range the head sends ("Broadcast RGB") */
		drm_hdmi_avi_infoframe_quant_range(&infoframe.avi, &nv_connector->base, mode,
						   asyh->procamp.limited ?
						   HDMI_QUANTIZATION_RANGE_LIMITED :
						   HDMI_QUANTIZATION_RANGE_FULL);
		if (conn_state)
			drm_hdmi_avi_infoframe_content_type(&infoframe.avi, conn_state);

		size = hdmi_infoframe_pack(&infoframe, args->data, data_len);
	} else {
		size = 0;
	}

	nvif_outp_infoframe(&nv_encoder->outp, NVIF_OUTP_INFOFRAME_V0_AVI, args, size);

	/* Vendor InfoFrame. */
	memset(args->data, 0, data_len);
	if (!drm_hdmi_vendor_infoframe_from_display_mode(&infoframe.vendor.hdmi,
							 &nv_connector->base, mode))
		size = hdmi_infoframe_pack(&infoframe, args->data, data_len);
	else
		size = 0;

	nvif_outp_infoframe(&nv_encoder->outp, NVIF_OUTP_INFOFRAME_V0_VSI, args, size);

	nv_encoder->hdmi.enabled = true;
}

/******************************************************************************
 * MST
 *****************************************************************************/
#define nv50_mstm(p) container_of((p), struct nv50_mstm, mgr)
#define nv50_mstc(p) container_of((p), struct nv50_mstc, connector)
#define nv50_msto(p) container_of((p), struct nv50_msto, encoder)

struct nv50_mstc {
	struct nv50_mstm *mstm;
	struct drm_dp_mst_port *port;
	struct drm_connector connector;

	struct drm_display_mode *native;
	struct edid *edid;
};

struct nv50_msto {
	struct drm_encoder encoder;

	/* head is statically assigned on msto creation */
	struct nv50_head *head;
	struct nv50_mstc *mstc;
	bool disabled;
	bool enabled;

	u32 display_id;
};

struct nouveau_encoder *nv50_real_outp(struct drm_encoder *encoder)
{
	struct nv50_msto *msto;

	if (encoder->encoder_type != DRM_MODE_ENCODER_DPMST)
		return nouveau_encoder(encoder);

	msto = nv50_msto(encoder);
	if (!msto->mstc)
		return NULL;
	return msto->mstc->mstm->outp;
}

static void
nv50_msto_cleanup(struct drm_atomic_commit *state,
		  struct drm_dp_mst_topology_state *new_mst_state,
		  struct drm_dp_mst_topology_mgr *mgr,
		  struct nv50_msto *msto)
{
	struct nouveau_drm *drm = nouveau_drm(msto->encoder.dev);
	struct drm_dp_mst_atomic_payload *new_payload =
		drm_atomic_get_mst_payload_state(new_mst_state, msto->mstc->port);
	struct drm_dp_mst_topology_state *old_mst_state =
		drm_atomic_get_old_mst_topology_state(state, mgr);
	const struct drm_dp_mst_atomic_payload *old_payload =
		drm_atomic_get_mst_payload_state(old_mst_state, msto->mstc->port);
	struct nv50_mstc *mstc = msto->mstc;
	struct nv50_mstm *mstm = mstc->mstm;

	NV_ATOMIC(drm, "%s: msto cleanup\n", msto->encoder.name);

	if (msto->disabled) {
		if (msto->head->func->display_id) {
			nvif_outp_dp_mst_id_put(&mstm->outp->outp, msto->display_id);
			msto->display_id = 0;
		}

		msto->mstc = NULL;
		msto->disabled = false;
		drm_dp_remove_payload_part2(mgr, new_mst_state, old_payload, new_payload);
	} else if (msto->enabled) {
		drm_dp_add_payload_part2(mgr, new_payload);
		msto->enabled = false;
	}
}

static void
nv50_msto_prepare(struct drm_atomic_commit *state,
		  struct drm_dp_mst_topology_state *mst_state,
		  struct drm_dp_mst_topology_mgr *mgr,
		  struct nv50_msto *msto)
{
	struct nouveau_drm *drm = nouveau_drm(msto->encoder.dev);
	struct nv50_mstc *mstc = msto->mstc;
	struct nv50_mstm *mstm = mstc->mstm;
	struct drm_dp_mst_atomic_payload *payload;
	int ret = 0;

	NV_ATOMIC(drm, "%s: msto prepare\n", msto->encoder.name);

	payload = drm_atomic_get_mst_payload_state(mst_state, mstc->port);

	if (msto->disabled) {
		drm_dp_remove_payload_part1(mgr, mst_state, payload);
		nvif_outp_dp_mst_vcpi(&mstm->outp->outp, msto->head->base.index, 0, 0, 0, 0);
		ret = 1;
	} else {
		if (msto->enabled)
			ret = drm_dp_add_payload_part1(mgr, mst_state, payload);
	}

	if (ret == 0) {
		nvif_outp_dp_mst_vcpi(&mstm->outp->outp, msto->head->base.index,
				      payload->vc_start_slot, payload->time_slots,
				      payload->pbn,
				      payload->time_slots * dfixed_trunc(mst_state->pbn_div));
	} else {
		nvif_outp_dp_mst_vcpi(&mstm->outp->outp, msto->head->base.index, 0, 0, 0, 0);
	}
}

static int
nv50_msto_atomic_check(struct drm_encoder *encoder,
		       struct drm_crtc_state *crtc_state,
		       struct drm_connector_state *conn_state)
{
	struct drm_atomic_commit *state = crtc_state->state;
	struct drm_connector *connector = conn_state->connector;
	struct drm_dp_mst_topology_state *mst_state;
	struct nv50_mstc *mstc = nv50_mstc(connector);
	struct nv50_mstm *mstm = mstc->mstm;
	struct nv50_head_atom *asyh = nv50_head_atom(crtc_state);
	int slots;
	int ret;

	ret = nv50_outp_atomic_check_view(encoder, crtc_state, conn_state,
					  mstc->native);
	if (ret)
		return ret;

	if (!drm_atomic_crtc_needs_modeset(crtc_state))
		return 0;

	/*
	 * When restoring duplicated states, we need to make sure that the bw
	 * remains the same and avoid recalculating it, as the connector's bpc
	 * may have changed after the state was duplicated
	 */
	if (!state->duplicated) {
		const int clock = crtc_state->adjusted_mode.clock;

		asyh->or.bpc = connector->display_info.bpc;
		/* Round down so PBN uses an encodable depth. */
		if (conn_state->max_requested_bpc)
			asyh->or.bpc = min_t(u8, asyh->or.bpc,
					     conn_state->max_requested_bpc & ~1);
		asyh->or.bpc = max(asyh->or.bpc, nouveau_dp_min_bpc(mstc->edid));
		asyh->dp.pbn = drm_dp_calc_pbn_mode(clock, asyh->or.bpc * 3 << 4);
	}

	mst_state = drm_atomic_get_mst_topology_state(state, &mstm->mgr);
	if (IS_ERR(mst_state))
		return PTR_ERR(mst_state);

	if (!mst_state->pbn_div.full) {
		struct nouveau_encoder *outp = mstc->mstm->outp;

		mst_state->pbn_div = drm_dp_get_vc_payload_bw(outp->dp.link_bw, outp->dp.link_nr);
	}

	slots = drm_dp_atomic_find_time_slots(state, &mstm->mgr, mstc->port, asyh->dp.pbn);
	if (slots < 0)
		return slots;

	asyh->dp.tu = slots;

	return 0;
}

static u8
nv50_dp_bpc_to_depth(unsigned int bpc)
{
	switch (bpc) {
	case  6: return NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_BPP_18_444;
	case  8: return NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_BPP_24_444;
	case 12: return NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_BPP_36_444;
	/* 837d's class header predates 48 bpp; the SOR encoding is shared */
	case 16: return NV887D_SOR_SET_CONTROL_PIXEL_DEPTH_BPP_48_444;
	case 10:
	default: return NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_BPP_30_444;
	}
}

static void
nv50_msto_atomic_enable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nv50_msto *msto = nv50_msto(encoder);
	struct nv50_head *head = msto->head;
	struct nv50_head_atom *asyh =
		nv50_head_atom(drm_atomic_get_new_crtc_state(state, &head->base.base));
	struct nv50_mstc *mstc = NULL;
	struct nv50_mstm *mstm = NULL;
	struct drm_connector *connector;
	struct drm_connector_list_iter conn_iter;
	u8 proto;

	drm_connector_list_iter_begin(encoder->dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) {
		if (connector->state->best_encoder == &msto->encoder) {
			mstc = nv50_mstc(connector);
			mstm = mstc->mstm;
			break;
		}
	}
	drm_connector_list_iter_end(&conn_iter);

	if (WARN_ON(!mstc))
		return;

	if (!mstm->links++) {
		nvif_outp_acquire_sor(&mstm->outp->outp, false /*TODO: MST audio... */);
		nouveau_dp_train(mstm->outp, true, 0, 0);
	}

	if (head->func->display_id) {
		if (!WARN_ON(nvif_outp_dp_mst_id_get(&mstm->outp->outp, &msto->display_id)))
			head->func->display_id(head, msto->display_id);
	}

	if (mstm->outp->outp.or.link & 1)
		proto = NV917D_SOR_SET_CONTROL_PROTOCOL_DP_A;
	else
		proto = NV917D_SOR_SET_CONTROL_PROTOCOL_DP_B;

	mstm->outp->update(mstm->outp, head->base.index, asyh, proto,
			   nv50_dp_bpc_to_depth(asyh->or.bpc));

	msto->mstc = mstc;
	msto->enabled = true;
	mstm->modified = true;
}

static void
nv50_msto_atomic_disable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nv50_msto *msto = nv50_msto(encoder);
	struct nv50_mstc *mstc = msto->mstc;
	struct nv50_mstm *mstm = mstc->mstm;

	if (msto->head->func->display_id)
		msto->head->func->display_id(msto->head, 0);

	mstm->outp->update(mstm->outp, msto->head->base.index, NULL, 0, 0);
	mstm->modified = true;
	if (!--mstm->links)
		mstm->disabled = true;
	msto->disabled = true;
}

static const struct drm_encoder_helper_funcs
nv50_msto_help = {
	.atomic_disable = nv50_msto_atomic_disable,
	.atomic_enable = nv50_msto_atomic_enable,
	.atomic_check = nv50_msto_atomic_check,
};

static void
nv50_msto_destroy(struct drm_encoder *encoder)
{
	struct nv50_msto *msto = nv50_msto(encoder);
	drm_encoder_cleanup(&msto->encoder);
	kfree(msto);
}

static const struct drm_encoder_funcs
nv50_msto = {
	.destroy = nv50_msto_destroy,
};

static struct nv50_msto *
nv50_msto_new(struct drm_device *dev, struct nv50_head *head, int id)
{
	struct nv50_msto *msto;
	int ret;

	msto = kzalloc_obj(*msto);
	if (!msto)
		return ERR_PTR(-ENOMEM);

	ret = drm_encoder_init(dev, &msto->encoder, &nv50_msto,
			       DRM_MODE_ENCODER_DPMST, "mst-%d", id);
	if (ret) {
		kfree(msto);
		return ERR_PTR(ret);
	}

	drm_encoder_helper_add(&msto->encoder, &nv50_msto_help);
	msto->encoder.possible_crtcs = drm_crtc_mask(&head->base.base);
	msto->head = head;
	return msto;
}

static struct drm_encoder *
nv50_mstc_atomic_best_encoder(struct drm_connector *connector,
			      struct drm_atomic_commit *state)
{
	struct drm_connector_state *connector_state = drm_atomic_get_new_connector_state(state,
											 connector);
	struct nv50_mstc *mstc = nv50_mstc(connector);
	struct drm_crtc *crtc = connector_state->crtc;

	if (!(mstc->mstm->outp->dcb->heads & drm_crtc_mask(crtc)))
		return NULL;

	return &nv50_head(crtc)->msto->encoder;
}

static enum drm_mode_status
nv50_mstc_mode_valid(struct drm_connector *connector,
		     const struct drm_display_mode *mode)
{
	struct nv50_mstc *mstc = nv50_mstc(connector);
	struct nouveau_encoder *outp = mstc->mstm->outp;
	enum drm_mode_status status;

	/* Reject modes exceeding the postcomp viewport limit where scaler
	 * limits are enforced. Atomic check applies the same limit.
	 */
	if (nouveau_display(connector->dev)->scaler_limits) {
		const u16 max = nouveau_display(connector->dev)->max_viewport;

		if (mode->hdisplay > max)
			return MODE_BAD_HVALUE;
		if (mode->vdisplay > max)
			return MODE_BAD_VVALUE;
	}

	/* TODO: calculate the PBN from the dotclock and validate against the
	 * MSTB's max possible PBN
	 */

	status = nv50_dp_mode_valid(outp, mode, nouveau_dp_min_bpc(mstc->edid), NULL);
	if (status != MODE_OK ||
	    !nouveau_display(connector->dev)->disp_imp)
		return status;

	return nv50_imp_mode_valid(connector, mode);
}

static int
nv50_mstc_get_modes(struct drm_connector *connector)
{
	struct nv50_mstc *mstc = nv50_mstc(connector);
	int ret = 0;

	mstc->edid = drm_dp_mst_get_edid(&mstc->connector, mstc->port->mgr, mstc->port);
	drm_connector_update_edid_property(&mstc->connector, mstc->edid);
	if (mstc->edid)
		ret = drm_add_edid_modes(&mstc->connector, mstc->edid);

	/*
	 * XXX: Since we don't use HDR in userspace quite yet, limit the bpc
	 * to 8 to save bandwidth on the topology. In the future, we'll want
	 * to properly fix this by dynamically selecting the highest possible
	 * bpc that would fit in the topology
	 */
	if (connector->display_info.bpc)
		connector->display_info.bpc =
			clamp(connector->display_info.bpc, 6U, 8U);
	else
		connector->display_info.bpc = 8;

	if (mstc->native)
		drm_mode_destroy(mstc->connector.dev, mstc->native);
	mstc->native = nouveau_conn_native_mode(&mstc->connector);
	return ret;
}

static int
nv50_mstc_atomic_check(struct drm_connector *connector,
		       struct drm_atomic_commit *state)
{
	struct nv50_mstc *mstc = nv50_mstc(connector);
	struct drm_dp_mst_topology_mgr *mgr = &mstc->mstm->mgr;

	return drm_dp_atomic_release_time_slots(state, mgr, mstc->port);
}

static int
nv50_mstc_detect(struct drm_connector *connector,
		 struct drm_modeset_acquire_ctx *ctx, bool force)
{
	struct nv50_mstc *mstc = nv50_mstc(connector);
	int ret;

	if (drm_connector_is_unregistered(connector))
		return connector_status_disconnected;

	ret = pm_runtime_get_sync(connector->dev->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_autosuspend(connector->dev->dev);
		return connector_status_disconnected;
	}

	ret = drm_dp_mst_detect_port(connector, ctx, mstc->port->mgr,
				     mstc->port);
	if (ret != connector_status_connected)
		goto out;

out:
	pm_runtime_mark_last_busy(connector->dev->dev);
	pm_runtime_put_autosuspend(connector->dev->dev);
	return ret;
}

static const struct drm_connector_helper_funcs
nv50_mstc_help = {
	.get_modes = nv50_mstc_get_modes,
	.mode_valid = nv50_mstc_mode_valid,
	.atomic_best_encoder = nv50_mstc_atomic_best_encoder,
	.atomic_check = nv50_mstc_atomic_check,
	.detect_ctx = nv50_mstc_detect,
};

static void
nv50_mstc_destroy(struct drm_connector *connector)
{
	struct nv50_mstc *mstc = nv50_mstc(connector);

	drm_connector_cleanup(&mstc->connector);
	drm_dp_mst_put_port_malloc(mstc->port);

	kfree(mstc);
}

/******************************************************************************
 * Connector debugfs
 *****************************************************************************/

/* Show probed limits alongside committed state to diagnose link setup. */
static const char *
nv50_link_config_or_depth_str(u8 depth)
{
	switch (depth) {
	case 8: return "36 bpp";
	case 6: return "30 bpp";
	case 5: return "24 bpp";
	case 2: return "18 bpp";
	case 0: return "default (24 bpp on GV100+)";
	default: return "?";
	}
}

static int
nv50_link_config_print(struct seq_file *m, struct drm_connector *connector,
		       bool mst)
{
	struct drm_device *dev = connector->dev;
	const struct drm_display_info *info = &connector->display_info;
	struct drm_modeset_acquire_ctx ctx;
	struct nouveau_encoder *nv_encoder = NULL;
	struct drm_connector_state *conn_state;
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;
	bool committed = false;
	int ret;

	/* Probed data needs mode_config.mutex, while routing and CRTC state
	 * need the modeset locks. An acquire context lets us back off if a
	 * concurrent atomic commit would deadlock.
	 */
	mutex_lock(&dev->mode_config.mutex);
	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, DRM_MODESET_ACQUIRE_INTERRUPTIBLE,
				   ret);

	seq_printf(m, "connector: %s, %s\n",
		   drm_get_connector_type_name(connector->connector_type),
		   drm_get_connector_status_name(connector->status));

	conn_state = connector->state;
	encoder = conn_state ? conn_state->best_encoder : NULL;
	if (!mst)
		nv_encoder = nouveau_connector(connector)->detected_encoder;
	if (encoder)
		nv_encoder = nv50_real_outp(encoder);

	if (connector->status == connector_status_connected && nv_encoder) {
		if (nv_encoder->dcb->type == DCB_OUTPUT_TMDS && info->is_hdmi) {
			seq_printf(m, "sink: scdc %s, scrambling %s, low-rate-scrambling %s\n",
				   str_yes_no(info->hdmi.scdc.supported),
				   str_yes_no(info->hdmi.scdc.scrambling.supported),
				   str_yes_no(info->hdmi.scdc.scrambling.low_rates));
			seq_printf(m, "tmds: edid max %d kHz, ceiling %u kHz\n",
				   info->max_tmds_clock,
				   nouveau_connector_tmds_link_bandwidth(connector));
			seq_printf(m, "allm: %s\n", str_yes_no(info->hdmi.allm));
			if (info->hdmi.vrr_cap.supported)
				seq_printf(m, "vrr range: %u-%u Hz\n",
					   info->hdmi.vrr_cap.vrr_min,
					   info->hdmi.vrr_cap.vrr_max);
			else
				seq_puts(m, "vrr range: none\n");
		} else if (nv_encoder->dcb->type == DCB_OUTPUT_DP) {
			seq_printf(m, "dp: sink max %d lanes @ %d kHz\n",
				   nv_encoder->dp.link_nr, nv_encoder->dp.link_bw);
		}

		/* Probed depth, including driver limits and fallback values. */
		seq_printf(m, "depth: probed %u bpc, property cap %u\n",
			   info->bpc, conn_state ? conn_state->max_requested_bpc : 0);
	}

	seq_puts(m, "--- committed ---\n");
	crtc = conn_state ? conn_state->crtc : NULL;
	if (crtc && crtc->state && crtc->state->active && encoder && nv_encoder) {
		const struct nv50_head_atom *armh = nv50_head_atom(crtc->state);
		const struct drm_display_mode *mode = &crtc->state->adjusted_mode;

		seq_printf(m, "mode: %dx%d @ %d kHz\n",
			   mode->hdisplay, mode->vdisplay, mode->clock);

		if (nv_encoder->dcb->type == DCB_OUTPUT_TMDS) {
			seq_printf(m, "protocol: TMDS%s\n", info->is_hdmi ? " (HDMI)" : "");
		} else if (nv_encoder->dcb->type == DCB_OUTPUT_DP) {
			/* Training may use fewer lanes or a lower rate than
			 * the probed limits.
			 */
			if (nv_encoder->dp.lt.nr)
				seq_printf(m, "protocol: DP %s, %d lanes @ %d kHz\n",
					   nv_encoder->dp.lt.mst ? "MST" : "SST",
					   nv_encoder->dp.lt.nr, nv_encoder->dp.lt.bw);
			else
				seq_puts(m, "protocol: DP, link down\n");
			if (mst)
				seq_printf(m, "mst: pbn %u, tu %u\n",
					   armh->dp.pbn, armh->dp.tu);
		}

		seq_printf(m, "depth: %u bpc (or.depth %s)\n", armh->or.bpc,
			   nv50_link_config_or_depth_str(armh->or.depth));

		if (nv50_disp(dev)->tile.nr_tiles && armh->mtc.tiles_mask)
			seq_printf(m, "tiles: mask %#04x (%d), phywins %#010x/%#010x\n",
				   armh->mtc.tiles_mask,
				   hweight8(armh->mtc.tiles_mask),
				   armh->mtc.phywins_mask[0],
				   armh->mtc.phywins_mask[1]);

		committed = true;
	}
	if (!committed)
		seq_puts(m, "inactive\n");

	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
	mutex_unlock(&dev->mode_config.mutex);
	return ret;
}

static int
nv50_link_config_show(struct seq_file *m, void *data)
{
	return nv50_link_config_print(m, m->private, false);
}
DEFINE_SHOW_ATTRIBUTE(nv50_link_config);

/* Update the property state without an atomic commit, so the cap takes
 * effect at the next modeset. Property writes and connector reset can
 * replace it.
 */
static int
nv50_max_bpc_get(void *data, u64 *val)
{
	struct drm_connector *connector = data;
	struct drm_device *dev = connector->dev;

	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	*val = connector->state->max_requested_bpc;
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	return 0;
}

static int
nv50_max_bpc_set(void *data, u64 val)
{
	struct drm_connector *connector = data;
	struct drm_device *dev = connector->dev;
	const struct drm_property *prop = connector->max_bpc_property;

	if (val < prop->values[0] || val > prop->values[1])
		return -EINVAL;

	drm_modeset_lock(&dev->mode_config.connection_mutex, NULL);
	connector->state->max_requested_bpc = val;
	drm_modeset_unlock(&dev->mode_config.connection_mutex);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(nv50_max_bpc_fops, nv50_max_bpc_get, nv50_max_bpc_set,
			 "%llu\n");

void
nv50_connector_debugfs_init(struct drm_connector *connector, struct dentry *root)
{
	if (nouveau_display(connector->dev)->disp.object.oclass < NV50_DISP)
		return;

	debugfs_create_file("link_config", 0444, root, connector,
			    &nv50_link_config_fops);

	if (connector->max_bpc_property)
		debugfs_create_file_unsafe("max_bpc", 0644, root, connector,
					   &nv50_max_bpc_fops);
}

static int
nv50_mst_link_config_show(struct seq_file *m, void *data)
{
	return nv50_link_config_print(m, m->private, true);
}
DEFINE_SHOW_ATTRIBUTE(nv50_mst_link_config);

/* MST connectors use nv50_mstc, so they need a separate callback. */
static void
nv50_mstc_debugfs_init(struct drm_connector *connector, struct dentry *root)
{
	debugfs_create_file("link_config", 0444, root, connector,
			    &nv50_mst_link_config_fops);

	if (connector->max_bpc_property)
		debugfs_create_file_unsafe("max_bpc", 0644, root, connector,
					   &nv50_max_bpc_fops);
}

static const struct drm_connector_funcs
nv50_mstc = {
	.reset = nouveau_conn_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = nv50_mstc_destroy,
	.atomic_duplicate_state = nouveau_conn_atomic_duplicate_state,
	.atomic_destroy_state = nouveau_conn_atomic_destroy_state,
	.atomic_set_property = nouveau_conn_atomic_set_property,
	.atomic_get_property = nouveau_conn_atomic_get_property,
	.debugfs_init = nv50_mstc_debugfs_init,
};

static int
nv50_mstc_new(struct nv50_mstm *mstm, struct drm_dp_mst_port *port,
	      const char *path, struct nv50_mstc **pmstc)
{
	struct drm_device *dev = mstm->outp->base.base.dev;
	struct drm_crtc *crtc;
	struct nv50_mstc *mstc;
	int ret;

	if (!(mstc = *pmstc = kzalloc_obj(*mstc)))
		return -ENOMEM;
	mstc->mstm = mstm;
	mstc->port = port;

	ret = drm_connector_dynamic_init(dev, &mstc->connector, &nv50_mstc,
					 DRM_MODE_CONNECTOR_DisplayPort, NULL);
	if (ret) {
		kfree(*pmstc);
		*pmstc = NULL;
		return ret;
	}

	drm_connector_helper_add(&mstc->connector, &nv50_mstc_help);

	mstc->connector.funcs->reset(&mstc->connector);
	nouveau_conn_attach_properties(&mstc->connector);

	drm_for_each_crtc(crtc, dev) {
		if (!(mstm->outp->dcb->heads & drm_crtc_mask(crtc)))
			continue;

		drm_connector_attach_encoder(&mstc->connector,
					     &nv50_head(crtc)->msto->encoder);
	}

	drm_object_attach_property(&mstc->connector.base, dev->mode_config.path_property, 0);
	drm_object_attach_property(&mstc->connector.base, dev->mode_config.tile_property, 0);
	drm_connector_set_path_property(&mstc->connector, path);
	drm_dp_mst_get_port_malloc(port);
	return 0;
}

static void
nv50_mstm_cleanup(struct drm_atomic_commit *state,
		  struct drm_dp_mst_topology_state *mst_state,
		  struct nv50_mstm *mstm)
{
	struct nouveau_drm *drm = nouveau_drm(mstm->outp->base.base.dev);
	struct drm_encoder *encoder;

	NV_ATOMIC(drm, "%s: mstm cleanup\n", mstm->outp->base.base.name);
	drm_dp_check_act_status(&mstm->mgr);

	drm_for_each_encoder(encoder, mstm->outp->base.base.dev) {
		if (encoder->encoder_type == DRM_MODE_ENCODER_DPMST) {
			struct nv50_msto *msto = nv50_msto(encoder);
			struct nv50_mstc *mstc = msto->mstc;
			if (mstc && mstc->mstm == mstm)
				nv50_msto_cleanup(state, mst_state, &mstm->mgr, msto);
		}
	}

	if (mstm->disabled) {
		nouveau_dp_power_down(mstm->outp);
		nvif_outp_release(&mstm->outp->outp);
		mstm->disabled = false;
	}

	mstm->modified = false;
}

static void
nv50_mstm_prepare(struct drm_atomic_commit *state,
		  struct drm_dp_mst_topology_state *mst_state,
		  struct nv50_mstm *mstm)
{
	struct nouveau_drm *drm = nouveau_drm(mstm->outp->base.base.dev);
	struct drm_encoder *encoder;

	NV_ATOMIC(drm, "%s: mstm prepare\n", mstm->outp->base.base.name);

	/* Disable payloads first */
	drm_for_each_encoder(encoder, mstm->outp->base.base.dev) {
		if (encoder->encoder_type == DRM_MODE_ENCODER_DPMST) {
			struct nv50_msto *msto = nv50_msto(encoder);
			struct nv50_mstc *mstc = msto->mstc;
			if (mstc && mstc->mstm == mstm && msto->disabled)
				nv50_msto_prepare(state, mst_state, &mstm->mgr, msto);
		}
	}

	/* Add payloads for new heads, while also updating the start slots of any unmodified (but
	 * active) heads that may have had their VC slots shifted left after the previous step
	 */
	drm_for_each_encoder(encoder, mstm->outp->base.base.dev) {
		if (encoder->encoder_type == DRM_MODE_ENCODER_DPMST) {
			struct nv50_msto *msto = nv50_msto(encoder);
			struct nv50_mstc *mstc = msto->mstc;
			if (mstc && mstc->mstm == mstm && !msto->disabled)
				nv50_msto_prepare(state, mst_state, &mstm->mgr, msto);
		}
	}
}

static struct drm_connector *
nv50_mstm_add_connector(struct drm_dp_mst_topology_mgr *mgr,
			struct drm_dp_mst_port *port, const char *path)
{
	struct nv50_mstm *mstm = nv50_mstm(mgr);
	struct nv50_mstc *mstc;
	int ret;

	ret = nv50_mstc_new(mstm, port, path, &mstc);
	if (ret)
		return NULL;

	return &mstc->connector;
}

static const struct drm_dp_mst_topology_cbs
nv50_mstm = {
	.add_connector = nv50_mstm_add_connector,
};

bool
nv50_mstm_service(struct nouveau_drm *drm,
		  struct nouveau_connector *nv_connector,
		  struct nv50_mstm *mstm)
{
	struct drm_dp_aux *aux = &nv_connector->aux;
	bool handled = true, ret = true;
	int rc;
	u8 esi[8] = {};

	while (handled) {
		u8 ack[8] = {};

		rc = drm_dp_dpcd_read(aux, DP_SINK_COUNT_ESI, esi, 8);
		if (rc != 8) {
			ret = false;
			break;
		}

		drm_dp_mst_hpd_irq_handle_event(&mstm->mgr, esi, ack, &handled);
		if (!handled)
			break;

		rc = drm_dp_dpcd_writeb(aux, DP_SINK_COUNT_ESI + 1, ack[1]);

		if (rc != 1) {
			ret = false;
			break;
		}

		drm_dp_mst_hpd_irq_send_new_request(&mstm->mgr);
	}

	if (!ret)
		NV_DEBUG(drm, "Failed to handle ESI on %s: %d\n",
			 nv_connector->base.name, rc);

	return ret;
}

void
nv50_mstm_remove(struct nv50_mstm *mstm)
{
	mstm->is_mst = false;
	drm_dp_mst_topology_mgr_set_mst(&mstm->mgr, false);
}

int
nv50_mstm_detect(struct nouveau_encoder *outp)
{
	struct nv50_mstm *mstm = outp->dp.mstm;
	struct drm_dp_aux *aux;
	int ret;

	if (!mstm || !mstm->can_mst)
		return 0;

	aux = mstm->mgr.aux;

	/* Clear any leftover MST state we didn't set ourselves by first
	 * disabling MST if it was already enabled
	 */
	ret = drm_dp_dpcd_writeb(aux, DP_MSTM_CTRL, 0);
	if (ret < 0)
		return ret;

	/* And start enabling */
	ret = drm_dp_mst_topology_mgr_set_mst(&mstm->mgr, true);
	if (ret)
		return ret;

	mstm->is_mst = true;
	return 1;
}

static void
nv50_mstm_fini(struct nouveau_encoder *outp)
{
	struct nv50_mstm *mstm = outp->dp.mstm;

	if (!mstm)
		return;

	/* Don't change the MST state of this connector until we've finished
	 * resuming, since we can't safely grab hpd_irq_lock in our resume
	 * path to protect mstm->is_mst without potentially deadlocking
	 */
	mutex_lock(&outp->dp.hpd_irq_lock);
	mstm->suspended = true;
	mutex_unlock(&outp->dp.hpd_irq_lock);

	if (mstm->is_mst)
		drm_dp_mst_topology_mgr_suspend(&mstm->mgr);
}

static void
nv50_mstm_init(struct nouveau_encoder *outp, bool runtime)
{
	struct nv50_mstm *mstm = outp->dp.mstm;
	int ret = 0;

	if (!mstm)
		return;

	if (mstm->is_mst) {
		ret = drm_dp_mst_topology_mgr_resume(&mstm->mgr, !runtime);
		if (ret == -1)
			nv50_mstm_remove(mstm);
	}

	mutex_lock(&outp->dp.hpd_irq_lock);
	mstm->suspended = false;
	mutex_unlock(&outp->dp.hpd_irq_lock);

	if (ret == -1)
		drm_kms_helper_hotplug_event(mstm->mgr.dev);
}

static void
nv50_mstm_del(struct nv50_mstm **pmstm)
{
	struct nv50_mstm *mstm = *pmstm;
	if (mstm) {
		drm_dp_mst_topology_mgr_destroy(&mstm->mgr);
		kfree(*pmstm);
		*pmstm = NULL;
	}
}

static int
nv50_mstm_new(struct nouveau_encoder *outp, struct drm_dp_aux *aux, int aux_max,
	      int conn_base_id, struct nv50_mstm **pmstm)
{
	const int max_payloads = hweight8(outp->dcb->heads);
	struct drm_device *dev = outp->base.base.dev;
	struct nv50_mstm *mstm;
	int ret;

	if (!(mstm = *pmstm = kzalloc_obj(*mstm)))
		return -ENOMEM;
	mstm->outp = outp;
	mstm->mgr.cbs = &nv50_mstm;

	ret = drm_dp_mst_topology_mgr_init(&mstm->mgr, dev, aux, aux_max,
					   max_payloads, conn_base_id);
	if (ret)
		return ret;

	return 0;
}

/******************************************************************************
 * SOR
 *****************************************************************************/
static void
nv50_sor_update(struct nouveau_encoder *nv_encoder, u8 head,
		struct nv50_head_atom *asyh, u8 proto, u8 depth)
{
	struct nv50_disp *disp = nv50_disp(nv_encoder->base.base.dev);
	struct nv50_core *core = disp->core;

	if (!asyh) {
		nv_encoder->ctrl &= ~BIT(head);
		if (NVDEF_TEST(nv_encoder->ctrl, NV507D, SOR_SET_CONTROL, OWNER, ==, NONE))
			nv_encoder->ctrl = 0;
	} else {
		nv_encoder->ctrl |= NVVAL(NV507D, SOR_SET_CONTROL, PROTOCOL, proto);
		nv_encoder->ctrl |= BIT(head);
		asyh->or.depth = depth;
	}

	core->func->sor->ctrl(core, nv_encoder->outp.or.id, nv_encoder->ctrl, asyh);
}

/* TODO: Should we extend this to PWM-only backlights?
 * As well, should we add a DRM helper for waiting for the backlight to acknowledge
 * the panel backlight has been shut off? Intel doesn't seem to do this, and uses a
 * fixed time delay from the vbios…
 */
static void
nv50_sor_atomic_disable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nv50_head *head = nv50_head(nv_encoder->crtc);
#ifdef CONFIG_DRM_NOUVEAU_BACKLIGHT
	struct nouveau_connector *nv_connector = nv50_outp_get_old_connector(state, nv_encoder);
	struct nouveau_drm *drm = nouveau_drm(nv_encoder->base.base.dev);
	struct nouveau_backlight *backlight = nv_connector->backlight;
	struct drm_dp_aux *aux = &nv_connector->aux;
	int ret;

	if (backlight && backlight->uses_dpcd) {
		ret = drm_edp_backlight_disable(aux, &backlight->edp_info);
		if (ret < 0)
			NV_ERROR(drm, "Failed to disable backlight on [CONNECTOR:%d:%s]: %d\n",
				 nv_connector->base.base.id, nv_connector->base.name, ret);
	}
#endif

	if (nv_encoder->dcb->type == DCB_OUTPUT_TMDS && nv_encoder->hdmi.enabled) {
		nvif_outp_hdmi(&nv_encoder->outp, head->base.index,
			       false, 0, 0, 0, false, false, false, 0);
		nv_encoder->hdmi.enabled = false;
	}

	if (nv_encoder->dcb->type == DCB_OUTPUT_DP)
		nouveau_dp_power_down(nv_encoder);

	if (head->func->display_id)
		head->func->display_id(head, 0);

	nv_encoder->update(nv_encoder, head->base.index, NULL, 0, 0);
	nv50_audio_disable(encoder, &head->base);
	nv_encoder->crtc = NULL;
}

// common/inc/displayport/displayport.h
#define DP_CONFIG_WATERMARK_ADJUST                   2
#define DP_CONFIG_WATERMARK_LIMIT                   20
#define DP_CONFIG_INCREASED_WATERMARK_ADJUST         8
#define DP_CONFIG_INCREASED_WATERMARK_LIMIT         22

static bool
nv50_sor_dp_watermark_sst(struct nouveau_encoder *outp,
			  struct nv50_head *head, struct nv50_head_atom *asyh)
{
	bool enhancedFraming = outp->dp.dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP;
	u64 minRate = outp->dp.link_bw * 1000;
	unsigned tuSize = 64;
	unsigned waterMark;
	unsigned hBlankSym;
	unsigned vBlankSym;
	unsigned watermarkAdjust = DP_CONFIG_WATERMARK_ADJUST;
	unsigned watermarkMinimum = DP_CONFIG_WATERMARK_LIMIT;
	// depth is multiplied by 16 in case of DSC enable
	s32 hblank_symbols;
	// number of link clocks per line.
	int vblank_symbols	  = 0;
	bool bEnableDsc = false;
	unsigned surfaceWidth = asyh->mode.h.blanks - asyh->mode.h.blanke;
	unsigned rasterWidth = asyh->mode.h.active;
	unsigned depth = asyh->or.bpc * 3;
	unsigned DSC_FACTOR = bEnableDsc ? 16 : 1;
	u64 pixelClockHz = asyh->mode.clock * 1000;
	u64 PrecisionFactor = 100000, ratioF, watermarkF;
	u32 numLanesPerLink = outp->dp.link_nr;
	u32 numSymbolsPerLine;
	u32 BlankingBits;
	u32 surfaceWidthPerLink;
	u32 PixelSteeringBits;
	u64 NumBlankingLinkClocks;
	u32 MinHBlank;

	if (outp->outp.info.dp.increased_wm) {
		watermarkAdjust = DP_CONFIG_INCREASED_WATERMARK_ADJUST;
		watermarkMinimum = DP_CONFIG_INCREASED_WATERMARK_LIMIT;
	}

	if ((pixelClockHz * depth) >= (8 * minRate * outp->dp.link_nr * DSC_FACTOR))
	{
		return false;
	}

	//
	// For DSC, if (pclk * bpp) < (1/64 * orclk * 8 * lanes) then some TU may end up with
	// 0 active symbols. This may cause HW hang. Bug 200379426
	//
	if ((bEnableDsc) &&
	    ((pixelClockHz * depth) < div_u64(8 * minRate * outp->dp.link_nr * DSC_FACTOR, 64)))
	{
		return false;
	}

	//
	//  Perform the SST calculation.
	//	For auto mode the watermark calculation does not need to track accumulated error the
	//	formulas for manual mode will not work.  So below calculation was extracted from the DTB.
	//
	ratioF = div_u64((u64)pixelClockHz * depth * PrecisionFactor, DSC_FACTOR);

	ratioF = div_u64(ratioF, 8 * (u64) minRate * outp->dp.link_nr);

	if (PrecisionFactor < ratioF) // Assert if we will end up with a negative number in below
		return false;

	watermarkF = div_u64(ratioF * tuSize * (PrecisionFactor - ratioF), PrecisionFactor);
	waterMark = (unsigned)(watermarkAdjust + (div_u64(2 * div_u64(depth * PrecisionFactor, 8 * numLanesPerLink * DSC_FACTOR) + watermarkF, PrecisionFactor)));

	//
	//  Bounds check the watermark
	//
	numSymbolsPerLine = div_u64(surfaceWidth * depth, 8 * outp->dp.link_nr * DSC_FACTOR);

	if (WARN_ON(waterMark > 39 || waterMark > numSymbolsPerLine))
		return false;

	//
	//  Clamp the low side
	//
	if (waterMark < watermarkMinimum)
		waterMark = watermarkMinimum;

	//Bits to send BS/BE/Extra symbols due to pixel padding
	//Also accounts for enhanced framing.
	BlankingBits = 3*8*numLanesPerLink + (enhancedFraming ? 3*8*numLanesPerLink : 0);

	//VBID/MVID/MAUD sent 4 times all the time
	BlankingBits += 3*8*4;

	surfaceWidthPerLink = surfaceWidth;

	//Extra bits sent due to pixel steering
	u32 remain;
	div_u64_rem(surfaceWidthPerLink, numLanesPerLink, &remain);
	PixelSteeringBits = remain ? div_u64((numLanesPerLink - remain) * depth, DSC_FACTOR) : 0;

	BlankingBits += PixelSteeringBits;
	NumBlankingLinkClocks = div_u64((u64)BlankingBits * PrecisionFactor, (8 * numLanesPerLink));
	MinHBlank = (u32)(div_u64(div_u64(NumBlankingLinkClocks * pixelClockHz, minRate), PrecisionFactor));
	MinHBlank += 12;

	if (WARN_ON(MinHBlank > rasterWidth - surfaceWidth))
		return false;

	// Bug 702290 - Active Width should be greater than 60
	if (WARN_ON(surfaceWidth <= 60))
		return false;


	hblank_symbols = (s32)(div_u64((u64)(rasterWidth - surfaceWidth - MinHBlank) * minRate, pixelClockHz));

	//reduce HBlank Symbols to account for secondary data packet
	hblank_symbols -= 1; //Stuffer latency to send BS
	hblank_symbols -= 3; //SPKT latency to send data to stuffer

	hblank_symbols -= numLanesPerLink == 1 ? 9  : numLanesPerLink == 2 ? 6 : 3;

	hBlankSym = (hblank_symbols < 0) ? 0 : hblank_symbols;

	// Refer to dev_disp.ref for more information.
	// # symbols/vblank = ((SetRasterBlankEnd.X + SetRasterSize.Width - SetRasterBlankStart.X - 40) * link_clk / pclk) - Y - 1;
	// where Y = (# lanes == 4) 12 : (# lanes == 2) ? 21 : 39
	if (surfaceWidth < 40)
	{
		vblank_symbols = 0;
	}
	else
	{
		vblank_symbols = (s32)((div_u64((u64)(surfaceWidth - 40) * minRate, pixelClockHz))) - 1;

		vblank_symbols -= numLanesPerLink == 1 ? 39  : numLanesPerLink == 2 ? 21 : 12;
	}

	vBlankSym = (vblank_symbols < 0) ? 0 : vblank_symbols;

	return nvif_outp_dp_sst(&outp->outp, head->base.index, waterMark, hBlankSym, vBlankSym);
}

static void
nv50_sor_atomic_enable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_crtc *nv_crtc = nv50_outp_get_new_crtc(state, nv_encoder);
	struct nv50_head_atom *asyh =
		nv50_head_atom(drm_atomic_get_new_crtc_state(state, &nv_crtc->base));
	struct drm_display_mode *mode = &asyh->state.adjusted_mode;
	struct nv50_disp *disp = nv50_disp(encoder->dev);
	struct nv50_head *head = nv50_head(&nv_crtc->base);
	struct nvif_outp *outp = &nv_encoder->outp;
	struct drm_device *dev = encoder->dev;
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nouveau_connector *nv_connector;
#ifdef CONFIG_DRM_NOUVEAU_BACKLIGHT
	struct nouveau_backlight *backlight;
#endif
	struct nvbios *bios = &drm->vbios;
	bool lvds_dual = false, lvds_8bpc = false, hda = false;
	u8 proto = NV507D_SOR_SET_CONTROL_PROTOCOL_CUSTOM;
	u8 depth = NV837D_SOR_SET_CONTROL_PIXEL_DEPTH_DEFAULT;

	nv_connector = nv50_outp_get_new_connector(state, nv_encoder);
	nv_encoder->crtc = &nv_crtc->base;

	if ((disp->disp->object.oclass == GT214_DISP ||
	     disp->disp->object.oclass >= GF110_DISP) &&
	    nv_encoder->dcb->type != DCB_OUTPUT_LVDS &&
	    nv_connector->base.display_info.has_audio)
		hda = true;

	if (!nvif_outp_acquired(outp))
		nvif_outp_acquire_sor(outp, hda);

	switch (nv_encoder->dcb->type) {
	case DCB_OUTPUT_TMDS:
		if (disp->disp->object.oclass != NV50_DISP &&
		    nv_connector->base.display_info.is_hdmi)
			nv50_hdmi_enable(encoder, nv_crtc, nv_connector, state, mode, hda);

		/* Keep the default SOR encoding at 8 bpc. Deep color needs
		 * an explicit depth.
		 */
		if (asyh->or.bpc > 8)
			depth = nv50_dp_bpc_to_depth(asyh->or.bpc);

		if (nv_encoder->outp.or.link & 1) {
			proto = NV507D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_A;
			/* Only enable dual-link if:
			 *  - Need to (i.e. rate > 165MHz)
			 *  - DCB says we can
			 *  - Not an HDMI monitor, since there's no dual-link
			 *    on HDMI.
			 */
			if (mode->clock >= 165000 &&
			    nv_encoder->dcb->duallink_possible &&
			    !nv_connector->base.display_info.is_hdmi)
				proto = NV507D_SOR_SET_CONTROL_PROTOCOL_DUAL_TMDS;
		} else {
			proto = NV507D_SOR_SET_CONTROL_PROTOCOL_SINGLE_TMDS_B;
		}
		break;
	case DCB_OUTPUT_LVDS:
		proto = NV507D_SOR_SET_CONTROL_PROTOCOL_LVDS_CUSTOM;

		if (bios->fp_no_ddc) {
			lvds_dual = bios->fp.dual_link;
			lvds_8bpc = bios->fp.if_is_24bit;
		} else {
			if (nv_connector->spwg_links) {
				if (nv_connector->spwg_links == 2)
					lvds_dual = true;
			} else
			if (mode->clock >= bios->fp.duallink_transition_clk) {
				lvds_dual = true;
			}

			if (lvds_dual) {
				if (bios->fp.strapless_is_24bit & 2)
					lvds_8bpc = true;
			} else {
				if (bios->fp.strapless_is_24bit & 1)
					lvds_8bpc = true;
			}

			if (asyh->or.bpc == 8)
				lvds_8bpc = true;
		}

		nvif_outp_lvds(&nv_encoder->outp, lvds_dual, lvds_8bpc);
		break;
	case DCB_OUTPUT_DP:
		nouveau_dp_train(nv_encoder, false, mode->clock, asyh->or.bpc);
		nv50_sor_dp_watermark_sst(nv_encoder, head, asyh);
		depth = nv50_dp_bpc_to_depth(asyh->or.bpc);

		if (nv_encoder->outp.or.link & 1)
			proto = NV887D_SOR_SET_CONTROL_PROTOCOL_DP_A;
		else
			proto = NV887D_SOR_SET_CONTROL_PROTOCOL_DP_B;

#ifdef CONFIG_DRM_NOUVEAU_BACKLIGHT
		backlight = nv_connector->backlight;
		if (backlight && backlight->uses_dpcd)
			drm_edp_backlight_enable(&nv_connector->aux, &backlight->edp_info,
						 backlight->dev->props.brightness);
#endif

		break;
	default:
		BUG();
		break;
	}

	if (head->func->display_id)
		head->func->display_id(head, BIT(nv_encoder->outp.id));

	nv_encoder->update(nv_encoder, nv_crtc->index, asyh, proto, depth);
}

static const struct drm_encoder_helper_funcs
nv50_sor_help = {
	.atomic_check = nv50_outp_atomic_check,
	.atomic_enable = nv50_sor_atomic_enable,
	.atomic_disable = nv50_sor_atomic_disable,
};

static void
nv50_sor_destroy(struct drm_encoder *encoder)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);

	nv50_mstm_del(&nv_encoder->dp.mstm);
	drm_encoder_cleanup(encoder);

	if (nv_encoder->dcb->type == DCB_OUTPUT_DP)
		mutex_destroy(&nv_encoder->dp.hpd_irq_lock);

	nvif_outp_dtor(&nv_encoder->outp);
	kfree(encoder);
}

static const struct drm_encoder_funcs
nv50_sor_func = {
	.destroy = nv50_sor_destroy,
};

static int
nv50_sor_create(struct nouveau_encoder *nv_encoder)
{
	struct drm_connector *connector = &nv_encoder->conn->base;
	struct nouveau_connector *nv_connector = nouveau_connector(connector);
	struct nouveau_drm *drm = nouveau_drm(connector->dev);
	struct nvkm_i2c *i2c = nvxx_i2c(drm);
	struct drm_encoder *encoder;
	struct dcb_output *dcbe = nv_encoder->dcb;
	struct nv50_disp *disp = nv50_disp(connector->dev);
	int type, ret;

	switch (dcbe->type) {
	case DCB_OUTPUT_LVDS: type = DRM_MODE_ENCODER_LVDS; break;
	case DCB_OUTPUT_TMDS:
	case DCB_OUTPUT_DP:
	default:
		type = DRM_MODE_ENCODER_TMDS;
		break;
	}

	nv_encoder->update = nv50_sor_update;

	encoder = to_drm_encoder(nv_encoder);
	drm_encoder_init(connector->dev, encoder, &nv50_sor_func, type,
			 "sor-%04x-%04x", dcbe->hasht, dcbe->hashm);
	drm_encoder_helper_add(encoder, &nv50_sor_help);

	drm_connector_attach_encoder(connector, encoder);

	disp->core->func->sor->get_caps(disp, nv_encoder, ffs(dcbe->or) - 1);
	nv50_outp_dump_caps(drm, nv_encoder);

	if (dcbe->type == DCB_OUTPUT_DP) {
		mutex_init(&nv_encoder->dp.hpd_irq_lock);

		if (disp->disp->object.oclass < GF110_DISP) {
			/* HW has no support for address-only
			 * transactions, so we're required to
			 * use custom I2C-over-AUX code.
			 */
			struct nvkm_i2c_aux *aux;

			aux = nvkm_i2c_aux_find(i2c, dcbe->i2c_index);
			if (!aux)
				return -EINVAL;

			nv_encoder->i2c = &aux->i2c;
		} else {
			nv_encoder->i2c = &nv_connector->aux.ddc;
		}

		if (nv_connector->type != DCB_CONNECTOR_eDP && nv_encoder->outp.info.dp.mst) {
			ret = nv50_mstm_new(nv_encoder, &nv_connector->aux,
					    16, nv_connector->base.base.id,
					    &nv_encoder->dp.mstm);
			if (ret)
				return ret;
		}
	} else
	if (nv_encoder->outp.info.ddc != NVIF_OUTP_DDC_INVALID) {
		struct nvkm_i2c_bus *bus =
			nvkm_i2c_bus_find(i2c, dcbe->i2c_index);
		if (bus)
			nv_encoder->i2c = &bus->i2c;
	}

	return 0;
}

/******************************************************************************
 * PIOR
 *****************************************************************************/
static int
nv50_pior_atomic_check(struct drm_encoder *encoder,
		       struct drm_crtc_state *crtc_state,
		       struct drm_connector_state *conn_state)
{
	int ret = nv50_outp_atomic_check(encoder, crtc_state, conn_state);
	if (ret)
		return ret;
	crtc_state->adjusted_mode.clock *= 2;
	return 0;
}

static void
nv50_pior_atomic_disable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nv50_core *core = nv50_disp(encoder->dev)->core;
	const u32 ctrl = NVDEF(NV507D, PIOR_SET_CONTROL, OWNER, NONE);

	core->func->pior->ctrl(core, nv_encoder->outp.or.id, ctrl, NULL);
	nv_encoder->crtc = NULL;
}

static void
nv50_pior_atomic_enable(struct drm_encoder *encoder, struct drm_atomic_commit *state)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct nouveau_crtc *nv_crtc = nv50_outp_get_new_crtc(state, nv_encoder);
	struct nv50_head_atom *asyh =
		nv50_head_atom(drm_atomic_get_new_crtc_state(state, &nv_crtc->base));
	struct nv50_core *core = nv50_disp(encoder->dev)->core;
	u32 ctrl = 0;

	switch (nv_crtc->index) {
	case 0: ctrl |= NVDEF(NV507D, PIOR_SET_CONTROL, OWNER, HEAD0); break;
	case 1: ctrl |= NVDEF(NV507D, PIOR_SET_CONTROL, OWNER, HEAD1); break;
	default:
		WARN_ON(1);
		break;
	}

	switch (asyh->or.bpc) {
	case 10: asyh->or.depth = NV837D_PIOR_SET_CONTROL_PIXEL_DEPTH_BPP_30_444; break;
	case  8: asyh->or.depth = NV837D_PIOR_SET_CONTROL_PIXEL_DEPTH_BPP_24_444; break;
	case  6: asyh->or.depth = NV837D_PIOR_SET_CONTROL_PIXEL_DEPTH_BPP_18_444; break;
	default: asyh->or.depth = NV837D_PIOR_SET_CONTROL_PIXEL_DEPTH_DEFAULT; break;
	}

	if (!nvif_outp_acquired(&nv_encoder->outp))
		nvif_outp_acquire_pior(&nv_encoder->outp);

	switch (nv_encoder->dcb->type) {
	case DCB_OUTPUT_TMDS:
		ctrl |= NVDEF(NV507D, PIOR_SET_CONTROL, PROTOCOL, EXT_TMDS_ENC);
		break;
	case DCB_OUTPUT_DP:
		ctrl |= NVDEF(NV507D, PIOR_SET_CONTROL, PROTOCOL, EXT_TMDS_ENC);
		nouveau_dp_train(nv_encoder, false, asyh->state.adjusted_mode.clock, 6);
		break;
	default:
		BUG();
		break;
	}

	core->func->pior->ctrl(core, nv_encoder->outp.or.id, ctrl, asyh);
	nv_encoder->crtc = &nv_crtc->base;
}

static const struct drm_encoder_helper_funcs
nv50_pior_help = {
	.atomic_check = nv50_pior_atomic_check,
	.atomic_enable = nv50_pior_atomic_enable,
	.atomic_disable = nv50_pior_atomic_disable,
};

static void
nv50_pior_destroy(struct drm_encoder *encoder)
{
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);

	nvif_outp_dtor(&nv_encoder->outp);

	drm_encoder_cleanup(encoder);

	mutex_destroy(&nv_encoder->dp.hpd_irq_lock);
	kfree(encoder);
}

static const struct drm_encoder_funcs
nv50_pior_func = {
	.destroy = nv50_pior_destroy,
};

static int
nv50_pior_create(struct nouveau_encoder *nv_encoder)
{
	struct drm_connector *connector = &nv_encoder->conn->base;
	struct drm_device *dev = connector->dev;
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nv50_disp *disp = nv50_disp(dev);
	struct nvkm_i2c *i2c = nvxx_i2c(drm);
	struct nvkm_i2c_bus *bus = NULL;
	struct nvkm_i2c_aux *aux = NULL;
	struct i2c_adapter *ddc;
	struct drm_encoder *encoder;
	struct dcb_output *dcbe = nv_encoder->dcb;
	int type;

	switch (dcbe->type) {
	case DCB_OUTPUT_TMDS:
		bus  = nvkm_i2c_bus_find(i2c, nv_encoder->outp.info.ddc);
		ddc  = bus ? &bus->i2c : NULL;
		type = DRM_MODE_ENCODER_TMDS;
		break;
	case DCB_OUTPUT_DP:
		aux  = nvkm_i2c_aux_find(i2c, nv_encoder->outp.info.dp.aux);
		ddc  = aux ? &aux->i2c : NULL;
		type = DRM_MODE_ENCODER_TMDS;
		break;
	default:
		return -ENODEV;
	}

	nv_encoder->i2c = ddc;

	mutex_init(&nv_encoder->dp.hpd_irq_lock);

	encoder = to_drm_encoder(nv_encoder);
	drm_encoder_init(connector->dev, encoder, &nv50_pior_func, type,
			 "pior-%04x-%04x", dcbe->hasht, dcbe->hashm);
	drm_encoder_helper_add(encoder, &nv50_pior_help);

	drm_connector_attach_encoder(connector, encoder);

	disp->core->func->pior->get_caps(disp, nv_encoder, ffs(dcbe->or) - 1);
	nv50_outp_dump_caps(drm, nv_encoder);

	return 0;
}

/******************************************************************************
 * Atomic
 *****************************************************************************/

static void
nv50_disp_atomic_commit_core(struct drm_atomic_commit *state, u32 *interlock)
{
	struct drm_dp_mst_topology_mgr *mgr;
	struct drm_dp_mst_topology_state *mst_state;
	struct nouveau_drm *drm = nouveau_drm(state->dev);
	struct nv50_disp *disp = nv50_disp(drm->dev);
	struct nv50_atom *atom = nv50_atom(state);
	struct nv50_core *core = disp->core;
	struct nv50_outp_atom *outp;
	struct nv50_mstm *mstm;
	int i;

	NV_ATOMIC(drm, "commit core %08x\n", interlock[NV50_DISP_INTERLOCK_BASE]);

	for_each_new_mst_mgr_in_state(state, mgr, mst_state, i) {
		mstm = nv50_mstm(mgr);
		if (mstm->modified)
			nv50_mstm_prepare(state, mst_state, mstm);
	}

	core->func->ntfy_init(disp->sync, NV50_DISP_CORE_NTFY);
	core->func->update(core, interlock, true);
	if (core->func->ntfy_wait_done(disp->sync, NV50_DISP_CORE_NTFY,
				       disp->core->chan.base.device))
		NV_ERROR(drm, "core notifier timeout\n");

	for_each_new_mst_mgr_in_state(state, mgr, mst_state, i) {
		mstm = nv50_mstm(mgr);
		if (mstm->modified)
			nv50_mstm_cleanup(state, mst_state, mstm);
	}

	list_for_each_entry(outp, &atom->outp, head) {
		if (outp->encoder->encoder_type != DRM_MODE_ENCODER_DPMST) {
			struct nouveau_encoder *nv_encoder = nouveau_encoder(outp->encoder);

			if (outp->enabled) {
				nv50_audio_enable(outp->encoder, nouveau_crtc(nv_encoder->crtc),
						  nv_encoder->conn, NULL, NULL);
				outp->enabled = outp->disabled = false;
			} else {
				if (outp->disabled) {
					nvif_outp_release(&nv_encoder->outp);
					outp->disabled = false;
				}
			}
		}
	}
}

static void
nv50_disp_atomic_commit_wndw(struct drm_atomic_commit *state, u32 *interlock)
{
	struct drm_plane_state *new_plane_state;
	struct drm_plane *plane;
	int i;

	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw *wndw = nv50_wndw(plane);
		if (interlock[wndw->interlock.type] & wndw->interlock.data) {
			if (wndw->func->update)
				wndw->func->update(wndw, interlock);
		}
	}
}

static void
nv50_disp_atomic_commit_tail(struct drm_atomic_commit *state)
{
	struct drm_device *dev = state->dev;
	struct drm_crtc_state *new_crtc_state, *old_crtc_state;
	struct drm_crtc *crtc;
	struct drm_plane_state *new_plane_state;
	struct drm_plane *plane;
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct nv50_disp *disp = nv50_disp(dev);
	struct nv50_atom *atom = nv50_atom(state);
	struct nv50_core *core = disp->core;
	struct nv50_outp_atom *outp, *outt;
	u32 interlock[NV50_DISP_INTERLOCK__SIZE] = {};
	int i;
	bool flushed = false;

	NV_ATOMIC(drm, "commit %d %d\n", atom->lock_core, atom->flush_disable);
	nv50_crc_atomic_stop_reporting(state);
	drm_atomic_helper_wait_for_fences(dev, state, false);
	drm_atomic_helper_wait_for_dependencies(state);
	drm_dp_mst_atomic_wait_for_dependencies(state);
	drm_atomic_helper_update_legacy_modeset_state(dev, state);
	drm_atomic_helper_calc_timestamping_constants(state);

	if (atom->lock_core)
		mutex_lock(&disp->mutex);

	/* A commit holding the core lock may have no core methods to flush, so
	 * force an update to latch the default tile/phywin clears. Do not retry a
	 * failure: tiles_protect describes init state, and a later attempt could
	 * clear assignments made by this commit.
	 */
	if (disp->tiles_pending && atom->lock_core && core->func->tiles_init) {
		if (core->func->tiles_init(core, disp->tiles_protect))
			drm_warn(dev, "default tile assignments not cleared\n");
		else
			interlock[NV50_DISP_INTERLOCK_CORE] |= 1;
		disp->tiles_pending = false;
	}

	/* Clear non-inherited windows once in this core update. A retry would
	 * reuse the init mask and could clear windows enabled by an
	 * intervening commit.
	 */
	if (disp->wndw_park_pending && atom->lock_core &&
	    core->func->wndw.usage_bounds) {
		unsigned long wndws = disp->wndw_park;
		int w, ret = 0;

		for_each_set_bit(w, &wndws, NV50_DISP_INIT_WNDWS) {
			ret = core->func->wndw.usage_bounds(core, w, 0, 0);
			if (ret)
				break;
		}
		if (ret)
			drm_warn(dev, "unused windows not parked\n");
		else
			interlock[NV50_DISP_INTERLOCK_CORE] |= 1;
		disp->wndw_park_pending = false;
	}

	/* Disable head(s). */
	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		struct nv50_head *head = nv50_head(crtc);

		NV_ATOMIC(drm, "%s: clr %04x (set %04x)\n", crtc->name,
			  asyh->clr.mask, asyh->set.mask);

		if (old_crtc_state->active && !new_crtc_state->active) {
			pm_runtime_put_noidle(dev->dev);
			drm_crtc_vblank_off(crtc);
		}

		if (asyh->clr.mask) {
			nv50_head_flush_clr(head, asyh, atom->flush_disable);
			interlock[NV50_DISP_INTERLOCK_CORE] |= 1;
		}
	}

	/* Disable plane(s). */
	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw_atom *asyw = nv50_wndw_atom(new_plane_state);
		struct nv50_wndw *wndw = nv50_wndw(plane);

		NV_ATOMIC(drm, "%s: clr %02x (set %02x)\n", plane->name,
			  asyw->clr.mask, asyw->set.mask);
		if (!asyw->clr.mask)
			continue;

		nv50_wndw_flush_clr(wndw, interlock, atom->flush_disable, asyw);
	}

	/* Disable output path(s). */
	list_for_each_entry(outp, &atom->outp, head) {
		const struct drm_encoder_helper_funcs *help;
		struct drm_encoder *encoder;

		encoder = outp->encoder;
		help = encoder->helper_private;

		NV_ATOMIC(drm, "%s: clr %02x (set %02x)\n", encoder->name,
			  outp->clr.mask, outp->set.mask);

		if (outp->clr.mask) {
			help->atomic_disable(encoder, state);
			outp->disabled = true;
			interlock[NV50_DISP_INTERLOCK_CORE] |= 1;
		}
	}

	/* Flush disable. */
	if (interlock[NV50_DISP_INTERLOCK_CORE]) {
		if (atom->flush_disable) {
			nv50_disp_atomic_commit_wndw(state, interlock);
			nv50_disp_atomic_commit_core(state, interlock);
			memset(interlock, 0x00, sizeof(interlock));

			flushed = true;
		}
	}

	if (flushed)
		nv50_crc_atomic_release_notifier_contexts(state);
	nv50_crc_atomic_init_notifier_contexts(state);

	/* Update output path(s). */
	list_for_each_entry(outp, &atom->outp, head) {
		const struct drm_encoder_helper_funcs *help;
		struct drm_encoder *encoder;

		encoder = outp->encoder;
		help = encoder->helper_private;

		NV_ATOMIC(drm, "%s: set %02x (clr %02x)\n", encoder->name,
			  outp->set.mask, outp->clr.mask);

		if (outp->set.mask) {
			help->atomic_enable(encoder, state);
			outp->enabled = true;
			interlock[NV50_DISP_INTERLOCK_CORE] = 1;
		}
	}

	/* Update head(s). */
	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		struct nv50_head *head = nv50_head(crtc);

		NV_ATOMIC(drm, "%s: set %04x (clr %04x)\n", crtc->name,
			  asyh->set.mask, asyh->clr.mask);

		if (asyh->set.mask) {
			if (asyh->set.imp && disp->core->func->wndw.usage_bounds) {
				int w;

				for (w = 0; w < 2; w++)
					disp->core->func->wndw.usage_bounds(disp->core,
						head->base.index * 2 + w,
						asyh->imp.prog.formats[w],
						asyh->imp.prog.fetch[w]);

				/* Disable watermarks with the planes, but
				 * defer re-enabling them until after the plane
				 * wait below.
				 */
				if (disp->disp->glitchy_mclk_switch &&
				    disp->core->func->mclk_war &&
				    asyh->imp.prog.mclk_war)
					disp->core->func->mclk_war(disp->core,
						head->base.index, true);
			}
			nv50_head_flush_set(head, asyh);
			interlock[NV50_DISP_INTERLOCK_CORE] = 1;
		}

		if (new_crtc_state->active) {
			if (!old_crtc_state->active) {
				drm_crtc_vblank_on(crtc);
				pm_runtime_get_noresume(dev->dev);
			}
			if (new_crtc_state->event)
				drm_crtc_vblank_get(crtc);
		}
	}

	/* Update window->head assignment.
	 *
	 * This has to happen in an update that's not interlocked with
	 * any window channels to avoid hitting HW error checks.
	 *
	 *TODO: Proper handling of window ownership (Turing apparently
	 *      supports non-fixed mappings).
	 */
	if (core->assign_windows) {
		core->func->wndw.owner(core);
		nv50_disp_atomic_commit_core(state, interlock);
		core->assign_windows = false;
		interlock[NV50_DISP_INTERLOCK_CORE] = 0;
	}

	/* Finish updating head(s)...
	 *
	 * NVD is rather picky about both where window assignments can change,
	 * *and* about certain core and window channel states matching.
	 *
	 * The EFI GOP driver on newer GPUs configures window channels with a
	 * different output format to what we do, and the core channel update
	 * in the assign_windows case above would result in a state mismatch.
	 *
	 * Delay some of the head update until after that point to workaround
	 * the issue.  This only affects the initial modeset.
	 *
	 * TODO: handle this better when adding flexible window mapping
	 */
	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		struct nv50_head *head = nv50_head(crtc);

		NV_ATOMIC(drm, "%s: set %04x (clr %04x)\n", crtc->name,
			  asyh->set.mask, asyh->clr.mask);

		if (asyh->set.mask) {
			nv50_head_flush_set_wndw(head, asyh);
			interlock[NV50_DISP_INTERLOCK_CORE] = 1;
		}
	}

	/* Update plane(s). */
	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw_atom *asyw = nv50_wndw_atom(new_plane_state);
		struct nv50_wndw *wndw = nv50_wndw(plane);

		NV_ATOMIC(drm, "%s: set %02x (clr %02x)\n", plane->name,
			  asyw->set.mask, asyw->clr.mask);
		if ( !asyw->set.mask &&
		    (!asyw->clr.mask || atom->flush_disable))
			continue;

		nv50_wndw_flush_set(wndw, interlock, asyw);
	}

	/* Flush update. */
	nv50_disp_atomic_commit_wndw(state, interlock);

	if (interlock[NV50_DISP_INTERLOCK_CORE]) {
		if (interlock[NV50_DISP_INTERLOCK_BASE] ||
		    interlock[NV50_DISP_INTERLOCK_OVLY] ||
		    interlock[NV50_DISP_INTERLOCK_WNDW] ||
		    !atom->state.legacy_cursor_update)
			nv50_disp_atomic_commit_core(state, interlock);
		else
			disp->core->func->update(disp->core, interlock, false);
	}

	if (atom->lock_core)
		mutex_unlock(&disp->mutex);

	list_for_each_entry_safe(outp, outt, &atom->outp, head) {
		list_del(&outp->head);
		kfree(outp);
	}

	/* Wait for HW to signal completion. */
	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw_atom *asyw = nv50_wndw_atom(new_plane_state);
		struct nv50_wndw *wndw = nv50_wndw(plane);
		int ret = nv50_wndw_wait_armed(wndw, asyw);
		if (ret)
			NV_ERROR(drm, "%s: timeout\n", plane->name);
	}

	/* Re-enabling watermarks before the plane transition could expose the
	 * old scanout to the Turing bug. Defer it to a separate update after
	 * the plane wait, like how OpenRM's post-flip path waits for the main
	 * window to idle.
	 */
	if (disp->disp->glitchy_mclk_switch && core->func->mclk_war) {
		u32 war_interlock[NV50_DISP_INTERLOCK__SIZE] = {};
		bool enable = false;

		mutex_lock(&disp->mutex);
		for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
			const struct nv50_head_atom *armh = nv50_head_atom(old_crtc_state);
			const struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);

			if (armh->imp.prog.mclk_war && !asyh->imp.prog.mclk_war) {
				core->func->mclk_war(core, nv50_head(crtc)->base.index,
						     false);
				enable = true;
			}
		}

		if (enable) {
			core->func->ntfy_init(disp->sync, NV50_DISP_CORE_NTFY);
			core->func->update(core, war_interlock, true);
			if (core->func->ntfy_wait_done(disp->sync, NV50_DISP_CORE_NTFY,
						       core->chan.base.device))
				NV_ERROR(drm, "core notifier timeout\n");
		}
		mutex_unlock(&disp->mutex);
	}

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (new_crtc_state->event) {
			unsigned long flags;
			/* Get correct count/ts if racing with vblank irq */
			if (new_crtc_state->active)
				drm_crtc_accurate_vblank_count(crtc);
			spin_lock_irqsave(&crtc->dev->event_lock, flags);
			drm_crtc_send_vblank_event(crtc, new_crtc_state->event);
			spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

			new_crtc_state->event = NULL;
			if (new_crtc_state->active)
				drm_crtc_vblank_put(crtc);
		}
	}

	nv50_crc_atomic_start_reporting(state);
	if (!flushed)
		nv50_crc_atomic_release_notifier_contexts(state);

	drm_atomic_helper_commit_hw_done(state);
	drm_atomic_helper_cleanup_planes(dev, state);
	drm_atomic_helper_commit_cleanup_done(state);
	drm_atomic_commit_put(state);

	/* Drop the RPM ref we got from nv50_disp_atomic_commit() */
	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);
}

static void
nv50_disp_atomic_commit_work(struct work_struct *work)
{
	struct drm_atomic_commit *state =
		container_of(work, typeof(*state), commit_work);
	nv50_disp_atomic_commit_tail(state);
}

static int
nv50_disp_atomic_commit(struct drm_device *dev,
			struct drm_atomic_commit *state, bool nonblock)
{
	struct drm_plane_state *new_plane_state;
	struct drm_plane *plane;
	int ret, i;

	ret = pm_runtime_get_sync(dev->dev);
	if (ret < 0 && ret != -EACCES) {
		pm_runtime_put_autosuspend(dev->dev);
		return ret;
	}

	ret = drm_atomic_helper_setup_commit(state, nonblock);
	if (ret)
		goto done;

	INIT_WORK(&state->commit_work, nv50_disp_atomic_commit_work);

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret)
		goto done;

	if (!nonblock) {
		ret = drm_atomic_helper_wait_for_fences(dev, state, true);
		if (ret)
			goto err_cleanup;
	}

	ret = drm_atomic_helper_swap_state(state, true);
	if (ret)
		goto err_cleanup;

	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw_atom *asyw = nv50_wndw_atom(new_plane_state);
		struct nv50_wndw *wndw = nv50_wndw(plane);

		if (asyw->set.image)
			nv50_wndw_ntfy_enable(wndw, asyw);
	}

	drm_atomic_commit_get(state);

	/*
	 * Grab another RPM ref for the commit tail, which will release the
	 * ref when it's finished
	 */
	pm_runtime_get_noresume(dev->dev);

	if (nonblock)
		queue_work(system_dfl_wq, &state->commit_work);
	else
		nv50_disp_atomic_commit_tail(state);

err_cleanup:
	if (ret)
		drm_atomic_helper_unprepare_planes(dev, state);
done:
	pm_runtime_put_autosuspend(dev->dev);
	return ret;
}

static struct nv50_outp_atom *
nv50_disp_outp_atomic_add(struct nv50_atom *atom, struct drm_encoder *encoder)
{
	struct nv50_outp_atom *outp;

	list_for_each_entry(outp, &atom->outp, head) {
		if (outp->encoder == encoder)
			return outp;
	}

	outp = kzalloc_obj(*outp);
	if (!outp)
		return ERR_PTR(-ENOMEM);

	list_add(&outp->head, &atom->outp);
	outp->encoder = encoder;
	return outp;
}

static int
nv50_disp_outp_atomic_check_clr(struct nv50_atom *atom,
				struct drm_connector_state *old_connector_state)
{
	struct drm_encoder *encoder = old_connector_state->best_encoder;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_crtc *crtc;
	struct nv50_outp_atom *outp;

	if (!(crtc = old_connector_state->crtc))
		return 0;

	old_crtc_state = drm_atomic_get_old_crtc_state(&atom->state, crtc);
	new_crtc_state = drm_atomic_get_new_crtc_state(&atom->state, crtc);
	if (old_crtc_state->active && drm_atomic_crtc_needs_modeset(new_crtc_state)) {
		outp = nv50_disp_outp_atomic_add(atom, encoder);
		if (IS_ERR(outp))
			return PTR_ERR(outp);

		if (outp->encoder->encoder_type == DRM_MODE_ENCODER_DPMST ||
		    nouveau_encoder(outp->encoder)->dcb->type == DCB_OUTPUT_DP)
			atom->flush_disable = true;
		outp->clr.ctrl = true;
		atom->lock_core = true;
	}

	return 0;
}

static int
nv50_disp_outp_atomic_check_set(struct nv50_atom *atom,
				struct drm_connector_state *connector_state)
{
	struct drm_encoder *encoder = connector_state->best_encoder;
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	struct nv50_outp_atom *outp;

	if (!(crtc = connector_state->crtc))
		return 0;

	new_crtc_state = drm_atomic_get_new_crtc_state(&atom->state, crtc);
	if (new_crtc_state->active && drm_atomic_crtc_needs_modeset(new_crtc_state)) {
		outp = nv50_disp_outp_atomic_add(atom, encoder);
		if (IS_ERR(outp))
			return PTR_ERR(outp);

		outp->set.ctrl = true;
		atom->lock_core = true;
	}

	return 0;
}

/* IMP reports how many tiles each head needs, leaving ownership to the driver.
 * Preserve compatible assignments and allocate scaling heads first so they can
 * use scarce TYPE_0 tiles. If a scaling allocation fails, reclaim TYPE_0 tiles
 * from modeset heads that do not need them, retry the failed head, then refill
 * the donors.
 */
static bool
nv50_head_needs_type0_tiles(const struct nv50_head_atom *asyh)
{
	/* Output scaling needs TYPE_0 tiles. Hardware YUV420 would also require
	 * them, but nouveau does not support it yet.
	 */
	return asyh->view.iW != asyh->view.oW || asyh->view.iH != asyh->view.oH;
}

/* Drop excess or incompatible tiles and any phywins beyond one per retained
 * tile on each window.
 */
static void
nv50_head_mtc_unassign_extra(struct nv50_disp *disp,
			     struct nv50_head_atom *asyh, u8 required)
{
	const bool type0 = nv50_head_needs_type0_tiles(asyh);
	unsigned long mask = asyh->mtc.tiles_mask;
	u8 tiles = 0, kept = 0, w;
	int bit;

	for_each_set_bit(bit, &mask, 8) {
		if (kept >= required)
			break;
		if (type0 && !(disp->tile.type0_tiles & BIT(bit)))
			continue;
		tiles |= BIT(bit);
		kept++;
	}

	asyh->mtc.tiles_mask = tiles;

	for (w = 0; w < 2; w++) {
		u32 phywins = 0;
		u8 kept_pw = 0;

		mask = asyh->mtc.phywins_mask[w];
		for_each_set_bit(bit, &mask, 32) {
			if (kept_pw >= kept)
				break;
			phywins |= BIT(bit);
			kept_pw++;
		}
		asyh->mtc.phywins_mask[w] = phywins;
	}
}

/* Use trimmed tiles first to leave TYPE_0 tiles available for scaling, but
 * allow unscaled heads to use TYPE_0 when the trimmed pool is exhausted.
 */
static u8
nv50_disp_tiles_get(struct nv50_disp *disp, u8 count, bool type0, u8 *free_tiles)
{
	u8 tiles = 0;
	int pass;

	for (pass = 0; pass < 2 && count; pass++) {
		const bool want_type0 = type0 || pass == 1;
		u8 candidates = *free_tiles & ~tiles;

		if (want_type0)
			candidates &= disp->tile.type0_tiles;
		else
			candidates &= ~disp->tile.type0_tiles;

		while (count && candidates) {
			const u8 tile = __ffs(candidates);

			tiles |= BIT(tile);
			candidates &= ~BIT(tile);
			count--;
		}

		if (type0)
			break;
	}

	if (count)
		return 0;

	*free_tiles &= ~tiles;
	return tiles;
}

static int
nv50_head_mtc_assign(struct nv50_disp *disp, struct nv50_head_atom *asyh,
		     u8 required, u8 *free_tiles, u32 *free_phywins)
{
	const bool type0 = nv50_head_needs_type0_tiles(asyh);
	const u8 have = hweight8(asyh->mtc.tiles_mask);
	u8 w;

	if (have < required) {
		const u8 tiles = nv50_disp_tiles_get(disp, required - have,
						     type0, free_tiles);

		if (!tiles)
			return -ENOSPC;
		asyh->mtc.tiles_mask |= tiles;
	}

	/* Each logical window needs one physical window for every tile
	 * assigned to its head.
	 */
	for (w = 0; w < 2; w++) {
		while (hweight32(asyh->mtc.phywins_mask[w]) < required) {
			u32 phywin;

			if (!*free_phywins)
				return -ENOSPC;

			phywin = __ffs(*free_phywins);
			asyh->mtc.phywins_mask[w] |= BIT(phywin);
			*free_phywins &= ~BIT(phywin);
		}
	}

	return 0;
}

/* Modeset heads without scaling can release TYPE_0 tiles and one phywin per
 * tile on each window. Return those resources to the pools so the failed
 * scaling allocation can be retried. The next pass refills the donor heads.
 */
static bool
nv50_disp_tiles_reclaim(struct nv50_disp *disp, struct drm_atomic_commit *state,
			u8 *free_tiles, u32 *free_phywins)
{
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	bool reclaimed = false;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		u8 tiles, w;

		if (!new_crtc_state->active ||
		    !drm_atomic_crtc_needs_modeset(new_crtc_state) ||
		    nv50_head_needs_type0_tiles(asyh))
			continue;

		tiles = asyh->mtc.tiles_mask & disp->tile.type0_tiles;
		if (!tiles)
			continue;

		for (w = 0; w < 2; w++) {
			unsigned long mask = asyh->mtc.phywins_mask[w];
			u32 phywins = 0;
			u8 n = 0;
			int bit;

			for_each_set_bit(bit, &mask, 32) {
				if (n++ >= hweight8(tiles))
					break;
				phywins |= BIT(bit);
			}

			asyh->mtc.phywins_mask[w] &= ~phywins;
			*free_phywins |= phywins;
		}

		asyh->mtc.tiles_mask &= ~tiles;
		*free_tiles |= tiles;
		reclaimed = true;
	}

	return reclaimed;
}

/* Turn the IMP reply's per-head tile counts into ownership. */
static int
nv50_disp_atomic_check_tiles(struct drm_device *dev,
			     struct drm_atomic_commit *state,
			     const struct nvif_disp_imp_check_v0 *imp,
			     const u8 *committed_tiles,
			     const u32 *committed_phywins)
{
	struct nv50_disp *disp = nv50_disp(dev);
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	const struct nvif_disp_imp_check_head_v0 *by_head[8] = {};
	u8 free_tiles = disp->tile.tiles;
	u32 free_phywins = disp->tile.phywins;
	int i, pass, ret;

	for (i = 0; i < imp->num_heads; i++) {
		const struct nvif_disp_imp_check_head_v0 *imph = &imp->head[i];

		if (imph->index >= ARRAY_SIZE(by_head))
			return -EINVAL;
		if (imph->required_tiles < 1)
			return -EINVAL;

		/* The query does not enable DSC or offer a slice mask, so IMP
		 * cannot return a DSC solution and the slice count is unused.
		 */
		by_head[imph->index] = imph;
	}

	/* Remove retained ownership from the free pools after trimming modeset
	 * heads. Resources released here can be reassigned in this commit
	 * because transfers force an early disable update below.
	 */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		const int head_idx = nv50_head(crtc)->base.index;
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);

		if (!new_crtc_state->active)
			continue;

		if (by_head[head_idx] &&
		    drm_atomic_crtc_needs_modeset(new_crtc_state))
			nv50_head_mtc_unassign_extra(disp, asyh,
						     by_head[head_idx]->required_tiles);

		free_tiles &= ~asyh->mtc.tiles_mask;
		free_phywins &= ~(asyh->mtc.phywins_mask[0] |
				  asyh->mtc.phywins_mask[1]);
	}

	/* Allocate scaling heads first so unscaled heads do not consume their
	 * TYPE_0 tiles. Iterating by head index makes the choice repeatable.
	 */
	for (pass = 0; pass < 2; pass++) {
		drm_for_each_crtc(crtc, dev) {
			const int head_idx = nv50_head(crtc)->base.index;
			struct nv50_head_atom *asyh;

			new_crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
			if (!new_crtc_state || !new_crtc_state->active ||
			    !by_head[head_idx] ||
			    !drm_atomic_crtc_needs_modeset(new_crtc_state))
				continue;

			asyh = nv50_head_atom(new_crtc_state);
			if (nv50_head_needs_type0_tiles(asyh) != (pass == 0))
				continue;

			ret = nv50_head_mtc_assign(disp, asyh,
						   by_head[head_idx]->required_tiles,
						   &free_tiles, &free_phywins);
			if (ret == -ENOSPC && pass == 0 &&
			    nv50_disp_tiles_reclaim(disp, state, &free_tiles,
						    &free_phywins))
				ret = nv50_head_mtc_assign(disp, asyh,
							   by_head[head_idx]->required_tiles,
							   &free_tiles, &free_phywins);
			if (ret) {
				drm_dbg_kms(dev,
					    "head-%d: no free tiles for a %d-tile assignment\n",
					    head_idx,
					    by_head[head_idx]->required_tiles);
				return ret;
			}
		}
	}

	/* The hardware cannot transfer tile or phywin ownership between heads
	 * in one update, so flush the old owner's disable before attaching the
	 * new owner.
	 */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		const int head_idx = nv50_head(crtc)->base.index;
		u32 phywins;
		int other;

		if (!drm_atomic_crtc_needs_modeset(new_crtc_state))
			continue;

		phywins = asyh->mtc.phywins_mask[0] | asyh->mtc.phywins_mask[1];

		for (other = 0; other < 8; other++) {
			if (other == head_idx)
				continue;
			if ((committed_tiles[other] & asyh->mtc.tiles_mask) ||
			    (committed_phywins[other] & phywins))
				nv50_atom(state)->flush_disable = true;
		}
	}

	return 0;
}

/* Format class of a framebuffer for the IMP window usage bound. */
static u8
nv50_imp_format_class(const struct drm_framebuffer *fb)
{
	switch (fb->format->format) {
	case DRM_FORMAT_YUYV:
	case DRM_FORMAT_UYVY:
		return NVIF_DISP_IMP_FORMAT_YUV_PACKED_422;
	default:
		break;
	}

	switch (fb->format->cpp[0]) {
	case 1: return NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP;
	case 2: return NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP;
	case 4: return NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP;
	default:
		break;
	}
	return NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP;
}

/* RGB bounds include all formats with equal or lower usage so negotiation
 * cannot drop below a plane's requirement. Packed YUV 4:2:2 uses a separate
 * bound.
 */
static u8
nv50_imp_ceiling(u8 class)
{
	if (class & NVIF_DISP_IMP_FORMAT_YUV_PACKED_422)
		return class;
	return (class << 1) - 1;
}

/* Use the input viewport plus filter and overfetch margins to bound the fetch
 * width at 1:1 input scaling, matching OpenRM's
 * nvGetMaxPixelsFetchedPerLine().
 */
static u16
nv50_imp_fetch_width(const struct nv50_head_atom *asyh)
{
	return ((((u32)asyh->view.iW + 14) * 0x400 + 1023) >> 10) + 8;
}

/* Without negotiated bounds, retain the pre-IMP behavior of allowing all
 * formats.
 */
static u8
nv50_imp_committed(const struct nv50_head_atom *asyh, int w)
{
	return asyh->imp.valid ? asyh->imp.wndw_formats[w] :
				 NVIF_DISP_IMP_FORMAT_ALL;
}

/* Follow OpenRM's downgrade order, removing packed YUV and 64-bpp RGB before
 * reducing taps and the second window's RGB bound. Keep primary RGB formats up
 * to 32 bpp so userspace can fall back to compositing. The notifier exposes
 * only two- and five-tap filtering, so skip the intermediate tap counts.
 */
#define NV50_IMP_RUNG_VTAPS 0xfe
#define NV50_IMP_RUNG_HTAPS 0xff

static const struct nv50_imp_downgrade {
	u8 wndw;
	u8 clr;
} nv50_imp_ladder[] = {
	{ 1, NVIF_DISP_IMP_FORMAT_YUV_PACKED_422 },
	{ 0, NVIF_DISP_IMP_FORMAT_YUV_PACKED_422 },
	{ 1, NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP },
	{ 0, NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP },
	{ NV50_IMP_RUNG_VTAPS, 0 },
	{ NV50_IMP_RUNG_HTAPS, 0 },
	{ 1, NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP },
	{ 1, NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP },
	{ 1, NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP },
};

/* Clear saved bounds for modeset heads when the configuration is empty or
 * lacks timings.
 */
static void
nv50_disp_imp_drop_bounds(struct drm_atomic_commit *state)
{
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	int i;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (drm_atomic_crtc_needs_modeset(new_crtc_state))
			nv50_head_atom(new_crtc_state)->imp.valid = false;
	}
}

/* Flips must fit the last negotiated bounds. For modesets, retry IMP with
 * reduced usage until the configuration fits or no further downgrade is
 * possible. Tiled hardware also needs the reply for resource allocation.
 */
static int
nv50_disp_atomic_check_imp(struct drm_device *dev, struct drm_atomic_commit *state)
{
	struct nv50_disp *disp = nv50_disp(dev);
	const bool static_wndw_map = disp->core->func->head->static_wndw_map != NULL;
	struct nvif_disp_imp_check_v0 imp = {};
	struct nv50_head_atom *asyh_of[8] = {};
	u8 committed_tiles[8] = {};
	u32 committed_phywins[8] = {};
	u8 demand[8][2] = {}, bounds[8][2] = {};
	u8 prog_seen[8] = {}, prog_class[8][2] = {};
	u8 modeset_heads = 0;
	struct drm_plane *plane;
	struct drm_plane_state *new_plane_state;
	struct drm_crtc_state *old_crtc_state, *new_crtc_state;
	struct drm_crtc *crtc;
	bool any_modeset = false;
	int ret, i, w;

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		/* Head indices must fit both the local ownership arrays and
		 * the IMP request.
		 */
		if (nv50_head(crtc)->base.index >= ARRAY_SIZE(committed_tiles))
			return -EINVAL;

		if (drm_atomic_crtc_needs_modeset(new_crtc_state))
			any_modeset = true;
	}

	/* Collect plane requirements before the IMP bypasses so flips remain
	 * constrained by committed bounds even when no query runs. Untouched
	 * planes already fit those bounds.
	 */
	for_each_new_plane_in_state(state, plane, new_plane_state, i) {
		struct nv50_wndw *wndw;
		u8 cls;

		if (plane->type == DRM_PLANE_TYPE_CURSOR)
			continue;

		wndw = nv50_wndw(plane);
		/* Only the fixed window mapping makes id / 2 a head index.
		 * Older base/overlay IDs would merge distinct windows into one
		 * demand slot.
		 */
		if (!static_wndw_map || wndw->id / 2 >= ARRAY_SIZE(demand))
			continue;

		prog_seen[wndw->id / 2] |= BIT(wndw->id & 1);
		/* An enabled off-screen window still consumes resources, even
		 * when DRM clears plane_state->visible. Use the window atom's
		 * visibility instead.
		 */
		if (!nv50_wndw_atom(new_plane_state)->visible ||
		    !new_plane_state->fb)
			continue;

		cls = nv50_imp_format_class(new_plane_state->fb);
		prog_class[wndw->id / 2][wndw->id & 1] = cls;
		demand[wndw->id / 2][wndw->id & 1] |= cls;
	}

	/* A flip cannot exceed the last negotiated bounds without a modeset.
	 * Heads with no valid bounds keep the permissive default, including
	 * when IMP is disabled or unsupported.
	 */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		const int head_idx = nv50_head(crtc)->base.index;

		if (drm_atomic_crtc_needs_modeset(new_crtc_state))
			continue;

		for (w = 0; w < 2; w++) {
			if (demand[head_idx][w] & ~nv50_imp_committed(asyh, w)) {
				drm_dbg_kms(dev,
					    "head-%d wndw-%d: format beyond the committed IMP bound\n",
					    head_idx, w);
				return -EINVAL;
			}
		}
	}

	/* Raise and lower usage bounds with the planes in the same interlocked
	 * update. OpenRM instead raises them before a flip and lowers them
	 * after the window channel idles.
	 */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nv50_head_atom *asyh = nv50_head_atom(new_crtc_state);
		const int head_idx = nv50_head(crtc)->base.index;

		if (!disp->core->func->wndw.usage_bounds)
			break;

		for (w = 0; w < 2; w++) {
			u8 formats = asyh->imp.prog.formats[w];
			u16 fetch;

			if (!new_crtc_state->active)
				formats = 0;
			else if (prog_seen[head_idx] & BIT(w))
				formats = prog_class[head_idx][w] ?
					nv50_imp_ceiling(prog_class[head_idx][w]) : 0;
			else if (drm_atomic_crtc_needs_modeset(new_crtc_state))
				/* A modeset includes every plane on the head,
				 * so an unseen window is unused.
				 */
				formats = 0;
			fetch = formats ? nv50_imp_fetch_width(asyh) : 0;

			if (formats == asyh->imp.prog.formats[w] &&
			    fetch == asyh->imp.prog.fetch[w])
				continue;

			asyh->imp.prog.formats[w] = formats;
			asyh->imp.prog.fetch[w] = fetch;
			asyh->set.imp = true;
			/* Changing usage bounds writes the core channel and
			 * therefore requires the core lock.
			 */
			nv50_atom(state)->lock_core = true;
		}

		/* Without a primary window, either an attached cursor or an
		 * active overlay requires the Turing workaround. Test cursor
		 * surface attachment rather than visibility, matching OpenRM's
		 * treatment of off-screen cursors.
		 */
		if (disp->disp->glitchy_mclk_switch && disp->core->func->mclk_war) {
			bool war = new_crtc_state->active &&
				   !asyh->imp.prog.formats[0] &&
				   (asyh->curs.surface || asyh->imp.prog.formats[1]);

			if (war != asyh->imp.prog.mclk_war) {
				asyh->imp.prog.mclk_war = war;
				asyh->set.imp = true;
				nv50_atom(state)->lock_core = true;
			}
		}
	}

	/* Only modesets renegotiate bounds, so flips within committed bounds
	 * need no query.
	 */
	if (!any_modeset)
		return 0;

	/* Every head is part of the query, inherited ones included. */
	drm_for_each_crtc(crtc, dev) {
		new_crtc_state = drm_atomic_get_crtc_state(state, crtc);
		if (IS_ERR(new_crtc_state))
			return PTR_ERR(new_crtc_state);
	}

	/* nvdisplay does not support interlaced scanout, and IMP has no
	 * second-field timings. Reject interlaced rasters even when IMP
	 * validation is bypassed.
	 */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (new_crtc_state->active &&
		    nv50_head_atom(new_crtc_state)->mode.interlace)
			return -EINVAL;
	}

	if (!nouveau_imp_check && !disp->tile.nr_tiles)
		return 0;

	/* Firmware without the IMP API. */
	if (disp->imp_unsupported)
		return 0;

	/* Save current ownership so the new assignment can be checked for
	 * transfers that require a separate disable update.
	 */
	for_each_oldnew_crtc_in_state(state, crtc, old_crtc_state, new_crtc_state, i) {
		const struct nv50_head_atom *armh = nv50_head_atom(old_crtc_state);
		const int head_idx = nv50_head(crtc)->base.index;

		/* The state now includes CRTCs added after the first bounds
		 * check.
		 */
		if (head_idx >= ARRAY_SIZE(committed_tiles))
			return -EINVAL;

		committed_tiles[head_idx] = armh->mtc.tiles_mask;
		committed_phywins[head_idx] = armh->mtc.phywins_mask[0] |
					      armh->mtc.phywins_mask[1];
	}

	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		struct nvif_disp_imp_check_head_v0 *imph;
		struct nv50_head_atom *asyh;
		const int head_idx = nv50_head(crtc)->base.index;
		const bool modeset = drm_atomic_crtc_needs_modeset(new_crtc_state);

		if (!new_crtc_state->active) {
			/* The inactive head no longer uses this
			 * configuration's bounds.
			 */
			nv50_head_atom(new_crtc_state)->imp.valid = false;
			continue;
		}

		asyh = nv50_head_atom(new_crtc_state);
		asyh_of[head_idx] = asyh;
		if (modeset)
			modeset_heads |= BIT(head_idx);

		/* Include inherited heads using their armed timings. Omitting a
		 * live head would let IMP accept configurations that exceed the
		 * available resources.
		 */
		if (!asyh->mode.clock) {
			drm_warn_once(dev, "head-%d active but undescribable\n", head_idx);

			/* Tile allocation needs a complete query, so fail if
			 * an active head cannot be described. On hardware
			 * without tiles, skip IMP rather than blocking all
			 * modesets on that head's missing state.
			 */
			if (disp->tile.nr_tiles)
				return -EINVAL;
			nv50_disp_imp_drop_bounds(state);
			return 0;
		}

		/* Start modeset heads at full bounds to recover capabilities
		 * that the new mode may allow, while keeping other heads
		 * within their committed bounds.
		 */
		for (w = 0; w < 2; w++) {
			if (modeset)
				bounds[head_idx][w] = NVIF_DISP_IMP_FORMAT_ALL;
			else
				bounds[head_idx][w] = nv50_imp_committed(asyh, w);
		}

		imph = &imp.head[imp.num_heads++];
		imph->index = head_idx;
		imph->vtaps = asyh->view.vtaps;
		/* A nonzero tile mask constrains IMP to the current
		 * assignment, which heads outside the modeset must retain.
		 * Leave modeset masks zero so IMP can choose their tile
		 * counts.
		 */
		if (disp->tile.nr_tiles && !modeset) {
			if (!asyh->mtc.tiles_mask)
				return -EINVAL;
			imph->tile_mask = asyh->mtc.tiles_mask;
		}
		imph->pclk_khz = asyh->mode.clock;
		imph->htotal = asyh->mode.h.active;
		imph->vtotal = asyh->mode.v.active;
		imph->hblanks = asyh->mode.h.blanks;
		imph->hblanke = asyh->mode.h.blanke;
		imph->vblanks = asyh->mode.v.blanks;
		imph->vblanke = asyh->mode.v.blanke;
		imph->in_w = asyh->view.iW;
		imph->in_h = asyh->view.iH;
		imph->out_w = asyh->view.oW;
		imph->out_h = asyh->view.oH;
	}

	if (!imp.num_heads) {
		nv50_disp_imp_drop_bounds(state);
		return 0;
	}

	imp.tiled = disp->tile.nr_tiles > 0;

	/* Client restore commits can run outside an ioctl with no runtime PM
	 * reference, so acquire one for the query.
	 */
	ret = nv50_disp_pm_get(dev);
	if (ret) {
		drm_warn_once(dev, "IMP check unavailable (%d), rejecting\n", ret);
		return ret;
	}

	for (;;) {
		bool changed = false;
		unsigned int rung;

		for (i = 0; i < imp.num_heads; i++) {
			struct nvif_disp_imp_check_head_v0 *imph = &imp.head[i];

			imph->vtaps = asyh_of[imph->index]->view.vtaps;
			for (w = 0; w < 2; w++)
				imph->wndw_formats[w] = bounds[imph->index][w];
		}

		ret = nvif_disp_imp_check(disp->disp, &imp);
		if (ret || imp.possible)
			break;

		/* Apply only the first eligible (rung, head) downgrade before
		 * retrying IMP, restarting from the top each time to preserve
		 * OpenRM's order. Never remove formats needed by this commit's
		 * planes.
		 */
		for (rung = 0; rung < ARRAY_SIZE(nv50_imp_ladder) && !changed; rung++) {
			const struct nv50_imp_downgrade *dg = &nv50_imp_ladder[rung];
			unsigned long mheads = modeset_heads;
			int h;

			for_each_set_bit(h, &mheads, 8) {
				struct nv50_head_atom *asyh = asyh_of[h];
				const struct nv50_scaler_caps *scaler =
					&disp->scaler[h];

				if (dg->wndw == NV50_IMP_RUNG_VTAPS) {
					if (asyh->view.vtaps != 5 ||
					    min(asyh->view.iW, asyh->view.oW) >
						scaler->taps2.max_pixels ||
					    asyh->view.max_v > scaler->taps2.max_v)
						continue;
					asyh->view.vtaps = 2;
					asyh->set.view = true;
				} else if (dg->wndw == NV50_IMP_RUNG_HTAPS) {
					if (asyh->view.htaps != 5 ||
					    asyh->view.max_h > scaler->taps2.max_h)
						continue;
					asyh->view.htaps = 2;
					asyh->set.view = true;
				} else {
					if (!(bounds[h][dg->wndw] & dg->clr) ||
					    (demand[h][dg->wndw] &&
					     (nv50_imp_ceiling(demand[h][dg->wndw]) & dg->clr)))
						continue;
					bounds[h][dg->wndw] &= ~dg->clr;
				}
				nv50_atom(state)->lock_core = true;
				changed = true;
				break;
			}
		}

		if (!changed)
			break;
	}
	pm_runtime_mark_last_busy(dev->dev);
	pm_runtime_put_autosuspend(dev->dev);

	if (ret == -ENODEV) {
		disp->imp_unsupported = true;
		return 0;
	}
	if (ret) {
		drm_warn_once(dev, "IMP check failed (%d), rejecting\n", ret);
		return -EINVAL;
	}
	if (!imp.possible) {
		drm_dbg_kms(dev, "IMP rejected the configuration\n");
		return -EINVAL;
	}

	/* Keep the accepted bounds in head state so later flips can be checked
	 * without another IMP query.
	 */
	for (i = 0; i < imp.num_heads; i++) {
		const int h = imp.head[i].index;
		struct nv50_head_atom *asyh = asyh_of[h];

		if (!asyh || !(modeset_heads & BIT(h)))
			continue;

		asyh->imp.wndw_formats[0] = bounds[h][0];
		asyh->imp.wndw_formats[1] = bounds[h][1];
		asyh->imp.valid = true;
	}

	if (!disp->tile.nr_tiles)
		return 0;

	return nv50_disp_atomic_check_tiles(dev, state, &imp, committed_tiles,
					    committed_phywins);
}

static int
nv50_disp_atomic_check(struct drm_device *dev, struct drm_atomic_commit *state)
{
	struct nv50_atom *atom = nv50_atom(state);
	struct nv50_core *core = nv50_disp(dev)->core;
	struct drm_connector_state *old_connector_state, *new_connector_state;
	struct drm_connector *connector;
	struct drm_crtc_state *new_crtc_state;
	struct drm_crtc *crtc;
	struct nv50_head *head;
	struct nv50_head_atom *asyh;
	int ret, i;

	if (core->assign_windows && core->func->head->static_wndw_map) {
		drm_for_each_crtc(crtc, dev) {
			new_crtc_state = drm_atomic_get_crtc_state(state,
								   crtc);
			if (IS_ERR(new_crtc_state))
				return PTR_ERR(new_crtc_state);

			head = nv50_head(crtc);
			asyh = nv50_head_atom(new_crtc_state);
			core->func->head->static_wndw_map(head, asyh);
		}
	}

	/* We need to handle colour management on a per-plane basis. */
	for_each_new_crtc_in_state(state, crtc, new_crtc_state, i) {
		if (new_crtc_state->color_mgmt_changed) {
			ret = drm_atomic_add_affected_planes(state, crtc);
			if (ret)
				return ret;
		}
	}

	ret = drm_atomic_helper_check(dev, state);
	if (ret)
		return ret;

	ret = nv50_disp_atomic_check_imp(dev, state);
	if (ret)
		return ret;

	for_each_oldnew_connector_in_state(state, connector, old_connector_state, new_connector_state, i) {
		ret = nv50_disp_outp_atomic_check_clr(atom, old_connector_state);
		if (ret)
			return ret;

		ret = nv50_disp_outp_atomic_check_set(atom, new_connector_state);
		if (ret)
			return ret;
	}

	ret = drm_dp_mst_atomic_check(state);
	if (ret)
		return ret;

	nv50_crc_atomic_check_outp(atom);

	return 0;
}

static void
nv50_disp_atomic_state_clear(struct drm_atomic_commit *state)
{
	struct nv50_atom *atom = nv50_atom(state);
	struct nv50_outp_atom *outp, *outt;

	list_for_each_entry_safe(outp, outt, &atom->outp, head) {
		list_del(&outp->head);
		kfree(outp);
	}

	drm_atomic_commit_default_clear(state);
}

static void
nv50_disp_atomic_state_free(struct drm_atomic_commit *state)
{
	struct nv50_atom *atom = nv50_atom(state);
	drm_atomic_commit_default_release(&atom->state);
	kfree(atom);
}

static struct drm_atomic_commit *
nv50_disp_atomic_state_alloc(struct drm_device *dev)
{
	struct nv50_atom *atom;
	if (!(atom = kzalloc_obj(*atom)) ||
	    drm_atomic_commit_init(dev, &atom->state) < 0) {
		kfree(atom);
		return NULL;
	}
	INIT_LIST_HEAD(&atom->outp);
	return &atom->state;
}

static const struct drm_mode_config_funcs
nv50_disp_func = {
	.fb_create = nouveau_user_framebuffer_create,
	.atomic_check = nv50_disp_atomic_check,
	.atomic_commit = nv50_disp_atomic_commit,
	.atomic_state_alloc = nv50_disp_atomic_state_alloc,
	.atomic_state_clear = nv50_disp_atomic_state_clear,
	.atomic_state_free = nv50_disp_atomic_state_free,
};

static const struct drm_mode_config_helper_funcs
nv50_disp_helper_func = {
	.atomic_commit_setup = drm_dp_mst_atomic_setup_commit,
};

/******************************************************************************
 * Init
 *****************************************************************************/

static void
nv50_display_fini(struct drm_device *dev, bool runtime, bool suspend)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct drm_encoder *encoder;

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		if (encoder->encoder_type != DRM_MODE_ENCODER_DPMST)
			nv50_mstm_fini(nouveau_encoder(encoder));
	}

	if (!runtime && !drm->headless)
		cancel_work_sync(&drm->hpd_work);
}

/* The head atom does not yet describe firmware scanout, so recover its
 * raster and viewport to include it in IMP validation.
 *
 * Derived fields such as blankus, the second interlace blanking region, and
 * scaler usage bounds remain zero. The mode/view checks recompute these
 * before setting the flags that cause them to be programmed.
 */
static int
nv50_head_armed_read(struct drm_device *dev, struct nouveau_crtc *nv_crtc,
		     struct nv50_head_atom *armh)
{
	struct nvif_head_armed_v0 args;
	int ret;

	/* -ENODEV indicates missing raster timings or pixel clock. */
	ret = nvif_head_armed(&nv_crtc->head, &args);
	if (ret) {
		if (ret != -ENODEV)
			drm_warn(dev, "head-%d is active but its raster could not be read (%d), IMP cannot describe it until it is modeset\n",
				 nv_crtc->index, ret);
		return ret;
	}

	armh->or.nhsync = args.nhsync;
	armh->or.nvsync = args.nvsync;
	armh->mtc.tiles_mask = args.tiles_mask;
	armh->mtc.phywins_mask[0] = args.phywins[0];
	armh->mtc.phywins_mask[1] = args.phywins[1];
	armh->mode.clock = div_u64(args.hz, 1000);
	armh->mode.interlace = args.interlace;
	armh->mode.h.active = args.htotal;
	armh->mode.h.synce = args.hsynce;
	armh->mode.h.blanke = args.hblanke;
	armh->mode.h.blanks = args.hblanks;
	armh->mode.v.active = args.vtotal;
	armh->mode.v.synce = args.vsynce;
	armh->mode.v.blanke = args.vblanke;
	armh->mode.v.blanks = args.vblanks;

	if (args.view) {
		armh->view.iW = args.iW;
		armh->view.iH = args.iH;
		armh->view.oW = args.oW;
		armh->view.oH = args.oH;
		armh->view.vtaps = args.vtaps;
		armh->view.htaps = args.htaps;
	}

	drm_dbg_kms(dev, "head-%d inherited: raster %ux%u, %u kHz, viewport %ux%u -> %ux%u\n",
		    nv_crtc->index, args.htotal, args.vtotal, armh->mode.clock,
		    args.iW, args.iH, args.oW, args.oH);
	return 0;
}

/* An enabled CRTC needs a mode blob for updates without a modeset. Rebuild
 * it from the armed raster, but leave it unset when viewport readback
 * reports scaling because the input mode cannot be recovered from output
 * timings. Older classes lack viewport readback, so this scaling check is
 * only possible on Volta and later.
 */
static bool
nv50_head_armed_mode(const struct nv50_head_atom *armh,
		     struct drm_display_mode *mode)
{
	const struct nv50_head_mode *m = &armh->mode;
	u32 vtotal = m->v.active;

	if (!m->clock || m->h.active <= m->h.blanke + 1 ||
	    m->h.blanks <= m->h.blanke)
		return false;

	if (armh->view.iW && (armh->view.iW != armh->view.oW ||
			      armh->view.iH != armh->view.oH))
		return false;

	if (m->interlace)
		vtotal = (m->v.active - 1) / 2;

	if (vtotal <= m->v.blanke + 1 || m->v.blanks <= m->v.blanke)
		return false;

	memset(mode, 0, sizeof(*mode));
	mode->clock = m->clock;
	mode->htotal = m->h.active;
	mode->hsync_start = m->h.active - m->h.blanke - 1;
	mode->hsync_end = mode->hsync_start + m->h.synce + 1;
	mode->hdisplay = m->h.blanks - m->h.blanke;
	mode->vtotal = vtotal;
	mode->vsync_start = vtotal - m->v.blanke - 1;
	mode->vsync_end = mode->vsync_start + m->v.synce + 1;
	mode->vdisplay = m->v.blanks - m->v.blanke;

	if (m->interlace) {
		mode->vtotal *= 2;
		mode->vsync_start *= 2;
		mode->vsync_end *= 2;
		mode->vdisplay *= 2;
		mode->flags |= DRM_MODE_FLAG_INTERLACE;
	}

	if (mode->hsync_end > mode->htotal || mode->vsync_end > mode->vtotal)
		return false;

	mode->flags |= armh->or.nhsync ? DRM_MODE_FLAG_NHSYNC : DRM_MODE_FLAG_PHSYNC;
	mode->flags |= armh->or.nvsync ? DRM_MODE_FLAG_NVSYNC : DRM_MODE_FLAG_PVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER;
	drm_mode_set_name(mode);
	drm_mode_set_crtcinfo(mode, CRTC_INTERLACE_HALVE_V | CRTC_STEREO_DOUBLE);
	return true;
}

static inline void
nv50_display_read_hw_or_state(struct drm_device *dev, struct nv50_disp *disp,
			      struct nouveau_encoder *outp)
{
	struct drm_crtc *crtc;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *conn;
	struct nv50_head_atom *armh;
	struct drm_display_mode mode;
	const u32 encoder_mask = drm_encoder_mask(&outp->base.base);
	bool found_conn = false, found_head = false;
	u8 proto;
	int head_idx;
	int ret;

	switch (outp->dcb->type) {
	case DCB_OUTPUT_TMDS:
		ret = nvif_outp_inherit_tmds(&outp->outp, &proto);
		break;
	case DCB_OUTPUT_DP:
		ret = nvif_outp_inherit_dp(&outp->outp, &proto);
		break;
	case DCB_OUTPUT_LVDS:
		ret = nvif_outp_inherit_lvds(&outp->outp, &proto);
		break;
	case DCB_OUTPUT_ANALOG:
		ret = nvif_outp_inherit_rgb_crt(&outp->outp, &proto);
		break;
	default:
		drm_dbg_kms(dev, "Readback for %s not implemented yet, skipping\n",
			    outp->base.base.name);
		drm_WARN_ON(dev, true);
		return;
	}

	if (ret < 0)
		return;

	head_idx = ret;

	drm_for_each_crtc(crtc, dev) {
		if (crtc->index != head_idx)
			continue;

		armh = nv50_head_atom(crtc->state);
		found_head = true;
		break;
	}
	if (drm_WARN_ON(dev, !found_head))
		return;

	/* Figure out which connector is being used by this encoder */
	drm_connector_list_iter_begin(dev, &conn_iter);
	nouveau_for_each_non_mst_connector_iter(conn, &conn_iter) {
		if (nouveau_connector(conn)->index == outp->dcb->connector) {
			found_conn = true;
			break;
		}
	}
	drm_connector_list_iter_end(&conn_iter);
	if (drm_WARN_ON(dev, !found_conn))
		return;

	/* An OR can still name a head with no raster or pixel clock. Unless its
	 * armed timings can be read, leave the head inactive and release the OR
	 * acquired during inheritance so the first modeset can acquire it again.
	 */
	if (nv50_head_armed_read(dev, nouveau_crtc(crtc), armh)) {
		drm_dbg_kms(dev, "%s: head-%d not inherited\n",
			    outp->base.base.name, crtc->index);
		nvif_outp_release(&outp->outp);
		return;
	}

	if (nv50_head_armed_mode(armh, &mode) &&
	    !drm_atomic_set_mode_for_crtc(&armh->state, &mode))
		drm_mode_copy(&armh->state.adjusted_mode, &mode);

	armh->state.encoder_mask |= encoder_mask;
	armh->state.connector_mask |= drm_connector_mask(conn);
	armh->state.active = true;
	armh->state.enable = true;
	pm_runtime_get_noresume(dev->dev);

	outp->crtc = crtc;
	outp->ctrl = NVVAL(NV507D, SOR_SET_CONTROL, PROTOCOL, proto) | BIT(crtc->index);

	drm_connector_get(conn);
	conn->state->crtc = crtc;
	conn->state->best_encoder = &outp->base.base;
}

/* Read back the currently programmed display state */
static void
nv50_display_read_hw_state(struct nouveau_drm *drm)
{
	struct drm_device *dev = drm->dev;
	struct drm_encoder *encoder;
	struct drm_plane *plane;
	struct drm_modeset_acquire_ctx ctx;
	struct nv50_disp *disp = nv50_disp(dev);
	int ret;

	DRM_MODESET_LOCK_ALL_BEGIN(dev, ctx, 0, ret);

	drm_for_each_encoder(encoder, dev) {
		if (encoder->encoder_type == DRM_MODE_ENCODER_DPMST)
			continue;

		nv50_display_read_hw_or_state(dev, disp, nouveau_encoder(encoder));
	}

	drm_for_each_plane(plane, dev)
		nv50_wndw_default_state(nv50_wndw(plane));

	DRM_MODESET_LOCK_ALL_END(dev, ctx, ret);
}

static int
nv50_display_init(struct drm_device *dev, bool resume, bool runtime)
{
	struct nv50_disp *disp = nv50_disp(dev);
	struct nv50_core *core = disp->core;
	struct drm_encoder *encoder;

	if (resume || runtime)
		core->func->init(core);

	list_for_each_entry(encoder, &dev->mode_config.encoder_list, head) {
		if (encoder->encoder_type != DRM_MODE_ENCODER_DPMST) {
			struct nouveau_encoder *nv_encoder =
				nouveau_encoder(encoder);
			nv50_mstm_init(nv_encoder, runtime);
		}
	}

	if (!resume)
		nv50_display_read_hw_state(nouveau_drm(dev));

	/* OpenRM clears window bounds at the first modeset after core
	 * allocation. Preserve the initial bounds of inherited heads here
	 * because they are already scanning out, seeding tracking so later
	 * plane updates can replace them. No scanout is inherited on resume,
	 * so all windows can be cleared before restore.
	 */
	if (core->func->wndw.usage_bounds) {
		struct drm_crtc *crtc;
		u32 park = GENMASK(NV50_DISP_INIT_WNDWS - 1, 0);

		drm_for_each_crtc(crtc, dev) {
			const bool inherited = !resume && !runtime &&
					       crtc->state && crtc->state->active;
			const int head_idx = nv50_head(crtc)->base.index;
			struct nv50_head_atom *armh;
			int w;

			if (!crtc->state)
				continue;

			if (inherited)
				park &= ~(BIT(head_idx * 2) | BIT(head_idx * 2 + 1));

			armh = nv50_head_atom(crtc->state);
			for (w = 0; w < 2; w++) {
				armh->imp.prog.formats[w] = inherited ?
					NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP |
					NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP |
					NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP |
					NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP : 0;
				armh->imp.prog.fetch[w] = inherited ? 0x7fff : 0;
			}
		}

		disp->wndw_park = park;
		disp->wndw_park_pending = park != 0;
	}

	if (core->func->tiles_init) {
		struct drm_crtc *crtc;
		u32 protect = 0;

		/* Inherited heads must retain the assignments they are scanning out with.
		 * Use active CRTC state from the output query to identify them, since
		 * unused heads may retain stale rasters. On resume all heads are off, so
		 * the restore commit can clear every default before reprogramming them.
		 */
		if (!resume && !runtime) {
			drm_for_each_crtc(crtc, dev) {
				struct nv50_head_atom *armh;
				int i = nv50_head(crtc)->base.index;

				if (!crtc->state || !crtc->state->active)
					continue;

				armh = nv50_head_atom(crtc->state);
				if (!armh->mtc.tiles_mask) {
					drm_warn(dev, "head-%d: tile ownership not read back, assuming the identity default\n",
						 i);
					armh->mtc.tiles_mask = BIT(i);
					armh->mtc.phywins_mask[0] = BIT(i * 2);
					armh->mtc.phywins_mask[1] = BIT(i * 2 + 1);
				}
				protect |= BIT(i);
			}
		}

		disp->tiles_protect = protect;
		disp->tiles_pending = true;
	}

	return 0;
}

static void
nv50_display_destroy(struct drm_device *dev)
{
	struct nv50_disp *disp = nv50_disp(dev);

	nv50_audio_component_fini(nouveau_drm(dev));

	nvif_object_unmap(&disp->caps);
	nvif_object_dtor(&disp->caps);
	nv50_core_del(&disp->core);

	nouveau_bo_unpin_del(&disp->sync);

	nouveau_display(dev)->priv = NULL;
	kfree(disp);
}

int
nv50_display_create(struct drm_device *dev)
{
	struct nouveau_drm *drm = nouveau_drm(dev);
	struct drm_connector *connector, *tmp;
	struct nv50_disp *disp;
	int ret, i;
	bool has_mst = false;

	disp = kzalloc_obj(*disp);
	if (!disp)
		return -ENOMEM;

	mutex_init(&disp->mutex);

	nouveau_display(dev)->priv = disp;
	nouveau_display(dev)->dtor = nv50_display_destroy;
	nouveau_display(dev)->init = nv50_display_init;
	nouveau_display(dev)->fini = nv50_display_fini;
	disp->disp = &nouveau_display(dev)->disp;
	dev->mode_config.funcs = &nv50_disp_func;
	dev->mode_config.helper_private = &nv50_disp_helper_func;
	dev->mode_config.quirk_addfb_prefer_xbgr_30bpp = true;
	dev->mode_config.normalize_zpos = true;

	/* small shared memory area we use for notifiers and semaphores */
	ret = nouveau_bo_new_map(&drm->client, NOUVEAU_GEM_DOMAIN_VRAM, PAGE_SIZE, &disp->sync);
	if (ret)
		goto out;

	/* allocate master evo channel */
	ret = nv50_core_new(drm, &disp->core);
	if (ret)
		goto out;

	disp->core->func->init(disp->core);
	if (disp->core->func->caps_init) {
		ret = disp->core->func->caps_init(drm, disp);
		if (ret)
			goto out;
	}

	/* Assign the correct format modifiers */
	if (disp->disp->object.oclass >= GB202_DISP)
		nouveau_display(dev)->format_modifiers = wndwca7e_modifiers;
	else if (disp->disp->object.oclass >= TU102_DISP)
		nouveau_display(dev)->format_modifiers = wndwc57e_modifiers;
	else
	if (drm->client.device.info.family >= NV_DEVICE_INFO_V0_FERMI)
		nouveau_display(dev)->format_modifiers = disp90xx_modifiers;
	else
		nouveau_display(dev)->format_modifiers = disp50xx_modifiers;

	/* FIXME: 256x256 cursors are supported on Kepler, however unlike Maxwell and later
	 * generations Kepler requires that we use small pages (4K) for cursor scanout surfaces. The
	 * proper fix for this is to teach nouveau to migrate fbs being used for the cursor plane to
	 * small page allocations in prepare_fb(). When this is implemented, we should also force
	 * large pages (128K) for ovly fbs in order to fix Kepler ovlys.
	 * But until then, just limit cursors to 128x128 - which is small enough to avoid ever using
	 * large pages.
	 */
	if (disp->disp->object.oclass >= GM107_DISP) {
		dev->mode_config.cursor_width = 256;
		dev->mode_config.cursor_height = 256;
	} else if (disp->disp->object.oclass >= GK104_DISP) {
		dev->mode_config.cursor_width = 128;
		dev->mode_config.cursor_height = 128;
	} else {
		dev->mode_config.cursor_width = 64;
		dev->mode_config.cursor_height = 64;
	}

	/* create encoder/connector objects based on VBIOS DCB table */
	for_each_set_bit(i, &disp->disp->outp_mask, sizeof(disp->disp->outp_mask) * 8) {
		struct nouveau_encoder *outp;

		outp = kzalloc_obj(*outp);
		if (!outp)
			break;

		ret = nvif_outp_ctor(disp->disp, "kmsOutp", i, &outp->outp);
		if (ret) {
			kfree(outp);
			continue;
		}

		connector = nouveau_connector_create(dev, outp->outp.info.conn);
		if (IS_ERR(connector)) {
			nvif_outp_dtor(&outp->outp);
			kfree(outp);
			continue;
		}

		outp->base.base.possible_crtcs = outp->outp.info.heads;
		outp->base.base.possible_clones = 0;
		outp->conn = nouveau_connector(connector);

		outp->dcb = kzalloc_obj(*outp->dcb);
		if (!outp->dcb)
			break;

		switch (outp->outp.info.proto) {
		case NVIF_OUTP_RGB_CRT:
			outp->dcb->type = DCB_OUTPUT_ANALOG;
			outp->dcb->crtconf.maxfreq = outp->outp.info.rgb_crt.freq_max;
			break;
		case NVIF_OUTP_TMDS:
			outp->dcb->type = DCB_OUTPUT_TMDS;
			outp->dcb->duallink_possible = outp->outp.info.tmds.dual;
			break;
		case NVIF_OUTP_LVDS:
			outp->dcb->type = DCB_OUTPUT_LVDS;
			outp->dcb->lvdsconf.use_acpi_for_edid = outp->outp.info.lvds.acpi_edid;
			break;
		case NVIF_OUTP_DP:
			outp->dcb->type = DCB_OUTPUT_DP;
			outp->dcb->dpconf.link_nr = outp->outp.info.dp.link_nr;
			outp->dcb->dpconf.link_bw = outp->outp.info.dp.link_bw;
			if (outp->outp.info.dp.mst)
				has_mst = true;
			break;
		default:
			WARN_ON(1);
			continue;
		}

		outp->dcb->heads = outp->outp.info.heads;
		outp->dcb->connector = outp->outp.info.conn;
		outp->dcb->i2c_index = outp->outp.info.ddc;

		switch (outp->outp.info.type) {
		case NVIF_OUTP_DAC : ret = nv50_dac_create(outp); break;
		case NVIF_OUTP_SOR : ret = nv50_sor_create(outp); break;
		case NVIF_OUTP_PIOR: ret = nv50_pior_create(outp); break;
		default:
			WARN_ON(1);
			continue;
		}

		if (ret) {
			NV_WARN(drm, "failed to create encoder %d/%d/%d: %d\n",
				i, outp->outp.info.type, outp->outp.info.proto, ret);
		}
	}

	/* cull any connectors we created that don't have an encoder */
	list_for_each_entry_safe(connector, tmp, &dev->mode_config.connector_list, head) {
		if (connector->possible_encoders)
			continue;

		NV_WARN(drm, "%s has no encoders, removing\n",
			connector->name);
		connector->funcs->destroy(connector);
	}

	/* create crtc objects to represent the hw heads */
	for_each_set_bit(i, &disp->disp->head_mask, sizeof(disp->disp->head_mask) * 8) {
		struct nv50_head *head;

		head = nv50_head_create(dev, i);
		if (IS_ERR(head)) {
			ret = PTR_ERR(head);
			goto out;
		}

		if (has_mst) {
			head->msto = nv50_msto_new(dev, head, i);
			if (IS_ERR(head->msto)) {
				ret = PTR_ERR(head->msto);
				head->msto = NULL;
				goto out;
			}

			/*
			 * FIXME: This is a hack to workaround the following
			 * issues:
			 *
			 * https://gitlab.gnome.org/GNOME/mutter/issues/759
			 * https://gitlab.freedesktop.org/xorg/xserver/merge_requests/277
			 *
			 * Once these issues are closed, this should be
			 * removed
			 */
			head->msto->encoder.possible_crtcs = disp->disp->head_mask;
		}
	}

	/* Disable vblank irqs aggressively for power-saving, safe on nv50+ */
	dev->vblank_disable_immediate = true;

	nv50_audio_component_init(drm);

out:
	if (ret)
		nv50_display_destroy(dev);
	return ret;
}

/******************************************************************************
 * Format modifiers
 *****************************************************************************/

/****************************************************************
 *            Log2(block height) ----------------------------+  *
 *            Page Kind ----------------------------------+  |  *
 *            Gob Height/Page Kind Generation ------+     |  |  *
 *                          Sector layout -------+  |     |  |  *
 *                          Compression ------+  |  |     |  |  */
const u64 disp50xx_modifiers[] = { /*         |  |  |     |  |  */
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 0),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x7a, 5),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 0),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x78, 5),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 0),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 1, 0x70, 5),
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

/****************************************************************
 *            Log2(block height) ----------------------------+  *
 *            Page Kind ----------------------------------+  |  *
 *            Gob Height/Page Kind Generation ------+     |  |  *
 *                          Sector layout -------+  |     |  |  *
 *                          Compression ------+  |  |     |  |  */
const u64 disp90xx_modifiers[] = { /*         |  |  |     |  |  */
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 0),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 1),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 2),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 3),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 4),
	DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 1, 0, 0xfe, 5),
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID
};

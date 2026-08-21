/*
 * Copyright 2021 Red Hat Inc.
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
#include "priv.h"
#include "conn.h"
#include "head.h"
#include "outp.h"

#include <nvif/class.h>
#include <nvif/if0010.h>

static int
nvkm_udisp_mthd_imp_check(struct nvkm_disp *disp, void *argv, u32 argc)
{
	union nvif_disp_imp_check_args *args = argv;
	struct nvkm_disp_imp_head heads[8];
	struct nvkm_disp_imp_result result;
	u32 seen = 0;
	int ret, i;

	if (argc != sizeof(args->v0) || args->v0.version != 0)
		return -ENOSYS;
	if (!disp->imp_check)
		return -ENODEV;
	if (args->v0.num_heads > ARRAY_SIZE(heads))
		return -EINVAL;

	for (i = 0; i < args->v0.num_heads; i++) {
		if (args->v0.head[i].index >= ARRAY_SIZE(result.head) ||
		    seen & BIT(args->v0.head[i].index))
			return -EINVAL;
		seen |= BIT(args->v0.head[i].index);

		heads[i].index = args->v0.head[i].index;
		heads[i].vtaps = args->v0.head[i].vtaps;
		heads[i].tile_mask = args->v0.head[i].tile_mask;
		heads[i].pclk_khz = args->v0.head[i].pclk_khz;
		heads[i].htotal = args->v0.head[i].htotal;
		heads[i].vtotal = args->v0.head[i].vtotal;
		heads[i].hblanks = args->v0.head[i].hblanks;
		heads[i].hblanke = args->v0.head[i].hblanke;
		heads[i].vblanks = args->v0.head[i].vblanks;
		heads[i].vblanke = args->v0.head[i].vblanke;
		heads[i].in_w = args->v0.head[i].in_w;
		heads[i].in_h = args->v0.head[i].in_h;
		heads[i].out_w = args->v0.head[i].out_w;
		heads[i].out_h = args->v0.head[i].out_h;
		heads[i].dsc_enable = args->v0.head[i].dsc_enable;
		heads[i].dsc_bpp_x16 = args->v0.head[i].dsc_bpp_x16;
		heads[i].dsc_slice_mask = args->v0.head[i].dsc_slice_mask;
		heads[i].wndw_formats[0] = args->v0.head[i].wndw_formats[0] &
					   NVIF_DISP_IMP_FORMAT_ALL;
		heads[i].wndw_formats[1] = args->v0.head[i].wndw_formats[1] &
					   NVIF_DISP_IMP_FORMAT_ALL;
	}

	ret = disp->imp_check(disp, args->v0.num_heads, args->v0.tiled, heads,
			      &result);
	if (ret)
		return ret;

	args->v0.possible = result.possible;
	for (i = 0; i < args->v0.num_heads; i++) {
		const u8 index = args->v0.head[i].index;

		args->v0.head[i].required_tiles =
			result.head[index].required_tiles;
		args->v0.head[i].dsc_slices = result.head[index].dsc_slices;
	}

	return 0;
}

static int
nvkm_udisp_mthd(struct nvkm_object *object, u32 mthd, void *argv, u32 argc)
{
	struct nvkm_disp *disp = nvkm_udisp(object);

	switch (mthd) {
	case NVIF_DISP_V0_IMP_CHECK:
		return nvkm_udisp_mthd_imp_check(disp, argv, argc);
	default:
		break;
	}

	return -EINVAL;
}

static int
nvkm_udisp_sclass(struct nvkm_object *object, int index, struct nvkm_oclass *sclass)
{
	struct nvkm_disp *disp = nvkm_udisp(object);

	if (index-- == 0) {
		sclass->base = (struct nvkm_sclass) { 0, 0, NVIF_CLASS_CONN };
		sclass->ctor = nvkm_uconn_new;
		return 0;
	}

	if (index-- == 0) {
		sclass->base = (struct nvkm_sclass) { 0, 0, NVIF_CLASS_OUTP };
		sclass->ctor = nvkm_uoutp_new;
		return 0;
	}

	if (index-- == 0) {
		sclass->base = (struct nvkm_sclass) { 0, 0, NVIF_CLASS_HEAD };
		sclass->ctor = nvkm_uhead_new;
		return 0;
	}

	if (disp->func->user[index].ctor) {
		sclass->base = disp->func->user[index].base;
		sclass->ctor = disp->func->user[index].ctor;
		return 0;
	}

	return -EINVAL;
}

static void *
nvkm_udisp_dtor(struct nvkm_object *object)
{
	struct nvkm_disp *disp = nvkm_udisp(object);

	spin_lock(&disp->client.lock);
	if (object == &disp->client.object)
		disp->client.object.func = NULL;
	spin_unlock(&disp->client.lock);
	return NULL;
}

static const struct nvkm_object_func
nvkm_udisp = {
	.dtor = nvkm_udisp_dtor,
	.mthd = nvkm_udisp_mthd,
	.sclass = nvkm_udisp_sclass,
};

int
nvkm_udisp_new(const struct nvkm_oclass *oclass, void *argv, u32 argc, struct nvkm_object **pobject)
{
	struct nvkm_disp *disp = nvkm_disp(oclass->engine);
	struct nvkm_conn *conn;
	struct nvkm_outp *outp;
	struct nvkm_head *head;
	union nvif_disp_args *args = argv;

	if (argc != sizeof(args->v0) || args->v0.version != 0)
		return -ENOSYS;

	spin_lock(&disp->client.lock);
	if (disp->client.object.func) {
		spin_unlock(&disp->client.lock);
		return -EBUSY;
	}
	nvkm_object_ctor(&nvkm_udisp, oclass, &disp->client.object);
	*pobject = &disp->client.object;
	spin_unlock(&disp->client.lock);

	args->v0.glitchy_mclk_switch = disp->glitchy_mclk_switch;

	args->v0.conn_mask = 0;
	list_for_each_entry(conn, &disp->conns, head)
		args->v0.conn_mask |= BIT(conn->index);

	args->v0.outp_mask = 0;
	list_for_each_entry(outp, &disp->outps, head)
		args->v0.outp_mask |= BIT(outp->index);

	args->v0.head_mask = 0;
	list_for_each_entry(head, &disp->heads, head)
		args->v0.head_mask |= BIT(head->id);

	return 0;
}

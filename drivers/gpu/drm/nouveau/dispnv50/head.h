#ifndef __NV50_KMS_HEAD_H__
#define __NV50_KMS_HEAD_H__
#define nv50_head(c) container_of((c), struct nv50_head, base.base)
#include <linux/workqueue.h>

#include "disp.h"
#include "atom.h"
#include "crc.h"
#include "lut.h"

#include "nouveau_crtc.h"
#include "nouveau_encoder.h"

struct nv50_head {
	const struct nv50_head_func *func;
	struct nv50_disp *disp;

	struct nouveau_crtc base;
	struct nv50_crc crc;
	struct nv50_lut olut;
	struct nv50_msto *msto;
};

struct nv50_head *nv50_head_create(struct drm_device *, int index);
void nv50_head_mode_from_drm(struct drm_display_mode *, struct nv50_head_mode *);
void nv50_head_flush_set(struct nv50_head *head, struct nv50_head_atom *asyh);
void nv50_head_flush_set_wndw(struct nv50_head *head, struct nv50_head_atom *asyh);
void nv50_head_flush_clr(struct nv50_head *head,
			 struct nv50_head_atom *asyh, bool flush);

/* Compute idle lines around the output viewport. Interlaced vblank uses
 * field coordinates while the viewport height covers a frame, so these
 * timings need the caller's fallback rather than this calculation.
 */
static inline bool
nv50_head_min_frame_idle(const struct nv50_head_atom *asyh, u16 *leading, u16 *trailing)
{
	const u16 active = asyh->mode.v.blanks - asyh->mode.v.blanke;
	/* Aspect ratio rounding can exceed the active raster by a line, so
	 * use the IMP request's clamp to program the same idle counts.
	 */
	const u16 idle_h = min_t(u16, asyh->view.oH, active);
	int overscan, lead;

	if (asyh->mode.interlace)
		return false;

	overscan = active / 2 - idle_h / 2;
	lead = asyh->mode.v.blanke + overscan + 1;

	/* Vsync and the back porch each need a line. The viewport must end
	 * within the frame to keep the trailing count from underflowing.
	 */
	if (lead < 2 || lead + idle_h > asyh->mode.v.active)
		return false;

	*leading = lead;
	*trailing = asyh->mode.v.active - (lead + idle_h);
	return true;
}

struct nv50_head_func {
	int (*view)(struct nv50_head *, struct nv50_head_atom *);
	int (*mode)(struct nv50_head *, struct nv50_head_atom *);
	bool (*olut)(struct nv50_head *, struct nv50_head_atom *, int);
	bool (*ilut_check)(int size);
	bool olut_identity;
	int  olut_size;
	int (*olut_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*olut_clr)(struct nv50_head *);
	void (*core_calc)(struct nv50_head *, struct nv50_head_atom *);
	int (*core_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*core_clr)(struct nv50_head *);
	int (*curs_layout)(struct nv50_head *, struct nv50_wndw_atom *,
			   struct nv50_head_atom *);
	int (*curs_format)(struct nv50_head *, struct nv50_wndw_atom *,
			   struct nv50_head_atom *);
	int (*curs_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*curs_clr)(struct nv50_head *);
	int (*base)(struct nv50_head *, struct nv50_head_atom *);
	int (*ovly)(struct nv50_head *, struct nv50_head_atom *);
	int (*dither)(struct nv50_head *, struct nv50_head_atom *);
	int (*procamp)(struct nv50_head *, struct nv50_head_atom *);
	int (*or)(struct nv50_head *, struct nv50_head_atom *);
	void (*static_wndw_map)(struct nv50_head *, struct nv50_head_atom *);
	int (*display_id)(struct nv50_head *, u32 display_id);
	int (*mtc_set)(struct nv50_head *, struct nv50_head_atom *);
	int (*mtc_clr)(struct nv50_head *);
};

extern const struct nv50_head_func head507d;
int head507d_view(struct nv50_head *, struct nv50_head_atom *);
int head507d_mode(struct nv50_head *, struct nv50_head_atom *);
bool head507d_olut(struct nv50_head *, struct nv50_head_atom *, int);
void head507d_core_calc(struct nv50_head *, struct nv50_head_atom *);
int head507d_core_clr(struct nv50_head *);
int head507d_curs_layout(struct nv50_head *, struct nv50_wndw_atom *,
			 struct nv50_head_atom *);
int head507d_curs_format(struct nv50_head *, struct nv50_wndw_atom *,
			 struct nv50_head_atom *);
int head507d_base(struct nv50_head *, struct nv50_head_atom *);
int head507d_ovly(struct nv50_head *, struct nv50_head_atom *);
int head507d_dither(struct nv50_head *, struct nv50_head_atom *);
int head507d_procamp(struct nv50_head *, struct nv50_head_atom *);

extern const struct nv50_head_func head827d;

extern const struct nv50_head_func head907d;
int head907d_view(struct nv50_head *, struct nv50_head_atom *);
int head907d_mode(struct nv50_head *, struct nv50_head_atom *);
bool head907d_olut(struct nv50_head *, struct nv50_head_atom *, int);
bool head907d_ilut_check(int size);
int head907d_olut_set(struct nv50_head *, struct nv50_head_atom *);
int head907d_olut_clr(struct nv50_head *);
int head907d_core_set(struct nv50_head *, struct nv50_head_atom *);
int head907d_core_clr(struct nv50_head *);
int head907d_curs_set(struct nv50_head *, struct nv50_head_atom *);
int head907d_curs_clr(struct nv50_head *);
int head907d_ovly(struct nv50_head *, struct nv50_head_atom *);
int head907d_procamp(struct nv50_head *, struct nv50_head_atom *);
int head907d_or(struct nv50_head *, struct nv50_head_atom *);

extern const struct nv50_head_func head917d;
int head917d_curs_layout(struct nv50_head *, struct nv50_wndw_atom *,
			 struct nv50_head_atom *);

extern const struct nv50_head_func headc37d;
int headc37d_view(struct nv50_head *, struct nv50_head_atom *);
int headc37d_curs_format(struct nv50_head *, struct nv50_wndw_atom *,
			 struct nv50_head_atom *);
int headc37d_curs_set(struct nv50_head *, struct nv50_head_atom *);
int headc37d_curs_clr(struct nv50_head *);
int headc37d_dither(struct nv50_head *, struct nv50_head_atom *);
void headc37d_static_wndw_map(struct nv50_head *, struct nv50_head_atom *);

extern const struct nv50_head_func headc57d;
bool headc57d_olut(struct nv50_head *, struct nv50_head_atom *, int size);

extern const struct nv50_head_func headca7d;
#endif

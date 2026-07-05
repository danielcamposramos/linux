/* SPDX-License-Identifier: MIT */
#ifndef __NVIF_IF0010_H__
#define __NVIF_IF0010_H__

union nvif_disp_args {
	struct nvif_disp_v0 {
		__u8 version;
		__u8 pad01[3];
		__u32 conn_mask;
		__u32 outp_mask;
		__u32 head_mask;
	} v0;
};

#define NVIF_DISP_V0_IMP_CHECK 0x00

/* Window format classes, using NVC372_CTRL_FORMAT_* values. */
#define NVIF_DISP_IMP_FORMAT_RGB_PACKED_1_BPP                               0x01
#define NVIF_DISP_IMP_FORMAT_RGB_PACKED_2_BPP                               0x02
#define NVIF_DISP_IMP_FORMAT_RGB_PACKED_4_BPP                               0x04
#define NVIF_DISP_IMP_FORMAT_RGB_PACKED_8_BPP                               0x08
#define NVIF_DISP_IMP_FORMAT_YUV_PACKED_422                                 0x10
#define NVIF_DISP_IMP_FORMAT_ALL                                            0x1f

union nvif_disp_imp_check_args {
	struct nvif_disp_imp_check_v0 {
		__u8  version;
		__u8  num_heads;
		__u8  possible;		/* out */
		__u8  tiled;		/* in: tiled GPU requiring an assignment */
		__u8  pad04[4];
		struct nvif_disp_imp_check_head_v0 {
			__u8  index;
			__u8  vtaps;		/* in: assigned output scaler vertical taps */
			__u8  tile_mask;	/* in: forced tiles, 0 lets IMP choose */
			__u8  dsc_enable;
			__u8  required_tiles;	/* out */
			__u8  dsc_slices;	/* out */
			__u16 dsc_bpp_x16;
			__u32 dsc_slice_mask;
			__u32 pclk_khz;
			__u16 htotal;
			__u16 vtotal;
			__u16 hblanks;		/* raster blank start/end */
			__u16 hblanke;
			__u16 vblanks;
			__u16 vblanke;
			__u16 in_w;		/* viewport */
			__u16 in_h;
			__u16 out_w;
			__u16 out_h;
			/* NVIF_DISP_IMP_FORMAT_* mask per window. Zero excludes
			 * it from IMP.
			 */
			__u8  wndw_formats[2];
			__u8  pad26[2];
		} head[8];
	} v0;
};
#endif

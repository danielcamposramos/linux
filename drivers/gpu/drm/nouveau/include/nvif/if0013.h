/* SPDX-License-Identifier: MIT */
#ifndef __NVIF_IF0013_H__
#define __NVIF_IF0013_H__

union nvif_head_args {
	struct nvif_head_v0 {
		__u8 version;
		__u8 id;
		__u8 pad02[6];
	} v0;
};

union nvif_head_event_args {
	struct nvif_head_event_vn {
	} vn;
};

#define NVIF_HEAD_V0_SCANOUTPOS 0x00

union nvif_head_scanoutpos_args {
	struct nvif_head_scanoutpos_v0 {
		__u8  version;
		__u8  pad01[7];
		__s64 time[2];
		__u16 vblanks;
		__u16 vblanke;
		__u16 vtotal;
		__u16 vline;
		__u16 hblanks;
		__u16 hblanke;
		__u16 htotal;
		__u16 hline;
	} v0;
};

#define NVIF_HEAD_V0_ARMED 0x01

union nvif_head_armed_args {
	struct nvif_head_armed_v0 {
		__u8  version;
		__u8  vtaps;
		__u8  htaps;
		__u8  interlace;
		/* Set when the class supplies viewport and tap readback
		 * (GV100+). Ignore those fields when clear. This flag does not
		 * report whether the head has a scaler.
		 */
		__u8  view;
		__u8  nhsync;
		__u8  nvsync;
		/* GB20x tile ownership, zero where the class has none. */
		__u8  tiles_mask;
		__u64 hz;
		__u16 htotal;
		__u16 hsynce;
		__u16 hblanke;
		__u16 hblanks;
		__u16 vtotal;
		__u16 vsynce;
		__u16 vblanke;
		__u16 vblanks;
		__u16 iW;
		__u16 iH;
		__u16 oW;
		__u16 oH;
		__u32 phywins[2];
	} v0;
};
#endif

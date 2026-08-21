/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2003-2021, NVIDIA CORPORATION. All rights reserved.
 */
#ifndef _clc573_h_
#define _clc573_h_

#define NVC573_SYS_CAP                                                       0x0
#define NVC573_SYS_CAP_HEAD_EXISTS(i)                            (0+(i)):(0+(i))
#define NVC573_SYS_CAP_HEAD_EXISTS__SIZE_1                                     8
#define NVC573_SYS_CAP_HEAD_EXISTS_NO                                 0x00000000
#define NVC573_SYS_CAP_HEAD_EXISTS_YES                                0x00000001

#define NVC573_POSTCOMP_HEAD_HDR_CAPA(i)                          (0x680+(i)*32)
#define NVC573_POSTCOMP_HEAD_HDR_CAPA_SCLR_PRESENT                         18:18
#define NVC573_POSTCOMP_HEAD_HDR_CAPA_SCLR_PRESENT_FALSE              0x00000000
#define NVC573_POSTCOMP_HEAD_HDR_CAPA_SCLR_PRESENT_TRUE               0x00000001
#define NVC573_POSTCOMP_HEAD_HDR_CAPC(i)                          (0x688+(i)*32)
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_VS_MAX_SCALE_FACTOR             28:28
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_VS_MAX_SCALE_FACTOR_2X     0x00000000
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_VS_MAX_SCALE_FACTOR_4X     0x00000001
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_HS_MAX_SCALE_FACTOR             30:30
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_HS_MAX_SCALE_FACTOR_2X     0x00000000
#define NVC573_POSTCOMP_HEAD_HDR_CAPC_SCLR_HS_MAX_SCALE_FACTOR_4X     0x00000001
#define NVC573_POSTCOMP_HEAD_HDR_CAPD(i)                          (0x68c+(i)*32)
#define NVC573_POSTCOMP_HEAD_HDR_CAPD_VSCLR_MAX_PIXELS_2TAP                 15:0
#define NVC573_POSTCOMP_HEAD_HDR_CAPD_VSCLR_MAX_PIXELS_5TAP                31:16
#endif // _clc573_h_

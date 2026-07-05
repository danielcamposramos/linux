/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#ifndef _clca73_h_
#define _clca73_h_

#define NVCA73_DISP_CAPABILITIES 0xCA73

#define NVCA73_SYS_CAP                                                       0x0
#define NVCA73_SYS_CAP_HEAD_EXISTS(i)                            (0+(i)):(0+(i))
#define NVCA73_SYS_CAP_HEAD_EXISTS__SIZE_1                                     8
#define NVCA73_SYS_CAP_HEAD_EXISTS_NO                                 0x00000000
#define NVCA73_SYS_CAP_HEAD_EXISTS_YES                                0x00000001
#define NVCA73_SYS_CAPB                                                      0x4
#define NVCA73_SYS_CAPB_WINDOW_EXISTS(i)                         (0+(i)):(0+(i))
#define NVCA73_SYS_CAPB_WINDOW_EXISTS__SIZE_1                                 32
#define NVCA73_SYS_CAPB_WINDOW_EXISTS_NO                              0x00000000
#define NVCA73_SYS_CAPB_WINDOW_EXISTS_YES                             0x00000001
#define NVCA73_SYS_CAPC                                                     0x20
#define NVCA73_SYS_CAPC_TILE_EXISTS(i)                           (0+(i)):(0+(i))
#define NVCA73_SYS_CAPC_TILE_EXISTS__SIZE_1                                    8
#define NVCA73_SYS_CAPC_TILE_EXISTS_NO                                0x00000000
#define NVCA73_SYS_CAPC_TILE_EXISTS_YES                               0x00000001
#define NVCA73_SYS_CAPC_TILE_SUPPORT_MULTI_TILE(i)               (8+(i)):(8+(i))
#define NVCA73_SYS_CAPC_TILE_SUPPORT_MULTI_TILE__SIZE_1                        8
#define NVCA73_SYS_CAPC_TILE_SUPPORT_MULTI_TILE_NO                    0x00000000
#define NVCA73_SYS_CAPC_TILE_SUPPORT_MULTI_TILE_YES                   0x00000001
#define NVCA73_IHUB_COMMON_CAPF                                             0x28
#define NVCA73_IHUB_COMMON_CAPF_PHYWIN_SUPPORT_MULTI_TILE(i)     (0+(i)):(0+(i))
#define NVCA73_IHUB_COMMON_CAPF_PHYWIN_SUPPORT_MULTI_TILE__SIZE_1             32
#define NVCA73_IHUB_COMMON_CAPF_PHYWIN_SUPPORT_MULTI_TILE_NO          0x00000000
#define NVCA73_IHUB_COMMON_CAPF_PHYWIN_SUPPORT_MULTI_TILE_YES         0x00000001
#define NVCA73_POSTCOMP_HDR_CAPA(i)                               (0x680+(i)*32)
#define NVCA73_POSTCOMP_HDR_CAPA_SCLR_PRESENT                              18:18
#define NVCA73_POSTCOMP_HDR_CAPA_SCLR_PRESENT_TRUE                    0x00000001
#define NVCA73_POSTCOMP_HDR_CAPA_SCLR_PRESENT_FALSE                   0x00000000
#define NVCA73_POSTCOMP_HDR_CAPA_VFILTER_PRESENT                           23:23
#define NVCA73_POSTCOMP_HDR_CAPA_VFILTER_PRESENT_TRUE                 0x00000001
#define NVCA73_POSTCOMP_HDR_CAPA_VFILTER_PRESENT_FALSE                0x00000000
#endif // _clca73_h_

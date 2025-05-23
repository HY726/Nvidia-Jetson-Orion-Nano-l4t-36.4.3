/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: MIT
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/*
 * g_allclasses.h
 *
 * Pull in all class headers or class number declarations.
 * The class list is generated by chip-config from Classes.pm
 *
 * NOTE: this file may be included multiple times
 *
 */

#if defined(SDK_ALL_CLASSES_INCLUDE_FULL_HEADER)

#include <class/cl0000.h>               // NV01_ROOT
#include <class/cl0001.h>               // NV01_ROOT_NON_PRIV
#include <class/cl0041.h>               // NV01_ROOT_CLIENT
#include <class/cl0020.h>               // NV0020_GPU_MANAGEMENT
#include <class/cl0080.h>               // NV01_DEVICE_0
#include <class/cl2080.h>               // NV20_SUBDEVICE_0
#include <class/cl2081.h>               // NV2081_BINAPI
#include <class/cl2082.h>               // NV2082_BINAPI_PRIVILEGED
#include <class/cl0002.h>               // NV01_CONTEXT_DMA
#include <class/cl003e.h>               // NV01_MEMORY_SYSTEM
#include <class/cl00c3.h>               // NV01_MEMORY_SYNCPOINT
#include <class/cl0071.h>               // NV01_MEMORY_SYSTEM_OS_DESCRIPTOR
#include <class/cl0076.h>               // NV01_MEMORY_FRAMEBUFFER_CONSOLE
#include <class/cl00f2.h>               // IO_VASPACE_A
#include <class/cl0005.h>               // NV01_EVENT
#include <nvos.h>                       // NV01_EVENT_KERNEL_CALLBACK
#include <nvos.h>                       // NV01_EVENT_OS_EVENT
#include <nvos.h>                       // NV01_EVENT_KERNEL_CALLBACK_EX
#include <class/clc372sw.h>             // NVC372_DISPLAY_SW
#include <class/clc673.h>               // NVC673_DISP_CAPABILITIES
#include <class/cl0073.h>               // NV04_DISPLAY_COMMON
#include <class/clc670.h>               // NVC670_DISPLAY
#include <class/clc671.h>               // NVC671_DISP_SF_USER
#include <class/clc67a.h>               // NVC67A_CURSOR_IMM_CHANNEL_PIO
#include <class/clc67b.h>               // NVC67B_WINDOW_IMM_CHANNEL_DMA
#include <class/clc67d.h>               // NVC67D_CORE_CHANNEL_DMA
#include <class/clc67e.h>               // NVC67E_WINDOW_CHANNEL_DMA
#include <class/clc77f.h>               // NVC77F_ANY_CHANNEL_DMA
#include <class/cl90ec.h>               // GF100_HDACODEC


#else // defined(SDK_ALL_CLASSES_INCLUDE_FULL_HEADER)


#ifndef NV01_ROOT
#define NV01_ROOT                                (0x00000000)
#endif
#ifndef NV1_ROOT
#define NV1_ROOT                                 (0x00000000) // alias
#endif
#ifndef NV01_NULL_OBJECT
#define NV01_NULL_OBJECT                         (0x00000000) // alias
#endif
#ifndef NV1_NULL_OBJECT
#define NV1_NULL_OBJECT                          (0x00000000) // alias
#endif

#ifndef NV01_ROOT_NON_PRIV
#define NV01_ROOT_NON_PRIV                       (0x00000001)
#endif
#ifndef NV1_ROOT_NON_PRIV
#define NV1_ROOT_NON_PRIV                        (0x00000001) // alias
#endif

#ifndef NV01_ROOT_CLIENT
#define NV01_ROOT_CLIENT                         (0x00000041)
#endif

#ifndef NV0020_GPU_MANAGEMENT
#define NV0020_GPU_MANAGEMENT                    (0x00000020)
#endif

#ifndef NV01_DEVICE_0
#define NV01_DEVICE_0                            (0x00000080)
#endif

#ifndef NV20_SUBDEVICE_0
#define NV20_SUBDEVICE_0                         (0x00002080)
#endif

#ifndef NV2081_BINAPI
#define NV2081_BINAPI                            (0x00002081)
#endif

#ifndef NV2082_BINAPI_PRIVILEGED
#define NV2082_BINAPI_PRIVILEGED                 (0x00002082)
#endif

#ifndef NV01_CONTEXT_DMA
#define NV01_CONTEXT_DMA                         (0x00000002)
#endif

#ifndef NV01_MEMORY_SYSTEM
#define NV01_MEMORY_SYSTEM                       (0x0000003e)
#endif
#ifndef NV1_MEMORY_SYSTEM
#define NV1_MEMORY_SYSTEM                        (0x0000003e) // alias
#endif

#ifndef NV01_MEMORY_SYNCPOINT
#define NV01_MEMORY_SYNCPOINT                    (0x000000c3)
#endif

#ifndef NV01_MEMORY_SYSTEM_OS_DESCRIPTOR
#define NV01_MEMORY_SYSTEM_OS_DESCRIPTOR         (0x00000071)
#endif

#ifndef NV01_MEMORY_FRAMEBUFFER_CONSOLE
#define NV01_MEMORY_FRAMEBUFFER_CONSOLE          (0x00000076)
#endif

#ifndef IO_VASPACE_A
#define IO_VASPACE_A                             (0x000000f2)
#endif

#ifndef NV01_EVENT
#define NV01_EVENT                               (0x00000005)
#endif
#ifndef NV1_EVENT
#define NV1_EVENT                                (0x00000005) // alias
#endif

#ifndef NV01_EVENT_KERNEL_CALLBACK
#define NV01_EVENT_KERNEL_CALLBACK               (0x00000078)
#endif
#ifndef NV1_EVENT_KERNEL_CALLBACK
#define NV1_EVENT_KERNEL_CALLBACK                (0x00000078) // alias
#endif

#ifndef NV01_EVENT_OS_EVENT
#define NV01_EVENT_OS_EVENT                      (0x00000079)
#endif
#ifndef NV1_EVENT_OS_EVENT
#define NV1_EVENT_OS_EVENT                       (0x00000079) // alias
#endif
#ifndef NV01_EVENT_WIN32_EVENT
#define NV01_EVENT_WIN32_EVENT                   (0x00000079) // alias
#endif
#ifndef NV1_EVENT_WIN32_EVENT
#define NV1_EVENT_WIN32_EVENT                    (0x00000079) // alias
#endif

#ifndef NV01_EVENT_KERNEL_CALLBACK_EX
#define NV01_EVENT_KERNEL_CALLBACK_EX            (0x0000007e)
#endif
#ifndef NV1_EVENT_KERNEL_CALLBACK_EX
#define NV1_EVENT_KERNEL_CALLBACK_EX             (0x0000007e) // alias
#endif

#ifndef NVC372_DISPLAY_SW
#define NVC372_DISPLAY_SW                        (0x0000c372)
#endif

#ifndef NVC673_DISP_CAPABILITIES
#define NVC673_DISP_CAPABILITIES                 (0x0000c673)
#endif

#ifndef NV04_DISPLAY_COMMON
#define NV04_DISPLAY_COMMON                      (0x00000073)
#endif

#ifndef NVC670_DISPLAY
#define NVC670_DISPLAY                           (0x0000c670)
#endif

#ifndef NVC671_DISP_SF_USER
#define NVC671_DISP_SF_USER                      (0x0000c671)
#endif

#ifndef NVC67A_CURSOR_IMM_CHANNEL_PIO
#define NVC67A_CURSOR_IMM_CHANNEL_PIO            (0x0000c67a)
#endif

#ifndef NVC67B_WINDOW_IMM_CHANNEL_DMA
#define NVC67B_WINDOW_IMM_CHANNEL_DMA            (0x0000c67b)
#endif

#ifndef NVC67D_CORE_CHANNEL_DMA
#define NVC67D_CORE_CHANNEL_DMA                  (0x0000c67d)
#endif

#ifndef NVC67E_WINDOW_CHANNEL_DMA
#define NVC67E_WINDOW_CHANNEL_DMA                (0x0000c67e)
#endif

#ifndef NVC77F_ANY_CHANNEL_DMA
#define NVC77F_ANY_CHANNEL_DMA                   (0x0000c77f)
#endif

#ifndef GF100_HDACODEC
#define GF100_HDACODEC                           (0x000090ec)
#endif


#endif // defined(SDK_ALL_CLASSES_INCLUDE_FULL_HEADER)

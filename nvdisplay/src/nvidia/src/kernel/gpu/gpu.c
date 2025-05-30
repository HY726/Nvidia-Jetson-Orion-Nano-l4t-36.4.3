/*
 * SPDX-FileCopyrightText: Copyright (c) 1993-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
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

/*!
 * @file
 * @brief HW State Routines: System Object Function Definitions.
 */


#include "lib/base_utils.h"
#include "gpu/gpu.h"
#include "gpu/disp/kern_disp.h"
#include "gpu/disp/inst_mem/disp_inst_mem.h"
#include "gpu/disp/console_mem/disp_console_mem.h"
#include "gpu/subdevice/subdevice.h"
#include "gpu/eng_desc.h"
#include "nv_ref.h"
#include "os/os.h"
#include "nvrm_registry.h"
#include "gpu_mgr/gpu_mgr.h"
#include "core/thread_state.h"
#include "core/locks.h"
#include "diagnostics/tracer.h"
#include "rmapi/client_resource.h"
#include "diagnostics/journal.h"
#include "rmapi/rs_utils.h"
#include "rmapi/rmapi_utils.h"
#include "core/hal_mgr.h"
#include "vgpu/rpc.h"
#include "jt.h"

#include <nverror.h>

#include "virtualization/hypervisor/hypervisor.h"

static NV_STATUS gpuDetermineVirtualMode(OBJGPU *);

#include "g_odb.h"

typedef struct GPUCHILDINFO *PGPUCHILDINFO;
typedef struct GPUCHILDINFO  GPUCHILDINFO;

typedef struct GPUCHILDTYPE *PGPUCHILDTYPE;
typedef struct GPUCHILDTYPE  GPUCHILDTYPE;

#define  RMTRACE_ENGINE_PROFILE_EVENT(EventName, EngineId, ReadCount, WriteCount)           \
{                                                                                           \
        RMTRACE_PROBE4(generic, marker,                                                     \
                       NvU32, EngineId, sizeof(NvU32),                                      \
                       char*, EventName, sizeof(EventName),                                 \
                       NvU32, ReadCount, sizeof(NvU32),                                      \
                       NvU32, WriteCount, sizeof(NvU32));                                   \
    }

// Public interface functions

static NV_STATUS gpuRemoveMissingEngines(OBJGPU *);

// local static function
static NV_STATUS _gpuChildrenPresentInit(OBJGPU *pGpu);
static NV_STATUS gpuCreateChildObjects(OBJGPU *, NvBool);
static NV_STATUS gpuStatePreLoad(OBJGPU *, NvU32);
static NV_STATUS gpuStatePostLoad(OBJGPU *, NvU32);
static NV_STATUS gpuStatePreUnload(OBJGPU *, NvU32);
static NV_STATUS gpuStatePostUnload(OBJGPU *, NvU32);
static void      gpuXlateHalImplToArchImpl(OBJGPU *, HAL_IMPLEMENTATION, NvU32 *, NvU32 *);
static NvBool    gpuSatisfiesTemporalOrder(OBJGPU *, HAL_IMPLEMENTATION);
static NvBool    gpuSatisfiesTemporalOrderMaskRev(OBJGPU *, HAL_IMPLEMENTATION, NvU32, NvU32, NvU32);
static NvBool    gpuShouldCreateObject(OBJGPU *pGpu, GPUCHILDINFO *pChildInfo);

static void gpuDestroyMissingEngine(OBJGPU *, OBJENGSTATE *);
static void gpuRemoveMissingEngineClasses(OBJGPU *, NvU32);

static NV_STATUS _gpuCreateEngineOrderList(OBJGPU *pGpu);
static void _gpuFreeEngineOrderList(OBJGPU *pGpu);


static void _gpuInitPciHandle(OBJGPU *pGpu);
static void _gpuInitPhysicalRmApi(OBJGPU *pGpu);
static NV_STATUS _gpuAllocateInternalObjects(OBJGPU *pGpu);
static void _gpuFreeInternalObjects(OBJGPU *pGpu);

typedef struct
{
    NvS32   childOrderIndex;
    NvS32   instanceID;
    NvU32   flags;
    NvBool  bStarted;
} ENGLIST_ITER, *PENGLIST_ITER;

static ENGLIST_ITER gpuGetEngineOrderListIter(OBJGPU *pGpu, NvU32 flags);
static NvBool gpuGetNextInEngineOrderList(OBJGPU *pGpu, ENGLIST_ITER *pIt, PENGDESCRIPTOR pEngDesc);

static inline void _setPlatformNoHostbridgeDetect(NvBool bValue)
{
}

// Describes a child type (e.g.: classId(OBJCE))
struct GPUCHILDTYPE
{
    NvBool           bConstructEarly;    // bConstructEarly objects are created in a separate step. FUSE must be created
                                         // before BIF since we need to know the OPSB fuse value for enabling/disabling
                                         // certain features in bifInitRegistryOverrides
    NvU32            instances;
    NvU32            gpuChildPtrOffset;
    NVOC_CLASS_ID    classId;
};

// List of all possible GPU offspring
static GPUCHILDTYPE gpuChildTypeList[] =
{
    #define GPU_CHILD(className, accessorName, numInstances, bConstructEarly, gpuField) \
        { bConstructEarly, numInstances, NV_OFFSETOF(OBJGPU, gpuField), classId(className) },

    #include "gpu/gpu_child_list.h"
};

// Describes a child instance (e.g.: classId(OBJCE) instanceID #1)
struct GPUCHILDINFO
{
    NvBool           bConstructEarly;
    ENGDESCRIPTOR    engDesc;
    NvU32            gpuChildPtrOffset;
    GPUCHILDTYPE    *pChildType;
};

static PGPUCHILDTYPE   gpuGetChildType(NVOC_CLASS_ID classId);
static NV_STATUS       gpuGetChildInfo(NVOC_CLASS_ID classId, NvU32 instanceID, PGPUCHILDINFO pChildInfoOut);
static NV_STATUS       _gpuChildNvocClassInfoGet(OBJGPU *pGpu, NVOC_CLASS_ID classId, const NVOC_CLASS_INFO **ppClassInfo);
static Dynamic       **gpuGetChildPtr(OBJGPU *pGpu, NvU32 gpuChildPtrOffset);

#define GPU_NUM_CHILD_TYPES \
    ((sizeof(gpuChildTypeList) / sizeof(GPUCHILDTYPE)))

void
_gpuDetectNvswitchSupport
(
    OBJGPU *pGpu
)
{
}

//
// Generate a 32-bit id from domain, bus and device tuple.
//
// This is a one way function that is not guaranteed to generate a unique id for
// each domain, bus, device tuple as domain alone can be 32-bit. Historically,
// we have been assuming that the domain can only be 16-bit, but that has never
// been true on Linux and Hyper-V virtualization has exposed that by using
// arbitrary 32-bit domains for passthrough GPUs. This is the only known case
// today that requires immediate support. The domains on Hyper-V come from
// hashing some system and GPU information and are claimed to be unique even if
// we consider the lower 16-bits only. Hence, as a temporary solution, only the
// lower 16-bits are used and it's asserted that top 16-bits are only non-0 on
// Hyper-V.
//
// Long term the 32-bit ids should be changed to 64-bit or the generation scheme
// should be changed to guarantee uniqueness. Both of these are impactful as the
// biggest user of this is the commonly used 32-bit OBJGPU::gpuId.
//
NvU32 gpuGenerate32BitId(NvU32 domain, NvU8 bus, NvU8 device)
{
    NvU32 id = gpuEncodeBusDevice(bus, device);

    // Include only the lower 16-bits to match the old gpuId scheme
    id |= (domain & 0xffff) << 16;

    return id;
}

NvU32 gpuGenerate32BitIdFromPhysAddr(RmPhysAddr addr)
{
    NvU32 id = NvU64_LO32(addr>>RM_PAGE_SHIFT);
    return id;
}

void gpuChangeComputeModeRefCount_IMPL(OBJGPU *pGpu, NvU32 command)
{
    switch(command)
    {
        case NV_GPU_COMPUTE_REFCOUNT_COMMAND_INCREMENT:
            NV_ASSERT(pGpu->computeModeRefCount >= 0);
            ++pGpu->computeModeRefCount;

            if (1 == pGpu->computeModeRefCount)
            {
                NV_PRINTF(LEVEL_INFO, "GPU (ID: 0x%x): new mode: COMPUTE\n",
                          pGpu->gpuId);

                timeoutInitializeGpuDefault(&pGpu->timeoutData, pGpu);
            }
            break;

        case NV_GPU_COMPUTE_REFCOUNT_COMMAND_DECREMENT:
            --pGpu->computeModeRefCount;
            NV_ASSERT(pGpu->computeModeRefCount >= 0);

            if (pGpu->computeModeRefCount < 0)
            {
                pGpu->computeModeRefCount = 0;
            }

            if (0 == pGpu->computeModeRefCount)
            {
                NV_PRINTF(LEVEL_INFO, "GPU (ID: 0x%x): new mode: GRAPHICS\n",
                          pGpu->gpuId);

                timeoutInitializeGpuDefault(&pGpu->timeoutData, pGpu);
            }
            break;

        default:
            NV_PRINTF(LEVEL_ERROR, "Bad command: 0x%x\n", command);
            NV_ASSERT(0);
            break;
    }
}

//
// gpuPostConstruct
//
// Called by the gpu manager to finish OBJGPU construction phase.
// Tasks handled here include binding a HAL module to the gpu
// and the construction of engine object offspring.
//
NV_STATUS
gpuPostConstruct_IMPL
(
    OBJGPU       *pGpu,
    GPUATTACHARG *pAttachArg
)
{
    NV_STATUS rmStatus;

    gpumgrAddDeviceInstanceToGpus(NVBIT(pGpu->gpuInstance));

    rmStatus = regAccessConstruct(&pGpu->registerAccess, pGpu);
    if (rmStatus != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "Failed to construct IO Apertures for attached devices \n");
        return rmStatus;
    }

    gpuInitChipInfo(pGpu);

    // check if RM is running in a virtualization mode
    // This function needs to be called before we use IS_VIRTUAL macro.
    // Because IS_VIRTUAL macro relies on this function to determine Virtual mode.
    // eg. gpuCreateChildObjects->...->xxxHalIfacesSetup_xxx relies on IS_VIRTUAL macro.
    rmStatus = gpuDetermineVirtualMode(pGpu);
    if (rmStatus != NV_OK)
        return rmStatus;

    pGpu->setProperty(pGpu, PDB_PROP_GPU_MOVE_CTX_BUFFERS_TO_PMA,
        gpuIsCtxBufAllocInPmaSupported_HAL(pGpu));
    //
    // gpuDetermineVirtualMode inits hPci but only for virtualization case. So if
    // it does not init it, do here for using it for non-virtualization as well
    //
    if (pGpu->hPci == NULL)
    {
        //
        // We don't check the return status. Even if PCI handle is not obtained
        // it should not block rest of the gpu init sequence.
        //
        _gpuInitPciHandle(pGpu);
    }

    //
    // Initialize the base offset for the virtual registers for physical function
    // or baremetal
    //
    pGpu->sriovState.virtualRegPhysOffset = gpuGetVirtRegPhysOffset_HAL(pGpu);

    NV_ASSERT_OK_OR_RETURN(
        _gpuChildrenPresentInit(pGpu));

    //
    // Initialize engine order before engine init/load/etc
    //
    rmStatus = _gpuCreateEngineOrderList(pGpu);
    if ( rmStatus != NV_OK )
        return rmStatus;

    gpuBuildClassDB(pGpu);

    // The first time the emulation setting is checked is in timeoutInitializeGpuDefault.
    pGpu->computeModeRefCount = 0;
    pGpu->hComputeModeReservation = NV01_NULL_OBJECT;

    // Setting default timeout values
    timeoutInitializeGpuDefault(&pGpu->timeoutData, pGpu);

    // Set 2 stage error recovery if Vista or Unix or GSP-RM.
    if (!IsAMODEL(pGpu))
    {
        pGpu->bTwoStageRcRecoveryEnabled = NV_TRUE;
    }

    if (hypervisorIsVgxHyper())
    {
        pGpu->setProperty(pGpu, PDB_PROP_GPU_IS_VIRTUALIZATION_MODE_HOST_VGPU, NV_TRUE);
    }

    // create core objects (i.e. bif)
    rmStatus = gpuCreateChildObjects(pGpu, /* bConstructEarly */ NV_TRUE);
    if (rmStatus != NV_OK)
        return rmStatus;

    gpuGetIdInfo_HAL(pGpu);
    gpuUpdateIdInfo_HAL(pGpu);

    _gpuInitPhysicalRmApi(pGpu);

    // need to get illumination values after the GPU Id
    // has been setup to allow for GPU specific settings
    gpuDeterminePersistantIllumSettings(pGpu);

#if NVCPU_IS_PPC64LE
    // Skip PCI Express Host Bridge initialization on PPC64 platforms
    _setPlatformNoHostbridgeDetect(NV_TRUE);
#endif

    // Construct and update the engine database
    rmStatus = gpuConstructEngineTable(pGpu);
    if (rmStatus != NV_OK)
        return rmStatus;
    rmStatus = gpuUpdateEngineTable(pGpu);
    if (rmStatus != NV_OK)
        return rmStatus;

    // create remaining gpu offspring
    rmStatus = gpuCreateChildObjects(pGpu, /* bConstructEarly */ NV_FALSE);
    if (rmStatus != NV_OK)
        return rmStatus;

    gpuGetHwDefaults(pGpu);

    // Set any state overrides required for L2 cache only mode
    if (gpuIsCacheOnlyModeEnabled(pGpu))
    {
        gpuSetCacheOnlyModeOverrides_HAL(pGpu);
    }

    // Register the OCA dump callback function.
    gpuDumpCallbackRegister(pGpu);

    // Initialize reference count for external kernel clients
    pGpu->externalKernelClientCount = 0;

    return NV_OK;
}

NV_STATUS gpuConstruct_IMPL
(
    OBJGPU *pGpu,
    NvU32   gpuInstance
)
{

    pGpu->gpuInstance = gpuInstance;
    pGpu->gspRmInitialized = NV_FALSE;

    // allocate OS-specific GPU extension area
    osInitOSHwInfo(pGpu);

    return gpuConstructPhysical(pGpu);
}

// NVOC-TODO : delete this after all Rmconfig modules migrated to NVOC
NV_STATUS
gpuBindHalLegacy_IMPL
(
    OBJGPU *pGpu,
    NvU32   chipId0,
    NvU32   chipId1,
    NvU32   socChipId0
)
{
    OBJSYS    *pSys = SYS_GET_INSTANCE();
    OBJHALMGR *pHalMgr = SYS_GET_HALMGR(pSys);
    NV_STATUS  status;

    // chipId0 and chipId1 needs to be function parameter since GPU Reg read
    // is not ready at this point.
    pGpu->chipId0 = chipId0;
    pGpu->chipId1 = chipId1;

    //
    // The system object will pass PMC_BOOT_0 and PMC_BOOT_42 to all the HAL's and return the
    // one that claims it supports this chip arch/implementation
    //
    status = halmgrGetHalForGpu(pHalMgr, socChipId0 ? socChipId0 : pGpu->chipId0, pGpu->chipId1, &pGpu->halImpl);
    if (status != NV_OK)
        return status;

    pGpu->pHal = halmgrGetHal(pHalMgr, pGpu->halImpl);

    return status;
}

static void
_gpuInitPciHandle
(
    OBJGPU *pGpu
)
{
    NvU32 domain   = gpuGetDomain(pGpu);
    NvU8  bus      = gpuGetBus(pGpu);
    NvU8  device   = gpuGetDevice(pGpu);
    NvU8  function = 0;

    pGpu->hPci = osPciInitHandle(domain, bus, device, function, NULL, NULL);
}

static NV_STATUS _gpuRmApiControl
(
    RM_API *pRmApi,
    NvHandle hClient,
    NvHandle hObject,
    NvU32 cmd,
    void *pParams,
    NvU32 paramsSize
)
{
    RmCtrlParams rmCtrlParams;
    CALL_CONTEXT callCtx, *oldCtx = NULL;
    RS_LOCK_INFO lockInfo = {0};
    NV_STATUS status = NV_OK;

    OBJGPU *pGpu = (OBJGPU*)pRmApi->pPrivateContext;

    // This API is only used to route locally on monolithic or UCODE
    NV_ASSERT_OR_RETURN(!IS_GSP_CLIENT(pGpu), NV_ERR_INVALID_STATE);

    //
    // The physical API can be used on any controls and any handles and it is
    // expected to be routed correctly. However, if the caller is using the GPU
    // internal handles, we can skip the resource server overhead and make a
    // direct function call instead.
    //
    // This optimization should be skipped on vGPU Guests as they should always
    // go through the resource server. The resource server is responsible for
    // deciding whether to RPC to the HOST (ROUTE_TO_VGPU_HOST) or call the 
    // implementation on the guest.
    //
    if (!IS_VIRTUAL(pGpu) && hClient == pGpu->hInternalClient && hObject == pGpu->hInternalSubdevice)
    {
        NV_ASSERT_OR_RETURN(pGpu->pCachedSubdevice && pGpu->pCachedRsClient, NV_ERR_INVALID_STATE);

        const struct NVOC_EXPORTED_METHOD_DEF *pEntry;
        pEntry = objGetExportedMethodDef((void*)pGpu->pCachedSubdevice, cmd);

        NV_ASSERT_OR_RETURN(pEntry != NULL, NV_ERR_NOT_SUPPORTED);

        NV_ASSERT_OR_RETURN(pEntry->paramSize == paramsSize, NV_ERR_INVALID_PARAM_STRUCT);
        NV_PRINTF(LEVEL_INFO, "GPU Internal RM control 0x%08x on gpuInst:%x hClient:0x%08x hSubdevice:0x%08x\n",
                  cmd, pGpu->gpuInstance, hClient, hObject);

        portMemSet(&rmCtrlParams, 0, sizeof(rmCtrlParams));
        rmCtrlParams.hClient    = hClient;
        rmCtrlParams.hObject    = hObject;
        rmCtrlParams.pGpu       = pGpu;
        rmCtrlParams.cmd        = cmd;
        rmCtrlParams.flags      = NVOS54_FLAGS_LOCK_BYPASS;
        rmCtrlParams.pParams    = pParams;
        rmCtrlParams.paramsSize = paramsSize;
        rmCtrlParams.secInfo.privLevel = RS_PRIV_LEVEL_KERNEL;
        rmCtrlParams.secInfo.paramLocation = PARAM_LOCATION_KERNEL;
        rmCtrlParams.bInternal  = NV_TRUE;

        lockInfo.flags = RM_LOCK_FLAGS_NO_GPUS_LOCK | RM_LOCK_FLAGS_NO_CLIENT_LOCK;
        rmCtrlParams.pLockInfo = &lockInfo;

        portMemSet(&callCtx, 0, sizeof(callCtx));
        callCtx.pResourceRef   = RES_GET_REF(pGpu->pCachedSubdevice);
        callCtx.pClient        = pGpu->pCachedRsClient;
        callCtx.secInfo        = rmCtrlParams.secInfo;
        callCtx.pServer        = &g_resServ;
        callCtx.pControlParams = &rmCtrlParams;
        callCtx.pLockInfo      = rmCtrlParams.pLockInfo;

        resservSwapTlsCallContext(&oldCtx, &callCtx);

        if (pEntry->paramSize == 0)
        {
            status = ((NV_STATUS(*)(void*))pEntry->pFunc)(pGpu->pCachedSubdevice);
        }
        else
        {
            status = ((NV_STATUS(*)(void*,void*))pEntry->pFunc)(pGpu->pCachedSubdevice, pParams);
        }

        resservRestoreTlsCallContext(oldCtx);
    }
    else
    {
        RM_API *pInternalRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
        NV_ASSERT_OR_RETURN(rmDeviceGpuLockIsOwner(pGpu->gpuInstance), NV_ERR_INVALID_LOCK_STATE);

        status = pInternalRmApi->Control(pInternalRmApi, hClient, hObject, cmd, pParams, paramsSize);
    }

    return status;
}

static NV_STATUS _gpuRmApiAllocWithHandle
(
    RM_API *pRmApi,
    NvHandle hClient,
    NvHandle hParent,
    NvHandle hObject,
    NvU32 hClass,
    void *pAllocParams,
    NvU32 paramsSize
)
{
    // Simple forwarder for now
    RM_API *pInternalRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    return pInternalRmApi->AllocWithHandle(pInternalRmApi, hClient, hParent, hObject, hClass, pAllocParams, paramsSize);
}
static NV_STATUS _gpuRmApiFree
(
    RM_API *pRmApi,
    NvHandle hClient,
    NvHandle hObject
)
{
    // Simple forwarder for now
    RM_API *pInternalRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
    return pInternalRmApi->Free(pInternalRmApi, hClient, hObject);
}

static void
_gpuInitPhysicalRmApi
(
    OBJGPU *pGpu
)
{
    // Populate all unused APIs with stubs
    pGpu->physicalRmApi = *rmapiGetInterface(RMAPI_STUBS);
    pGpu->physicalRmApi.pPrivateContext = pGpu;

    portMemSet(&pGpu->physicalRmApi.defaultSecInfo, 0,
               sizeof(pGpu->physicalRmApi.defaultSecInfo));
    pGpu->physicalRmApi.defaultSecInfo.privLevel     = RS_PRIV_LEVEL_KERNEL;
    pGpu->physicalRmApi.defaultSecInfo.paramLocation = PARAM_LOCATION_KERNEL;
    pGpu->physicalRmApi.bHasDefaultSecInfo           = NV_TRUE;
    pGpu->physicalRmApi.bTlsInternal                 = NV_TRUE;
    pGpu->physicalRmApi.bApiLockInternal             = NV_TRUE;
    pGpu->physicalRmApi.bRmSemaInternal              = NV_TRUE;
    pGpu->physicalRmApi.bGpuLockInternal             = NV_TRUE;

    // Only initialize the methods that exist on GSP/DCE as well
    pGpu->physicalRmApi.Control                      = _gpuRmApiControl;
    pGpu->physicalRmApi.AllocWithHandle              = _gpuRmApiAllocWithHandle;
    pGpu->physicalRmApi.Free                         = _gpuRmApiFree;
}

static NV_STATUS
_gpuInitChipInfo
(
    OBJGPU *pGpu
)
{
    RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);
    const NvU32 paramSize = sizeof(NV2080_CTRL_INTERNAL_GPU_GET_CHIP_INFO_PARAMS);
    NV_STATUS status;

    pGpu->pChipInfo = portMemAllocNonPaged(paramSize);
    NV_ASSERT_OR_RETURN(pGpu->pChipInfo != NULL, NV_ERR_NO_MEMORY);

    portMemSet(pGpu->pChipInfo, 0, paramSize);

    NV_ASSERT_OK_OR_GOTO(status, pRmApi->Control(pRmApi, pGpu->hInternalClient, pGpu->hInternalSubdevice,
                                                 NV2080_CTRL_CMD_INTERNAL_GPU_GET_CHIP_INFO,
                                                 pGpu->pChipInfo, paramSize), done);

    pGpu->chipInfo.subRevision  = pGpu->pChipInfo->chipSubRev;
    pGpu->idInfo.PCIDeviceID    = pGpu->pChipInfo->pciDeviceId;
    pGpu->idInfo.PCISubDeviceID = pGpu->pChipInfo->pciSubDeviceId;
    pGpu->idInfo.PCIRevisionID  = pGpu->pChipInfo->pciRevisionId;

done:
    if (status != NV_OK)
    {
        portMemFree(pGpu->pChipInfo);
        pGpu->pChipInfo = NULL;
    }

    return status;
}

static NV_STATUS
gpuInitVmmuInfo
(
    OBJGPU *pGpu
)
{
    NV_STATUS  status;
    RM_API    *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);

    NV2080_CTRL_GPU_GET_VMMU_SEGMENT_SIZE_PARAMS params;

    pGpu->vmmuSegmentSize = 0;

    status = pRmApi->Control(pRmApi, pGpu->hInternalClient, pGpu->hInternalSubdevice,
                             NV2080_CTRL_CMD_GPU_GET_VMMU_SEGMENT_SIZE,
                             &params, sizeof(params));

    if (status == NV_ERR_NOT_SUPPORTED)
    {
        // Leave segment size initialized to zero to signal no VMMU present on physical
        return NV_OK;
    }
    else if (status != NV_OK)
    {
        return status;
    }

    pGpu->vmmuSegmentSize = params.vmmuSegmentSize;

    return status;
}

static NV_STATUS _gpuAllocateInternalObjects
(
    OBJGPU *pGpu
)
{
    NV_STATUS status = NV_OK;

    if (IS_GSP_CLIENT(pGpu))
    {
        if (IsT234DorBetter(pGpu))
        {
            //
            // NOTE: We add +1 to the client base because DCE-RM will also
            // allocate internal objects, taking the !IS_GSP_CLIENT path below.
            //
            pGpu->hInternalClient = RS_CLIENT_INTERNAL_HANDLE_BASE + 1;
            pGpu->hInternalDevice = NV_GPU_INTERNAL_DEVICE_HANDLE;
            pGpu->hInternalSubdevice = NV_GPU_INTERNAL_SUBDEVICE_HANDLE;
        }
    }
    else
    {
        RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);
        NV_ASSERT_OK_OR_RETURN(rmapiutilAllocClientAndDeviceHandles(
            pRmApi, pGpu, &pGpu->hInternalClient, &pGpu->hInternalDevice, &pGpu->hInternalSubdevice));

        NV_ASSERT_OK_OR_GOTO(status, serverGetClientUnderLock(&g_resServ, pGpu->hInternalClient,
            &pGpu->pCachedRsClient), done);
        NV_ASSERT_OK_OR_GOTO(status, subdeviceGetByHandle(pGpu->pCachedRsClient, pGpu->hInternalSubdevice,
            &pGpu->pCachedSubdevice), done);
    }

    NV_PRINTF(LEVEL_INFO, "GPU-%d allocated hInternalClient=0x%08x\n", pGpu->gpuInstance, pGpu->hInternalClient);

done:
    if (status != NV_OK)
    {
        _gpuFreeInternalObjects(pGpu);
    }

    return status;
}

static void _gpuFreeInternalObjects
(
    OBJGPU *pGpu
)
{
    if (IS_GSP_CLIENT(pGpu))
    {
        rmapiControlCacheFreeObjectEntry(pGpu->hInternalClient, pGpu->hInternalSubdevice);
        rmapiControlCacheFreeObjectEntry(pGpu->hInternalClient, pGpu->hInternalDevice);
    }
    else
    {
        RM_API *pRmApi = rmapiGetInterface(RMAPI_GPU_LOCK_INTERNAL);

        rmapiutilFreeClientAndDeviceHandles(pRmApi,
            &pGpu->hInternalClient, &pGpu->hInternalDevice, &pGpu->hInternalSubdevice);
    }
}

static NV_STATUS
_gpuCreateEngineOrderList
(
     OBJGPU *pGpu
)
{
    NvU32                  i;
    NvU32                  numClassDesc;
    NvU32                  numLists;
    NV_STATUS              status = NV_OK;
    PGPU_ENGINE_ORDER      pEngineOrder = &pGpu->engineOrder;
    NvU32                  numEngineDesc, curEngineDesc;
    NvU32                  listTypes[] = {GCO_LIST_INIT, GCO_LIST_LOAD, GCO_LIST_UNLOAD, GCO_LIST_DESTROY};
    PENGDESCRIPTOR        *ppEngDescriptors[4];
    ENGLIST_ITER           it;
    ENGDESCRIPTOR          engDesc;

    ct_assert(NV_ARRAY_ELEMENTS(ppEngDescriptors) == NV_ARRAY_ELEMENTS(listTypes));

#define GPU_CHILD(a, b, numInstances, c, d) +numInstances

    struct ChildList {
        char children[ 0 +
            #include "gpu/gpu_child_list.h"
        ];
    };

    //
    // The maximum number of engines known to RM controls
    // must be at least the number of actual OBJGPU children.
    //
    ct_assert(NV2080_CTRL_GPU_MAX_ENGINE_OBJECTS >=
        sizeof(((struct ChildList*)(NULL))->children) /* sizeof(ChildList::children) */);

    numLists = NV_ARRAY_ELEMENTS(listTypes);

    ppEngDescriptors[0] = &pEngineOrder->pEngineInitDescriptors;
    ppEngDescriptors[1] = &pEngineOrder->pEngineLoadDescriptors;
    ppEngDescriptors[2] = &pEngineOrder->pEngineUnloadDescriptors;
    ppEngDescriptors[3] = &pEngineOrder->pEngineDestroyDescriptors;

    //
    // Find the size of the engine descriptor list. The sizes of all lists
    // are checked for consistency to catch mistakes.
    //
    // The list is copied into OBJGPU storage as it's modified during
    // dynamic engine removal (e.g.: gpuMissingEngDescriptor).
    //
    numEngineDesc = 0;

    for (i = 0; i < numLists; i++)
    {
        curEngineDesc = 0;

        it = gpuGetEngineOrderListIter(pGpu, listTypes[i]);

        while (gpuGetNextInEngineOrderList(pGpu, &it, &engDesc))
        {
            curEngineDesc++;
        }

        if ((numEngineDesc != 0) && (numEngineDesc != curEngineDesc))
        {
            NV_PRINTF(LEVEL_ERROR,
                      "Sizes of all engine order lists do not match!\n");
            NV_ASSERT(0);
            status = NV_ERR_INVALID_STATE;
            goto done;
        }

        numEngineDesc = curEngineDesc;
    }

    pEngineOrder->numEngineDescriptors = numEngineDesc;


    for (i = 0; i < numLists; i++)
    {
        curEngineDesc = 0;

        *ppEngDescriptors[i] = portMemAllocNonPaged(sizeof(ENGDESCRIPTOR) * numEngineDesc);
        if ( NULL == *ppEngDescriptors[i])
        {
             NV_ASSERT(0);
             status = NV_ERR_NO_MEMORY;
             goto done;
        }

        it = gpuGetEngineOrderListIter(pGpu, listTypes[i]);

        while (gpuGetNextInEngineOrderList(pGpu, &it, &engDesc))
        {
            (*ppEngDescriptors[i])[curEngineDesc] = engDesc;
            curEngineDesc++;
        }
    }

    pEngineOrder->pClassDescriptors = gpuGetClassDescriptorList_HAL(pGpu, &numClassDesc);
    pEngineOrder->numClassDescriptors = numClassDesc;

    return NV_OK;

done:
    portMemFree(pEngineOrder->pEngineInitDescriptors);
    pEngineOrder->pEngineInitDescriptors = NULL;

    portMemFree(pEngineOrder->pEngineDestroyDescriptors);
    pEngineOrder->pEngineDestroyDescriptors = NULL;

    portMemFree(pEngineOrder->pEngineLoadDescriptors);
    pEngineOrder->pEngineLoadDescriptors = NULL;

    portMemFree(pEngineOrder->pEngineUnloadDescriptors);
    pEngineOrder->pEngineUnloadDescriptors = NULL;

    return status;
}

static void
_gpuFreeEngineOrderList
(
    OBJGPU *pGpu
)
{
    PGPU_ENGINE_ORDER pEngineOrder = &pGpu->engineOrder;

    if (!pEngineOrder->pEngineInitDescriptors)
        return;

    portMemFree(pEngineOrder->pEngineInitDescriptors);
    portMemFree(pEngineOrder->pEngineDestroyDescriptors);
    portMemFree(pEngineOrder->pEngineLoadDescriptors);
    portMemFree(pEngineOrder->pEngineUnloadDescriptors);

    pEngineOrder->pEngineInitDescriptors    = NULL;
    pEngineOrder->pEngineDestroyDescriptors = NULL;
    pEngineOrder->pEngineLoadDescriptors    = NULL;
    pEngineOrder->pEngineUnloadDescriptors  = NULL;
    pEngineOrder->pClassDescriptors         = NULL;
}

/*!
 * @brief Returns a pointer to the GPU's pointer to a child specified by its childInfo
 *
 * @param[in] pGpu            OBJPGU pointer
 * @param[in] pChildInfo      Pointer to table entry
 */
static Dynamic**
gpuGetChildPtr(OBJGPU *pGpu, NvU32 gpuChildPtrOffset)
{
     return (Dynamic**)((NvU8*)pGpu + gpuChildPtrOffset);
}

/*!
 * @brief Looks up for an instance of engine
 *
 * @param[in]  classId
 * @param[in]  instanceID
 * @param[out] pChildInfoOut
 */
static NV_STATUS
gpuGetChildInfo(NVOC_CLASS_ID classId, NvU32 instanceID, PGPUCHILDINFO pChildInfoOut)
{
    PGPUCHILDTYPE pChildType;

    NV_ASSERT_OR_RETURN(pChildInfoOut, NV_ERR_INVALID_STATE);

    pChildType = gpuGetChildType(classId);

    NV_ASSERT_OR_RETURN(pChildType && (instanceID < pChildType->instances), NV_ERR_INVALID_OBJECT);

    pChildInfoOut->engDesc = MKENGDESC(classId, instanceID);
    pChildInfoOut->bConstructEarly = pChildType->bConstructEarly;
    pChildInfoOut->pChildType = pChildType;

    pChildInfoOut->gpuChildPtrOffset = pChildType->gpuChildPtrOffset + sizeof(void *) * instanceID;

    return NV_OK;
}

/*!
 * @brief   For a given @ref OBJGPU, given the @ref NVOC_CLASS_ID for an object
 *          that is a @ref OBJGPU child, this function will return the
 *          @ref NVOC_CLASS_INFO for the concrete class type that should be
 *          instantiated for the field with that @ref NVOC_CLASS_ID
 *
 * @details This function helps support polymorphism of @ref OBJGPU children.
 *          The provided @ref NVOC_CLASS_ID is the class ID for the base class
 *          of the pointer field in @ref OBJGPU (e.g., @ref OBJGPU::pPmu), which
 *          can be made to point at different sub-classes at runtime. This
 *          function, given that @ref NVOC_CLASS_ID, provides the information
 *          about which concrete sub-class should actually be constructed.
 *
 * @param[in]   pGpu
 * @param[in]   classId
 *  @ref NVOC_CLASS_ID for the class type of the base class pointer in
 *  @ref OBJGPU
 * @param[out]  ppClassInfo
 *  Pointer to location into which to store pointer to the class info for the
 *  concrete class to be constructed
 *
 * @return  @ref NV_OK
 *  Success
 * @return  @ref NV_ERR_INVALID_STATE
 *  No match for classId found within this @ref OBJGPU's children
 */
static NV_STATUS
_gpuChildNvocClassInfoGet
(
    OBJGPU                 *pGpu,
    NVOC_CLASS_ID           classId,
    const NVOC_CLASS_INFO **ppClassInfo
)
{
    NvU32 i;

    for (i = 0U; i < pGpu->numChildrenPresent; i++)
    {
        if (classId == pGpu->pChildrenPresent[i].classId)
        {
            *ppClassInfo = pGpu->pChildrenPresent[i].pClassInfo;
            return NV_OK;
        }
    }

    DBG_BREAKPOINT();
    return NV_ERR_INVALID_STATE;
}

/*!
 * @brief Looks up for a class in the table based on class id
 *
 * All classes are uniquely identified by their classId.
 *
 * @param[in] classId         NVOC_CLASS_ID
 */
static PGPUCHILDTYPE
gpuGetChildType(NVOC_CLASS_ID classId)
{
    NvU32 i;

    for (i = 0; i < GPU_NUM_CHILD_TYPES; i++)
    {
        if (gpuChildTypeList[i].classId == classId)
        {
            return &gpuChildTypeList[i];
        }
    }

    return NULL;
}

/**
 * @brief Initializes iterator for all possible ENGDESCRIPTORs that could be GPU
 *        children.
 *
 * @return        GPU_CHILD_ITER
 */
GPU_CHILD_ITER
gpuGetPossibleEngDescriptorIter(void)
{
    GPU_CHILD_ITER it = {0};
    return it;
}

/**
 * @brief Iterator over all possible ENGDESCRIPTORs that could be GPU children.
 *
 * @param[in,out] pIt          Iterator
 * @param[out]    pEngDesc     The next engine descriptor
 *
 * @return        NV_TRUE if *pEngDesc is valid, NV_FALSE if there are no more engines
 */
NvBool
gpuGetNextPossibleEngDescriptor(GPU_CHILD_ITER *pIt, ENGDESCRIPTOR *pEngDesc)
{
    PGPUCHILDTYPE pChildType;
    GPUCHILDINFO childInfo;

    if (pIt->childTypeIdx >= GPU_NUM_CHILD_TYPES)
        return NV_FALSE;

    pChildType = &gpuChildTypeList[pIt->childTypeIdx];

    // Advance instance #
    if (pIt->childInst < pChildType->instances)
    {
           NV_STATUS status = gpuGetChildInfo(pChildType->classId, pIt->childInst, &childInfo);

           NV_ASSERT(status == NV_OK);

           pIt->gpuChildPtrOffset = childInfo.gpuChildPtrOffset;
           pIt->childInst++;

           *pEngDesc = childInfo.engDesc;
           return NV_TRUE;
    }

    pIt->childTypeIdx++;
    pIt->childInst = 0;

    // Recurse (max depth is 1)
    return gpuGetNextPossibleEngDescriptor(pIt, pEngDesc);
}

/*!
 * @brief Returns the unshared engstate for the child object with the given engine
 * descriptor (i.e.: the ENGSTATE without any of the SLI sharing hacks).
 *
 * All engines are uniquely identified by their engine descriptor.
 *
 * @param[in] pGpu     OBJGPU pointer
 * @param[in] engDesc  ENGDESCRIPTOR
 */
POBJENGSTATE
gpuGetEngstateNoShare_IMPL(OBJGPU *pGpu, ENGDESCRIPTOR engDesc)
{
    ENGSTATE_ITER it = gpuGetEngstateIter(pGpu);
    OBJENGSTATE *pEngstate;

    while (gpuGetNextEngstate(pGpu, &it, &pEngstate))
    {
        if (engstateGetDescriptor(pEngstate) == engDesc)
        {
            return pEngstate;
        }
    }

    return NULL;
}

/*!
 * @brief Returns the engstate for the child object with the given engine descriptor
 *
 * All engines are uniquely identified by their engine descriptor.
 *
 * @param[in] pGpu     OBJGPU pointer
 * @param[in] engDesc  ENGDESCRIPTOR
 */
POBJENGSTATE
gpuGetEngstate_IMPL(OBJGPU *pGpu, ENGDESCRIPTOR engDesc)
{

    // Everything else is unshared
    return gpuGetEngstateNoShare(pGpu, engDesc);
}


/*!
 * @brief Iterates over pGpu's child engstates. Returns NV_FALSE when there are
 *        no more.
 *
 * @param[in]      pGpu         OBJGPU pointer
 * @param[in,out]  pIt          Iterator
 * @param[out]     ppEngState   The next engstate
 *
 * @return         NV_TRUE if ppEngstate is valid, NV_FALSE if no more found
 */
NvBool
gpuGetNextEngstate_IMPL(OBJGPU *pGpu, ENGSTATE_ITER *pIt, OBJENGSTATE **ppEngstate)
{
    ENGDESCRIPTOR   engDesc;
    OBJENGSTATE    *pEngstate;
    Dynamic       **ppChild;

    while (gpuGetNextPossibleEngDescriptor(pIt, &engDesc))
    {
        ppChild = gpuGetChildPtr(pGpu, pIt->gpuChildPtrOffset);
        if (*ppChild != NULL)
        {
            pEngstate = dynamicCast(*ppChild, OBJENGSTATE);
            if (pEngstate != NULL)
            {
                *ppEngstate = pEngstate;
                return NV_TRUE;
            }
        }
    }

    return NV_FALSE;
}

/*!
 * @brief The generic object constructor
 *
 * @param[in] pGpu         POBJGPU
 * @param[in] classId      NVOC_CLASS_ID
 * @param[in] instanceID   NvU32
 *
 */
NV_STATUS
gpuCreateObject_IMPL
(
    OBJGPU       *pGpu,
    NVOC_CLASS_ID classId,
    NvU32         instanceID
)
{
    NV_STATUS                   status;
    OBJENGSTATE                *pEngstate;
    GPUCHILDINFO                childInfo;
    Dynamic                   **ppChildPtr;
    const NVOC_CLASS_INFO      *pClassInfo;
    Dynamic                    *pConcreteChild      = NULL;
    ENGSTATE_TRANSITION_DATA    engTransitionData;

    status       = gpuGetChildInfo(classId, instanceID, &childInfo);

    NV_ASSERT_OR_RETURN(status == NV_OK, status);

    ppChildPtr    = gpuGetChildPtr(pGpu, childInfo.gpuChildPtrOffset);

    NV_ASSERT_OK_OR_RETURN(
        _gpuChildNvocClassInfoGet(pGpu, classId, &pClassInfo));

    // Ask the object database utility to create a child object.
    status = objCreateDynamic(&pConcreteChild, pGpu, pClassInfo);

    if (status != NV_OK)
    {
        return status;
    }
    NV_ASSERT_OR_RETURN(pConcreteChild != NULL, NV_ERR_INVALID_STATE);

    //
    // Cast back to a pointer to the base class and assign it into the pointer
    // in OBJGPU
    //
    *ppChildPtr = objDynamicCastById(pConcreteChild, classId);
    NV_ASSERT_TRUE_OR_GOTO(status,
        (*ppChildPtr != NULL),
        NV_ERR_INVALID_STATE,
        gpuCreateObject_exit);

    pEngstate = dynamicCast(*ppChildPtr, OBJENGSTATE);

    if (pEngstate == NULL)
    {
        status = NV_ERR_INVALID_STATE;
        goto gpuCreateObject_exit;
    }

    status = engstateConstructBase(pEngstate, pGpu, childInfo.engDesc);
    NV_ASSERT_OR_GOTO(status == NV_OK, gpuCreateObject_exit);

    engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_CONSTRUCT, &engTransitionData);
    status = engstateConstructEngine(pGpu, pEngstate, childInfo.engDesc);
    engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_CONSTRUCT, &engTransitionData);

    // If engine is missing, free it immediately
    if (pEngstate->getProperty(pEngstate, PDB_PROP_ENGSTATE_IS_MISSING))
    {
        status = NV_ERR_NOT_SUPPORTED;
    }

gpuCreateObject_exit:
    if (status != NV_OK)
    {
        objDelete(pConcreteChild);
        *ppChildPtr    = NULL;
    }

    return status;
}


void
gpuDestruct_IMPL
(
    OBJGPU *pGpu
)
{
    HWBC_LIST           *pGpuHWBCList = NULL;
    int                  typeNum;
    int                  instNum;
    GPUCHILDTYPE        *pChildTypeCur;
    GPUCHILDINFO         childInfoCur;
    Dynamic            **pChildPtr;

    // Free GPU shared data
    if (pGpu->userSharedData.pMemDesc != NULL)
    {
        gpushareddataDestroy(pGpu);
    }

    // Free children in reverse order from construction
    for (typeNum = GPU_NUM_CHILD_TYPES - 1; typeNum >= 0; typeNum--)
    {
        pChildTypeCur = &gpuChildTypeList[typeNum];

        for (instNum = pChildTypeCur->instances - 1; instNum >= 0; instNum--)
        {
            NV_STATUS status;

            status = gpuGetChildInfo(pChildTypeCur->classId, instNum, &childInfoCur);

            NV_ASSERT(status == NV_OK);

            pChildPtr = gpuGetChildPtr(pGpu, childInfoCur.gpuChildPtrOffset);

            objDelete(*pChildPtr);
            *pChildPtr = NULL;
        }
    }

    //
    // If device instance is unassigned, we haven't initialized far enough to
    // do any accounting with it
    //
    if (gpuGetDeviceInstance(pGpu) != NV_MAX_DEVICES)
    {
        rmapiReportLeakedDevices(gpuGetGpuMask(pGpu));
    }

    _gpuFreeEngineOrderList(pGpu);

    portMemFree(pGpu->pUserRegisterAccessMap);
    pGpu->pUserRegisterAccessMap = NULL;

    portMemFree(pGpu->pUnrestrictedRegisterAccessMap);
    pGpu->pUnrestrictedRegisterAccessMap = NULL;

    portMemFree(pGpu->pDeviceInfoTable);
    pGpu->pDeviceInfoTable = NULL;
    pGpu->numDeviceInfoEntries = 0;

    pGpu->userRegisterAccessMapSize = 0;

    gpuDestroyEngineTable(pGpu);
    gpuDestroyClassDB(pGpu);
    osDestroyOSHwInfo(pGpu);

    while(pGpu->pHWBCList)
    {
        pGpuHWBCList = pGpu->pHWBCList;
        pGpu->pHWBCList = pGpuHWBCList->pNext;
        portMemFree(pGpuHWBCList);
    }

    //
    // Destroy and free the RegisterAccess object linked to this GPU
    // This should be moved out to gpu_mgr in the future to line up with
    // the construction, but currently depends on pGpu still existing
    //
    regAccessDestruct(&pGpu->registerAccess);

    NV_ASSERT(pGpu->numConstructedFalcons == 0);

    portMemFree(pGpu->pRegopOffsetScratchBuffer);
    pGpu->pRegopOffsetScratchBuffer = NULL;

    portMemFree(pGpu->pRegopOffsetAddrScratchBuffer);
    pGpu->pRegopOffsetAddrScratchBuffer = NULL;

    pGpu->regopScratchBufferMaxOffsets = 0;

    NV_ASSERT(pGpu->numSubdeviceBackReferences == 0);
    portMemFree(pGpu->pSubdeviceBackReferences);
    pGpu->pSubdeviceBackReferences = NULL;
    pGpu->numSubdeviceBackReferences = 0;
    pGpu->maxSubdeviceBackReferences = 0;

    gpuDestructPhysical(pGpu);
}

/*!
 * @brief   Initializes @ref OBJGPU::pChildrenPresent data
 *
 * @param[in]   pGpu
 *
 * @return  @ref NV_OK
 *  Success
 */
static NV_STATUS
_gpuChildrenPresentInit
(
    OBJGPU *pGpu
)
{
    pGpu->pChildrenPresent =
        gpuGetChildrenPresent_HAL(pGpu, &pGpu->numChildrenPresent);
    return NV_OK;
}

static NV_STATUS
gpuCreateChildObjects
(
    OBJGPU *pGpu,
    NvBool  bConstructEarly
)
{
    PGPUCHILDTYPE  pChildTypeCur;
    GPUCHILDINFO   childInfoCur;
    NvU32          t, i;
    NV_STATUS      rmStatus = NV_OK;


    for (t = 0; t < GPU_NUM_CHILD_TYPES; t++)
    {
        pChildTypeCur = &gpuChildTypeList[t];

        for (i = 0; i < pChildTypeCur->instances; i++)
        {
            NVOC_CLASS_ID classId = pChildTypeCur->classId;

            rmStatus = gpuGetChildInfo(classId, i, &childInfoCur);

            NV_ASSERT(rmStatus == NV_OK);

            if ((bConstructEarly == childInfoCur.bConstructEarly) &&
                gpuShouldCreateObject(pGpu,
                                      &childInfoCur))
            {
                rmStatus = gpuCreateObject(pGpu, classId, i);

                // RMCONFIG:  Bail on errors unless the feature/object/engine/class
                //            is simply unsupported
                if (rmStatus == NV_ERR_NOT_SUPPORTED)
                {
                    rmStatus = NV_OK;
                }
                else if (rmStatus != NV_OK)
                {
                    return rmStatus;
                }
            }
        }

        // Bail out of both loops.
        if (rmStatus != NV_OK)
        {
            break;
        }
    }

    return rmStatus;
}

static NvBool
gpuShouldCreateObject
(
    OBJGPU *pGpu,
    GPUCHILDINFO *pChildInfo
)
{
    NvBool retVal = NV_FALSE;
    NvU32 childIdx;

    // Let the HAL confirm that we should create an object for this engine.
    for (childIdx = 0; childIdx < pGpu->numChildrenPresent; childIdx++)
    {
        if ((ENGDESC_FIELD(pChildInfo->engDesc, _CLASS) ==
                pGpu->pChildrenPresent[childIdx].classId))
        {
            retVal = (ENGDESC_FIELD(pChildInfo->engDesc, _INST) <
                        pGpu->pChildrenPresent[childIdx].instances);
            break;
        }
    }

    return retVal;
}

NvU32
gpuGetGpuMask_IMPL
(
    OBJGPU *pGpu
)
{
    if (IsSLIEnabled(pGpu))
    {
        return 1 << (gpumgrGetSubDeviceInstanceFromGpu(pGpu));
    }
    else
    {
        return 1 << (pGpu->gpuInstance);
    }
}

static NV_STATUS gspSupportsEngine(OBJGPU *pGpu, ENGDESCRIPTOR engdesc, NvBool *supports)
{
    RM_ENGINE_TYPE clientEngineId = 0;

    if (!IS_GSP_CLIENT(pGpu))
        return NV_WARN_NOTHING_TO_DO;

    if (gpuXlateEngDescToClientEngineId(pGpu, engdesc, &clientEngineId) != NV_OK)
    {
        NV_PRINTF(LEVEL_INFO, "Failed to xlate engdesc 0x%x\n", engdesc);
        return NV_WARN_NOTHING_TO_DO;
    }

    if (pGpu->gspSupportedEngines == NULL)
    {
        pGpu->gspSupportedEngines = portMemAllocNonPaged(sizeof(*pGpu->gspSupportedEngines));
        NV_ASSERT_OR_RETURN(pGpu->gspSupportedEngines != NULL, NV_ERR_NO_MEMORY);

        RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);

        NV_STATUS status = pRmApi->Control(pRmApi,
                        pGpu->hInternalClient,
                        pGpu->hInternalSubdevice,
                        NV2080_CTRL_CMD_GPU_GET_ENGINES_V2,
                        pGpu->gspSupportedEngines,
                        sizeof(*pGpu->gspSupportedEngines));

        if (status != NV_OK)
        {
            portMemFree(pGpu->gspSupportedEngines);
            return status;
        }
    }

    NvU32 i;
    for (i = 0; i < pGpu->gspSupportedEngines->engineCount; i++)
    {
        if (gpuGetRmEngineType(pGpu->gspSupportedEngines->engineList[i]) == clientEngineId)
        {
            *supports = NV_TRUE;
            return NV_OK;
        }
    }

    *supports = NV_FALSE;
    return NV_OK;
}

/*!
 *   The engine removal protocol is as follows:
 *   -   engines returning an error code from ConstructEngine will be immediately
 *       removed (this happens in gpuCreateObject)
 *   -   engines may set ENGSTATE_IS_MISSING at any time before gpuStatePreInit
 *   -   engines with ENGSTATE_IS_MISSING set at gpuStatePreInit will be removed
 *   -   engines that return NV_FALSE from engstateIsPresent at gpuStatePreInit
 *       will be removed
 *
 *   gpuRemoveMissingEngines takes place before the main loop in gpuStatePreInit
 *   and is responsible for removing engines satisfying the last two bullets
 *   above.
 *
 *   Additionally, note that this function handles engines that were never
 *   present according to gpuChildrenPresent_HAL; these engines' associated
 *   classes must be removed from the class DB, and that is handled via this
 *   function.
 */
static NV_STATUS
gpuRemoveMissingEngines
(
    OBJGPU        *pGpu
)
{
    NvU32          curEngDescIdx;
    PENGDESCRIPTOR engDescriptorList = gpuGetInitEngineDescriptors(pGpu);
    NvU32          numEngDescriptors = gpuGetNumEngDescriptors(pGpu);
    NV_STATUS      rmStatus = NV_OK;
    NvU32          curClassDescIdx;

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        OBJENGSTATE  *pEngstate;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        NVOC_CLASS_ID curClassId = ENGDESC_FIELD(curEngDescriptor, _CLASS);

        if (curClassId == classId(OBJINVALID))
        {
            continue;
        }

        pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate != NULL)
        {
            if (!pEngstate->getProperty(pEngstate, PDB_PROP_ENGSTATE_IS_MISSING) &&
                engstateIsPresent(pGpu, pEngstate))
            {
                continue;
            }

            gpuDestroyMissingEngine(pGpu, pEngstate);
            pEngstate = NULL;
        }

        //
        // pEngstate is NULL or missing, so we must be sure to unregister
        // all associated API classes and remove the stale engine descriptors
        // from the GPU HAL engine lists.
        //
        NV_PRINTF(LEVEL_INFO, "engine %d:%d is missing, removing\n",
                  ENGDESC_FIELD(curEngDescriptor, _CLASS),
                  ENGDESC_FIELD(curEngDescriptor, _INST));

        rmStatus = gpuDeleteEngineOnPreInit(pGpu, curEngDescriptor);
        NV_ASSERT(rmStatus == NV_OK || !"Error while trying to remove missing engine");
    }

    //
    // Check the rest of the class descriptors for engines that are not part of
    // the present list, and therefore not part of the init list, and ensure
    // that those get removed from the class DB.
    //
    for (curClassDescIdx = 0U;
         curClassDescIdx < pGpu->engineOrder.numClassDescriptors;
         curClassDescIdx++)
    {
        const GPU_RESOURCE_DESC *const pCurDesc     =
            &pGpu->engineOrder.pClassDescriptors[curClassDescIdx];
        NvBool bGspSupportsEngine;

        //
        // Skip any classes which:
        //  1.) Do not have an engine class ID
        //  2.) Have an engine of GPU. ENG_GPU does not correspond to an
        //      OBJENGSTATE, and it can never be missing.
        //  3.) Have an OBJENGSTATE that is present
        //
        if ((ENGDESC_FIELD(pCurDesc->engDesc, _CLASS) == classId(OBJINVALID)) ||
            (pCurDesc->engDesc == ENG_GPU) ||
            gpuGetEngstate(pGpu, pCurDesc->engDesc) != NULL)
        {
            continue;
        }

        //
        // If the engstate is NULL, the engine may still be supported on GSP. If
        // it is, we can skip removing it.
        //
        // Note that if the function returns NV_WARN_NOTHING_TO_DO, this means
        // GSP-RM is not running and thus the engine cannot be supported within
        // GSP.
        //
        rmStatus = gspSupportsEngine(pGpu, pCurDesc->engDesc, &bGspSupportsEngine);
        if (rmStatus == NV_WARN_NOTHING_TO_DO)
        {
            rmStatus = NV_OK;
            bGspSupportsEngine = NV_FALSE;
        }
        NV_ASSERT_OK_OR_RETURN(rmStatus);
        if (bGspSupportsEngine)
        {
            continue;
        }

        NV_ASSERT_OK_OR_RETURN(
            gpuDeleteClassFromClassDBByEngTag(pGpu, pCurDesc->engDesc));
    }

    // Update the engine table after deleting any classes from the class DB
    NV_ASSERT_OK_OR_RETURN(
        gpuUpdateEngineTable(pGpu));

    return rmStatus;
}

/*
 * Removing classes from classDB of a missing engine
 */
static void
gpuRemoveMissingEngineClasses
(
    OBJGPU      *pGpu,
    NvU32       missingEngDescriptor
)
{
    NvU32   numClasses, i;
    NvU32  *pClassList = NULL;
    if (gpuGetClassList(pGpu, &numClasses, NULL, missingEngDescriptor) == NV_OK)
    {
        pClassList = portMemAllocNonPaged(sizeof(NvU32) * numClasses);
        if (NV_OK == gpuGetClassList(pGpu, &numClasses, pClassList, missingEngDescriptor))
        {
            for (i = 0; i < numClasses; i++)
            {
                gpuDeleteClassFromClassDBByClassId(pGpu, pClassList[i]);
            }
        }
        portMemFree(pClassList);
        pClassList = NULL;
    }
}

/*
 *  Destroy and unregister engine object of a missing engine
 */
static void
gpuDestroyMissingEngine
(
    OBJGPU      *pGpu,
    OBJENGSTATE *pEngstate
)
{
    GPUCHILDINFO  childInfo;
    Dynamic     **pChildPtr;
    NV_STATUS     status;
    ENGDESCRIPTOR engDesc;

    engstateInitMissing(pGpu, pEngstate);

    engDesc = engstateGetDescriptor(pEngstate);

    status = gpuGetChildInfo(ENGDESC_FIELD(engDesc, _CLASS), ENGDESC_FIELD(engDesc, _INST), &childInfo);

    NV_ASSERT_OR_RETURN_VOID(status == NV_OK);

    pChildPtr = gpuGetChildPtr(pGpu, childInfo.gpuChildPtrOffset);

    objDelete(*pChildPtr);
    *pChildPtr = NULL;
}

/*
 * @brief Find if given engine descriptor is supported by GPU
 *
 * @param[in] pGpu              OBJGPU pointer
 * @param[in] descriptor        engine descriptor to search for
 *
 * @returns NV_TRUE if given engine descriptor was found in a
 * given engine descriptor list, NV_FALSE otherwise.
 *
 */
NvBool
gpuIsEngDescSupported_IMPL
(
        OBJGPU *pGpu,
        NvU32 descriptor
)
{
    NvU32           numEngDescriptors   = gpuGetNumEngDescriptors(pGpu);
    PENGDESCRIPTOR  pEngDescriptor      = gpuGetInitEngineDescriptors(pGpu);
    NvU32           counter             = 0;
    NvBool          engDescriptorFound  = NV_FALSE;

    for (counter = 0; counter < numEngDescriptors; counter++)
    {
        if (pEngDescriptor[counter] == descriptor)
        {
            engDescriptorFound = NV_TRUE;
            break;
        }
    }

    return engDescriptorFound;
}
/*!
 * @brief Mark given Engine Descriptor with ENG_INVALID engine descriptor.
 *
 * Note: It is legal to have more than one entry with equal Descriptor
 * in the Engine Descriptor list.
  *
 * @param[in] pEngDescriptor    Pointer to array of engine descriptors
 * @param[in] maxDescriptors    Size of engine descriptor array
 * @param[in] descriptor        Engine descriptor to be changed to ENG_INVALID engine descriptor
 *
 * @returns void
 */
static void
gpuMissingEngDescriptor(PENGDESCRIPTOR pEngDescriptor, NvU32 maxDescriptors,
                        ENGDESCRIPTOR descriptor)
{
    NvU32 counter;

    for (counter = 0; counter < maxDescriptors; counter++)
    {
        if (pEngDescriptor[counter] == descriptor)
        {
            pEngDescriptor[counter] = ENG_INVALID;
        }
    }
}


/*!
 * @brief Delete an engine from class DB.
 *
 * WARNING! Function doesn't remove INIT/DESTROY engines from HAL lists.
 * gpuInitEng and gpuDestroyEng won't be no-ops for relevant engine.
 *
 * Use case:
 *  If an engine needs to be removed, but StateInit/Destroy are required.
 *  It's better to use gpuDeleteEngineOnPreInit instead.
 *
 * @param[in] pGpu          OBJGPU pointer
 * @param[in] engDesc       Engine ID to search and remove
 *
 * @returns NV_STATUS - NV_OK always.
 */
NV_STATUS
gpuDeleteEngineFromClassDB_IMPL(OBJGPU *pGpu, NvU32 engDesc)
{
    PENGDESCRIPTOR pEngDesc          = NULL;
    NvU32          numEngDescriptors = gpuGetNumEngDescriptors(pGpu);
    NvU32          engDescriptor     = engDesc;

    // remove Class tagged with engDesc from Class Database
    gpuDeleteClassFromClassDBByEngTag(pGpu, engDesc);

    //
    // Bug 370327
    // Q: Why remove load/unload?
    // A: Since this engine does not exist, we should prevent hw accesses to it
    //    which should ideally only take place in load/unload ( not init/destroy )
    //
    // Q: Why not remove init/destroy, the engines gone right?
    // A: If init does some alloc and loadhw does the probe then removing destroy
    //    will leak.
    //

    // Remove load
    pEngDesc = gpuGetLoadEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    // Remove unload
    pEngDesc = gpuGetUnloadEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    pGpu->engineDB.bValid = NV_FALSE;

    return NV_OK;
}

/*!
 * @brief Delete an engine from class DB only prior or on gpuPreInit stage.
 *
 * WARNING! Function must be used only before INIT stage, to avoid leaks.
 * See gpuDeleteEngineFromClassDB for more information.
 *
 * Function removes Classes with given Engine Tag from class DB
 * and removes Engines from HAL lists with equal Engine Tags.
 * Function doesn't remove Engines from HAL Sync list,
 * see gpuDeleteEngineFromClassDB for more information.
 *
 * Use case:
 * Any platform where an engine is absent and it is required to
 * prevent engine's load/unload and init/destroy calls from getting executed.
 * In other words, this function is used when it is OK to remove/STUB all
 * of the HALs of an engine without jeopardizing the initialization and
 * operation of other engines.
 *
 * @param[in] pGpu          OBJGPU pointer
 * @param[in] engDesc       Engine ID to search and remove
 *
 * @returns NV_STATUS - NV_OK on success, error otherwise.
 *
 */
NV_STATUS
gpuDeleteEngineOnPreInit_IMPL(OBJGPU *pGpu, NvU32 engDesc)
{
    PENGDESCRIPTOR pEngDesc          = NULL;
    NvU32          numEngDescriptors = gpuGetNumEngDescriptors(pGpu);
    ENGDESCRIPTOR  engDescriptor     = engDesc;
    NV_STATUS      rmStatus = NV_OK;
    NvBool bGspSupported = NV_FALSE;

    rmStatus = gspSupportsEngine(pGpu, engDesc, &bGspSupported);
    if (rmStatus == NV_WARN_NOTHING_TO_DO)
        rmStatus = NV_OK;

    NV_ASSERT_OK_OR_RETURN(rmStatus);

    // remove Class tagged with engDesc from Class Database.
    if (!bGspSupported)
        gpuDeleteClassFromClassDBByEngTag(pGpu, engDesc);

    // Remove Load Engine Descriptors
    pEngDesc = gpuGetLoadEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    // Remove Unload Engine Descriptors
    pEngDesc = gpuGetUnloadEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    // Remove Init Engine Descriptors
    pEngDesc = gpuGetInitEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    // Remove Destroy Engine Descriptors
    pEngDesc = gpuGetDestroyEngineDescriptors(pGpu);
    gpuMissingEngDescriptor(pEngDesc, numEngDescriptors,
                            engDescriptor);

    if (!bGspSupported)
    {
        rmStatus = gpuUpdateEngineTable(pGpu);
        if (rmStatus != NV_OK)
        {
            NV_PRINTF(LEVEL_ERROR, "Update engine table operation failed!\n");
            DBG_BREAKPOINT();
        }
    }

    return rmStatus;
}

/*!
 * @brief Perform GPU pre init tasks
 *
 * Function tries to pre-init all engines from HAL Init Engine Descriptor list.
 * If engine is not present, or its engine pre-init function reports it is unsupported
 * then engine will be deleted from Class DB and HAL lists.
 *
 * TODO: Merge structurally equivalent code with other gpuState* functions.
 * TODO: Fix "init missing" concept to not create unsupported objects at all.
 *
 * @param[in] pGpu  OBJGPU pointer
 *
 * @returns NV_OK upon successful pre-initialization
 */
NV_STATUS
gpuStatePreInit_IMPL
(
    OBJGPU *pGpu
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    NV_STATUS      rmStatus = NV_OK;

    LOCK_ASSERT_AND_RETURN(rmGpuLockIsOwner());

    NV_ASSERT_OK_OR_RETURN(_gpuAllocateInternalObjects(pGpu));
    NV_ASSERT_OK_OR_RETURN(_gpuInitChipInfo(pGpu));
    NV_ASSERT_OK_OR_RETURN(gpuConstructUserRegisterAccessMap(pGpu));
    NV_ASSERT_OK_OR_RETURN(gpuBuildGenericKernelFalconList(pGpu));

    rmStatus = gpuRemoveMissingEngines(pGpu);
    NV_ASSERT(rmStatus == NV_OK);

    pGpu->bFullyConstructed = NV_TRUE;

    engDescriptorList = gpuGetInitEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_PRE_INIT, &engTransitionData);
        rmStatus = engstateStatePreInit(pGpu, pEngstate);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_PRE_INIT, &engTransitionData);

        if (rmStatus == NV_ERR_NOT_SUPPORTED)
        {
            switch (curEngDescriptor)
            {
                //
                // Allow removing kernel engines in StatePreInit if their
                // physical counterpart is absent.
                //
                case ENG_KERNEL_DISPLAY:
                    // On Displayless GPU's, Display Engine is not present. So, RM should not keep the display
                    // classes in GET_CLASSLIST. Hence removing the Display classes from the ClassDB
                    gpuRemoveMissingEngineClasses(pGpu, ENG_KERNEL_DISPLAY);
                    break;
                //
                // Explicitly track engines that trigger this block
                // so that we can verify they function properly
                // after they are no longer removed here.
                //
                case ENG_INFOROM:
                    // TODO: try to remove this special case
                    NV_PRINTF(LEVEL_WARNING,
                        "engine removal in PreInit with NV_ERR_NOT_SUPPORTED is deprecated (%s)\n",
                        engstateGetName(pEngstate));
                    break;
                default:
                    NV_PRINTF(LEVEL_ERROR,
                        "disallowing NV_ERR_NOT_SUPPORTED PreInit removal of untracked engine (%s)\n",
                        engstateGetName(pEngstate));
                    DBG_BREAKPOINT();
                    NV_ASSERT(0);
                    break;
            }

            gpuDestroyMissingEngine(pGpu, pEngstate);
            pEngstate = NULL;

            rmStatus = gpuDeleteEngineOnPreInit(pGpu, curEngDescriptor);
            // TODO: destruct engine here after MISSING support is removed
            NV_ASSERT(rmStatus == NV_OK || !"Error while trying to remove missing engine");
        }
        else if (rmStatus != NV_OK)
        {
            break;
        }
    }

    gpuInitOptimusSettings(pGpu);

    return rmStatus;
}

// TODO: Merge structurally equivalent code with other gpuState* functions.
NV_STATUS
gpuStateInit_IMPL
(
    OBJGPU *pGpu
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    NV_STATUS      rmStatus = NV_OK;

    // Initialize numaNodeId to invalid node ID as "0" can be considered valid node
    pGpu->numaNodeId = NV0000_CTRL_NO_NUMA_NODE;

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // TODO: Move the below code into appropriate ENGSTATE objects.
    //       DO NOT ADD MORE SPECIAL CASES HERE!
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    engDescriptorList = gpuGetInitEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    LOCK_ASSERT_AND_RETURN(rmGpuLockIsOwner());

    // Do this before calling stateInit() of child engines.
    objCreate(&pGpu->pPrereqTracker, pGpu, PrereqTracker, pGpu);

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // TODO: Move the above code into appropriate ENGSTATE objects.
    //       DO NOT ADD MORE SPECIAL CASES HERE!
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_INIT, &engTransitionData);
        rmStatus = engstateStateInit(pGpu, pEngstate);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_INIT, &engTransitionData);

        // RMCONFIG:  Bail on errors unless the feature/object/engine/class
        //            is simply unsupported

        if (rmStatus == NV_ERR_NOT_SUPPORTED)
            rmStatus = NV_OK;
        if (rmStatus != NV_OK)
            goto gpuStateInit_exit;
    }

    // Set a property indicating that VF BAR0 MMU TLB Invalidation register emulation is required or not.
    if (hypervisorIsVgxHyper())
    {
        if (
           0)
        {
            NvU32 data32 = NV_REG_STR_BUG_3007008_EMULATE_VF_MMU_TLB_INVALIDATE_DEFAULT;

            // Registry override to change default mode, i.e, emulate VF MMU TLB Invalidation register
            if ((osReadRegistryDword(pGpu, NV_REG_STR_BUG_3007008_EMULATE_VF_MMU_TLB_INVALIDATE, &data32) == NV_OK) &&
                (data32 == NV_REG_STR_BUG_3007008_EMULATE_VF_MMU_TLB_INVALIDATE_DISABLE))
            {
                pGpu->setProperty(pGpu, PDB_PROP_GPU_BUG_3007008_EMULATE_VF_MMU_TLB_INVALIDATE, NV_FALSE);
            }
        }
        else
        {
            pGpu->setProperty(pGpu, PDB_PROP_GPU_BUG_3007008_EMULATE_VF_MMU_TLB_INVALIDATE, NV_FALSE);
        }
    }

    // Set a property indicating that the state initialization has been done
    pGpu->setProperty(pGpu, PDB_PROP_GPU_STATE_INITIALIZED, NV_TRUE);

gpuStateInit_exit:
    return rmStatus;
}

/*!
 * @brief Top level pre-load routine
 *
 * Provides a mechanism to resolve cyclic dependencies between engines.
 *
 * StatePreLoad() is called before StateLoad() likewise StatePostUnload() is
 * called after StateUnload().
 *
 * Dependencies which are DAGs should continue to be resolved by reordering the
 * engine descriptor lists. Reordering the descriptor lists won't solve cyclic
 * dependencies as at least one constraint would always be violated.
 *
 * TODO: Merge structurally equivalent code with other gpuState* functions.
 *
 * @param[in] pGpu       OBJPGU pointer
 * @param[in] flags      Type of transition
 */
static NV_STATUS
gpuStatePreLoad
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    NV_STATUS      rmStatus = NV_OK;

    engDescriptorList = gpuGetLoadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        RMTRACE_ENGINE_PROFILE_EVENT("gpuStatePreLoadEngStart", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_PRE_LOAD, &engTransitionData);
        rmStatus = engstateStatePreLoad(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_PRE_LOAD, &engTransitionData);

        RMTRACE_ENGINE_PROFILE_EVENT("gpuStatePreLoadEngEnd", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

        //
        // An engine load leaving the broadcast status to NV_TRUE
        // will most likely mess up the pre-load of the next engines
        //
        NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

        // RMCONFIG:  Bail on errors unless the feature/object/engine/class
        //            is simply unsupported
        if (rmStatus == NV_ERR_NOT_SUPPORTED)
            rmStatus = NV_OK;
        if (rmStatus != NV_OK)
            break;

        //
        // Release and re-acquire the lock to allow interrupts
        //
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);

        rmStatus = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE,
                                     RM_LOCK_MODULES_STATE_LOAD);
        if (rmStatus != NV_OK)
            break;
    }

    return rmStatus;
}

// TODO: Merge structurally equivalent code with other gpuState* functions.
NV_STATUS
gpuStateLoad_IMPL
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    NV_STATUS      rmStatus = NV_OK;
    NvU32          status   = NV_OK;

    pGpu->registerAccess.regReadCount = pGpu->registerAccess.regWriteCount = 0;
    RMTRACE_ENGINE_PROFILE_EVENT("gpuStateLoadStart", pGpu->gpuId, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

    _gpuDetectNvswitchSupport(pGpu);

    // Initialize SRIOV specific members of OBJGPU
    status = gpuInitSriov_HAL(pGpu);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "Error initializing SRIOV: 0x%0x\n", status);
        return status;
    }

    if (!(flags & GPU_STATE_FLAGS_PRESERVING))
    {
        // It is a no-op on baremetal and inside non SRIOV guest.
        rmStatus = gpuCreateDefaultClientShare_HAL(pGpu);
        if (rmStatus != NV_OK)
        {
            return rmStatus;
        }
    }
    NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

    rmStatus = gpuStatePreLoad(pGpu, flags);
    if (rmStatus != NV_OK)
    {
        //
        // return early if we broke out of the preLoad sequence with
        // rmStatus != NV_OK
        //
        return rmStatus;
    }

    engDescriptorList = gpuGetLoadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Set indicator that we are running state load
    pGpu->bStateLoading = NV_TRUE;

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        RMTRACE_ENGINE_PROFILE_EVENT("gpuStateLoadEngStart", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_LOAD, &engTransitionData);
        rmStatus = engstateStateLoad(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_LOAD, &engTransitionData);


        // TODO: This is temporary and may be dead with TESLA
        if (rmStatus == NV_ERR_INVALID_ADDRESS)
        {
            NV_PRINTF(LEVEL_ERROR, "NV_ERR_INVALID_ADDRESS is no longer supported in StateLoad (%s)\n",
                engstateGetName(pEngstate));
            DBG_BREAKPOINT();
        }

        //
        // An engine load leaving the broadcast status to NV_TRUE
        // will most likely mess up the load of the next engines
        //
        NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

        // RMCONFIG:  Bail on errors unless the feature/object/engine/class
        //            is simply unsupported
        if (rmStatus == NV_ERR_NOT_SUPPORTED)
            rmStatus = NV_OK;
        if (rmStatus != NV_OK)
        {
            goto gpuStateLoad_exit;
        }

        //
        // Release and re-acquire the lock to allow interrupts
        //
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);

        rmStatus = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE,
                                     RM_LOCK_MODULES_STATE_LOAD);
        if (rmStatus != NV_OK)
            goto gpuStateLoad_exit;

        RMTRACE_ENGINE_PROFILE_EVENT("gpuStateLoadEngEnd", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);
    }

    rmStatus = gpuInitVmmuInfo(pGpu);
    if (rmStatus != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR, "Error initializing VMMU info: 0x%0x\n", status);
        goto gpuStateLoad_exit;
    }

    {
        // Perform post load operations
        rmStatus = gpuStatePostLoad(pGpu, flags);
        if (rmStatus != NV_OK)
            goto gpuStateLoad_exit;

    }

    // Clear indicator that we are running state load
    pGpu->bStateLoading = NV_FALSE;

    // Set a property indicating that the state load has been done
    pGpu->bStateLoaded = NV_TRUE;

    RMTRACE_ENGINE_PROFILE_EVENT("gpuStateLoadEnd", pGpu->gpuId, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

gpuStateLoad_exit:
    return rmStatus;
}

static NV_STATUS
_gpuSetVgpuMgrConfig
(
    OBJGPU *pGpu
)
{

    return NV_OK;
}

/*!
 * @brief Top level post-load routine
 *
 * Provides a mechanism to resolve cyclic dependencies between engines. For
 * example, OBJFB depends on OBJCE on Fermi (for memory scrubbing), likewise
 * OBJCE also depends on OBJFB (for instance memory).
 *
 * StatePostLoad() is called after StateLoad() likewise StatePreUnload() is
 * called prior to StateUnload().
 *
 * Dependencies which are DAGs should continue to be resolved by reordering the
 * engine descriptor lists. Reordering the descriptor lists won't solve cyclic
 * dependencies as at least one constraint would always be violated.
 *
 * TODO: Merge structurally equivalent code with other gpuState* functions.
 *
 * @param[in] pGpu       OBJPGU pointer
 * @param[in] flags      Type of transition
 */
static NV_STATUS
gpuStatePostLoad
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    NV_STATUS      rmStatus = NV_OK;

    engDescriptorList = gpuGetLoadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        RMTRACE_ENGINE_PROFILE_EVENT("gpuStatePostLoadEngStart", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);
        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_POST_LOAD, &engTransitionData);
        rmStatus = engstateStatePostLoad(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_POST_LOAD, &engTransitionData);
        RMTRACE_ENGINE_PROFILE_EVENT("gpuStatePostLoadEngEnd", curEngDescriptor, pGpu->registerAccess.regReadCount, pGpu->registerAccess.regWriteCount);

        // RMCONFIG:  Bail on errors unless the feature/object/engine/class
        //            is simply unsupported
        if (rmStatus == NV_ERR_NOT_SUPPORTED)
            rmStatus = NV_OK;
        if (rmStatus != NV_OK)
            goto gpuStatePostLoad_exit;

        //
        // Release and re-acquire the lock to allow interrupts
        //
        rmGpuLocksRelease(GPUS_LOCK_FLAGS_NONE, NULL);

        rmStatus = rmGpuLocksAcquire(GPUS_LOCK_FLAGS_NONE,
                                     RM_LOCK_MODULES_STATE_LOAD);

        if (rmStatus != NV_OK)
            goto gpuStatePostLoad_exit;
    }

    // Caching GID data, the GID is generated by PMU and passed to RM during PMU INIT message.
    //NV_ASSERT_OK(gpuGetGidInfo(pGpu, NULL, NULL, DRF_DEF(2080_GPU_CMD, _GPU_GET_GID_FLAGS, _TYPE, _SHA1)));

    NV_CHECK_OK_OR_GOTO(rmStatus,
                        LEVEL_ERROR,
                        _gpuSetVgpuMgrConfig(pGpu),
                        gpuStatePostLoad_exit);

    pGpu->boardInfo = portMemAllocNonPaged(sizeof(*pGpu->boardInfo));
    if (pGpu->boardInfo)
    {
        // To avoid potential race of xid reporting with the control, zero it out
        portMemSet(pGpu->boardInfo, '\0', sizeof(*pGpu->boardInfo));

        RM_API *pRmApi = GPU_GET_PHYSICAL_RMAPI(pGpu);

        if(pRmApi->Control(pRmApi,
                           pGpu->hInternalClient,
                           pGpu->hInternalSubdevice,
                           NV2080_CTRL_CMD_GPU_GET_OEM_BOARD_INFO,
                           pGpu->boardInfo,
                           sizeof(*pGpu->boardInfo)) != NV_OK)
        {
            portMemFree(pGpu->boardInfo);
            pGpu->boardInfo = NULL;
        }
    }

gpuStatePostLoad_exit:
    return rmStatus;
}

/*!
 * @brief Top level pre-unload routine
 *
 * Provides a mechanism to resolve cyclic dependencies between engines. For
 * example, OBJFB depends on OBJCE on Fermi (for memory scrubbing), likewise
 * OBJCE also depends on OBJFB (for instance memory).
 *
 * StatePostLoad() is called after StateLoad() likewise StatePreUnload() is
 * called prior to StateUnload().
 *
 * Dependencies which are DAGs should continue to be resolved by reordering the
 * engine descriptor lists. Reordering the descriptor lists won't solve cyclic
 * dependencies as at least one constraint would always be violated.
 *
 * TODO: Merge structurally equivalent code with other gpuState* functions.
 *
 * @param[in] pGpu       OBJPGU pointer
 * @param[in] flags      Type of transition
 */
static NV_STATUS
gpuStatePreUnload
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR      engDescriptorList;
    NvU32               numEngDescriptors;
    NvU32               curEngDescIdx;
    NV_STATUS           rmStatus = NV_OK;

    rmapiControlCacheFreeAllCacheForGpu(pGpu->gpuInstance);

    portMemFree(pGpu->boardInfo);
    pGpu->boardInfo = NULL;

    engDescriptorList = gpuGetUnloadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];
        OBJENGSTATE *pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_PRE_UNLOAD, &engTransitionData);
        rmStatus = engstateStatePreUnload(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_PRE_UNLOAD, &engTransitionData);

        //
        // During unload, failure of a single engine may not be fatal.
        // ASSERT if there is a failure, but ignore the status and continue
        // unloading other engines to prevent (worse) memory leaks.
        //
        if (rmStatus != NV_OK)
        {
            if (rmStatus != NV_ERR_NOT_SUPPORTED)
            {
                NV_PRINTF(LEVEL_ERROR,
                          "Failed to pre unload engine with descriptor index: 0x%x and descriptor: 0x%x\n",
                          curEngDescIdx, curEngDescriptor);
                if (!IS_FMODEL(pGpu))
                {
                    NV_ASSERT(0);
                }
            }
            rmStatus = NV_OK;
        }

        // Ensure that intr on other GPUs are serviced
        gpuServiceInterruptsAllGpus(pGpu);
    }

    return rmStatus;
}

NV_STATUS
gpuEnterShutdown_IMPL
(
    OBJGPU *pGpu
)
{
    NV_STATUS rmStatus = gpuStateUnload(pGpu, GPU_STATE_DEFAULT);
    if (rmStatus != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "failed to unload the device with error 0x%x\n", rmStatus);
    }

    return rmStatus;
}

// TODO: Merge structurally equivalent code with other gpuState* functions.
NV_STATUS
gpuStateUnload_IMPL
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    OBJENGSTATE   *pEngstate;
    NV_STATUS      rmStatus = NV_OK;
    NV_STATUS      fatalErrorStatus = NV_OK;

    NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

    // Set indicator that state is currently unloading.
    pGpu->bStateUnloading = NV_TRUE;

    {
        rmStatus = gpuStatePreUnload(pGpu, flags);
    }

    if (rmStatus != NV_OK)
        return rmStatus;

    engDescriptorList = gpuGetUnloadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];

        pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_UNLOAD, &engTransitionData);
        rmStatus = engstateStateUnload(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_UNLOAD, &engTransitionData);

        //
        // An engine unload leaving the broadcast status to NV_TRUE
        // will most likely mess up the unload of the next engines
        //
        NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

        //
        // During unload, failure of a single engine may not be fatal.
        // ASSERT if there is a failure, but ignore the status and continue
        // unloading other engines to prevent (worse) memory leaks.
        //
        if (rmStatus != NV_OK)
        {
            if (rmStatus != NV_ERR_NOT_SUPPORTED)
            {
                NV_PRINTF(LEVEL_ERROR,
                          "Failed to unload engine with descriptor index: 0x%x and descriptor: 0x%x\n",
                          curEngDescIdx, curEngDescriptor);
                if (!IS_FMODEL(pGpu))
                {
                    NV_ASSERT(0);

                    if (flags & GPU_STATE_FLAGS_PRESERVING)
                    {
                        //
                        // FBSR can fail due to low sysmem.
                        // So return error.
                        // See bugs 2051056, 2049141
                        //
                        if (objDynamicCastById(pEngstate, classId(MemorySystem)))
                        {
                            fatalErrorStatus = rmStatus;
                        }
                    }
                }
            }
            rmStatus = NV_OK;
        }
        // Ensure that intr on other GPUs are serviced
        gpuServiceInterruptsAllGpus(pGpu);
    }

    // Call the gpuStatePostUnload routine
    rmStatus = gpuStatePostUnload(pGpu, flags);
    NV_ASSERT_OK(rmStatus);

    if(!(flags & GPU_STATE_FLAGS_PRESERVING))
       gpuDestroyDefaultClientShare_HAL(pGpu);

    // De-init SRIOV
    gpuDeinitSriov_HAL(pGpu);

    // Set indicator that state unload finished.
    pGpu->bStateUnloading = NV_FALSE;

    // Set a property indicating that the state unload has been done
    if (rmStatus == NV_OK)
    {
        pGpu->bStateLoaded = NV_FALSE;
    }

    if (fatalErrorStatus != NV_OK)
    {
        rmStatus = fatalErrorStatus;
    }

    return rmStatus;
}

/*!
 * @brief Top level post-unload routine
 *
 * Provides a mechanism to resolve cyclic dependencies between engines.
 *
 *
 * Dependencies which are DAGs should continue to be resolved by reordering the
 * engine descriptor lists. Reordering the descriptor lists won't solve cyclic
 * dependencies as at least one constraint would always be violated.
 *
 * TODO: Merge structurally equivalent code with other gpuState* functions.
 *
 * @param[in] pGpu       OBJPGU pointer
 * @param[in] flags      Type of transition
 */
static NV_STATUS
gpuStatePostUnload
(
    OBJGPU *pGpu,
    NvU32   flags
)
{
    PENGDESCRIPTOR      engDescriptorList;
    NvU32               numEngDescriptors;
    NvU32               curEngDescIdx;
    OBJENGSTATE        *pEngstate;
    NV_STATUS           rmStatus = NV_OK;

    engDescriptorList = gpuGetUnloadEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];

        pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_POST_UNLOAD, &engTransitionData);
        rmStatus = engstateStatePostUnload(pGpu, pEngstate, flags);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_POST_UNLOAD, &engTransitionData);

        //
        // An engine post-unload leaving the broadcast status to NV_TRUE
        // will most likely mess up the post-unload of the next engines
        //
        NV_ASSERT(!gpumgrGetBcEnabledStatus(pGpu));

        //
        // During unload, failure of a single engine may not be fatal.
        // ASSERT if there is a failure, but ignore the status and continue
        // unloading other engines to prevent (worse) memory leaks.
        //
        if (rmStatus != NV_OK)
        {
            if (rmStatus != NV_ERR_NOT_SUPPORTED)
            {
                NV_PRINTF(LEVEL_ERROR,
                          "Failed to post unload engine with descriptor index: 0x%x and descriptor: 0x%x\n",
                          curEngDescIdx, curEngDescriptor);
                if (!IS_FMODEL(pGpu))
                {
                    NV_ASSERT(0);
                }
            }
            rmStatus = NV_OK;
        }

        // Ensure that intr on other GPUs are serviced
        gpuServiceInterruptsAllGpus(pGpu);
    }

    return rmStatus;
}

NV_STATUS
gpuStateDestroy_IMPL
(
    OBJGPU *pGpu
)
{
    PENGDESCRIPTOR engDescriptorList;
    NvU32          numEngDescriptors;
    NvU32          curEngDescIdx;
    OBJENGSTATE   *pEngstate;
    NV_STATUS      rmStatus = NV_OK;

    engDescriptorList = gpuGetDestroyEngineDescriptors(pGpu);
    numEngDescriptors = gpuGetNumEngDescriptors(pGpu);

    NV_RM_RPC_SIM_FREE_INFRA(pGpu, rmStatus);

    // Order is determined by gpuGetChildrenOrder_HAL pulling gpuChildOrderList array
    for (curEngDescIdx = 0; curEngDescIdx < numEngDescriptors; curEngDescIdx++)
    {
        ENGSTATE_TRANSITION_DATA engTransitionData;
        ENGDESCRIPTOR curEngDescriptor = engDescriptorList[curEngDescIdx];

        pEngstate = gpuGetEngstate(pGpu, curEngDescriptor);

        if (pEngstate == NULL)
        {
            continue;
        }

        engstateLogStateTransitionPre(pEngstate, ENGSTATE_STATE_DESTROY, &engTransitionData);
        engstateStateDestroy(pGpu, pEngstate);
        engstateLogStateTransitionPost(pEngstate, ENGSTATE_STATE_DESTROY, &engTransitionData);
    }

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // TODO: Move the below code into appropriate ENGSTATE objects.
    //       DO NOT ADD MORE SPECIAL CASES HERE!
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    // Do this after calling stateDestroy() of child engines.
    objDelete(pGpu->pPrereqTracker);
    pGpu->pPrereqTracker = NULL;

    // Clear the property indicating that the state initialization has been done
    if (rmStatus == NV_OK)
    {
        pGpu->setProperty(pGpu, PDB_PROP_GPU_STATE_INITIALIZED, NV_FALSE);
    }

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // TODO: Move the above code into appropriate ENGSTATE objects.
    //       DO NOT ADD MORE SPECIAL CASES HERE!
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    _gpuFreeInternalObjects(pGpu);
    gpuDestroyGenericKernelFalconList(pGpu);

    portMemFree(pGpu->gspSupportedEngines);
    pGpu->gspSupportedEngines = NULL;

    portMemFree(pGpu->pChipInfo);
    pGpu->pChipInfo = NULL;

    pGpu->bFullyConstructed = NV_FALSE;

    gpuDeinitOptimusSettings(pGpu);

    return rmStatus;
}

//
// Logic: If arch = requested AND impl = requested --> NV_TRUE
//        OR If arch = requested AND impl = requested AND maskRev = requested --> NV_TRUE
//        OR If arch = requested AND impl = requested AND rev = requested --> NV_TRUE
//
NvBool
gpuIsImplementation_IMPL
(
    OBJGPU *pGpu,
    HAL_IMPLEMENTATION halImpl,
    NvU32 maskRevision,
    NvU32 revision
)
{
    NvU32 gpuArch, gpuImpl;
    NvBool result = NV_FALSE;

    NV_ASSERT(revision == GPU_NO_REVISION);

    gpuXlateHalImplToArchImpl(pGpu, halImpl, &gpuArch, &gpuImpl);

    result = ((gpuGetChipArch(pGpu) == gpuArch) &&
              (gpuGetChipImpl(pGpu) == gpuImpl));

    if (maskRevision != GPU_NO_MASK_REVISION)
    {
        result = result && (GPU_GET_MASKREVISION(pGpu) == maskRevision);
    }

    return result;
}

/*!
 * @brief Initialize SBIOS settings for Optimus GOLD to driver loaded state.
 *
 * @param[in]  pGpu    GPU object pointer
 *
 * @return NV_OK
 */
NV_STATUS
gpuInitOptimusSettings_IMPL(OBJGPU *pGpu)
{

    return NV_OK;
}

/*!
 * @brief Restore SBIOS settings for Optimus GOLD to driver unloaded state.
 *
 * @param[in]  pGpu    GPU object pointer
 *
 * @return NV_OK if successful
 * @return NV_ERR_INVALID_STATE if SBIOS failed to acknowledge the restore request
 * @return Bubbles up error codes on ACPI call failure
 */
NV_STATUS
gpuDeinitOptimusSettings_IMPL(OBJGPU *pGpu)
{

    return NV_OK;
}

// Check the software state to decide if we are in full power mode or not.
NvBool
gpuIsGpuFullPower_IMPL
(
    OBJGPU *pGpu
)
{
    NvBool retVal = NV_TRUE;

    //
    // SW may have indicated that the GPU ins in standby, hibernate, or powered off,
    // indicating a logical power state.
    //
    if (pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_STANDBY) ||
        pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_HIBERNATE))
    {
        retVal = NV_FALSE;
    }

    return retVal;
}

// Check the software state to decide if we are in full power mode or not.
NvBool
gpuIsGpuFullPowerForPmResume_IMPL
(
    OBJGPU *pGpu
)
{
    NvBool retVal = NV_TRUE;
    //
    // SW may have indicated that the GPU ins in standby, resume, hibernate, or powered off,
    // indicating a logical power state.
    //
    if ((!pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_PM_RESUME_CODEPATH)) &&
        (pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_STANDBY) ||
         pGpu->getProperty(pGpu, PDB_PROP_GPU_IN_HIBERNATE)))
    {
        retVal = NV_FALSE;
    }
    return retVal;
}

static NV_STATUS
gpuDetermineVirtualMode
(
    OBJGPU *pGpu
)
{
    OBJSYS *pSys = SYS_GET_INSTANCE();
    OBJOS  *pOS = SYS_GET_OS(pSys);
    OBJGPU *pGpuTemp;
    OBJHYPERVISOR *pHypervisor = SYS_GET_HYPERVISOR(pSys);
    NvU32 gpuAttachMask, gpuInstance;
    NvBool bIsVirtual = NV_FALSE;

    if (pGpu->bIsSOC || pGpu->getProperty(pGpu, PDB_PROP_GPU_TEGRA_SOC_NVDISPLAY))
    {
        return NV_OK;
    }

    // Using Hypervisor native interface to detect
    if (pHypervisor && (!pHypervisor->bDetected))
        hypervisorDetection(pHypervisor, pOS);

    gpumgrGetGpuAttachInfo(NULL, &gpuAttachMask);
    gpuInstance = 0;

    // Checking if the VM is already vGPU enabled.
    // NMOS and vGPU cannot be simultaneously enabled on a VM.
    if (pGpu->bIsPassthru)
    {
        while ((pGpuTemp = gpumgrGetNextGpu(gpuAttachMask, &gpuInstance)) != NULL)
        {
            if (IS_VIRTUAL(pGpuTemp))
            {
                NV_PRINTF(LEVEL_ERROR,
                          "vGPU and Passthrough not supported simultaneously on the same VM.\n");
                pGpu->bIsPassthru = NV_FALSE;
                return NV_ERR_NOT_SUPPORTED;
            }
        }
    }

    // Early detection at GPU creation time should be consistent to the real detection
    NV_ASSERT_OR_RETURN(pGpu->isVirtual == bIsVirtual, NV_ERR_INVALID_STATE);

    return NV_OK;
}

NvBool
gpuIsImplementationOrBetter_IMPL
(
    OBJGPU *pGpu,
    HAL_IMPLEMENTATION halImpl,
    NvU32 maskRevision,
    NvU32 revision
)
{
    NvU32 gpuArch, gpuImpl;
    NvU32 chipArch;
    NvBool result = NV_FALSE;

    NV_ASSERT(revision == GPU_NO_REVISION);

    gpuXlateHalImplToArchImpl(pGpu, halImpl, &gpuArch, &gpuImpl);

    // "is implementation or better" is only defined between 2 gpus within
    // the same "gpu series" as defined in config/Chips.pm and nv_arch.h
    chipArch = gpuGetChipArch(pGpu);

    if (DRF_VAL(GPU, _ARCHITECTURE, _SERIES, chipArch) == DRF_VAL(GPU, _ARCHITECTURE, _SERIES, gpuArch))
    {
        if (maskRevision != GPU_NO_MASK_REVISION)
        {
            result = gpuSatisfiesTemporalOrderMaskRev(pGpu, halImpl, gpuArch,
                                                      gpuImpl, maskRevision);
        }
        else
        {
            // In case there is a temporal ordering we need to account for
            result = gpuSatisfiesTemporalOrder(pGpu, halImpl);
        }
    }

    return result;
}

static void
gpuXlateHalImplToArchImpl
(
    OBJGPU *pGpu,
    HAL_IMPLEMENTATION halImpl,
    NvU32 *gpuArch,
    NvU32 *gpuImpl
)
{
    switch (halImpl)
    {
        case HAL_IMPL_GM107:
        {
            *gpuArch = GPU_ARCHITECTURE_MAXWELL;
            *gpuImpl = GPU_IMPLEMENTATION_GM107;
            break;
        }

        case HAL_IMPL_GM108:
        {
            *gpuArch = GPU_ARCHITECTURE_MAXWELL;
            *gpuImpl = GPU_IMPLEMENTATION_GM108;
            break;
        }

        case HAL_IMPL_GM200:
        {
            *gpuArch = GPU_ARCHITECTURE_MAXWELL2;
            *gpuImpl = GPU_IMPLEMENTATION_GM200;
            break;
        }

        case HAL_IMPL_GM204:
        {
            *gpuArch = GPU_ARCHITECTURE_MAXWELL2;
            *gpuImpl = GPU_IMPLEMENTATION_GM204;
            break;
        }

        case HAL_IMPL_GM206:
        {
            *gpuArch = GPU_ARCHITECTURE_MAXWELL2;
            *gpuImpl = GPU_IMPLEMENTATION_GM206;
            break;
        }

        case HAL_IMPL_GP100:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP100;
            break;
        }

        case HAL_IMPL_GP102:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP102;
            break;
        }

        case HAL_IMPL_GP104:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP104;
            break;
        }

        case HAL_IMPL_GP106:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP106;
            break;
        }

        case HAL_IMPL_GP107:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP107;
            break;
        }

        case HAL_IMPL_GP108:
        {
            *gpuArch = GPU_ARCHITECTURE_PASCAL;
            *gpuImpl = GPU_IMPLEMENTATION_GP108;
            break;
        }

        case HAL_IMPL_GV100:
        {
            *gpuArch = GPU_ARCHITECTURE_VOLTA;
            *gpuImpl = GPU_IMPLEMENTATION_GV100;
            break;
        }

        case HAL_IMPL_GV11B:
        {
            *gpuArch = GPU_ARCHITECTURE_VOLTA2;
            *gpuImpl = GPU_IMPLEMENTATION_GV11B;
            break;
        }

        case HAL_IMPL_TU102:
        {
            *gpuArch = GPU_ARCHITECTURE_TURING;
            *gpuImpl = GPU_IMPLEMENTATION_TU102;
            break;
        }

        case HAL_IMPL_TU104:
        {
            *gpuArch = GPU_ARCHITECTURE_TURING;
            *gpuImpl = GPU_IMPLEMENTATION_TU104;
            break;
        }

        case HAL_IMPL_TU106:
        {
            *gpuArch = GPU_ARCHITECTURE_TURING;
            *gpuImpl = GPU_IMPLEMENTATION_TU106;
            break;
        }

        case HAL_IMPL_TU116:
        {
            *gpuArch = GPU_ARCHITECTURE_TURING;
            *gpuImpl = GPU_IMPLEMENTATION_TU116;
            break;
        }

        case HAL_IMPL_TU117:
        {
            *gpuArch = GPU_ARCHITECTURE_TURING;
            *gpuImpl = GPU_IMPLEMENTATION_TU117;
            break;
        }

        case HAL_IMPL_AMODEL:
        {
            *gpuArch = GPU_ARCHITECTURE_SIMS;
            *gpuImpl = GPU_IMPLEMENTATION_AMODEL;
            break;
        }

        case HAL_IMPL_T124:
        {
            *gpuArch = GPU_ARCHITECTURE_T12X;
            *gpuImpl = GPU_IMPLEMENTATION_T124;
            break;
        }

        case HAL_IMPL_T132:
        {
            *gpuArch = GPU_ARCHITECTURE_T13X;
            *gpuImpl = GPU_IMPLEMENTATION_T132;
            break;
        }

        case HAL_IMPL_T210:
        {
            *gpuArch = GPU_ARCHITECTURE_T21X;
            *gpuImpl = GPU_IMPLEMENTATION_T210;
            break;
        }

        case HAL_IMPL_T186:
        {
            *gpuArch = GPU_ARCHITECTURE_T18X;
            *gpuImpl = GPU_IMPLEMENTATION_T186;
            break;
        }

        case HAL_IMPL_T194:
        {
            *gpuArch = GPU_ARCHITECTURE_T19X;
            *gpuImpl = GPU_IMPLEMENTATION_T194;
            break;
        }

        case HAL_IMPL_T234D:
        {
            *gpuArch = GPU_ARCHITECTURE_T23X;
            *gpuImpl = GPU_IMPLEMENTATION_T234D;
            break;
        }

        case HAL_IMPL_GA100:
        {
            *gpuArch = GPU_ARCHITECTURE_AMPERE;
            *gpuImpl = GPU_IMPLEMENTATION_GA100;
            break;
        }

        case HAL_IMPL_GA102:
        {
            *gpuArch = GPU_ARCHITECTURE_AMPERE;
            *gpuImpl = GPU_IMPLEMENTATION_GA102;
            break;
        }

        case HAL_IMPL_GA102F:
        {
            *gpuArch = GPU_ARCHITECTURE_AMPERE;
            *gpuImpl = GPU_IMPLEMENTATION_GA102F;
            break;
        }

        case HAL_IMPL_GA104:
        {
            *gpuArch = GPU_ARCHITECTURE_AMPERE;
            *gpuImpl = GPU_IMPLEMENTATION_GA104;
            break;
        }


        default:
        {
            *gpuArch = 0;
            *gpuImpl = 0;
            NV_PRINTF(LEVEL_ERROR, "Invalid halimpl\n");
            DBG_BREAKPOINT();
            break;
        }
    }
}

//
// default Logic: If halImpl is equal or greater than requested --> NV_TRUE
//
// Arch and impl IDs are not guaranteed to be ordered.
// "halImpl" is used here to match the ordering in chip-config/NVOC
//
// NOTE: only defined for gpus within same gpu series
//
static NvBool
gpuSatisfiesTemporalOrder
(
    OBJGPU *pGpu,
    HAL_IMPLEMENTATION halImpl
)
{
    NvBool result = NV_FALSE;

    switch (halImpl)
    {
        default:
        {
            HAL_IMPLEMENTATION chipImpl = pGpu->halImpl;
            NV_ASSERT(chipImpl < HAL_IMPL_MAXIMUM);

            result = (chipImpl >= halImpl);

            break;
        }
    }

    return result;
}

//
// default Logic: If arch = requested AND impl = requested AND
//                 maskRev is >= requested --> NV_TRUE
//
static NvBool
gpuSatisfiesTemporalOrderMaskRev
(
    OBJGPU *pGpu,
    HAL_IMPLEMENTATION halImpl,
    NvU32 gpuArch,
    NvU32 gpuImpl,
    NvU32 maskRevision
)
{
    NvBool result = NV_FALSE;

    result = ((gpuGetChipArch(pGpu)== gpuArch) &&
              (gpuGetChipImpl(pGpu) == gpuImpl) &&
              (GPU_GET_MASKREVISION(pGpu) >= maskRevision));

    return result;
}

// =============== Engine Database ==============================

typedef struct {
    RM_ENGINE_TYPE clientEngineId;
    NVOC_CLASS_ID class;
    NvU32         instance;
    NvBool        bHostEngine;
} EXTERN_TO_INTERNAL_ENGINE_ID;

static const EXTERN_TO_INTERNAL_ENGINE_ID rmClientEngineTable[] =
{
    { RM_ENGINE_TYPE_GR0,        classId(Graphics)   , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_GR1,        classId(Graphics)   , 1,  NV_TRUE },
    { RM_ENGINE_TYPE_GR2,        classId(Graphics)   , 2,  NV_TRUE },
    { RM_ENGINE_TYPE_GR3,        classId(Graphics)   , 3,  NV_TRUE },
    { RM_ENGINE_TYPE_GR4,        classId(Graphics)   , 4,  NV_TRUE },
    { RM_ENGINE_TYPE_GR5,        classId(Graphics)   , 5,  NV_TRUE },
    { RM_ENGINE_TYPE_GR6,        classId(Graphics)   , 6,  NV_TRUE },
    { RM_ENGINE_TYPE_GR7,        classId(Graphics)   , 7,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY0,      classId(OBJCE)      , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY1,      classId(OBJCE)      , 1,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY2,      classId(OBJCE)      , 2,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY3,      classId(OBJCE)      , 3,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY4,      classId(OBJCE)      , 4,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY5,      classId(OBJCE)      , 5,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY6,      classId(OBJCE)      , 6,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY7,      classId(OBJCE)      , 7,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY8,      classId(OBJCE)      , 8,  NV_TRUE },
    { RM_ENGINE_TYPE_COPY9,      classId(OBJCE)      , 9,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC0,     classId(OBJBSP)     , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC1,     classId(OBJBSP)     , 1,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC2,     classId(OBJBSP)     , 2,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC3,     classId(OBJBSP)     , 3,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC4,     classId(OBJBSP)     , 4,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC5,     classId(OBJBSP)     , 5,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC6,     classId(OBJBSP)     , 6,  NV_TRUE },
    { RM_ENGINE_TYPE_NVDEC7,     classId(OBJBSP)     , 7,  NV_TRUE },
    { RM_ENGINE_TYPE_CIPHER,     classId(OBJCIPHER)  , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_NVENC0,     classId(OBJMSENC)   , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_NVENC1,     classId(OBJMSENC)   , 1,  NV_TRUE },
    { RM_ENGINE_TYPE_NVENC2,     classId(OBJMSENC)   , 2,  NV_TRUE },
    { RM_ENGINE_TYPE_SW,         classId(OBJSWENG)   , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_SEC2,       classId(OBJSEC2)    , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG0,    classId(OBJNVJPG)   , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG1,    classId(OBJNVJPG)   , 1,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG2,    classId(OBJNVJPG)   , 2,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG3,    classId(OBJNVJPG)   , 3,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG4,    classId(OBJNVJPG)   , 4,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG5,    classId(OBJNVJPG)   , 5,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG6,    classId(OBJNVJPG)   , 6,  NV_TRUE },
    { RM_ENGINE_TYPE_NVJPEG7,    classId(OBJNVJPG)   , 7,  NV_TRUE },
    { RM_ENGINE_TYPE_OFA0,       classId(OBJOFA)     , 0,  NV_TRUE },
    { RM_ENGINE_TYPE_DPU,        classId(OBJDPU)     , 0,  NV_FALSE },
    { RM_ENGINE_TYPE_PMU,        classId(Pmu)        , 0,  NV_FALSE },
    { RM_ENGINE_TYPE_FBFLCN,     classId(OBJFBFLCN)  , 0,  NV_FALSE },
    { RM_ENGINE_TYPE_HOST,       classId(KernelFifo) , 0,  NV_FALSE },
};

NV_STATUS gpuConstructEngineTable_IMPL
(
    OBJGPU *pGpu
)
{
    NvU32        engineIdx    = 0;

    // Alloc engine DB
    pGpu->engineDB.bValid = NV_FALSE;
    pGpu->engineDB.pType = portMemAllocNonPaged(
                        NV_ARRAY_ELEMENTS(rmClientEngineTable) * sizeof(*pGpu->engineDB.pType));
    if (pGpu->engineDB.pType == NULL)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "gpuConstructEngineTable: Could not allocate engine DB\n");
        DBG_BREAKPOINT();
        return NV_ERR_NO_MEMORY;
    }
    pGpu->engineDB.size = 0; // That's right, its the size not the capacity
                             // of the engineDB

    // Initialize per-GPU per-engine list of non-stall interrupt event nodes.
    for (engineIdx = 0; engineIdx < (NvU32)RM_ENGINE_TYPE_LAST; engineIdx++)
    {
        NV_STATUS status = gpuEngineEventNotificationListCreate(pGpu,
            &pGpu->engineNonstallIntrEventNotifications[engineIdx]);
        if (status != NV_OK)
        {
            gpuDestroyEngineTable(pGpu);
            return status;
        }
    }

    return NV_OK;
}

NV_STATUS gpuUpdateEngineTable_IMPL
(
    OBJGPU *pGpu
)
{
    NV_STATUS status       = NV_OK;
    NvU32     counter      = 0;
    NvU32     numClasses   = 0;

    if (pGpu->engineDB.pType == NULL)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "gpuUpdateEngineTable: EngineDB has not been created yet\n");
        DBG_BREAKPOINT();
        return NV_ERR_INVALID_STATE;
    }

    if (pGpu->engineDB.bValid)
    {
        return NV_OK;
    }

    // Read through the classDB and populate engineDB
    pGpu->engineDB.size = 0;
    for (counter = 0; counter < NV_ARRAY_ELEMENTS(rmClientEngineTable); counter++)
    {
        // There are tests such as ClassA06fTest that attempt to bind all engines reported
        if (!rmClientEngineTable[counter].bHostEngine)
        {
            continue;
        }

        status = gpuGetClassList(pGpu, &numClasses, NULL,
                                       MKENGDESC(rmClientEngineTable[counter].class, rmClientEngineTable[counter].instance));
        if ((status != NV_OK) || ( numClasses == 0))
        {
            continue;
        }
        pGpu->engineDB.pType[pGpu->engineDB.size++] =
            rmClientEngineTable[counter].clientEngineId;
    }

    pGpu->engineDB.bValid = NV_TRUE;

    return NV_OK;
}
void gpuDestroyEngineTable_IMPL(OBJGPU *pGpu)
{
    for (NvU32 engineIdx = 0; engineIdx < (NvU32)RM_ENGINE_TYPE_LAST; engineIdx++)
        gpuEngineEventNotificationListDestroy(pGpu,
            pGpu->engineNonstallIntrEventNotifications[engineIdx]);

    if (pGpu->engineDB.pType)
    {
        pGpu->engineDB.size  = 0;
        portMemFree(pGpu->engineDB.pType);
        pGpu->engineDB.pType = NULL;
        pGpu->engineDB.bValid = NV_FALSE;
    }
}

NvBool gpuCheckEngineTable_IMPL
(
    OBJGPU *pGpu,
    RM_ENGINE_TYPE engType
)
{
    NvU32 engineIdx;

    if (!IS_MODS_AMODEL(pGpu))
    {
        NV_ASSERT_OR_RETURN(pGpu->engineDB.bValid, NV_FALSE);
    }

    NV_ASSERT_OR_RETURN(engType < RM_ENGINE_TYPE_LAST, NV_FALSE);

    for (engineIdx = 0; engineIdx < pGpu->engineDB.size; engineIdx++)
    {
        if (engType ==  pGpu->engineDB.pType[engineIdx])
        {
            return NV_TRUE;
        }
    }

    return NV_FALSE;
}

NV_STATUS
gpuXlateClientEngineIdToEngDesc_IMPL
(
    OBJGPU        *pGpu,
    RM_ENGINE_TYPE clientEngineID,
    ENGDESCRIPTOR *pEngDesc

)
{
    NvU32    counter;

    for (counter = 0; counter < NV_ARRAY_ELEMENTS(rmClientEngineTable); counter++)
    {
        if (rmClientEngineTable[counter].clientEngineId == clientEngineID)
        {
            *pEngDesc = MKENGDESC(rmClientEngineTable[counter].class, rmClientEngineTable[counter].instance);
            return NV_OK;
        }
    }

    return NV_ERR_INVALID_ARGUMENT;
}

NV_STATUS
gpuXlateEngDescToClientEngineId_IMPL
(
    OBJGPU       *pGpu,
    ENGDESCRIPTOR engDesc,
    RM_ENGINE_TYPE *pClientEngineID
)
{
    NvU32    counter;

    for (counter = 0; counter < NV_ARRAY_ELEMENTS(rmClientEngineTable); counter++)
    {
        if (MKENGDESC(rmClientEngineTable[counter].class, rmClientEngineTable[counter].instance) == engDesc)
        {
            *pClientEngineID = rmClientEngineTable[counter].clientEngineId;
            return NV_OK;
        }
    }

    return NV_ERR_INVALID_ARGUMENT;
}

NV_STATUS
gpuGetFlcnFromClientEngineId_IMPL
(
    OBJGPU       *pGpu,
    RM_ENGINE_TYPE clientEngineId,
    Falcon      **ppFlcn
)
{
    *ppFlcn = NULL;
    return NV_ERR_NOT_SUPPORTED;
}

NV_STATUS
gpuGetGidInfo_IMPL
(
    OBJGPU  *pGpu,
    NvU8   **ppGidString,
    NvU32   *pGidStrlen,
    NvU32    gidFlags
)
{
    NV_STATUS rmStatus = NV_OK;
    NvU8      gidData[RM_SHA1_GID_SIZE];
    NvU32     gidSize = RM_SHA1_GID_SIZE;

    if (!FLD_TEST_DRF(2080_GPU_CMD,_GPU_GET_GID_FLAGS,_TYPE,_SHA1,gidFlags))
    {
        return NV_ERR_INVALID_FLAGS;
    }

    if (pGpu->gpuUuid.isInitialized)
    {
        portMemCopy(gidData, gidSize, &pGpu->gpuUuid.uuid[0], gidSize);
        goto fillGidData;
    }

    rmStatus = gpuGenGidData_HAL(pGpu, gidData, gidSize, gidFlags);

    if (rmStatus != NV_OK)
    {
        return rmStatus;
    }

    // if not cached, cache it here
    portMemCopy(&pGpu->gpuUuid.uuid[0], gidSize, gidData, gidSize);
    pGpu->gpuUuid.isInitialized = NV_TRUE;

fillGidData:
    if (ppGidString != NULL)
    {
        if (FLD_TEST_DRF(2080_GPU_CMD, _GPU_GET_GID_FLAGS, _FORMAT, _BINARY,
                         gidFlags))
        {
            //
            // Instead of transforming the Gid into a string, just use it in its
            // original binary form. The allocation rules are the same as those
            // followed by the transformGidToUserFriendlyString routine: we
            // allocate ppGidString here, and the caller frees ppGidString.
            //
            *ppGidString = portMemAllocNonPaged(gidSize);
            if (*ppGidString == NULL)
            {
                return NV_ERR_NO_MEMORY;
            }

            portMemCopy(*ppGidString, gidSize, gidData, gidSize);
            *pGidStrlen = gidSize;
        }
        else
        {
            NV_ASSERT_OR_RETURN(pGidStrlen != NULL, NV_ERR_INVALID_ARGUMENT);
            rmStatus = transformGidToUserFriendlyString(gidData, gidSize,
                ppGidString, pGidStrlen, gidFlags);
        }
    }

    return rmStatus;
}

void
gpuSetDisconnectedProperties_IMPL
(
    OBJGPU *pGpu
)
{
    pGpu->setProperty(pGpu, PDB_PROP_GPU_IS_LOST, NV_TRUE);
    pGpu->setProperty(pGpu, PDB_PROP_GPU_IS_CONNECTED, NV_FALSE);
    pGpu->setProperty(pGpu, PDB_PROP_GPU_IN_PM_CODEPATH, NV_FALSE);
    pGpu->setProperty(pGpu, PDB_PROP_GPU_IN_STANDBY, NV_FALSE);
    pGpu->bInD3Cold = NV_FALSE;
    pGpu->setProperty(pGpu, PDB_PROP_GPU_IN_HIBERNATE, NV_FALSE);
    SET_GPU_GC6_STATE(pGpu, GPU_GC6_STATE_POWERED_ON);

}

NV_STATUS
gpuAddConstructedFalcon_IMPL
(
    OBJGPU  *pGpu,
    Falcon *pFlcn
)
{
    NV_ASSERT_OR_RETURN(pFlcn, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(
        pGpu->numConstructedFalcons < NV_ARRAY_ELEMENTS(pGpu->constructedFalcons),
        NV_ERR_BUFFER_TOO_SMALL);

    pGpu->constructedFalcons[pGpu->numConstructedFalcons++] = pFlcn;
    return NV_OK;
}

NV_STATUS
gpuRemoveConstructedFalcon_IMPL
(
    OBJGPU  *pGpu,
    Falcon *pFlcn
)
{
    NvU32 i, j;
    for (i = 0; i < pGpu->numConstructedFalcons; i++)
    {
        if (pGpu->constructedFalcons[i] == pFlcn)
        {
            for (j = i+1; j < pGpu->numConstructedFalcons; j++)
            {
                pGpu->constructedFalcons[j-1] = pGpu->constructedFalcons[j];
            }
            pGpu->numConstructedFalcons--;
            pGpu->constructedFalcons[pGpu->numConstructedFalcons] = NULL;
            return NV_OK;
        }
    }
    NV_ASSERT_FAILED("Attempted to remove a non-existent initialized Falcon!");
    return NV_ERR_OBJECT_NOT_FOUND;
}

NV_STATUS
gpuGetConstructedFalcon_IMPL
(
    OBJGPU  *pGpu,
    NvU32 index,
    Falcon  **ppFlcn
)
{
    if (index >= pGpu->numConstructedFalcons)
        return NV_ERR_OUT_OF_RANGE;

    *ppFlcn = pGpu->constructedFalcons[index];
    NV_ASSERT(*ppFlcn != NULL);
    return NV_OK;
}

NV_STATUS gpuBuildGenericKernelFalconList_IMPL(OBJGPU *pGpu)
{
    return NV_OK;
}

void gpuDestroyGenericKernelFalconList_IMPL(OBJGPU *pGpu)
{
}


GenericKernelFalcon *
gpuGetGenericKernelFalconForEngine_IMPL
(
    OBJGPU  *pGpu,
    ENGDESCRIPTOR engDesc
)
{
    return NULL;
}

void gpuRegisterGenericKernelFalconIntrService_IMPL(OBJGPU *pGpu, void *pRecords)
{
}

/**
 * @brief Initializes iterator for ENGDESCRIPTOR load order
 *
 * @return        GPU_CHILD_ITER
 */
static ENGLIST_ITER
gpuGetEngineOrderListIter(OBJGPU *pGpu, NvU32 flags)
{
    ENGLIST_ITER it = { 0 };
    it.flags = flags;
    return it;
}


static const GPUCHILDPRESENT *
gpuFindChildPresent(const GPUCHILDPRESENT *pChildPresentList, NvU32 numChildPresent, NvU32 classId)
{
    NvU32 i;

    for (i = 0; i < numChildPresent; i++)
    {
        if (pChildPresentList[i].classId == classId)
            return &pChildPresentList[i];
    }

    return NULL;
}

/*!
 * @brief Get GFID State
 *
 * @param[in] pGpu   OBJGPU pointer
 * @param[in] gfid   GFID to be validated
 * @param[out] Current state of GFID
 */
NV_STATUS
gpuGetGfidState(OBJGPU *pGpu, NvU32 gfid, GFID_ALLOC_STATUS *pState)
{
    if (!gpuIsSriovEnabled(pGpu))
        return NV_OK;

    if (pGpu->sriovState.pAllocatedGfids == NULL || pState == NULL)
    {
        return NV_ERR_INVALID_ADDRESS;
    }

    // Sanity check on GFID
    if (gfid > pGpu->sriovState.maxGfid)
    {
        return NV_ERR_OUT_OF_RANGE;
    }

    *pState = (GFID_ALLOC_STATUS)pGpu->sriovState.pAllocatedGfids[gfid];

    return NV_OK;
}

/*!
 * @brief Set/Unset bit in pAllocatedGfids
 *
 * @param[in] pGpu   OBJGPU pointer
 * @param[in] gfid   GFID to be set/unset (Assumes GFID is sanity checked before calling this function)
 * @param[in] bInUse NV_TRUE if GFID in use
 */
void
gpuSetGfidUsage_IMPL(OBJGPU *pGpu, NvU32 gfid, NvBool bInUse)
{
    NV_ASSERT_OR_RETURN_VOID(pGpu->sriovState.pAllocatedGfids != NULL);

    if (bInUse == NV_TRUE)
        pGpu->sriovState.pAllocatedGfids[gfid] = GFID_ALLOCATED;
    else
        pGpu->sriovState.pAllocatedGfids[gfid] = GFID_FREE;
}

/*!
 * @brief Set pAllocatedGfids allocated status as being invalidated
 *
 * @param[in] pGpu   OBJGPU pointer
 * @param[in] gfid   GFID to be set/unset (Assumes GFID is sanity checked before calling this function)
 */
void
gpuSetGfidInvalidated_IMPL(OBJGPU *pGpu, NvU32 gfid)
{
    NV_ASSERT_OR_RETURN_VOID(pGpu->sriovState.pAllocatedGfids != NULL);

    pGpu->sriovState.pAllocatedGfids[gfid] = GFID_INVALIDATED;
}

/**
 * @brief Iterates over the engine ordering list
 *
 * @param[in,out] pIt          Iterator
 * @param[out]    pEngDesc     The next engine descriptor
 *
 * @return        NV_TRUE if *pEngDesc is valid, NV_FALSE if there are no more engines
 */
NvBool
gpuGetNextInEngineOrderList(OBJGPU *pGpu, ENGLIST_ITER *pIt, PENGDESCRIPTOR pEngDesc)
{
    NvBool                 bReverse = !!(pIt->flags & (GCO_LIST_UNLOAD | GCO_LIST_DESTROY));
    const GPUCHILDORDER   *pChildOrderList;
    NvU32                  numChildOrder;
    const GPUCHILDPRESENT *pCurChildPresent;
    const GPUCHILDORDER   *pCurChildOrder;
    NvBool                 bAdvance = NV_FALSE;

    pChildOrderList = gpuGetChildrenOrder_HAL(pGpu, &numChildOrder);

    if (!pIt->bStarted)
    {
        pIt->bStarted = NV_TRUE;
        pIt->childOrderIndex = bReverse ? (NvS32)numChildOrder - 1 : 0;
    }

    while (1)
    {
        if (bAdvance)
            pIt->childOrderIndex += bReverse ? -1 : 1;

        if ((pIt->childOrderIndex >= (NvS32)numChildOrder) || (pIt->childOrderIndex < 0))
            return NV_FALSE;

        pCurChildOrder = &pChildOrderList[pIt->childOrderIndex];

        if ((pCurChildOrder->flags & pIt->flags) != pIt->flags)
        {
            bAdvance = NV_TRUE;
            continue;
        }

        pCurChildPresent = gpuFindChildPresent(pGpu->pChildrenPresent,
            pGpu->numChildrenPresent, pCurChildOrder->classId);

        if (!pCurChildPresent)
        {
            bAdvance = NV_TRUE;
            continue;
        }

        if (bAdvance)
        {
            pIt->instanceID = bReverse ? pCurChildPresent->instances - 1 : 0;
        }

        if ((pIt->instanceID < (NvS32)pCurChildPresent->instances) && (pIt->instanceID >= 0))
        {
            *pEngDesc = MKENGDESC(pCurChildOrder->classId, pIt->instanceID);

            pIt->instanceID += bReverse ? -1 : 1;

            return NV_TRUE;
        }

        bAdvance = NV_TRUE;
    }

    return NV_FALSE;
}


NV_STATUS
gpuInitDispIpHal_IMPL
(
    OBJGPU *pGpu,
    NvU32   ipver
)
{
    RmHalspecOwner *pRmHalspecOwner = staticCast(pGpu, RmHalspecOwner);
    DispIpHal *pDispIpHal = &pRmHalspecOwner->dispIpHal;
    KernelDisplay *pKernelDisplay = GPU_GET_KERNEL_DISPLAY(pGpu);

    //
    // 0xFFFFFFFF ipver value happens when Display engines is disabled.  NVOC Disp IP
    // halspec doesn't have a hal variant maps to this value.  Convert it to DISPv0000.
    //
    if (ipver == 0xFFFFFFFF)
    {
        ipver = 0;
    }
    else if (ipver == 0x03010000)
    {
        //
        // Display on GV100 has 0x0301 IP ver while it uses v0300 manuals.  It is listed
        // in disp.def IP_VERSIONS table as v03_00 since we added the chip.  This wasn't a
        // problem in chip-config as there it maps a range of IP ver to an implementation.
        // Versions in "v0300 <= ipver < 0400" map to _v03_00 or lower IP version function.
        // NVOC maps exact number but not range, thus we need to override the value when
        // initializing halspec.
        //
        ipver = 0x03000000;
    }

    __nvoc_init_halspec_DispIpHal(pDispIpHal, ipver & 0xFFFF0000);

    if ((ipver & 0xFFFF0000) != 0)
    {
        DispIpHal dispIpHalv00;
        __nvoc_init_halspec_DispIpHal(&dispIpHalv00, 0);

        //
        // At GPU creation time, dispIpHal.__nvoc_HalVarIdx is initialized with DISPv0000.
        // Any valid non-zero IP version listed in halspec DispIpHal assigns __nvoc_HalVarIdx
        // to different value.
        //
        // If dispIpHal.__nvoc_HalVarIdx keeps same idx of DISPv0000 for a non-zero ipver,
        // this means the IP ver is not listed in halspec DispIpHal and should be fixed.
        //
        // NVOC-TODO : make __nvoc_init_halspec_DispIpHal return error code and remove the check
        if (pDispIpHal->__nvoc_HalVarIdx == dispIpHalv00.__nvoc_HalVarIdx)
        {
            NV_PRINTF(LEVEL_ERROR, "Invalid dispIpHal.__nvoc_HalVarIdx %d for Disp IP Vertion 0x%08x\n",
                pDispIpHal->__nvoc_HalVarIdx, ipver);

            NV_ASSERT(0);
            return NV_ERR_INVALID_STATE;
        }
    }

    void __nvoc_init_funcTable_KernelDisplay(KernelDisplay *, RmHalspecOwner *);
    void __nvoc_init_dataField_KernelDisplay(KernelDisplay*, RmHalspecOwner* );
    __nvoc_init_funcTable_KernelDisplay(pKernelDisplay, pRmHalspecOwner);
    __nvoc_init_dataField_KernelDisplay(pKernelDisplay, pRmHalspecOwner);

    void __nvoc_init_funcTable_DisplayInstanceMemory(DisplayInstanceMemory *, RmHalspecOwner *);
    void __nvoc_init_dataField_DisplayInstanceMemory(DisplayInstanceMemory *, RmHalspecOwner *);
    __nvoc_init_funcTable_DisplayInstanceMemory(KERNEL_DISPLAY_GET_INST_MEM(pKernelDisplay),
                                                pRmHalspecOwner);
    __nvoc_init_dataField_DisplayInstanceMemory(KERNEL_DISPLAY_GET_INST_MEM(pKernelDisplay),
                                                pRmHalspecOwner);

    void __nvoc_init_funcTable_KernelHead(KernelHead *, RmHalspecOwner *);
    void __nvoc_init_dataField_KernelHead(KernelHead *, RmHalspecOwner *);
    NvU32 headIdx;

    for (headIdx = 0; headIdx < OBJ_MAX_HEADS; headIdx++)
    {
        __nvoc_init_funcTable_KernelHead(KDISP_GET_HEAD(pKernelDisplay, headIdx),
                                         pRmHalspecOwner);
        __nvoc_init_dataField_KernelHead(KDISP_GET_HEAD(pKernelDisplay, headIdx),
                                         pRmHalspecOwner);
    }

    void __nvoc_init_funcTable_DisplayConsoleMemory(DisplayConsoleMemory *, RmHalspecOwner *);
    void __nvoc_init_dataField_DisplayConsoleMemory(DisplayConsoleMemory *, RmHalspecOwner *);
    __nvoc_init_funcTable_DisplayConsoleMemory(KERNEL_DISPLAY_GET_CONSOLE_MEM(pKernelDisplay),
                                                pRmHalspecOwner);
    __nvoc_init_dataField_DisplayConsoleMemory(KERNEL_DISPLAY_GET_CONSOLE_MEM(pKernelDisplay),
                                                pRmHalspecOwner);

    return NV_OK;
}

/*!
 * @brief: Initialize chip related info
 *  This function fills up the chip info structure of OBJGPU.
 *
 * @param[in]  pGpu          OBJGPU pointer
 *
 * @returns void
 */

void
gpuInitChipInfo_IMPL
(
    OBJGPU *pGpu
)
{
    //
    // NOTE: Register access and DRF field splitting should generally always
    // go in HAL functions, but PMC_BOOT_0 and PMC_BOOT_42 are an exception
    // as these are guaranteed to remain the same across chips, since we use
    // them to figure out which chip it is and how to wire up the HALs.
    //
    pGpu->chipInfo.pmcBoot0.impl          = DRF_VAL(_PMC, _BOOT_0, _IMPLEMENTATION, pGpu->chipId0);
    pGpu->chipInfo.pmcBoot0.arch          = DRF_VAL(_PMC, _BOOT_0, _ARCHITECTURE, pGpu->chipId0) << GPU_ARCH_SHIFT;
    pGpu->chipInfo.pmcBoot0.majorRev      = DRF_VAL(_PMC, _BOOT_0, _MAJOR_REVISION, pGpu->chipId0);
    pGpu->chipInfo.pmcBoot0.minorRev      = DRF_VAL(_PMC, _BOOT_0, _MINOR_REVISION, pGpu->chipId0);
    pGpu->chipInfo.pmcBoot0.minorExtRev   = NV2080_CTRL_GPU_INFO_MINOR_REVISION_EXT_NONE;
    pGpu->chipInfo.pmcBoot42.impl         = DRF_VAL(_PMC, _BOOT_42, _IMPLEMENTATION, pGpu->chipId1);
    pGpu->chipInfo.pmcBoot42.arch         = DRF_VAL(_PMC, _BOOT_42, _ARCHITECTURE, pGpu->chipId1) << GPU_ARCH_SHIFT;
    pGpu->chipInfo.pmcBoot42.majorRev     = DRF_VAL(_PMC, _BOOT_42, _MAJOR_REVISION, pGpu->chipId1);
    pGpu->chipInfo.pmcBoot42.minorRev     = DRF_VAL(_PMC, _BOOT_42, _MINOR_REVISION, pGpu->chipId1);
    pGpu->chipInfo.pmcBoot42.minorExtRev  = DRF_VAL(_PMC, _BOOT_42, _MINOR_EXTENDED_REVISION, pGpu->chipId1);

    //
    // SOC do not use pmcBoot0/pmcBoot42 and instead write the impl details to
    // these top level chipInfo fields, which is what the getters return.
    //
    pGpu->chipInfo.implementationId = pGpu->chipInfo.pmcBoot42.impl;
    pGpu->chipInfo.platformId       = pGpu->chipInfo.pmcBoot42.arch;
}

/*!
 * @brief: Returns physical address of end of DMA accessible range.
 *
 * @param[in]  pGpu  GPU object pointer
 *
 * @returns physical address of end of DMA accessible range
 */
RmPhysAddr
gpuGetDmaEndAddress_IMPL(OBJGPU *pGpu)
{
    NvU32 numPhysAddrBits = gpuGetPhysAddrWidth_HAL(pGpu, ADDR_SYSMEM);
    RmPhysAddr dmaWindowStartAddr = gpuGetDmaStartAddress(pGpu);

    return dmaWindowStartAddr + (1ULL << numPhysAddrBits) - 1;
}

VGPU_STATIC_INFO *gpuGetStaticInfo(OBJGPU *pGpu)
{

    return NULL;
}

GspStaticConfigInfo *gpuGetGspStaticInfo(OBJGPU *pGpu)
{
    return NULL;
}

OBJRPC *gpuGetGspClientRpc(OBJGPU *pGpu)
{
    if (IS_GSP_CLIENT(pGpu))
    {
        if (IsT234DorBetter(pGpu))
        {
            NV_ASSERT_OR_RETURN(GPU_GET_DCECLIENTRM(pGpu) != NULL, NULL);
            return GPU_GET_DCECLIENTRM(pGpu)->pRpc;
        }
    }
    return NULL;
}

OBJRPC *gpuGetVgpuRpc(OBJGPU *pGpu)
{
    return NULL;
}

OBJRPC *gpuGetRpc(OBJGPU *pGpu)
{
    if (IS_VIRTUAL(pGpu))
        return gpuGetVgpuRpc(pGpu);

    if (IS_GSP_CLIENT(pGpu))
        return gpuGetGspClientRpc(pGpu);

    return NULL;
}

/*!
 * @brief: Check if system memory is accessible by GPU
 *         Dependent on NV2080_CTRL_CMD_BUS_SYSMEM_ACCESS only exercised on Windows.
 *
 * @param[in]  pGpu          OBJGPU pointer
 *
 * @returns NvBool  NV_TRUE is system memory is accessible,
 *                  NV_FALSE otherwise
 */
NvBool
gpuCheckSysmemAccess_IMPL(OBJGPU* pGpu)
{
    return NV_TRUE;
}

/*!
 * @brief Read the pcie spec registers using config cycles
 *
 * @param[in] pGpu     GPU object pointer
 * @param[in] index    Register offset in PCIe config space
 * @param[out] pData    Value of the register
 *
 * @returns    NV_OK on success
 */
NV_STATUS
gpuReadBusConfigCycle_IMPL
(
    OBJGPU *pGpu,
    NvU32   index,
    NvU32   *pData
)
{
    NvU32 domain   = gpuGetDomain(pGpu);
    NvU8  bus      = gpuGetBus(pGpu);
    NvU8  device   = gpuGetDevice(pGpu);
    NvU8  function = 0;

    if (pGpu->hPci == NULL)
    {
        pGpu->hPci = osPciInitHandle(domain, bus, device, function, NULL, NULL);
    }

    *pData = osPciReadDword(pGpu->hPci, index);

    return NV_OK;
}

/*!
 * @brief Write to pcie spec registers using config cycles
 *
 * @param[in] pGpu     GPU object pointer
 * @param[in] index    Register offset in PCIe config space
 * @param[in] value    Write this value to the register
 *
 * @returns    NV_OK on success
 */
NV_STATUS
gpuWriteBusConfigCycle_IMPL
(
    OBJGPU *pGpu,
    NvU32   index,
    NvU32   value
)
{
    NvU32 domain   = gpuGetDomain(pGpu);
    NvU8  bus      = gpuGetBus(pGpu);
    NvU8  device   = gpuGetDevice(pGpu);
    NvU8  function = 0;

    if (pGpu->hPci == NULL)
    {
        pGpu->hPci = osPciInitHandle(domain, bus, device, function, NULL, NULL);
    }

    osPciWriteDword(pGpu->hPci, index, value);

    return NV_OK;
}


NV_STATUS
gpuGetDeviceEntryByType_IMPL
(
    OBJGPU                    *pGpu,
    NvU32                      deviceTypeEnum,
    NvS32                      groupId,
    NvU32                      instanceId,
    const DEVICE_INFO2_ENTRY **ppDeviceEntry
)
{
    NvU32 i;
    NV_ASSERT_OR_RETURN(ppDeviceEntry != NULL, NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OR_RETURN(groupId == DEVICE_INFO2_ENTRY_GROUP_ID_ANY ||
                            groupId >= 0,
                        NV_ERR_INVALID_ARGUMENT);
    NV_ASSERT_OK_OR_RETURN(gpuConstructDeviceInfoTable_HAL(pGpu));

    *ppDeviceEntry = NULL;

    for (i = 0; i < pGpu->numDeviceInfoEntries; ++i)
    {
        const DEVICE_INFO2_ENTRY *pEntry = &pGpu->pDeviceInfoTable[i];
        if (pEntry->typeEnum == deviceTypeEnum &&
            (groupId == DEVICE_INFO2_ENTRY_GROUP_ID_ANY ||
             pEntry->groupId == (NvU32)groupId) &&
            pEntry->instanceId == instanceId)
        {
            *ppDeviceEntry = pEntry;
            return NV_OK;
        }
    }
    return NV_ERR_OBJECT_NOT_FOUND;
}

/*!
 * @brief Sets the GC6/JT SBIOS capability
 *
 * The capabilities are retrieved from the SBIOS through JT_FUNC_CAPS subfunction
 *
 * @param[in]  pGpu GPU object pointer
 *
 * @return status   bubble up the return status from osCallACPI_DSM
 */
NV_STATUS
gpuSetGC6SBIOSCapabilities_IMPL(OBJGPU *pGpu)
{
    NV_STATUS   status;

    pGpu->acpiMethodData.jtMethodData.bSBIOSCaps = NV_FALSE;

    if ((!pGpu->acpiMethodData.bValid) ||
        (pGpu->acpiMethodData.jtMethodData.status != NV_OK))
    {
        RMTRACE_SBIOS (_ACPI_DSM_ERROR, pGpu->gpuId, ACPI_DSM_FUNCTION_JT, JT_FUNC_CAPS, 0, 0, 0, 0, 0);
        return NV_ERR_NOT_SUPPORTED;
    }

    status = gpuJtVersionSanityCheck_HAL(pGpu);
    if (status != NV_OK)
    {
        NV_PRINTF(LEVEL_ERROR,
                  "Unsupported JT revision ID. GC6 is being disabled.\n");
        RMTRACE_SBIOS (_ACPI_DSM_ERROR, pGpu->gpuId, ACPI_DSM_FUNCTION_JT, JT_FUNC_CAPS, 0, 0, 0, 0, 0);
        return status;
    }

    if (FLD_TEST_DRF(_JT_FUNC, _CAPS, _JT_ENABLED, _TRUE, pGpu->acpiMethodData.jtMethodData.jtCaps))
    {
        pGpu->acpiMethodData.jtMethodData.bSBIOSCaps = NV_TRUE;

        switch (pGpu->acpiMethodData.jtMethodData.jtRevId)
        {
            case NV_JT_FUNC_CAPS_REVISION_ID_1_03:
                // GC6 2.0 production
                break;
            case NV_JT_FUNC_CAPS_REVISION_ID_2_00:
                // GC6 3.0 and FGC6 production
                break;
            default:
                NV_PRINTF(LEVEL_ERROR,
                          "Unsupported JT revision ID. GC6 is being disabled. Update the "
                          "board EC PIC FW. On Windows, update the SBIOS GC6 AML as well.\n");
                DBG_BREAKPOINT();
                pGpu->acpiMethodData.jtMethodData.bSBIOSCaps = NV_FALSE;
                break;
        }

    }

    RMTRACE_GPU(_GC6_SBIOS_CAP, pGpu->gpuId, pGpu->acpiMethodData.jtMethodData.jtCaps, pGpu->acpiMethodData.jtMethodData.jtRevId, pGpu->acpiMethodData.jtMethodData.bSBIOSCaps, 0, 0, 0, 0);
    RMTRACE_SBIOS (_ACPI_DSM_METHOD, pGpu->gpuId, ACPI_DSM_FUNCTION_JT, JT_FUNC_CAPS, pGpu->acpiMethodData.jtMethodData.jtCaps, 0, 0, 0, 0);

    return NV_OK;
}

NV_STATUS gpuSimEscapeWrite(OBJGPU *pGpu, const char *path, NvU32 Index, NvU32 Size, NvU32 Value)
{
    return osSimEscapeWrite(pGpu, path, Index, Size, Value);
}

NV_STATUS gpuSimEscapeWriteBuffer(OBJGPU *pGpu, const char *path, NvU32 Index, NvU32 Size, void* pBuffer)
{
    return osSimEscapeWriteBuffer(pGpu, path, Index, Size, pBuffer);
}

NV_STATUS gpuSimEscapeRead(OBJGPU *pGpu, const char *path, NvU32 Index, NvU32 Size, NvU32 *Value)
{
    return osSimEscapeRead(pGpu, path, Index, Size, Value);
}

NV_STATUS gpuSimEscapeReadBuffer(OBJGPU *pGpu, const char *path, NvU32 Index, NvU32 Size, void* pBuffer)
{
    return osSimEscapeReadBuffer(pGpu, path, Index, Size, pBuffer);
}

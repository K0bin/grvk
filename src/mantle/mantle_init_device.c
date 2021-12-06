#include <stdio.h>
#include "mantle_internal.h"
#include "mantle_object.h"

#define NVIDIA_VENDOR_ID 0x10de
#define INVALID_QUEUE_INDEX (~0u)

static char* getGrvkEngineName(
    const GR_CHAR* engineName)
{
    size_t engineNameLen = (engineName != NULL ? strlen(engineName) : 0);
    size_t grvkEngineNameLen = engineNameLen + 64;
    char* grvkEngineName = malloc(sizeof(char) * grvkEngineNameLen);

    if (engineName == NULL) {
        snprintf(grvkEngineName, grvkEngineNameLen, "[GRVK %s]", GRVK_VERSION);
    } else {
        snprintf(grvkEngineName, grvkEngineNameLen, "[GRVK %s] %s", GRVK_VERSION, engineName);
    }

    return grvkEngineName;
}

static VkBuffer allocateAtomicCounterBuffer(
    const GrDevice* grDevice,
    unsigned slotCount)
{
    VkDeviceMemory vkMemory = VK_NULL_HANDLE;
    VkBuffer vkBuffer = VK_NULL_HANDLE;
    VkResult vkRes;

    unsigned memoryTypeIndex = 0;

    for (unsigned i = 0; i < grDevice->memoryProperties.memoryTypeCount; i++) {
        if (grDevice->memoryProperties.memoryTypes[i].propertyFlags &
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            memoryTypeIndex = i;
            break;
        }
    }

    const VkMemoryAllocateInfo allocateInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = NULL,
        .allocationSize = slotCount * sizeof(uint32_t),
        .memoryTypeIndex = memoryTypeIndex,
    };

    vkRes = VKD.vkAllocateMemory(grDevice->device, &allocateInfo, NULL, &vkMemory);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkAllocateMemory failed (%d)\n", vkRes);
        return VK_NULL_HANDLE;
    }

    const VkBufferCreateInfo bufferCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .size = allocateInfo.allocationSize,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
    };

    vkRes = VKD.vkCreateBuffer(grDevice->device, &bufferCreateInfo, NULL, &vkBuffer);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkCreateBuffer failed (%d)\n", vkRes);
        return VK_NULL_HANDLE;
    }

    vkRes = VKD.vkBindBufferMemory(grDevice->device, vkBuffer, vkMemory, 0);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkBindBufferMemory failed (%d)\n", vkRes);
        return VK_NULL_HANDLE;
    }

    return vkBuffer;
}

// Initialization and Device Functions

GR_RESULT GR_STDCALL grInitAndEnumerateGpus(
    const GR_APPLICATION_INFO* pAppInfo,
    const GR_ALLOC_CALLBACKS* pAllocCb,
    GR_UINT* pGpuCount,
    GR_PHYSICAL_GPU gpus[GR_MAX_PHYSICAL_GPUS])
{
    LOGT("%p %p %p\n", pAppInfo, pAllocCb, pGpuCount);
    VkInstance vkInstance = VK_NULL_HANDLE;
    VkResult vkRes;

    vulkanLoaderLibraryInit(&vkl);

    LOGI("app \"%s\" (%08X), engine \"%s\" (%08X), api %08X\n",
         pAppInfo->pAppName, pAppInfo->appVersion,
         pAppInfo->pEngineName, pAppInfo->engineVersion,
         pAppInfo->apiVersion);

    quirkInit(pAppInfo);

    if (pAllocCb != NULL) {
        LOGW("unhandled alloc callbacks\n");
    }

    char* grvkEngineName = getGrvkEngineName(pAppInfo->pEngineName);

    const VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pNext = NULL,
        .pApplicationName = pAppInfo->pAppName,
        .applicationVersion = pAppInfo->appVersion,
        .pEngineName = grvkEngineName,
        .engineVersion = pAppInfo->engineVersion,
        .apiVersion = VK_API_VERSION_1_2,
    };

    const char *instanceExtensions[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
    };

    const VkInstanceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .pApplicationInfo = &appInfo,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = NULL,
        .enabledExtensionCount = COUNT_OF(instanceExtensions),
        .ppEnabledExtensionNames = instanceExtensions,
    };

    vkRes = vkl.vkCreateInstance(&createInfo, NULL, &vkInstance);
    free(grvkEngineName);

    if (vkRes != VK_SUCCESS) {
        LOGE("vkCreateInstance failed (%d)\n", vkRes);

        if (vkRes == VK_ERROR_INCOMPATIBLE_DRIVER) {
            LOGE("incompatible driver detected. Vulkan 1.2 support is required\n");
        }

        return GR_ERROR_INITIALIZATION_FAILED;
    }

    vulkanLoaderInstanceInit(&vki, vkInstance);

    uint32_t vkPhysicalDeviceCount = 0;
    vki.vkEnumeratePhysicalDevices(vkInstance, &vkPhysicalDeviceCount, NULL);
    if (vkPhysicalDeviceCount > GR_MAX_PHYSICAL_GPUS) {
        vkPhysicalDeviceCount = GR_MAX_PHYSICAL_GPUS;
    }

    VkPhysicalDevice physicalDevices[GR_MAX_PHYSICAL_GPUS];
    vki.vkEnumeratePhysicalDevices(vkInstance, &vkPhysicalDeviceCount, physicalDevices);

    *pGpuCount = vkPhysicalDeviceCount;
    for (int i = 0; i < *pGpuCount; i++) {
        GrPhysicalGpu* grPhysicalGpu = malloc(sizeof(GrPhysicalGpu));
        *grPhysicalGpu = (GrPhysicalGpu) {
            .grBaseObj = { GR_OBJ_TYPE_PHYSICAL_GPU },
            .instance = vkInstance,
            .physicalDevice = physicalDevices[i],
            .physicalDeviceProps = { 0 }, // Initialized below
        };

        vki.vkGetPhysicalDeviceProperties(physicalDevices[i], &grPhysicalGpu->physicalDeviceProps);

        gpus[i] = (GR_PHYSICAL_GPU)grPhysicalGpu;
    }

    return GR_SUCCESS;
}

GR_RESULT GR_STDCALL grGetGpuInfo(
    GR_PHYSICAL_GPU gpu,
    GR_ENUM infoType,
    GR_SIZE* pDataSize,
    GR_VOID* pData)
{
    LOGT("%p 0x%X %p %p\n", gpu, infoType, pDataSize, pData);
    GrPhysicalGpu* grPhysicalGpu = (GrPhysicalGpu*)gpu;

    if (grPhysicalGpu == NULL) {
        return GR_ERROR_INVALID_HANDLE;
    } else if (GET_OBJ_TYPE(grPhysicalGpu) != GR_OBJ_TYPE_PHYSICAL_GPU) {
        return GR_ERROR_INVALID_OBJECT_TYPE;
    } else if (pDataSize == NULL) {
        return GR_ERROR_INVALID_POINTER;
    }

    const VkPhysicalDeviceProperties* props = &grPhysicalGpu->physicalDeviceProps;
    unsigned expectedSize = 0;

    switch (infoType) {
    case GR_INFO_TYPE_PHYSICAL_GPU_PROPERTIES:
        expectedSize = sizeof(GR_PHYSICAL_GPU_PROPERTIES);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_PROPERTIES, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        GR_PHYSICAL_GPU_PROPERTIES* gpuProps = (GR_PHYSICAL_GPU_PROPERTIES*)pData;
        *gpuProps = (GR_PHYSICAL_GPU_PROPERTIES) {
            .apiVersion = 0x19000, // 19.4.3
            .driverVersion = 0x49C00000, // 19.4.3
            .vendorId = props->vendorID,
            .deviceId = props->deviceID,
            .gpuType = getGrPhysicalGpuType(props->deviceType),
            .gpuName = "", // Initialized below
            .maxMemRefsPerSubmission = 16384, // 19.4.3
            .reserved = 4200043, // 19.4.3
            .maxInlineMemoryUpdateSize = 32768, // 19.4.3
            .maxBoundDescriptorSets = 2, // 19.4.3
            .maxThreadGroupSize = props->limits.maxComputeWorkGroupSize[0],
            .timestampFrequency = 1000000000.f / props->limits.timestampPeriod,
            .multiColorTargetClears = true, // 19.4.3
        };
        strncpy(gpuProps->gpuName, props->deviceName, GR_MAX_PHYSICAL_GPU_NAME);
        break;
    case GR_INFO_TYPE_PHYSICAL_GPU_PERFORMANCE:
        expectedSize = sizeof(GR_PHYSICAL_GPU_PERFORMANCE);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_PERFORMANCE, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        *(GR_PHYSICAL_GPU_PERFORMANCE*)pData = (GR_PHYSICAL_GPU_PERFORMANCE) {
            .maxGpuClock = 1000.f,
            .aluPerClock = 16.f, // 19.4.3
            .texPerClock = 16.f, // 19.4.3
            .primsPerClock = 2.f, // 19.4.3
            .pixelsPerClock = 16.f, // 19.4.3
        };
        break;
    case GR_INFO_TYPE_PHYSICAL_GPU_QUEUE_PROPERTIES:
        expectedSize = 4 * sizeof(GR_PHYSICAL_GPU_QUEUE_PROPERTIES);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_QUEUE_PROPERTIES, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        // Values from 19.4.3 driver
        ((GR_PHYSICAL_GPU_QUEUE_PROPERTIES*)pData)[0] = (GR_PHYSICAL_GPU_QUEUE_PROPERTIES) {
            .queueType = GR_QUEUE_UNIVERSAL,
            .queueCount = 1,
            .maxAtomicCounters = UNIVERSAL_ATOMIC_COUNTERS_COUNT,
            .supportsTimestamps = true, // TODO check for support
        };
        ((GR_PHYSICAL_GPU_QUEUE_PROPERTIES*)pData)[1] = (GR_PHYSICAL_GPU_QUEUE_PROPERTIES) {
            .queueType = GR_QUEUE_COMPUTE,
            .queueCount = 1,
            .maxAtomicCounters = COMPUTE_ATOMIC_COUNTERS_COUNT,
            .supportsTimestamps = true, // TODO check for support
        };
        ((GR_PHYSICAL_GPU_QUEUE_PROPERTIES*)pData)[2] = (GR_PHYSICAL_GPU_QUEUE_PROPERTIES) {
            .queueType = GR_EXT_QUEUE_DMA,
            .queueCount = 1,
            .maxAtomicCounters = 0,
            // FIXME Vulkan doesn't allow using vkCmdCopyQueryPoolResults on transfer queues
            .supportsTimestamps = true, // TODO check for support
        };
        ((GR_PHYSICAL_GPU_QUEUE_PROPERTIES*)pData)[3] = (GR_PHYSICAL_GPU_QUEUE_PROPERTIES) {
            .queueType = GR_EXT_QUEUE_TIMER,
            .queueCount = 0, // TODO implement
            .maxAtomicCounters = 0,
            .supportsTimestamps = false,
        };
        break;
    case GR_INFO_TYPE_PHYSICAL_GPU_MEMORY_PROPERTIES:
        expectedSize = sizeof(GR_PHYSICAL_GPU_MEMORY_PROPERTIES);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_MEMORY_PROPERTIES, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        // TODO don't expose unsupported features?
        *(GR_PHYSICAL_GPU_MEMORY_PROPERTIES*)pData = (GR_PHYSICAL_GPU_MEMORY_PROPERTIES) {
            .flags = GR_MEMORY_VIRTUAL_REMAPPING_SUPPORT | // 19.4.3
                     GR_MEMORY_PINNING_SUPPORT | // 19.4.3
                     GR_MEMORY_PREFER_GLOBAL_REFS, // 19.4.3
            .virtualMemPageSize = 4096, // 19.4.3
            .maxVirtualMemSize = 1086626725888ull, // 19.4.3
            .maxPhysicalMemSize = 10354294784ull, // 19.4.3
        };
        break;
    case GR_INFO_TYPE_PHYSICAL_GPU_IMAGE_PROPERTIES:
        expectedSize = sizeof(GR_PHYSICAL_GPU_IMAGE_PROPERTIES);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_IMAGE_PROPERTIES, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        *(GR_PHYSICAL_GPU_IMAGE_PROPERTIES*)pData = (GR_PHYSICAL_GPU_IMAGE_PROPERTIES) {
            .maxSliceWidth = props->limits.maxImageDimension1D,
            .maxSliceHeight = props->limits.maxImageDimension2D,
            .maxDepth = props->limits.maxImageDimension3D,
            .maxArraySlices = props->limits.maxImageArrayLayers,
            .reserved1 = 0, // 19.4.3
            .reserved2 = 0, // 19.4.3
            .maxMemoryAlignment = 262144, // 19.4.3
            .sparseImageSupportLevel = 0, // 19.4.3
            .flags = 0x0, // 19.4.3
        };
        break;
    case GR_EXT_INFO_TYPE_PHYSICAL_GPU_SUPPORTED_AXL_VERSION:
        expectedSize = sizeof(GR_PHYSICAL_GPU_SUPPORTED_AXL_VERSION);

        if (pData == NULL) {
            break;
        } else if (*pDataSize < expectedSize) {
            LOGW("can't write GR_PHYSICAL_GPU_SUPPORTED_AXL_VERSION, got size %d, expected %d\n",
                 *pDataSize, expectedSize);
            return GR_ERROR_INVALID_MEMORY_SIZE;
        }

        *(GR_PHYSICAL_GPU_SUPPORTED_AXL_VERSION*)pData = (GR_PHYSICAL_GPU_SUPPORTED_AXL_VERSION) {
            .minVersion = 0,
            .maxVersion = UINT32_MAX,
        };
        break;
    default:
        LOGE("unsupported info type 0x%X\n", infoType);
        return GR_ERROR_INVALID_VALUE;
    }

    assert(expectedSize != 0);
    *pDataSize = expectedSize;

    return GR_SUCCESS;
}

GR_RESULT GR_STDCALL grCreateDevice(
    GR_PHYSICAL_GPU gpu,
    const GR_DEVICE_CREATE_INFO* pCreateInfo,
    GR_DEVICE* pDevice)
{
    LOGT("%p %p %p\n", gpu, pCreateInfo, pDevice);
    GR_RESULT res = GR_SUCCESS;
    VkResult vkRes;
    GrPhysicalGpu* grPhysicalGpu = (GrPhysicalGpu*)gpu;
    VULKAN_DEVICE vkd;
    VkDevice vkDevice = VK_NULL_HANDLE;
    uint32_t vkUniversalQueueFamilyIndex = INVALID_QUEUE_INDEX;
    unsigned vkUniversalQueueCount = 0;
    uint32_t vkUniversalQueueUsedCount = 0;
    uint32_t vkComputeQueueFamilyIndex = INVALID_QUEUE_INDEX;
    unsigned vkComputeQueueCount = 0;
    uint32_t vkComputeQueueUsedCount = 0;
    uint32_t vkDmaQueueFamilyIndex = INVALID_QUEUE_INDEX;
    unsigned vkDmaQueueCount = 0;
    uint32_t vkDmaQueueUsedCount = 0;
    uint32_t universalQueueFamilyIndex = INVALID_QUEUE_INDEX;
    uint32_t universalQueueIndex = 0;
    uint32_t computeQueueFamilyIndex = INVALID_QUEUE_INDEX;
    uint32_t computeQueueIndex = 0;
    uint32_t dmaQueueFamilyIndex = INVALID_QUEUE_INDEX;
    uint32_t dmaQueueIndex = 0;
    uint32_t driverVersion;

    const VkPhysicalDeviceProperties* props = &grPhysicalGpu->physicalDeviceProps;

    if (props->vendorID == NVIDIA_VENDOR_ID) {
        // Fix up driver version
        driverVersion = VK_MAKE_VERSION(
            VK_VERSION_MAJOR(props->driverVersion),
            VK_VERSION_MINOR(props->driverVersion >> 0) >> 2,
            VK_VERSION_PATCH(props->driverVersion >> 2) >> 4);
    } else {
        driverVersion = props->driverVersion;
    }

    LOGI("%04X:%04X \"%s\" (Vulkan %d.%d.%d, driver %d.%d.%d)\n",
         props->vendorID, props->deviceID, props->deviceName,
         VK_VERSION_MAJOR(props->apiVersion),
         VK_VERSION_MINOR(props->apiVersion),
         VK_VERSION_PATCH(props->apiVersion),
         VK_VERSION_MAJOR(driverVersion),
         VK_VERSION_MINOR(driverVersion),
         VK_VERSION_PATCH(driverVersion));

    uint32_t vkQueueFamilyPropertyCount = 0;
    vki.vkGetPhysicalDeviceQueueFamilyProperties(grPhysicalGpu->physicalDevice,
                                                 &vkQueueFamilyPropertyCount, NULL);

    VkQueueFamilyProperties* queueFamilyProperties =
        malloc(sizeof(VkQueueFamilyProperties) * vkQueueFamilyPropertyCount);
    vki.vkGetPhysicalDeviceQueueFamilyProperties(grPhysicalGpu->physicalDevice,
                                                 &vkQueueFamilyPropertyCount,
                                                 queueFamilyProperties);

    // Count Vulkan queues
    for (unsigned i = 0; i < vkQueueFamilyPropertyCount; i++) {
        const VkQueueFamilyProperties* queueFamilyProperty = &queueFamilyProperties[i];

        if ((queueFamilyProperty->queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            vkUniversalQueueFamilyIndex = i;
            vkUniversalQueueCount = queueFamilyProperty->queueCount;
        } else if (queueFamilyProperty->queueFlags & VK_QUEUE_COMPUTE_BIT) {
            vkComputeQueueFamilyIndex = i;
            vkComputeQueueCount = queueFamilyProperty->queueCount;
        } else if (queueFamilyProperty->queueFlags & VK_QUEUE_TRANSFER_BIT) {
            vkDmaQueueFamilyIndex = i;
            vkDmaQueueCount = queueFamilyProperty->queueCount;
        }
    }

    // Figure out which Vulkan queues of each family will be used
    // NOTE: we assume no more than one queue is requested per type
    for (unsigned i = 0; i < pCreateInfo->queueRecordCount; i++) {
        const GR_DEVICE_QUEUE_CREATE_INFO* requestedQueue = &pCreateInfo->pRequestedQueues[i];

        switch (requestedQueue->queueType) {
        case GR_QUEUE_UNIVERSAL:
            vkUniversalQueueUsedCount = MIN(vkUniversalQueueUsedCount + 1, vkUniversalQueueCount);
            universalQueueFamilyIndex = vkUniversalQueueFamilyIndex;
            universalQueueIndex = vkUniversalQueueUsedCount - 1;
            break;
        case GR_QUEUE_COMPUTE:
            if (vkComputeQueueCount > 0) {
                vkComputeQueueUsedCount = MIN(vkComputeQueueUsedCount + 1, vkComputeQueueCount);
                computeQueueFamilyIndex = vkComputeQueueFamilyIndex;
                computeQueueIndex = vkComputeQueueUsedCount - 1;
            } else {
                vkUniversalQueueUsedCount = MIN(vkUniversalQueueUsedCount + 1, vkUniversalQueueCount);
                computeQueueFamilyIndex = vkUniversalQueueFamilyIndex;
                computeQueueIndex = vkUniversalQueueUsedCount - 1;
                LOGI("compute queue remapped to universal queue %d\n", computeQueueIndex);
            }
            break;
        case GR_EXT_QUEUE_DMA:
            if (vkDmaQueueCount > 0) {
                vkDmaQueueUsedCount = MIN(vkDmaQueueUsedCount + 1, vkDmaQueueCount);
                dmaQueueFamilyIndex = vkDmaQueueFamilyIndex;
                dmaQueueIndex = vkDmaQueueUsedCount - 1;
            } else if (vkComputeQueueCount > 0) {
                vkComputeQueueUsedCount = MIN(vkComputeQueueUsedCount + 1, vkComputeQueueCount);
                dmaQueueFamilyIndex = vkComputeQueueFamilyIndex;
                dmaQueueIndex = vkComputeQueueUsedCount - 1;
                LOGI("DMA queue remapped to compute queue %d\n", dmaQueueIndex);
            } else {
                vkUniversalQueueUsedCount = MIN(vkUniversalQueueUsedCount + 1, vkUniversalQueueCount);
                dmaQueueFamilyIndex = vkUniversalQueueFamilyIndex;
                dmaQueueIndex = vkUniversalQueueUsedCount - 1;
                LOGI("DMA queue remapped to universal queue %d\n", dmaQueueIndex);
            }
            break;
        }
    }

    float priorities[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    unsigned queueCreateInfoCount = 0;
    VkDeviceQueueCreateInfo queueCreateInfos[3];

    if (vkUniversalQueueUsedCount > 0) {
        queueCreateInfos[queueCreateInfoCount] = (VkDeviceQueueCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .queueFamilyIndex = vkUniversalQueueFamilyIndex,
            .queueCount = vkUniversalQueueUsedCount,
            .pQueuePriorities = priorities,
        };
        queueCreateInfoCount++;
    }
    if (vkComputeQueueUsedCount > 0) {
        queueCreateInfos[queueCreateInfoCount] = (VkDeviceQueueCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .queueFamilyIndex = vkComputeQueueFamilyIndex,
            .queueCount = vkComputeQueueUsedCount,
            .pQueuePriorities = priorities,
        };
        queueCreateInfoCount++;
    }
    if (vkDmaQueueUsedCount > 0) {
        queueCreateInfos[queueCreateInfoCount] = (VkDeviceQueueCreateInfo) {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = NULL,
            .flags = 0,
            .queueFamilyIndex = vkDmaQueueFamilyIndex,
            .queueCount = vkDmaQueueUsedCount,
            .pQueuePriorities = priorities,
        };
        queueCreateInfoCount++;
    }

    if (res != GR_SUCCESS) {
        goto bail;
    }

    VkPhysicalDeviceCustomBorderColorFeaturesEXT customBorderColor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,
        .pNext = NULL,
        .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE,
    };
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicState = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,
        .pNext = &customBorderColor,
        .extendedDynamicState = VK_TRUE,
    };
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT demoteToHelperInvocation = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT,
        .pNext = &extendedDynamicState,
        .shaderDemoteToHelperInvocation = VK_TRUE,
    };
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamicRendering = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
        .pNext = &demoteToHelperInvocation,
        .dynamicRendering = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features vulkan12DeviceFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &dynamicRendering,
        .samplerMirrorClampToEdge = VK_TRUE,
        .separateDepthStencilLayouts = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 deviceFeatures = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vulkan12DeviceFeatures,
        .features = {
            .imageCubeArray = VK_TRUE,
            .independentBlend = VK_TRUE,
            .geometryShader = VK_TRUE,
            .tessellationShader = VK_TRUE,
            .sampleRateShading = VK_TRUE,
            .dualSrcBlend = VK_TRUE,
            .logicOp = VK_TRUE,
            .depthClamp = VK_TRUE,
            .fillModeNonSolid = VK_TRUE,
            .multiViewport = VK_TRUE,
            .samplerAnisotropy = VK_TRUE,
            .fragmentStoresAndAtomics = VK_TRUE,
            .shaderStorageImageReadWithoutFormat = VK_TRUE,
            .shaderStorageImageWriteWithoutFormat = VK_TRUE,
            .occlusionQueryPrecise = VK_TRUE,
            .pipelineStatisticsQuery = VK_TRUE,
            .fragmentStoresAndAtomics = VK_TRUE,
        },
    };

    const char *deviceExtensions[] = {
        VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
        VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    const VkDeviceCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &deviceFeatures,
        .flags = 0,
        .queueCreateInfoCount = queueCreateInfoCount,
        .pQueueCreateInfos = queueCreateInfos,
        .enabledLayerCount = 0,
        .ppEnabledLayerNames = NULL,
        .enabledExtensionCount = COUNT_OF(deviceExtensions),
        .ppEnabledExtensionNames = deviceExtensions,
        .pEnabledFeatures = NULL,
    };

    vkRes = vki.vkCreateDevice(grPhysicalGpu->physicalDevice, &createInfo, NULL, &vkDevice);
    if (vkRes != VK_SUCCESS) {
        LOGE("vkCreateDevice failed (%d)\n", vkRes);

        if (vkRes == VK_ERROR_EXTENSION_NOT_PRESENT) {
            LOGE("missing extension. make sure your Vulkan driver supports:\n");
            for (unsigned i = 0; i < COUNT_OF(deviceExtensions); i++) {
                LOGE("- %s\n", deviceExtensions[i]);
            }
        }

        res = GR_ERROR_INITIALIZATION_FAILED;
        goto bail;
    }

    vulkanLoaderDeviceInit(&vkd, vkDevice);

    VkPhysicalDeviceMemoryProperties memoryProperties;
    vki.vkGetPhysicalDeviceMemoryProperties(grPhysicalGpu->physicalDevice, &memoryProperties);

    VkMemoryPropertyFlags memTypeRetainMask = 0;
    VkMemoryRequirements reqs;
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .size = 16,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
            | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL
    };
    VkBuffer buffer;
    vkd.vkCreateBuffer(vkDevice, &bufferInfo, NULL, &buffer);
    vkd.vkGetBufferMemoryRequirements(vkDevice, buffer, &reqs);
    vkd.vkDestroyBuffer(vkDevice, buffer, NULL);
    memTypeRetainMask |= reqs.memoryTypeBits;

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = NULL,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {
            .width = 16,
            .height = 16,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT
            | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    VkImage image;
    vkd.vkCreateImage(vkDevice, &imageInfo, NULL, &image);
    vkd.vkGetImageMemoryRequirements(vkDevice, image, &reqs);
    vkd.vkDestroyImage(vkDevice, image, NULL);
    memTypeRetainMask |= reqs.memoryTypeBits;

    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_LINEAR;
    vkd.vkCreateImage(vkDevice, &imageInfo, NULL, &image);
    vkd.vkGetImageMemoryRequirements(vkDevice, image, &reqs);
    vkd.vkDestroyImage(vkDevice, image, NULL);
    memTypeRetainMask |= reqs.memoryTypeBits;

    GrvkMemoryHeaps heaps = {
        .heapCount = 0
    };

    for (int i = 0; i < memoryProperties.memoryTypeCount && heaps.heapCount < 8; i++) {
        VkMemoryType *memoryType = &memoryProperties.memoryTypes[i];
        if (memoryType->propertyFlags == 0)
            continue;

        GrvkMemoryHeap heap = {
            .vkMemoryTypeIndex = i,
            .vkMemoryHeapIndex = memoryType->heapIndex,
            .vkPropertyFlags = memoryType->propertyFlags,
            .size = memoryProperties.memoryHeaps[memoryType->heapIndex].size,
        };
        heaps.heaps[heaps.heapCount] = heap;
        heaps.heapCount++;
    }

    for (int i = 0; i < memoryProperties.memoryTypeCount && heaps.heapCount < 8; i++) {
        VkMemoryType *memoryType = &memoryProperties.memoryTypes[i];
        if (memoryType->propertyFlags != 0 || (memTypeRetainMask & (1 << i)) == 0)
            continue;

        GrvkMemoryHeap heap = {
            .vkMemoryTypeIndex = i,
            .vkMemoryHeapIndex = memoryType->heapIndex,
            .vkPropertyFlags = memoryType->propertyFlags,
            .size = memoryProperties.memoryHeaps[memoryType->heapIndex].size,
        };
        heaps.heaps[heaps.heapCount] = heap;
        heaps.heapCount++;
    }

    bool virtualizeMemory = props->vendorID == NVIDIA_VENDOR_ID;

    VmaAllocator allocator;
    if (virtualizeMemory) {
        VkMemoryPropertyFlags mask = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        memset(&heaps, 0, sizeof(GrvkMemoryHeaps));
        VkMemoryPropertyFlags requestedHeaps[4];
        requestedHeaps[0] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        requestedHeaps[1] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        requestedHeaps[2] = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        requestedHeaps[3] = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (int i = 0; i < 4; i++) {
            for (int memTypeI = 0; memTypeI < memoryProperties.memoryTypeCount; memTypeI++) {
                VkMemoryType *memoryType = &memoryProperties.memoryTypes[memTypeI];
                if ((memoryType->propertyFlags & mask) == requestedHeaps[i]) {
                    LOGD("Mantle heap %d mapped to Vulkan memory type: %d\n", heaps.heapCount, memTypeI);
                    GrvkMemoryHeap *heap = &heaps.heaps[heaps.heapCount];
                    heaps.heapCount++;
                    heap->vkMemoryTypeIndex = memTypeI;
                    heap->vkMemoryHeapIndex = memoryType->heapIndex;
                    heap->size = memoryProperties.memoryHeaps[memoryType->heapIndex].size;
                    heap->vkPropertyFlags = requestedHeaps[i];
                }
            }
        }
        if (heaps.heaps[3].vkMemoryTypeIndex == UINT_MAX) {
            heaps.heapCount = 3;
        }

        VmaVulkanFunctions vkFuncs = {
            .vkGetPhysicalDeviceProperties = vki.vkGetPhysicalDeviceProperties,
            .vkGetPhysicalDeviceMemoryProperties = vki.vkGetPhysicalDeviceMemoryProperties,
            .vkAllocateMemory = vkd.vkAllocateMemory,
            .vkFreeMemory = vkd.vkFreeMemory,
            .vkMapMemory = vkd.vkMapMemory,
            .vkUnmapMemory = vkd.vkUnmapMemory,
            .vkFlushMappedMemoryRanges = vkd.vkFlushMappedMemoryRanges,
            .vkInvalidateMappedMemoryRanges = vkd.vkInvalidateMappedMemoryRanges,
            .vkBindBufferMemory = vkd.vkBindBufferMemory,
            .vkBindImageMemory = vkd.vkBindImageMemory,
            .vkGetBufferMemoryRequirements = vkd.vkGetBufferMemoryRequirements,
            .vkGetImageMemoryRequirements = vkd.vkGetImageMemoryRequirements,
            .vkCreateBuffer = vkd.vkCreateBuffer,
            .vkDestroyBuffer = vkd.vkDestroyBuffer,
            .vkCreateImage = vkd.vkCreateImage,
            .vkDestroyImage = vkd.vkDestroyImage,
            .vkCmdCopyBuffer = vkd.vkCmdCopyBuffer,
            .vkGetBufferMemoryRequirements2KHR = vkd.vkGetBufferMemoryRequirements2,
            .vkGetImageMemoryRequirements2KHR = vkd.vkGetImageMemoryRequirements2,
            .vkBindBufferMemory2KHR = vkd.vkBindBufferMemory2,
            .vkBindImageMemory2KHR = vkd.vkBindImageMemory2,
            .vkGetPhysicalDeviceMemoryProperties2KHR = vki.vkGetPhysicalDeviceMemoryProperties2,
        };

        VmaAllocatorCreateInfo allocatorInfo = {
            .vulkanApiVersion = VK_API_VERSION_1_2,
            .physicalDevice = grPhysicalGpu->physicalDevice,
            .device = vkDevice,
            .instance = grPhysicalGpu->instance,
            .flags = 0,
            .preferredLargeHeapBlockSize = 0,
            .pAllocationCallbacks = NULL,
            .pDeviceMemoryCallbacks = NULL,
            .frameInUseCount = 0,
            .pHeapSizeLimit = NULL,
            .pRecordSettings = NULL,
            .pTypeExternalMemoryHandleTypes = NULL,
            .pVulkanFunctions = &vkFuncs,
        };
        vkRes = vmaCreateAllocator(&allocatorInfo, &allocator);
        if (vkRes != VK_SUCCESS) {
            LOGE("vmaCreateAllocator failed (%d)\n", vkRes);
            res = GR_ERROR_INITIALIZATION_FAILED;
            goto bail;
        }
    }

    GrDevice* grDevice = malloc(sizeof(GrDevice));
    *grDevice = (GrDevice) {
        .grBaseObj = { GR_OBJ_TYPE_DEVICE },
        .vkd = vkd,
        .device = vkDevice,
        .physicalDevice = grPhysicalGpu->physicalDevice,
        .memoryProperties = memoryProperties,
        .grUniversalQueue = NULL, // Initialized below
        .grComputeQueue = NULL, // Initialized below
        .grDmaQueue = NULL, // Initialized below
        .universalAtomicCounterBuffer = VK_NULL_HANDLE, // Initialized below
        .computeAtomicCounterBuffer = VK_NULL_HANDLE, // Initialized below
        .grBorderColorPalette = NULL,
        .heaps = heaps,
        .virtualizeMemory = virtualizeMemory,
        .allocator = allocator,
    };

    if (universalQueueFamilyIndex != INVALID_QUEUE_INDEX) {
        grDevice->grUniversalQueue =
            grQueueCreate(grDevice, universalQueueFamilyIndex, universalQueueIndex);
        grDevice->universalAtomicCounterBuffer =
            allocateAtomicCounterBuffer(grDevice, UNIVERSAL_ATOMIC_COUNTERS_COUNT);
    }
    if (computeQueueFamilyIndex != INVALID_QUEUE_INDEX) {
        grDevice->grComputeQueue =
            grQueueCreate(grDevice, computeQueueFamilyIndex, computeQueueIndex);
        grDevice->computeAtomicCounterBuffer =
            allocateAtomicCounterBuffer(grDevice, COMPUTE_ATOMIC_COUNTERS_COUNT);
    }
    if (dmaQueueFamilyIndex != INVALID_QUEUE_INDEX) {
        grDevice->grDmaQueue = grQueueCreate(grDevice, dmaQueueFamilyIndex, dmaQueueIndex);
    }

    *pDevice = (GR_DEVICE)grDevice;

bail:
    if (res != GR_SUCCESS) {
        vkd.vkDestroyDevice(vkDevice, NULL);
    }

    return res;
}

GR_RESULT GR_STDCALL grDestroyDevice(
    GR_DEVICE device)
{
    LOGT("%p\n", device);
    GrDevice* grDevice = (GrDevice*)device;

    if (grDevice == NULL) {
        return GR_ERROR_INVALID_HANDLE;
    } else if (GET_OBJ_TYPE(grDevice) != GR_OBJ_TYPE_DEVICE) {
        return GR_ERROR_INVALID_OBJECT_TYPE;
    }

    VKD.vkDestroyDevice(grDevice->device, NULL);
    free(grDevice);

    return GR_SUCCESS;
}

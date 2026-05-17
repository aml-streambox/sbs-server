#define _GNU_SOURCE
#define SBS_LOG_COMP "compositor"

#include "sbs/compositor.h"
#include "sbs/dmabuf_alloc.h"
#include "sbs/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <drm/drm_fourcc.h>

#ifdef __aarch64__
#include <arm_neon.h>
#endif

static const char *instance_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
};

static const char *device_extensions[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
};

static const VkApplicationInfo app_info = {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "sbs-server",
    .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
    .pEngineName = "sbs",
    .engineVersion = VK_MAKE_VERSION(0, 0, 1),
    .apiVersion = VK_API_VERSION_1_1,
};

#define SBS_HDR_LUT_SIZE 33u
#define SBS_LUT_MAX_SIZE 65u
#define SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE SBS_NATIVE_CANVAS_RING_SIZE
#define SBS_NATIVE_TOTAL_ENTRY_COUNT (SBS_NATIVE_CANVAS_RING_SIZE * 2u)
#define SBS_NATIVE_TIMING_QUERY_MARKS (SBS_NATIVE_TIMING_MAX_LAYERS + 4u)
#define SBS_NATIVE_YUV_PC_SIZE 96u
#define SBS_NATIVE_P010_DIRECT_PC_SIZE 128u
#define SBS_NATIVE_DOWNSCALE_PC_SIZE 20u
#define SBS_NATIVE_OCCLUSION_MAX_RECTS 32u
#define SBS_NATIVE_ROTATION_SHIFT 8u
#define SBS_NATIVE_ROTATION_MASK (3u << SBS_NATIVE_ROTATION_SHIFT)
#define SBS_NATIVE_FLIP_HORIZONTAL (1u << 10u)
#define SBS_NATIVE_FLIP_VERTICAL (1u << 11u)
#ifndef SBS_DRM_FORMAT_AMLY
#define SBS_DRM_FORMAT_AMLY 0x594c4d41u
#endif
#ifndef DRM_FORMAT_ABGR8888
#define DRM_FORMAT_ABGR8888 0x34324241
#endif

static bool device_extension_required(const char *name)
{
    return strcmp(name, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0 ||
           strcmp(name, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0 ||
           strcmp(name, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0 ||
           strcmp(name, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0;
}

static const char *format_name(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_R8_UNORM:
        return "R8_UNORM";
    case VK_FORMAT_R8G8_UNORM:
        return "R8G8_UNORM";
    case VK_FORMAT_R16_UNORM:
        return "R16_UNORM";
    case VK_FORMAT_R16G16_UNORM:
        return "R16G16_UNORM";
    default:
        return "unknown";
    }
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static uint32_t native_timing_query_base(uint32_t entry_idx)
{
    if (entry_idx >= SBS_NATIVE_TOTAL_ENTRY_COUNT)
        return UINT32_MAX;
    return entry_idx * SBS_NATIVE_TIMING_QUERY_MARKS;
}

/* ── Vulkan instance ──────────────────────────────────────────── */

static int create_instance(sbs_compositor_t *comp)
{
    uint32_t api_version = 0;
    vkEnumerateInstanceVersion(&api_version);
    LOG_I("Vulkan instance version: %u.%u.%u",
          VK_VERSION_MAJOR(api_version),
          VK_VERSION_MINOR(api_version),
          VK_VERSION_PATCH(api_version));

    uint32_t ext_count = 0;
    const char **exts = NULL;

    if (VK_VERSION_MINOR(api_version) < 1) {
        ext_count = sizeof(instance_extensions) / sizeof(instance_extensions[0]);
        exts = instance_extensions;
    }

    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };

    VkResult res = vkCreateInstance(&ci, NULL, &comp->instance);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateInstance failed: %d", res);
        return -1;
    }
    LOG_I("VkInstance created");
    return 0;
}

/* ── Physical device selection ────────────────────────────────── */

static int select_physical_device(sbs_compositor_t *comp)
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(comp->instance, &count, NULL);
    if (count == 0) {
        LOG_E("no Vulkan physical devices found");
        return -1;
    }

    VkPhysicalDevice *devices = malloc(count * sizeof(VkPhysicalDevice));
    if (!devices)
        return -1;
    vkEnumeratePhysicalDevices(comp->instance, &count, devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    uint32_t selected_queue = UINT32_MAX;
    uint32_t selected_timestamp_valid_bits = 0;

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        LOG_I("GPU[%u]: %s (api %u.%u.%u)", i, props.deviceName,
              VK_VERSION_MAJOR(props.apiVersion),
              VK_VERSION_MINOR(props.apiVersion),
              VK_VERSION_PATCH(props.apiVersion));

        uint32_t qf_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qf_count, NULL);
        VkQueueFamilyProperties *qf = malloc(qf_count * sizeof(VkQueueFamilyProperties));
        if (!qf)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qf_count, qf);

        for (uint32_t j = 0; j < qf_count; j++) {
            if (qf[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                if (selected == VK_NULL_HANDLE ||
                    props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                    selected = devices[i];
                    selected_queue = j;
                    selected_timestamp_valid_bits = qf[j].timestampValidBits;
                }
                break;
            }
        }
        free(qf);
    }
    free(devices);

    if (selected == VK_NULL_HANDLE) {
        LOG_E("no suitable GPU found");
        return -1;
    }

    comp->physical_device = selected;
    comp->queue_family = selected_queue;
    vkGetPhysicalDeviceMemoryProperties(selected, &comp->mem_props);

    VkPhysicalDeviceProperties sel_props;
    vkGetPhysicalDeviceProperties(selected, &sel_props);
    comp->timestamp_period_ns = sel_props.limits.timestampPeriod;
    comp->timestamp_valid_bits = selected_timestamp_valid_bits;
    LOG_I("selected GPU: %s (queue family %u, timestampPeriod=%.3fns validBits=%u)",
          sel_props.deviceName, selected_queue,
          (double)comp->timestamp_period_ns, comp->timestamp_valid_bits);
    return 0;
}

/* ── Logical device ───────────────────────────────────────────── */

static int create_device(sbs_compositor_t *comp)
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = comp->queue_family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(comp->physical_device, NULL, &ext_count, NULL);
    VkExtensionProperties *exts = malloc(ext_count * sizeof(VkExtensionProperties));
    if (!exts)
        return -1;
    vkEnumerateDeviceExtensionProperties(comp->physical_device, NULL, &ext_count, exts);

    const char *enabled[8];
    uint32_t enabled_count = 0;
    uint32_t wanted_count = sizeof(device_extensions) / sizeof(device_extensions[0]);

    bool missing_required_ext = false;
    for (uint32_t i = 0; i < wanted_count && enabled_count < 8; i++) {
        bool found = false;
        for (uint32_t j = 0; j < ext_count; j++) {
            if (strcmp(device_extensions[i], exts[j].extensionName) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            enabled[enabled_count++] = device_extensions[i];
            LOG_I("device ext: %s (available)", device_extensions[i]);
        } else {
            if (device_extension_required(device_extensions[i])) {
                LOG_E("required device ext: %s (NOT available)", device_extensions[i]);
                missing_required_ext = true;
            } else {
                LOG_W("device ext: %s (NOT available)", device_extensions[i]);
            }
        }
    }
    free(exts);
    if (missing_required_ext)
        return -1;

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr_features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .samplerYcbcrConversion = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 features2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &ycbcr_features,
    };
    vkGetPhysicalDeviceFeatures2(comp->physical_device, &features2);
    if (ycbcr_features.samplerYcbcrConversion) {
        LOG_I("samplerYcbcrConversion supported");
    } else {
        LOG_W("samplerYcbcrConversion NOT supported");
    }

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &features2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = enabled_count,
        .ppEnabledExtensionNames = enabled,
    };

    VkResult res = vkCreateDevice(comp->physical_device, &dci, NULL, &comp->device);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateDevice failed: %d", res);
        return -1;
    }

    vkGetDeviceQueue(comp->device, comp->queue_family, 0, &comp->graphics_queue);
    LOG_I("VkDevice created, graphics queue acquired");
    return 0;
}

/* ── Command pool ─────────────────────────────────────────────── */

static int create_command_pool(sbs_compositor_t *comp)
{
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = comp->queue_family,
    };

    VkResult res = vkCreateCommandPool(comp->device, &cpci, NULL, &comp->command_pool);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateCommandPool failed: %d", res);
        return -1;
    }

    /* Allocate legacy RGB target command buffers, native program/preview
     * canvas command buffers, and one upload command buffer. */
    const uint32_t cmd_count = SBS_RENDER_TARGET_COUNT +
                               SBS_NATIVE_TOTAL_ENTRY_COUNT + 1;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = comp->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = cmd_count,
    };
    VkCommandBuffer cmd_buffers[SBS_RENDER_TARGET_COUNT + SBS_NATIVE_TOTAL_ENTRY_COUNT + 1];
    res = vkAllocateCommandBuffers(comp->device, &cbai, cmd_buffers);
    if (res != VK_SUCCESS) {
        LOG_E("vkAllocateCommandBuffers failed: %d", res);
        return -1;
    }
    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++)
        comp->targets[i].cmd_buffer = cmd_buffers[i];
    for (int i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        comp->native_canvas.entries[i].cmd_buffer =
            cmd_buffers[SBS_RENDER_TARGET_COUNT + i];
    for (int i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        comp->native_preview.entries[i].cmd_buffer =
            cmd_buffers[SBS_RENDER_TARGET_COUNT + SBS_NATIVE_CANVAS_RING_SIZE + i];
    comp->upload_cb = cmd_buffers[SBS_RENDER_TARGET_COUNT + SBS_NATIVE_TOTAL_ENTRY_COUNT];

    /* Upload fence */
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    res = vkCreateFence(comp->device, &fci, NULL, &comp->upload_fence);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateFence (upload) failed: %d", res);
        return -1;
    }

    /* Export fence — used by sbs_compositor_export_target_fd() */
    res = vkCreateFence(comp->device, &fci, NULL, &comp->export_fence);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateFence (export) failed: %d", res);
        return -1;
    }

    LOG_I("command pool, %u RGB + %u native + %u preview + 1 upload command buffers created",
          SBS_RENDER_TARGET_COUNT, SBS_NATIVE_CANVAS_RING_SIZE,
          SBS_NATIVE_CANVAS_RING_SIZE);
    return 0;
}

static int create_native_timing_query_pool(sbs_compositor_t *comp)
{
    const char *env = getenv("SBS_NATIVE_GPU_TIMING");
    if (!env || env[0] != '1')
        return 0;
    const char *detail_env = getenv("SBS_NATIVE_GPU_TIMING_DETAIL");
    comp->native_timing_detail = detail_env && detail_env[0] == '1';

    if (!comp || comp->timestamp_valid_bits == 0 ||
        comp->timestamp_period_ns <= 0.0f) {
        LOG_I("native GPU timing disabled: timestamps unsupported");
        return 0;
    }

    VkQueryPoolCreateInfo qpci = {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_TIMING_QUERY_MARKS,
    };
    VkResult res = vkCreateQueryPool(comp->device, &qpci, NULL,
                                     &comp->native_timing_query_pool);
    if (res != VK_SUCCESS) {
        LOG_W("native GPU timing query pool unavailable: %d", res);
        comp->native_timing_query_pool = VK_NULL_HANDLE;
        comp->native_timing_available = false;
        return 0;
    }

    comp->native_timing_available = true;
    LOG_I("native GPU timing enabled: queries=%u period=%.3fns validBits=%u detail=%s",
          qpci.queryCount, (double)comp->timestamp_period_ns,
          comp->timestamp_valid_bits,
          comp->native_timing_detail ? "yes" : "no");
    return 0;
}

/* ── Helper: find memory type ─────────────────────────────────── */

static uint32_t find_memory_type(sbs_compositor_t *comp, uint32_t type_bits,
                                  VkMemoryPropertyFlags props)
{
    for (uint32_t i = 0; i < comp->mem_props.memoryTypeCount; i++) {
        if ((type_bits & (1 << i)) &&
            (comp->mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static int check_native_yuv_plane_format(sbs_compositor_t *comp,
                                         const char *label,
                                         VkFormat format,
                                         uint64_t modifier,
                                         VkImageUsageFlags usage,
                                         VkFormatFeatureFlags required_features)
{
    VkDrmFormatModifierPropertiesListEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 fmt_props2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &mod_list,
    };

    vkGetPhysicalDeviceFormatProperties2(comp->physical_device, format, &fmt_props2);
    if (mod_list.drmFormatModifierCount == 0) {
        LOG_E("native YUV %s format %s has no DRM modifier support",
              label, format_name(format));
        return -1;
    }

    VkDrmFormatModifierPropertiesEXT *mods = calloc(mod_list.drmFormatModifierCount,
                                                     sizeof(*mods));
    if (!mods)
        return -1;

    mod_list.pDrmFormatModifierProperties = mods;
    vkGetPhysicalDeviceFormatProperties2(comp->physical_device, format, &fmt_props2);

    bool found_modifier = false;
    VkFormatFeatureFlags features = 0;
    for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++) {
        if (mods[i].drmFormatModifier != modifier)
            continue;
        if (mods[i].drmFormatModifierPlaneCount != 1) {
            LOG_E("native YUV %s format %s modifier 0x%lx has %u planes, expected 1",
                  label, format_name(format), (unsigned long)modifier,
                  mods[i].drmFormatModifierPlaneCount);
            free(mods);
            return -1;
        }
        features = mods[i].drmFormatModifierTilingFeatures;
        found_modifier = true;
        break;
    }
    free(mods);

    if (!found_modifier) {
        LOG_E("native YUV %s format %s missing linear DRM modifier support",
              label, format_name(format));
        return -1;
    }
    if ((features & required_features) != required_features) {
        LOG_E("native YUV %s format %s missing features: have=0x%x need=0x%x",
              label, format_name(format), (unsigned)features,
              (unsigned)required_features);
        return -1;
    }

    VkPhysicalDeviceExternalImageFormatInfo ext_img_fmt = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &ext_img_fmt,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkPhysicalDeviceImageFormatInfo2 fmt_info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &drm_mod_query,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = usage,
    };
    VkExternalImageFormatProperties ext_props = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 img_props = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &ext_props,
    };

    VkResult res = vkGetPhysicalDeviceImageFormatProperties2(comp->physical_device,
                                                              &fmt_info,
                                                              &img_props);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV %s format %s external image query failed: %d",
              label, format_name(format), res);
        return -1;
    }

    VkExternalMemoryFeatureFlags ext_features =
        ext_props.externalMemoryProperties.externalMemoryFeatures;
    if (!(ext_features & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        LOG_E("native YUV %s format %s is not DMA-BUF importable (features=0x%x)",
              label, format_name(format), (unsigned)ext_features);
        return -1;
    }
    if (img_props.imageFormatProperties.maxExtent.width < comp->width ||
        img_props.imageFormatProperties.maxExtent.height < comp->height) {
        LOG_E("native YUV %s format %s max extent %ux%u is smaller than canvas %ux%u",
              label, format_name(format),
              img_props.imageFormatProperties.maxExtent.width,
              img_props.imageFormatProperties.maxExtent.height,
              comp->width, comp->height);
        return -1;
    }

    LOG_I("native YUV %s format=%s modifier=0x%lx features=0x%x external=0x%x max=%ux%u",
          label, format_name(format), (unsigned long)modifier,
          (unsigned)features, (unsigned)ext_features,
          img_props.imageFormatProperties.maxExtent.width,
          img_props.imageFormatProperties.maxExtent.height);
    return 0;
}

static VkDeviceSize native_canvas_align_stride(VkDeviceSize stride)
{
    const VkDeviceSize alignment = 64u;
    return (stride + alignment - 1u) & ~(alignment - 1u);
}

static int check_native_yuv_capabilities(sbs_compositor_t *comp)
{
    if ((comp->width & 1u) || (comp->height & 1u)) {
        LOG_E("native YUV canvas requires even dimensions, got %ux%u",
              comp->width, comp->height);
        return -1;
    }

    const uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    struct {
        const char *label;
        VkFormat format;
    } planes[] = {
        { "SDR Y",  VK_FORMAT_R8_UNORM },
        { "SDR UV", VK_FORMAT_R8G8_UNORM },
        { "HDR Y",  VK_FORMAT_R16_UNORM },
        { "HDR UV", VK_FORMAT_R16G16_UNORM },
    };

    for (uint32_t i = 0; i < sizeof(planes) / sizeof(planes[0]); i++) {
        if (check_native_yuv_plane_format(comp, planes[i].label, planes[i].format,
                                          modifier, usage, features) != 0) {
            LOG_E("native YUV capability gate failed at %s", planes[i].label);
            return -1;
        }
    }

    comp->native_yuv.available = true;
    comp->native_yuv.drm_modifier = modifier;
    comp->native_yuv.sdr_y_format = VK_FORMAT_R8_UNORM;
    comp->native_yuv.sdr_uv_format = VK_FORMAT_R8G8_UNORM;
    comp->native_yuv.hdr_y_format = VK_FORMAT_R16_UNORM;
    comp->native_yuv.hdr_uv_format = VK_FORMAT_R16G16_UNORM;

    comp->native_yuv.sdr_y_stride = native_canvas_align_stride(comp->width);
    comp->native_yuv.sdr_uv_stride = comp->native_yuv.sdr_y_stride;
    comp->native_yuv.sdr_uv_offset = comp->native_yuv.sdr_y_stride * comp->height;
    comp->native_yuv.sdr_total_size = comp->native_yuv.sdr_uv_offset +
        comp->native_yuv.sdr_uv_stride * (comp->height / 2u);

    comp->native_yuv.hdr_y_stride = native_canvas_align_stride((VkDeviceSize)comp->width * 2u);
    comp->native_yuv.hdr_uv_stride = comp->native_yuv.hdr_y_stride;
    comp->native_yuv.hdr_uv_offset = comp->native_yuv.hdr_y_stride * comp->height;
    comp->native_yuv.hdr_total_size = comp->native_yuv.hdr_uv_offset +
        comp->native_yuv.hdr_uv_stride * comp->height;

    LOG_I("native YUV SDR layout: Y=%s stride=%lu UV=%s stride=%lu uv_offset=%lu total=%lu modifier=0x%lx",
          format_name(comp->native_yuv.sdr_y_format),
          (unsigned long)comp->native_yuv.sdr_y_stride,
          format_name(comp->native_yuv.sdr_uv_format),
          (unsigned long)comp->native_yuv.sdr_uv_stride,
          (unsigned long)comp->native_yuv.sdr_uv_offset,
          (unsigned long)comp->native_yuv.sdr_total_size,
          (unsigned long)comp->native_yuv.drm_modifier);
    LOG_I("native YUV HDR10 layout: Y=%s stride=%lu UV=%s stride=%lu uv_offset=%lu total=%lu modifier=0x%lx",
          format_name(comp->native_yuv.hdr_y_format),
          (unsigned long)comp->native_yuv.hdr_y_stride,
          format_name(comp->native_yuv.hdr_uv_format),
          (unsigned long)comp->native_yuv.hdr_uv_stride,
          (unsigned long)comp->native_yuv.hdr_uv_offset,
          (unsigned long)comp->native_yuv.hdr_total_size,
          (unsigned long)comp->native_yuv.drm_modifier);
    return 0;
}

static void destroy_native_canvas_entry(sbs_compositor_t *comp,
                                        sbs_native_canvas_entry_t *entry)
{
    VkCommandBuffer cmd_buffer;

    if (!comp || !comp->device || !entry)
        return;

    cmd_buffer = entry->cmd_buffer;

    if (entry->fence)
        vkDestroyFence(comp->device, entry->fence, NULL);
    if (entry->y.view)
        vkDestroyImageView(comp->device, entry->y.view, NULL);
    if (entry->uv.view)
        vkDestroyImageView(comp->device, entry->uv.view, NULL);
    if (entry->y.image)
        vkDestroyImage(comp->device, entry->y.image, NULL);
    if (entry->uv.image)
        vkDestroyImage(comp->device, entry->uv.image, NULL);
    if (entry->y.memory && entry->y.memory != entry->uv.memory)
        vkFreeMemory(comp->device, entry->y.memory, NULL);
    if (entry->uv.memory)
        vkFreeMemory(comp->device, entry->uv.memory, NULL);
    if (entry->backing_fd >= 0)
        close(entry->backing_fd);
    if (entry->backing_mapped && entry->backing_size > 0)
        munmap(entry->backing_mapped, entry->backing_size);
    if (entry->encoder_memory && entry->encoder_mapped)
        vkUnmapMemory(comp->device, entry->encoder_memory);
    if (entry->encoder_buffer)
        vkDestroyBuffer(comp->device, entry->encoder_buffer, NULL);
    if (entry->encoder_memory)
        vkFreeMemory(comp->device, entry->encoder_memory, NULL);

    memset(entry, 0, sizeof(*entry));
    entry->cmd_buffer = cmd_buffer;
    entry->backing_fd = -1;
    entry->backing_mapped = NULL;
    entry->encoder_buffer = VK_NULL_HANDLE;
    entry->encoder_memory = VK_NULL_HANDLE;
    entry->encoder_mapped = NULL;
    entry->encoder_size = 0;
    entry->y.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    entry->uv.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    atomic_store_explicit(&entry->refcount, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                          memory_order_relaxed);
}

static void destroy_native_canvas_ring(sbs_compositor_t *comp)
{
    if (!comp)
        return;

    if (comp->native_canvas.mailboxes_initialized) {
        for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_MAILBOX_COUNT; i++) {
            sbs_native_canvas_mailbox_t *mailbox = &comp->native_canvas.mailboxes[i];
            pthread_mutex_lock(&mailbox->lock);
            while (mailbox->count > 0) {
                uint32_t entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
                mailbox->read_idx = (mailbox->read_idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
                mailbox->count--;
                sbs_compositor_native_canvas_unref(comp, entry_idx);
            }
            mailbox->has_entry = false;
            pthread_mutex_unlock(&mailbox->lock);
            pthread_mutex_destroy(&mailbox->lock);
        }
    }

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        destroy_native_canvas_entry(comp, &comp->native_canvas.entries[i]);
    memset(&comp->native_canvas, 0, sizeof(comp->native_canvas));
}

static int allocate_native_canvas_entry(sbs_compositor_t *comp,
                                        sbs_native_canvas_entry_t *entry,
                                        uint32_t index,
                                        sbs_export_color_mode_t color_mode,
                                        uint32_t width,
                                        uint32_t height);
static int create_native_encoder_buffer(sbs_compositor_t *comp,
                                        sbs_native_canvas_entry_t *entry,
                                        VkDeviceSize size);
static const char *native_canvas_color_name(sbs_export_color_mode_t color_mode);

static void wait_native_preview_refs_released(sbs_compositor_t *comp)
{
    if (!comp)
        return;

    for (uint32_t attempt = 0; attempt < 500; attempt++) {
        bool busy = false;
        for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
            sbs_native_canvas_entry_t *entry = &comp->native_preview.entries[i];
            if (atomic_load_explicit(&entry->refcount, memory_order_acquire) > 0) {
                busy = true;
                break;
            }
        }
        if (!busy)
            return;
        usleep(1000);
    }

    LOG_W("native preview ring teardown timed out waiting for leased entries");
}

static void destroy_native_preview_ring(sbs_compositor_t *comp)
{
    if (!comp)
        return;

    if (comp->native_canvas.mailboxes_initialized) {
        sbs_native_canvas_mailbox_t *mailbox =
            &comp->native_canvas.mailboxes[SBS_NATIVE_CANVAS_MAILBOX_PREVIEW];
        pthread_mutex_lock(&mailbox->lock);
        while (mailbox->count > 0) {
            uint32_t entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
            mailbox->read_idx = (mailbox->read_idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
            mailbox->count--;
            sbs_compositor_native_canvas_unref(comp, entry_idx);
        }
        mailbox->has_entry = false;
        pthread_mutex_unlock(&mailbox->lock);
    }

    wait_native_preview_refs_released(comp);

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        destroy_native_canvas_entry(comp, &comp->native_preview.entries[i]);

    comp->native_preview.initialized = false;
    comp->native_preview.width = 0;
    comp->native_preview.height = 0;
    comp->native_preview.frame_interval = 0;
    comp->native_preview.last_submit_frame = 0;
    comp->native_preview.color_mode = SBS_EXPORT_COLOR_SDR;
    comp->native_preview.write_idx = 0;
    atomic_store_explicit(&comp->native_preview.last_rendered_entry, UINT32_MAX,
                          memory_order_relaxed);
}

static int create_native_preview_ring(sbs_compositor_t *comp,
                                      uint32_t width,
                                      uint32_t height,
                                      uint32_t frame_interval,
                                      sbs_export_color_mode_t color_mode)
{
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    if (!comp || !comp->native_canvas.initialized || !comp->native_yuv.available)
        return -1;
    if (width == 0 || height == 0 || (width & 1u) || (height & 1u)) {
        LOG_E("native preview requires even dimensions, got %ux%u", width, height);
        return -1;
    }

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        sbs_native_canvas_entry_t *entry = &comp->native_preview.entries[i];
        VkCommandBuffer cmd_buffer = entry->cmd_buffer;
        memset(entry, 0, sizeof(*entry));
        entry->cmd_buffer = cmd_buffer;
        entry->backing_fd = -1;
        entry->drm_modifier = comp->native_yuv.drm_modifier;
        entry->y.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        entry->uv.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        atomic_store_explicit(&entry->refcount, 0, memory_order_relaxed);
        atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                              memory_order_relaxed);

        VkResult res = vkCreateFence(comp->device, &fci, NULL, &entry->fence);
        if (res != VK_SUCCESS) {
            LOG_E("vkCreateFence native preview[%u] failed: %d", i, res);
            destroy_native_preview_ring(comp);
            return -1;
        }

        if (allocate_native_canvas_entry(comp, entry, i,
                                         color_mode,
                                         width, height) != 0) {
            destroy_native_preview_ring(comp);
            return -1;
        }
        if (color_mode != SBS_EXPORT_COLOR_HDR10 && !entry->encoder_mapped &&
            create_native_encoder_buffer(comp, entry, entry->backing_size) != 0) {
            LOG_E("native preview[%u] host buffer allocation failed", i);
            destroy_native_preview_ring(comp);
            return -1;
        }
    }

    comp->native_preview.width = width;
    comp->native_preview.height = height;
    comp->native_preview.frame_interval = frame_interval > 0 ? frame_interval : 1;
    comp->native_preview.last_submit_frame = 0;
    comp->native_preview.color_mode = color_mode;
    comp->native_preview.write_idx = 0;
    atomic_store_explicit(&comp->native_preview.last_rendered_entry, UINT32_MAX,
                          memory_order_relaxed);
    comp->native_preview.initialized = true;
    LOG_I("native preview GPU ring initialized: %ux%u mode=%s entries=%u interval=%u",
          width, height, native_canvas_color_name(color_mode),
          SBS_NATIVE_CANVAS_RING_SIZE,
          comp->native_preview.frame_interval);
    return 0;
}

static sbs_export_color_mode_t native_preview_desired_color_mode(const sbs_compositor_t *comp)
{
    if (!comp || !comp->native_canvas.initialized ||
        !comp->native_canvas.entries[0].allocated)
        return SBS_EXPORT_COLOR_SDR;
    return comp->native_canvas.entries[0].color_mode;
}

static int create_native_canvas_ring(sbs_compositor_t *comp)
{
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };

    if (!comp->native_yuv.available) {
        LOG_E("native canvas ring requires native YUV capabilities");
        return -1;
    }

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_MAILBOX_COUNT; i++) {
        sbs_native_canvas_mailbox_t *mailbox = &comp->native_canvas.mailboxes[i];
        pthread_mutex_init(&mailbox->lock, NULL);
        mailbox->has_entry = false;
        mailbox->entry_idx = 0;
        mailbox->frame_number = 0;
        memset(mailbox->entries, 0, sizeof(mailbox->entries));
        mailbox->read_idx = 0;
        mailbox->write_idx = 0;
        mailbox->count = 0;
        mailbox->published = 0;
        mailbox->dropped = 0;
        mailbox->consumed = 0;
        mailbox->late = 0;
    }
    comp->native_canvas.mailboxes_initialized = true;

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[i];
        memset(entry, 0, sizeof(*entry));
        entry->backing_fd = -1;
        entry->drm_modifier = comp->native_yuv.drm_modifier;
        entry->y.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        entry->uv.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        atomic_store_explicit(&entry->refcount, 0, memory_order_relaxed);
        atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                              memory_order_relaxed);

        VkResult res = vkCreateFence(comp->device, &fci, NULL, &entry->fence);
        if (res != VK_SUCCESS) {
            LOG_E("vkCreateFence native canvas[%u] failed: %d", i, res);
            destroy_native_canvas_ring(comp);
            return -1;
        }
    }

    comp->native_canvas.write_idx = 0;
    atomic_store_explicit(&comp->native_canvas.current_entry, 0,
                          memory_order_relaxed);
    atomic_store_explicit(&comp->native_canvas.last_rendered_entry, 0,
                          memory_order_relaxed);
    comp->native_canvas.initialized = true;
    LOG_I("native canvas ring initialized (%u entries)",
          SBS_NATIVE_CANVAS_RING_SIZE);
    return 0;
}

static VkImageUsageFlags native_canvas_plane_usage(void)
{
    return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
           VK_IMAGE_USAGE_SAMPLED_BIT |
           VK_IMAGE_USAGE_STORAGE_BIT |
           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

static int create_native_canvas_plane(sbs_compositor_t *comp,
                                      sbs_native_canvas_entry_t *entry,
                                      sbs_native_canvas_plane_t *plane,
                                      const char *label,
                                      VkFormat format,
                                      uint32_t width,
                                      uint32_t height,
                                      VkDeviceSize offset,
                                      VkDeviceSize stride)
{
    VkSubresourceLayout plane_layout = {
        .offset = 0,
        .size = 0,
        .rowPitch = stride,
        .arrayPitch = 0,
        .depthPitch = 0,
    };
    VkImageDrmFormatModifierExplicitCreateInfoEXT drm_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = entry->drm_modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane_layout,
    };
    VkExternalMemoryImageCreateInfo ext_ci = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm_ci,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &ext_ci,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = native_canvas_plane_usage(),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = vkCreateImage(comp->device, &ici, NULL, &plane->image);
    if (res != VK_SUCCESS) {
        LOG_E("native canvas %s image create failed: %d", label, res);
        return -1;
    }

    VkMemoryDedicatedRequirements ded_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 mem_req2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &ded_reqs,
    };
    VkImageMemoryRequirementsInfo2 img_mem_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = plane->image,
    };
    vkGetImageMemoryRequirements2(comp->device, &img_mem_info, &mem_req2);

    if ((offset % mem_req2.memoryRequirements.alignment) != 0) {
        LOG_E("native canvas %s offset %lu is not aligned to %lu",
              label,
              (unsigned long)offset,
              (unsigned long)mem_req2.memoryRequirements.alignment);
        return -1;
    }
    if (offset + mem_req2.memoryRequirements.size > entry->backing_size) {
        LOG_E("native canvas %s image requires [%lu,%lu), backing has %lu",
              label,
              (unsigned long)offset,
              (unsigned long)(offset + mem_req2.memoryRequirements.size),
              (unsigned long)entry->backing_size);
        return -1;
    }

    uint32_t mem_type = find_memory_type(comp, mem_req2.memoryRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        mem_type = find_memory_type(comp, mem_req2.memoryRequirements.memoryTypeBits, 0);
    if (mem_type == UINT32_MAX) {
        LOG_E("native canvas %s image has no compatible memory type", label);
        return -1;
    }

    int import_fd = dup(entry->backing_fd);
    if (import_fd < 0) {
        LOG_E("native canvas %s DMA-BUF dup failed: %s", label, strerror(errno));
        return -1;
    }

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = import_fd,
    };
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = &import_info,
        .image = plane->image,
    };
    if (ded_reqs.requiresDedicatedAllocation && offset != 0) {
        LOG_E("native canvas %s requires dedicated allocation but uses backing offset %lu",
              label, (unsigned long)offset);
        close(import_fd);
        return -1;
    }
    bool use_dedicated = ded_reqs.requiresDedicatedAllocation ||
                         (ded_reqs.prefersDedicatedAllocation && offset == 0);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = use_dedicated ? (void *)&dedicated : (void *)&import_info,
        .allocationSize = entry->backing_size,
        .memoryTypeIndex = mem_type,
    };

    res = vkAllocateMemory(comp->device, &mai, NULL, &plane->memory);
    if (res != VK_SUCCESS) {
        close(import_fd);
        LOG_E("native canvas %s memory import failed: %d", label, res);
        return -1;
    }

    res = vkBindImageMemory(comp->device, plane->image, plane->memory, offset);
    if (res != VK_SUCCESS) {
        LOG_E("native canvas %s bind failed: %d", label, res);
        return -1;
    }

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = plane->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    res = vkCreateImageView(comp->device, &ivci, NULL, &plane->view);
    if (res != VK_SUCCESS) {
        LOG_E("native canvas %s view create failed: %d", label, res);
        return -1;
    }

    plane->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    plane->format = format;
    plane->width = width;
    plane->height = height;
    plane->offset = offset;
    plane->stride = stride;
    return 0;
}

static int create_native_encoder_buffer(sbs_compositor_t *comp,
                                        sbs_native_canvas_entry_t *entry,
                                        VkDeviceSize size)
{
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkResult res = vkCreateBuffer(comp->device, &bci, NULL, &entry->encoder_buffer);
    if (res != VK_SUCCESS) {
        LOG_W("native encoder buffer create failed: %d", res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(comp->device, entry->encoder_buffer, &mem_req);
    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mem_type == UINT32_MAX) {
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (mem_type == UINT32_MAX) {
        LOG_W("native encoder buffer has no host-visible memory type");
        return -1;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    res = vkAllocateMemory(comp->device, &mai, NULL, &entry->encoder_memory);
    if (res != VK_SUCCESS) {
        LOG_W("native encoder buffer memory alloc failed: %d", res);
        return -1;
    }
    res = vkBindBufferMemory(comp->device, entry->encoder_buffer,
                             entry->encoder_memory, 0);
    if (res != VK_SUCCESS) {
        LOG_W("native encoder buffer bind failed: %d", res);
        return -1;
    }
    res = vkMapMemory(comp->device, entry->encoder_memory, 0, size, 0,
                      &entry->encoder_mapped);
    if (res != VK_SUCCESS) {
        LOG_W("native encoder buffer map failed: %d", res);
        entry->encoder_mapped = NULL;
        return -1;
    }
    entry->encoder_size = size;
    memset(entry->encoder_mapped, 0, (size_t)size);
    LOG_I("native encoder host buffer size=%lu mem_type=%u flags=0x%x",
          (unsigned long)size, mem_type,
          comp->mem_props.memoryTypes[mem_type].propertyFlags);
    return 0;
}

static const char *native_canvas_color_name(sbs_export_color_mode_t color_mode)
{
    return color_mode == SBS_EXPORT_COLOR_HDR10 ? "HDR10" : "SDR";
}

typedef struct sbs_native_canvas_layout {
    uint32_t width;
    uint32_t height;
    uint32_t uv_width;
    uint32_t uv_height;
    VkFormat y_format;
    VkFormat uv_format;
    VkDeviceSize y_stride;
    VkDeviceSize uv_stride;
    VkDeviceSize uv_offset;
    VkDeviceSize total_size;
} sbs_native_canvas_layout_t;

static int native_canvas_compute_layout(const sbs_compositor_t *comp,
                                        uint32_t width,
                                        uint32_t height,
                                        sbs_export_color_mode_t color_mode,
                                        sbs_native_canvas_layout_t *layout)
{
    bool hdr10 = color_mode == SBS_EXPORT_COLOR_HDR10;

    if (!comp || !layout || width == 0 || height == 0 ||
        (width & 1u) || (height & 1u)) {
        return -1;
    }

    memset(layout, 0, sizeof(*layout));
    layout->width = width;
    layout->height = height;
    layout->uv_width = width / 2u;
    layout->uv_height = height / 2u;
    layout->y_format = hdr10 ? comp->native_yuv.hdr_y_format
                             : comp->native_yuv.sdr_y_format;
    layout->uv_format = hdr10 ? comp->native_yuv.hdr_uv_format
                              : comp->native_yuv.sdr_uv_format;

    if (hdr10) {
        layout->y_stride = native_canvas_align_stride((VkDeviceSize)width * 2u);
        layout->uv_stride = layout->y_stride;
        layout->uv_offset = layout->y_stride * height;
        /* Preserve the existing contiguous P010 allocation contract used by
         * the Wave521 path: Y plus a full-height interleaved UV allocation. */
        layout->total_size = layout->uv_offset + layout->uv_stride * height;
    } else {
        layout->y_stride = native_canvas_align_stride(width);
        layout->uv_stride = layout->y_stride;
        layout->uv_offset = layout->y_stride * height;
        layout->total_size = layout->uv_offset +
            layout->uv_stride * (height / 2u);
    }

    return 0;
}

static int allocate_native_canvas_entry(sbs_compositor_t *comp,
                                        sbs_native_canvas_entry_t *entry,
                                        uint32_t index,
                                        sbs_export_color_mode_t color_mode,
                                        uint32_t width,
                                        uint32_t height)
{
    sbs_dmabuf_alloc_t alloc;
    sbs_dmabuf_buffer_t backing = { .fd = -1 };
    sbs_native_canvas_layout_t layout;
    const char *mode_name = native_canvas_color_name(color_mode);

    if (native_canvas_compute_layout(comp, width, height, color_mode,
                                     &layout) != 0) {
        LOG_E("native canvas %s[%u] invalid layout %ux%u",
              mode_name, index, width, height);
        return -1;
    }

    if (sbs_dmabuf_alloc_open(&alloc) != SBS_OK || !alloc.available) {
        LOG_E("native canvas %s requires a real DMA-BUF heap", mode_name);
        sbs_dmabuf_alloc_close(&alloc);
        return -1;
    }

    int alloc_rc = SBS_ERR_IO;
    if (color_mode == SBS_EXPORT_COLOR_HDR10) {
        alloc_rc = sbs_dmabuf_alloc_buffer_from_heap(
            &alloc, SBS_DMABUF_HEAP_CODECMM, layout.total_size, 0, &backing);
        if (alloc_rc != SBS_OK) {
            alloc_rc = sbs_dmabuf_alloc_buffer_from_heap(
                &alloc, SBS_DMABUF_HEAP_LINUX_CMA, layout.total_size, 0, &backing);
        }
        if (alloc_rc != SBS_OK) {
            alloc_rc = sbs_dmabuf_alloc_buffer_from_heap(
                &alloc, SBS_DMABUF_HEAP_GFX, layout.total_size, 0, &backing);
        }
        if (alloc_rc != SBS_OK) {
            alloc_rc = sbs_dmabuf_alloc_buffer_from_heap(
                &alloc, SBS_DMABUF_HEAP_SYSTEM, layout.total_size, 0, &backing);
        }
    } else {
        alloc_rc = sbs_dmabuf_alloc_buffer_from_heap(
            &alloc, SBS_DMABUF_HEAP_CODECMM, layout.total_size, 0, &backing);
    }
    if (alloc_rc != SBS_OK) {
        alloc_rc = sbs_dmabuf_alloc_buffer(&alloc, layout.total_size, 0, &backing);
    }
    if (alloc_rc != SBS_OK || backing.fd < 0) {
        LOG_E("native canvas %s[%u] backing allocation failed", mode_name, index);
        sbs_dmabuf_alloc_close(&alloc);
        return -1;
    }
    sbs_dmabuf_alloc_close(&alloc);

    if (backing.heap == SBS_DMABUF_HEAP_MEMFD) {
        LOG_E("native canvas %s[%u] refused memfd backing", mode_name, index);
        close(backing.fd);
        return -1;
    }

    entry->backing_fd = backing.fd;
    entry->backing_size = backing.size;
    if (backing.heap == SBS_DMABUF_HEAP_SYSTEM) {
        entry->backing_mapped = mmap(NULL, backing.size, PROT_READ,
                                     MAP_SHARED, backing.fd, 0);
        if (entry->backing_mapped == MAP_FAILED)
            entry->backing_mapped = NULL;
    }
    entry->drm_modifier = comp->native_yuv.drm_modifier;
    entry->color_mode = color_mode;

    char y_label[32];
    char uv_label[32];
    snprintf(y_label, sizeof(y_label), "%s Y", mode_name);
    snprintf(uv_label, sizeof(uv_label), "%s UV", mode_name);

    if (create_native_canvas_plane(comp, entry, &entry->y, y_label,
                                   layout.y_format,
                                   layout.width, layout.height,
                                   0, layout.y_stride) != 0)
        return -1;

    if (create_native_canvas_plane(comp, entry, &entry->uv, uv_label,
                                   layout.uv_format,
                                   layout.uv_width, layout.uv_height,
                                   layout.uv_offset, layout.uv_stride) != 0)
        return -1;

    if (getenv("SBS_NATIVE_ENCODER_HOST_BUFFER") &&
        create_native_encoder_buffer(comp, entry, layout.total_size) != 0) {
        LOG_W("native canvas %s[%u] encoder host buffer unavailable; encoder will use DMA-BUF path",
              mode_name, index);
    }

    entry->allocated = true;
    LOG_I("native canvas %s[%u] %ux%u fd=%d size=%lu y_stride=%lu uv_stride=%lu uv_offset=%lu",
           mode_name, index, width, height, entry->backing_fd,
           (unsigned long)entry->backing_size,
           (unsigned long)entry->y.stride,
           (unsigned long)entry->uv.stride,
          (unsigned long)entry->uv.offset);
    return 0;
}

static int allocate_native_canvas_entries(sbs_compositor_t *comp,
                                          sbs_export_color_mode_t color_mode)
{
    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        if (allocate_native_canvas_entry(comp,
                                         &comp->native_canvas.entries[i],
                                         i, color_mode,
                                         comp->width, comp->height) != 0) {
            destroy_native_canvas_ring(comp);
            return -1;
        }
    }
    LOG_I("native canvas %s plane targets allocated (%u entries)",
          native_canvas_color_name(color_mode), SBS_NATIVE_CANVAS_RING_SIZE);
    return 0;
}

int sbs_compositor_configure_native_preview(sbs_compositor_t *comp,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t frame_interval)
{
    if (!comp || !comp->native_canvas.initialized)
        return -1;

    if (width == 0 || height == 0) {
        if (comp->native_preview.initialized) {
            vkDeviceWaitIdle(comp->device);
            destroy_native_preview_ring(comp);
            LOG_I("native preview GPU ring disabled");
        }
        return 0;
    }

    sbs_export_color_mode_t color_mode = native_preview_desired_color_mode(comp);

    if (comp->native_preview.initialized) {
        if (comp->native_preview.width == width &&
            comp->native_preview.height == height &&
            comp->native_preview.color_mode == color_mode) {
            uint32_t interval = frame_interval > 0 ? frame_interval : 1;
            if (comp->native_preview.frame_interval != interval) {
                comp->native_preview.frame_interval = interval;
                LOG_I("native preview GPU cadence updated: interval=%u", interval);
            }
            return 0;
        }
        vkDeviceWaitIdle(comp->device);
        destroy_native_preview_ring(comp);
    }

    return create_native_preview_ring(comp, width, height, frame_interval,
                                      color_mode);
}

int sbs_compositor_native_canvas_acquire(sbs_compositor_t *comp,
                                         uint32_t *entry_idx_out,
                                         sbs_native_canvas_entry_t **entry_out)
{
    if (!comp || !comp->native_canvas.initialized)
        return -1;

    for (uint32_t n = 0; n < SBS_NATIVE_CANVAS_RING_SIZE; n++) {
        uint32_t idx = (comp->native_canvas.write_idx + n) %
                       SBS_NATIVE_CANVAS_RING_SIZE;
        sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[idx];
        int state = atomic_load_explicit(&entry->state, memory_order_acquire);
        uint32_t refs = atomic_load_explicit(&entry->refcount, memory_order_acquire);

        if (!entry->allocated || refs != 0)
            continue;
        if (state != SBS_NATIVE_CANVAS_ENTRY_FREE &&
            state != SBS_NATIVE_CANVAS_ENTRY_READY)
            continue;

        VkResult res = vkGetFenceStatus(comp->device, entry->fence);
        if (res != VK_SUCCESS)
            continue;

        comp->native_canvas.write_idx = (idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
        atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_RENDERING,
                              memory_order_release);
        atomic_store_explicit(&entry->refcount, 0, memory_order_release);
        atomic_store_explicit(&comp->native_canvas.current_entry, idx,
                              memory_order_release);
        if (entry_idx_out)
            *entry_idx_out = idx;
        if (entry_out)
            *entry_out = entry;
        return 0;
    }

    return -1;
}

static int sbs_compositor_native_preview_acquire(sbs_compositor_t *comp,
                                                 uint32_t *entry_idx_out,
                                                 sbs_native_canvas_entry_t **entry_out)
{
    if (!comp || !comp->native_preview.initialized)
        return -1;

    for (uint32_t n = 0; n < SBS_NATIVE_CANVAS_RING_SIZE; n++) {
        uint32_t idx = (comp->native_preview.write_idx + n) %
                       SBS_NATIVE_CANVAS_RING_SIZE;
        sbs_native_canvas_entry_t *entry = &comp->native_preview.entries[idx];
        int state = atomic_load_explicit(&entry->state, memory_order_acquire);
        uint32_t refs = atomic_load_explicit(&entry->refcount, memory_order_acquire);

        if (!entry->allocated || refs != 0)
            continue;
        if (state != SBS_NATIVE_CANVAS_ENTRY_FREE &&
            state != SBS_NATIVE_CANVAS_ENTRY_READY)
            continue;
        if (vkGetFenceStatus(comp->device, entry->fence) != VK_SUCCESS)
            continue;

        comp->native_preview.write_idx = (idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
        atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_RENDERING,
                              memory_order_release);
        atomic_store_explicit(&entry->refcount, 0, memory_order_release);
        if (entry_idx_out)
            *entry_idx_out = idx;
        if (entry_out)
            *entry_out = entry;
        return 0;
    }

    return -1;
}

void sbs_compositor_native_canvas_mark_rendering(sbs_compositor_t *comp,
                                                 uint32_t entry_idx)
{
    if (!comp || entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE)
        return;
    sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[entry_idx];
    atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_RENDERING,
                          memory_order_release);
    atomic_store_explicit(&entry->refcount, 0, memory_order_release);
}

static void sbs_compositor_native_canvas_release_failed(sbs_compositor_t *comp,
                                                        uint32_t entry_idx)
{
    if (!comp || entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE)
        return;
    sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[entry_idx];
    entry->timing_query_valid = false;
    atomic_store_explicit(&entry->refcount, 0, memory_order_release);
    atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                          memory_order_release);
}

static bool sbs_compositor_native_canvas_mailbox_publish(sbs_compositor_t *comp,
                                                          sbs_native_canvas_mailbox_type_t type,
                                                          uint32_t entry_idx,
                                                          uint64_t frame_number);
static void sbs_compositor_native_canvas_poll_ready(sbs_compositor_t *comp);
static void sbs_compositor_native_preview_poll_ready(sbs_compositor_t *comp);
static int sbs_compositor_submit_native_preview_from_entry(sbs_compositor_t *comp,
                                                           sbs_native_canvas_entry_t *src_entry,
                                                           uint64_t frame_number,
                                                           uint64_t content_frame_number);

void sbs_compositor_native_canvas_mark_ready(sbs_compositor_t *comp,
                                             uint32_t entry_idx,
                                             uint64_t frame_number)
{
    if (!comp || entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE)
        return;
    sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[entry_idx];
    entry->frame_number = frame_number;
    atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_READY,
                          memory_order_release);
    atomic_store_explicit(&comp->native_canvas.last_rendered_entry, entry_idx,
                          memory_order_release);

    sbs_compositor_native_canvas_mailbox_publish(
        comp, SBS_NATIVE_CANVAS_MAILBOX_OUTPUT, entry_idx, frame_number);
    sbs_compositor_native_canvas_mailbox_publish(
        comp, SBS_NATIVE_CANVAS_MAILBOX_SNAPSHOT, entry_idx, frame_number);
}

static bool sbs_compositor_native_canvas_mailbox_publish(sbs_compositor_t *comp,
                                                           sbs_native_canvas_mailbox_type_t type,
                                                           uint32_t entry_idx,
                                                           uint64_t frame_number)
{
    uint32_t dropped_entry_idx = UINT32_MAX;
    uint32_t capacity = 1u;

    if (!comp || type >= SBS_NATIVE_CANVAS_MAILBOX_COUNT)
        return false;
    if (!sbs_compositor_native_canvas_ref(comp, entry_idx))
        return false;

    sbs_native_canvas_mailbox_t *mailbox = &comp->native_canvas.mailboxes[type];
    pthread_mutex_lock(&mailbox->lock);
    if (mailbox->count >= capacity) {
        dropped_entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
        mailbox->read_idx = (mailbox->read_idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
        mailbox->count--;
        mailbox->dropped++;
    }

    mailbox->entries[mailbox->write_idx].entry_idx = entry_idx;
    mailbox->entries[mailbox->write_idx].frame_number = frame_number;
    mailbox->write_idx = (mailbox->write_idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
    mailbox->count++;
    mailbox->has_entry = true;
    mailbox->entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
    mailbox->frame_number = mailbox->entries[mailbox->read_idx].frame_number;
    mailbox->published++;
    pthread_mutex_unlock(&mailbox->lock);

    if (dropped_entry_idx != UINT32_MAX)
        sbs_compositor_native_canvas_unref(comp, dropped_entry_idx);
    return true;
}

bool sbs_compositor_native_canvas_republish_last(sbs_compositor_t *comp,
                                                 uint64_t frame_number)
{
    if (!comp || !comp->native_canvas.initialized)
        return false;

    sbs_compositor_native_canvas_poll_ready(comp);
    sbs_compositor_native_preview_poll_ready(comp);

    uint32_t entry_idx = atomic_load_explicit(&comp->native_canvas.last_rendered_entry,
                                              memory_order_acquire);
    if (entry_idx >= SBS_NATIVE_CANVAS_RING_SIZE)
        return false;

    sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[entry_idx];
    if (atomic_load_explicit(&entry->state, memory_order_acquire) !=
        SBS_NATIVE_CANVAS_ENTRY_READY)
        return false;
    if (vkGetFenceStatus(comp->device, entry->fence) != VK_SUCCESS)
        return false;

    bool output = sbs_compositor_native_canvas_mailbox_publish(
        comp, SBS_NATIVE_CANVAS_MAILBOX_OUTPUT, entry_idx, frame_number);
    bool snapshot = sbs_compositor_native_canvas_mailbox_publish(
        comp, SBS_NATIVE_CANVAS_MAILBOX_SNAPSHOT, entry_idx, frame_number);
    bool preview = false;
    if (comp->native_preview.initialized) {
        uint32_t preview_idx = atomic_load_explicit(&comp->native_preview.last_rendered_entry,
                                                    memory_order_acquire);
        if (preview_idx >= SBS_NATIVE_CANVAS_RING_SIZE) {
            (void)sbs_compositor_submit_native_preview_from_entry(
                comp, entry, frame_number, entry->content_frame_number);
        }
        if (preview_idx < SBS_NATIVE_CANVAS_RING_SIZE) {
            sbs_native_canvas_entry_t *preview_entry = &comp->native_preview.entries[preview_idx];
            if (atomic_load_explicit(&preview_entry->state, memory_order_acquire) ==
                SBS_NATIVE_CANVAS_ENTRY_READY &&
                vkGetFenceStatus(comp->device, preview_entry->fence) == VK_SUCCESS) {
                preview = sbs_compositor_native_canvas_mailbox_publish(
                    comp, SBS_NATIVE_CANVAS_MAILBOX_PREVIEW,
                    SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE + preview_idx,
                    frame_number);
            }
        }
    }
    return output || snapshot || preview;
}

static sbs_native_canvas_entry_t *native_canvas_lookup_entry(sbs_compositor_t *comp,
                                                             uint32_t entry_idx)
{
    if (!comp)
        return NULL;
    if (entry_idx < SBS_NATIVE_CANVAS_RING_SIZE)
        return &comp->native_canvas.entries[entry_idx];

    if (entry_idx >= SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE &&
        entry_idx < SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE + SBS_NATIVE_CANVAS_RING_SIZE &&
        comp->native_preview.initialized) {
        return &comp->native_preview.entries[
            entry_idx - SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE];
    }

    return NULL;
}

bool sbs_compositor_native_canvas_ref(sbs_compositor_t *comp,
                                      uint32_t entry_idx)
{
    sbs_native_canvas_entry_t *entry = native_canvas_lookup_entry(comp, entry_idx);
    if (!entry)
        return false;
    if (atomic_load_explicit(&entry->state, memory_order_acquire) !=
        SBS_NATIVE_CANVAS_ENTRY_READY)
        return false;
    atomic_fetch_add_explicit(&entry->refcount, 1, memory_order_acq_rel);
    return true;
}

void sbs_compositor_native_canvas_unref(sbs_compositor_t *comp,
                                          uint32_t entry_idx)
{
    sbs_native_canvas_entry_t *entry = native_canvas_lookup_entry(comp, entry_idx);
    if (!entry)
        return;
    uint32_t prev = atomic_load_explicit(&entry->refcount, memory_order_acquire);
    while (prev > 0) {
        if (atomic_compare_exchange_weak_explicit(&entry->refcount, &prev, prev - 1u,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire))
            break;
    }
}

bool sbs_compositor_native_canvas_mailbox_acquire(sbs_compositor_t *comp,
                                                  sbs_native_canvas_mailbox_type_t type,
                                                  uint32_t *entry_idx_out,
                                                  uint64_t *frame_number_out,
                                                  sbs_native_canvas_entry_t **entry_out)
{
    if (!comp || type >= SBS_NATIVE_CANVAS_MAILBOX_COUNT)
        return false;

    sbs_native_canvas_mailbox_t *mailbox = &comp->native_canvas.mailboxes[type];
    pthread_mutex_lock(&mailbox->lock);
    if (mailbox->count == 0) {
        mailbox->has_entry = false;
        mailbox->late++;
        pthread_mutex_unlock(&mailbox->lock);
        return false;
    }

    uint32_t entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
    uint64_t frame_number = mailbox->entries[mailbox->read_idx].frame_number;
    mailbox->read_idx = (mailbox->read_idx + 1u) % SBS_NATIVE_CANVAS_RING_SIZE;
    mailbox->count--;
    mailbox->consumed++;
    mailbox->has_entry = mailbox->count > 0;
    if (mailbox->has_entry) {
        mailbox->entry_idx = mailbox->entries[mailbox->read_idx].entry_idx;
        mailbox->frame_number = mailbox->entries[mailbox->read_idx].frame_number;
    } else {
        mailbox->entry_idx = entry_idx;
        mailbox->frame_number = frame_number;
    }
    pthread_mutex_unlock(&mailbox->lock);

    sbs_native_canvas_entry_t *entry = native_canvas_lookup_entry(comp, entry_idx);
    if (!entry) {
        sbs_compositor_native_canvas_unref(comp, entry_idx);
        return false;
    }

    if (entry_idx_out)
        *entry_idx_out = entry_idx;
    if (frame_number_out)
        *frame_number_out = frame_number;
    if (entry_out)
        *entry_out = entry;
    return true;
}

void sbs_compositor_native_canvas_mailbox_release(sbs_compositor_t *comp,
                                                  uint32_t entry_idx)
{
    sbs_compositor_native_canvas_unref(comp, entry_idx);
}

static void native_timing_read_and_log(sbs_compositor_t *comp,
                                       uint32_t entry_idx,
                                       sbs_native_canvas_entry_t *entry)
{
    if (!comp || !entry || !entry->timing_query_valid ||
        !comp->native_timing_available ||
        comp->native_timing_query_pool == VK_NULL_HANDLE)
        return;

    uint32_t mark_count = entry->timing_mark_count;
    if (mark_count < 4u || mark_count > SBS_NATIVE_TIMING_QUERY_MARKS)
        mark_count = 4u;

    uint64_t ts[SBS_NATIVE_TIMING_QUERY_MARKS] = {0};
    VkResult res = vkGetQueryPoolResults(comp->device,
                                          comp->native_timing_query_pool,
                                          entry->timing_query_base,
                                          mark_count,
                                          sizeof(ts), ts, sizeof(ts[0]),
                                          VK_QUERY_RESULT_64_BIT);
    if (res != VK_SUCCESS) {
        entry->timing_query_valid = false;
        return;
    }

    entry->timing_query_valid = false;
    double period_ms = (double)comp->timestamp_period_ns / 1000000.0;
    uint32_t copy_idx = mark_count - 1u;
    uint32_t layers_done_idx = mark_count - 2u;
    double clear_ms = (double)(ts[1] - ts[0]) * period_ms;
    double layers_ms = (double)(ts[layers_done_idx] - ts[1]) * period_ms;
    double copy_ms = (double)(ts[copy_idx] - ts[layers_done_idx]) * period_ms;
    double total_ms = (double)(ts[copy_idx] - ts[0]) * period_ms;
    double age_ms = entry->submit_time_us > 0
        ? (double)(monotonic_us() - entry->submit_time_us) / 1000.0 : 0.0;
    bool preview = entry_idx >= SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE;

    static uint64_t count[2] = {0, 0};
    static double total_sum[2] = {0.0, 0.0};
    static double total_max[2] = {0.0, 0.0};
    uint32_t k = preview ? 1u : 0u;
    count[k]++;
    total_sum[k] += total_ms;
    if (total_ms > total_max[k])
        total_max[k] = total_ms;

    bool should_log = count[k] <= 5 || count[k] % 60 == 0 || total_ms > 25.0;
    if (should_log) {
        double avg_ms = total_sum[k] / (double)count[k];
        LOG_I("native GPU timing: kind=%s frame=%lu entry=%u total=%.2fms avg=%.2fms max=%.2fms clear=%.2fms layers=%.2fms copy=%.2fms poll_age=%.2fms",
              preview ? "preview" : "output",
              (unsigned long)entry->frame_number,
              entry_idx, total_ms, avg_ms, total_max[k],
              clear_ms, layers_ms, copy_ms, age_ms);
        if (comp->native_timing_detail && entry->timing_layer_count > 0 && mark_count > 4u) {
            uint32_t layer_marks = layers_done_idx > 2u ? layers_done_idx - 2u : 0u;
            uint32_t layer_count = entry->timing_layer_count < layer_marks
                ? entry->timing_layer_count : layer_marks;
            uint32_t prev = 1u;
            double layer_sum = 0.0;
            for (uint32_t i = 0; i < layer_count; i++) {
                uint32_t mark = 2u + i;
                double layer_ms = (double)(ts[mark] - ts[prev]) * period_ms;
                layer_sum += layer_ms;
                LOG_I("native GPU detail layer: kind=%s frame=%lu layer=%u time=%.2fms label=%s",
                      preview ? "preview" : "output",
                      (unsigned long)entry->frame_number,
                      i, layer_ms,
                      entry->timing_layer_labels[i][0]
                          ? entry->timing_layer_labels[i] : "?");
                prev = mark;
            }
            double post_ms = (double)(ts[layers_done_idx] - ts[prev]) * period_ms;
            LOG_I("native GPU detail summary: kind=%s frame=%lu clear=%.2fms layers_sum=%.2fms post=%.2fms copy=%.2fms total=%.2fms marks=%u",
                  preview ? "preview" : "output",
                  (unsigned long)entry->frame_number,
                  clear_ms, layer_sum, post_ms, copy_ms, total_ms, mark_count);
        }
    }
}

static void sbs_compositor_native_canvas_poll_ready(sbs_compositor_t *comp)
{
    if (!comp || !comp->native_canvas.initialized)
        return;

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        sbs_native_canvas_entry_t *entry = &comp->native_canvas.entries[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) !=
            SBS_NATIVE_CANVAS_ENTRY_RENDERING)
            continue;
        if (vkGetFenceStatus(comp->device, entry->fence) == VK_SUCCESS) {
            native_timing_read_and_log(comp, i, entry);
            sbs_compositor_native_canvas_mark_ready(comp, i, entry->frame_number);
        }
    }
}

static void sbs_compositor_native_preview_poll_ready(sbs_compositor_t *comp)
{
    if (!comp || !comp->native_preview.initialized)
        return;

    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        sbs_native_canvas_entry_t *entry = &comp->native_preview.entries[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) !=
            SBS_NATIVE_CANVAS_ENTRY_RENDERING)
            continue;
        if (vkGetFenceStatus(comp->device, entry->fence) != VK_SUCCESS)
            continue;

        native_timing_read_and_log(comp,
            SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE + i, entry);
        atomic_store_explicit(&entry->state, SBS_NATIVE_CANVAS_ENTRY_READY,
                              memory_order_release);
        atomic_store_explicit(&comp->native_preview.last_rendered_entry, i,
                              memory_order_release);
        sbs_compositor_native_canvas_mailbox_publish(
            comp, SBS_NATIVE_CANVAS_MAILBOX_PREVIEW,
            SBS_NATIVE_PREVIEW_ENTRY_INDEX_BASE + i,
            entry->frame_number);
    }
}

/* ── Compositor 3D LUT helpers ────────────────────────────────── */

static float clampf01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

static uint8_t unorm8(float v)
{
    v = clampf01(v);
    return (uint8_t)(v * 255.0f + 0.5f);
}

static uint16_t unorm16d(double v)
{
    if (v < 0.0)
        v = 0.0;
    if (v > 1.0)
        v = 1.0;
    return (uint16_t)(v * 65535.0 + 0.5);
}

static void bt2020_linear_to_bt709_linear(float r, float g, float b,
                                          float *r_out, float *g_out, float *b_out)
{
    if (r_out)
        *r_out = clampf01( 1.6605f * r - 0.5876f * g - 0.0728f * b);
    if (g_out)
        *g_out = clampf01(-0.1246f * r + 1.1329f * g - 0.0083f * b);
    if (b_out)
        *b_out = clampf01(-0.0182f * r - 0.1006f * g + 1.1187f * b);
}

static float pq_eotf_nits(float code)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float n = powf(clampf01(code), 1.0f / m2);
    float d = c2 - c3 * n;
    if (d <= 0.0f)
        return 10000.0f;
    return 10000.0f * powf(fmaxf((n - c1) / d, 0.0f), 1.0f / m1);
}

static float srgb_encode(float linear)
{
    linear = clampf01(linear);
    if (linear <= 0.0031308f)
        return linear * 12.92f;
    return 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

static void generate_hdr_to_sdr_lut(uint8_t *data, uint32_t size)
{
    uint32_t p = 0;

    for (uint32_t b = 0; b < size; b++) {
        float bf = (float)b / (float)(size - 1u);
        for (uint32_t g = 0; g < size; g++) {
            float gf = (float)g / (float)(size - 1u);
            for (uint32_t r = 0; r < size; r++) {
                float rf = (float)r / (float)(size - 1u);
                float rn = pq_eotf_nits(rf);
                float gn = pq_eotf_nits(gf);
                float bn = pq_eotf_nits(bf);
                float luma = rn * 0.2627f + gn * 0.6780f + bn * 0.0593f;
                float mapped_luma = 100.0f * luma / (luma + 100.0f);
                float scale = luma > 0.0001f ? mapped_luma / luma : 0.0f;
                float r709, g709, b709;

                bt2020_linear_to_bt709_linear((rn * scale) / 100.0f,
                                              (gn * scale) / 100.0f,
                                              (bn * scale) / 100.0f,
                                              &r709, &g709, &b709);

                data[p++] = unorm8(srgb_encode(r709));
                data[p++] = unorm8(srgb_encode(g709));
                data[p++] = unorm8(srgb_encode(b709));
                data[p++] = 255;
            }
        }
    }
}

static void hdr_ycbcr_to_rgb_bt2020(double y_norm, double cb_norm, double cr_norm,
                                    double *r, double *g, double *b)
{
    double y = (y_norm - (64.0 / 1023.0)) * (1023.0 / 876.0);
    double cb = (cb_norm - (512.0 / 1023.0)) * (1023.0 / 896.0);
    double cr = (cr_norm - (512.0 / 1023.0)) * (1023.0 / 896.0);

    *r = y + 1.4746 * cr;
    *g = y - 0.1646 * cb - 0.5714 * cr;
    *b = y + 1.8814 * cb;

    if (*r < 0.0) *r = 0.0;
    if (*r > 1.0) *r = 1.0;
    if (*g < 0.0) *g = 0.0;
    if (*g > 1.0) *g = 1.0;
    if (*b < 0.0) *b = 0.0;
    if (*b > 1.0) *b = 1.0;
}

static double hdr_pq_eotf(double e)
{
    const double m1 = 0.1593017578125;
    const double m2 = 78.84375;
    const double c1 = 0.8359375;
    const double c2 = 18.8515625;
    const double c3 = 18.6875;

    if (e <= 0.0)
        return 0.0;
    double ep = pow(e, 1.0 / m2);
    double num = ep - c1;
    if (num < 0.0)
        num = 0.0;
    double den = c2 - c3 * ep;
    if (den <= 0.0)
        return 0.0;
    return pow(num / den, 1.0 / m1);
}

static void hdr_bt2020_to_bt709(double r_in, double g_in, double b_in,
                                double *r_out, double *g_out, double *b_out)
{
    double r =  1.6605 * r_in - 0.5876 * g_in - 0.0728 * b_in;
    double g = -0.1246 * r_in + 1.1329 * g_in - 0.0083 * b_in;
    double b = -0.0182 * r_in - 0.1006 * g_in + 1.1187 * b_in;
    double max_c = r;
    double min_c = r;

    if (g > max_c) max_c = g;
    if (b > max_c) max_c = b;
    if (g < min_c) min_c = g;
    if (b < min_c) min_c = b;

    if (min_c < 0.0 || max_c > 1.0) {
        double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        double t = 1.0;
        if (min_c < 0.0) {
            double t2 = lum / (lum - min_c);
            if (t2 < t) t = t2;
        }
        if (max_c > 1.0) {
            double t2 = (1.0 - lum) / (max_c - lum + 1e-6);
            if (t2 < t) t = t2;
        }
        if (t < 0.0) t = 0.0;
        r = lum + t * (r - lum);
        g = lum + t * (g - lum);
        b = lum + t * (b - lum);
    }

    if (r < 0.0) r = 0.0;
    if (r > 1.0) r = 1.0;
    if (g < 0.0) g = 0.0;
    if (g > 1.0) g = 1.0;
    if (b < 0.0) b = 0.0;
    if (b > 1.0) b = 1.0;

    *r_out = r;
    *g_out = g;
    *b_out = b;
}

static void hdr_tonemap_reinhard(double r_in, double g_in, double b_in,
                                 double *r_out, double *g_out, double *b_out)
{
    const double exposure = 100.0; /* 10000 nits PQ scale / 100 nits SDR ref */
    const double lw = 10.0;       /* 1000 nits peak / 100 nits SDR ref */
    double r = r_in * exposure;
    double g = g_in * exposure;
    double b = b_in * exposure;
    double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;

    if (lum < 1e-6) {
        *r_out = 0.0;
        *g_out = 0.0;
        *b_out = 0.0;
        return;
    }

    double lum_mapped = lum * (1.0 + lum / (lw * lw)) / (1.0 + lum);
    double scale = lum_mapped / lum;
    *r_out = r * scale;
    *g_out = g * scale;
    *b_out = b * scale;

    if (*r_out > 1.0) *r_out = 1.0;
    if (*g_out > 1.0) *g_out = 1.0;
    if (*b_out > 1.0) *b_out = 1.0;
}

static double hdr_bt709_oetf(double l)
{
    if (l < 0.018)
        return 4.5 * l;
    return 1.099 * pow(l, 0.45) - 0.099;
}

static void hdr_rgb_to_ycbcr_bt709(double r, double g, double b,
                                   double *y_out, double *cb_out, double *cr_out)
{
    double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    double cb = (b - y) / 1.8556;
    double cr = (r - y) / 1.5748;

    *y_out = y * (876.0 / 1023.0) + (64.0 / 1023.0);
    *cb_out = cb * (896.0 / 1023.0) + (512.0 / 1023.0);
    *cr_out = cr * (896.0 / 1023.0) + (512.0 / 1023.0);

    if (*y_out < 0.0) *y_out = 0.0;
    if (*y_out > 1.0) *y_out = 1.0;
    if (*cb_out < 0.0) *cb_out = 0.0;
    if (*cb_out > 1.0) *cb_out = 1.0;
    if (*cr_out < 0.0) *cr_out = 0.0;
    if (*cr_out > 1.0) *cr_out = 1.0;
}

static void hdr10_ycbcr_pipeline(double y_in, double cb_in, double cr_in,
                                 double *y_out, double *cb_out, double *cr_out)
{
    double r, g, b;

    hdr_ycbcr_to_rgb_bt2020(y_in, cb_in, cr_in, &r, &g, &b);
    r = hdr_pq_eotf(r);
    g = hdr_pq_eotf(g);
    b = hdr_pq_eotf(b);
    hdr_bt2020_to_bt709(r, g, b, &r, &g, &b);
    hdr_tonemap_reinhard(r, g, b, &r, &g, &b);
    r = hdr_bt709_oetf(r);
    g = hdr_bt709_oetf(g);
    b = hdr_bt709_oetf(b);
    hdr_rgb_to_ycbcr_bt709(r, g, b, y_out, cb_out, cr_out);
}

static void generate_hdr_to_sdr_ycbcr_lut(uint16_t *data, uint32_t size)
{
    uint32_t p = 0;

    for (uint32_t cr = 0; cr < size; cr++) {
        double crf = (double)cr / (double)(size - 1u);
        for (uint32_t cb = 0; cb < size; cb++) {
            double cbf = (double)cb / (double)(size - 1u);
            for (uint32_t y = 0; y < size; y++) {
                double yf = (double)y / (double)(size - 1u);
                double y_out, cb_out, cr_out;

                hdr10_ycbcr_pipeline(yf, cbf, crf, &y_out, &cb_out, &cr_out);
                data[p++] = unorm16d(y_out);
                data[p++] = unorm16d(cb_out);
                data[p++] = unorm16d(cr_out);
                data[p++] = 0xFFFFu;
            }
        }
    }
}

static char *trim_ascii(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1)))
        *(--end) = '\0';
    return s;
}

static int parse_cube_lut(const char *path, uint8_t **data_out, uint32_t *size_out)
{
    FILE *fp;
    char line[512];
    uint8_t *data = NULL;
    uint32_t size = 0;
    uint32_t count = 0;
    uint32_t expected = 0;

    if (!path || !*path || !data_out || !size_out)
        return -1;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        char *s = trim_ascii(line);
        char *endptr;
        float r, g, b;

        if (!*s || *s == '#')
            continue;
        if (strncmp(s, "TITLE", 5) == 0 ||
            strncmp(s, "DOMAIN_MIN", 10) == 0 ||
            strncmp(s, "DOMAIN_MAX", 10) == 0) {
            continue;
        }
        if (sscanf(s, "LUT_3D_SIZE %u", &size) == 1) {
            if (size < 2 || size > SBS_LUT_MAX_SIZE)
                goto fail;
            expected = size * size * size;
            data = calloc(expected, 4);
            if (!data)
                goto fail;
            continue;
        }
        if (!data)
            continue;

        r = strtof(s, &endptr);
        if (endptr == s)
            continue;
        s = endptr;
        g = strtof(s, &endptr);
        if (endptr == s)
            continue;
        s = endptr;
        b = strtof(s, &endptr);
        if (endptr == s)
            continue;

        if (count >= expected)
            goto fail;
        data[count * 4u + 0u] = unorm8(r);
        data[count * 4u + 1u] = unorm8(g);
        data[count * 4u + 2u] = unorm8(b);
        data[count * 4u + 3u] = 255;
        count++;
    }

    fclose(fp);
    if (!data || count != expected)
        goto fail_no_fp;

    *data_out = data;
    *size_out = size;
    return 0;

fail:
    fclose(fp);
fail_no_fp:
    free(data);
    return -1;
}

static void destroy_lut_texture(sbs_compositor_t *comp,
                                VkImage *image,
                                VkDeviceMemory *memory,
                                VkImageView *view)
{
    if (*view) {
        vkDestroyImageView(comp->device, *view, NULL);
        *view = VK_NULL_HANDLE;
    }
    if (*image) {
        vkDestroyImage(comp->device, *image, NULL);
        *image = VK_NULL_HANDLE;
    }
    if (*memory) {
        vkFreeMemory(comp->device, *memory, NULL);
        *memory = VK_NULL_HANDLE;
    }
}

static int create_lut_texture_from_data(sbs_compositor_t *comp,
                                        const void *data,
                                        uint32_t size,
                                        VkFormat format,
                                        uint32_t texel_size,
                                        VkImage *image_out,
                                        VkDeviceMemory *memory_out,
                                        VkImageView *view_out)
{
    VkDeviceSize bytes = (VkDeviceSize)size * size * size * texel_size;
    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkResult res;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_3D,
        .format = format,
        .extent = { size, size, size },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    res = vkCreateImage(comp->device, &ici, NULL, &image);
    if (res != VK_SUCCESS)
        return -1;

    VkMemoryRequirements img_req;
    vkGetImageMemoryRequirements(comp->device, image, &img_req);
    uint32_t img_mem_type = find_memory_type(comp, img_req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_mem_type == UINT32_MAX)
        img_mem_type = find_memory_type(comp, img_req.memoryTypeBits, 0);
    if (img_mem_type == UINT32_MAX)
        goto fail;

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = img_req.size,
        .memoryTypeIndex = img_mem_type,
    };
    res = vkAllocateMemory(comp->device, &mai, NULL, &memory);
    if (res != VK_SUCCESS)
        goto fail;
    res = vkBindImageMemory(comp->device, image, memory, 0);
    if (res != VK_SUCCESS)
        goto fail;

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    res = vkCreateBuffer(comp->device, &bci, NULL, &staging_buf);
    if (res != VK_SUCCESS)
        goto fail;

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(comp->device, staging_buf, &buf_req);
    uint32_t buf_mem_type = find_memory_type(comp, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_mem_type == UINT32_MAX)
        goto fail;

    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_req.size,
        .memoryTypeIndex = buf_mem_type,
    };
    res = vkAllocateMemory(comp->device, &bmai, NULL, &staging_mem);
    if (res != VK_SUCCESS)
        goto fail;
    res = vkBindBufferMemory(comp->device, staging_buf, staging_mem, 0);
    if (res != VK_SUCCESS)
        goto fail;

    void *mapped = NULL;
    res = vkMapMemory(comp->device, staging_mem, 0, bytes, 0, &mapped);
    if (res != VK_SUCCESS)
        goto fail;
    memcpy(mapped, data, (size_t)bytes);
    vkUnmapMemory(comp->device, staging_mem);

    VkCommandBuffer cb = comp->upload_cb;
    vkWaitForFences(comp->device, 1, &comp->upload_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(comp->device, 1, &comp->upload_fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &cbbi);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy copy = {
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = { size, size, size },
    };
    vkCmdCopyBufferToImage(cb, staging_buf, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    vkEndCommandBuffer(cb);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, comp->upload_fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS)
        goto fail;
    res = vkWaitForFences(comp->device, 1, &comp->upload_fence, VK_TRUE, UINT64_MAX);
    if (res != VK_SUCCESS)
        goto fail;

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_3D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    res = vkCreateImageView(comp->device, &ivci, NULL, &view);
    if (res != VK_SUCCESS)
        goto fail;

    vkDestroyBuffer(comp->device, staging_buf, NULL);
    vkFreeMemory(comp->device, staging_mem, NULL);
    *image_out = image;
    *memory_out = memory;
    *view_out = view;
    return 0;

fail:
    if (staging_buf)
        vkDestroyBuffer(comp->device, staging_buf, NULL);
    if (staging_mem)
        vkFreeMemory(comp->device, staging_mem, NULL);
    if (view)
        vkDestroyImageView(comp->device, view, NULL);
    if (image)
        vkDestroyImage(comp->device, image, NULL);
    if (memory)
        vkFreeMemory(comp->device, memory, NULL);
    return -1;
}

static int create_lut_texture_from_rgba8(sbs_compositor_t *comp,
                                         const uint8_t *data,
                                         uint32_t size,
                                         VkImage *image_out,
                                         VkDeviceMemory *memory_out,
                                         VkImageView *view_out)
{
    return create_lut_texture_from_data(comp, data, size,
                                        VK_FORMAT_R8G8B8A8_UNORM, 4u,
                                        image_out, memory_out, view_out);
}

static int create_lut_texture_from_rgba16(sbs_compositor_t *comp,
                                          const uint16_t *data,
                                          uint32_t size,
                                          VkImage *image_out,
                                          VkDeviceMemory *memory_out,
                                          VkImageView *view_out)
{
    return create_lut_texture_from_data(comp, data, size,
                                        VK_FORMAT_R16G16B16A16_UNORM,
                                        4u * (uint32_t)sizeof(uint16_t),
                                        image_out, memory_out, view_out);
}

static int create_hdr_ycbcr_lut_sampler(sbs_compositor_t *comp)
{
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };

    VkResult res = vkCreateSampler(comp->device, &sci, NULL,
                                   &comp->hdr_ycbcr_lut_sampler);
    if (res != VK_SUCCESS) {
        LOG_E("HDR YCbCr LUT sampler create failed: %d", res);
        return -1;
    }
    return 0;
}

static int create_builtin_hdr_lut(sbs_compositor_t *comp)
{
    uint32_t size = SBS_HDR_LUT_SIZE;
    VkDeviceSize rgba8_bytes = (VkDeviceSize)size * size * size * 4u;
    VkDeviceSize rgba16_bytes = (VkDeviceSize)size * size * size * 4u * sizeof(uint16_t);
    uint8_t *data = malloc((size_t)rgba8_bytes);
    if (!data)
        return -1;

    generate_hdr_to_sdr_lut(data, size);
    if (create_lut_texture_from_rgba8(comp, data, size,
                                      &comp->hdr_lut_image,
                                      &comp->hdr_lut_memory,
                                      &comp->hdr_lut_view) != 0) {
        free(data);
        return -1;
    }
    free(data);
    comp->hdr_lut_size = size;

    uint16_t *ycbcr_data = malloc((size_t)rgba16_bytes);
    if (!ycbcr_data)
        return -1;
    generate_hdr_to_sdr_ycbcr_lut(ycbcr_data, size);
    if (create_lut_texture_from_rgba16(comp, ycbcr_data, size,
                                       &comp->hdr_ycbcr_lut_image,
                                       &comp->hdr_ycbcr_lut_memory,
                                       &comp->hdr_ycbcr_lut_view) != 0) {
        free(ycbcr_data);
        return -1;
    }
    free(ycbcr_data);
    comp->hdr_ycbcr_lut_size = size;

    if (create_hdr_ycbcr_lut_sampler(comp) != 0)
        return -1;

    LOG_I("built-in HDR->SDR LUTs ready (%u^3 RGB, YCbCr)", size);
    return 0;
}

/* ── Render targets ───────────────────────────────────────────── */

static int create_render_targets(sbs_compositor_t *comp)
{
    VkFormat format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_STORAGE_BIT |
                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;

    /* Query DRM format modifiers that support our usage + DMA-BUF export */
    VkDrmFormatModifierPropertiesListEXT mod_list = {
        .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
    };
    VkFormatProperties2 fmt_props2 = {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = &mod_list,
    };
    vkGetPhysicalDeviceFormatProperties2(comp->physical_device, format, &fmt_props2);

    uint64_t chosen_modifier = UINT64_MAX;
    bool dmabuf_export = false;

    LOG_I("DRM format modifier count: %u", mod_list.drmFormatModifierCount);
    if (mod_list.drmFormatModifierCount > 0) {
        VkDrmFormatModifierPropertiesEXT *mods = malloc(
            mod_list.drmFormatModifierCount * sizeof(*mods));
        if (mods) {
            mod_list.pDrmFormatModifierProperties = mods;
            vkGetPhysicalDeviceFormatProperties2(comp->physical_device, format, &fmt_props2);

            for (uint32_t m = 0; m < mod_list.drmFormatModifierCount; m++) {
                if (mods[m].drmFormatModifierPlaneCount != 1)
                    continue;
                VkFormatFeatureFlags feat = mods[m].drmFormatModifierTilingFeatures;
                if (!(feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
                    continue;

                LOG_I("DRM modifier 0x%lx: planes=%u features=0x%x",
                      (unsigned long)mods[m].drmFormatModifier,
                      mods[m].drmFormatModifierPlaneCount,
                      (unsigned)feat);

                if (mods[m].drmFormatModifier == DRM_FORMAT_MOD_LINEAR) {
                    chosen_modifier = DRM_FORMAT_MOD_LINEAR;
                    break;
                }
                if (chosen_modifier == UINT64_MAX)
                    chosen_modifier = mods[m].drmFormatModifier;
            }
            free(mods);
        }
    }

    if (chosen_modifier != UINT64_MAX) {
        LOG_I("using DRM modifier 0x%lx for render targets", (unsigned long)chosen_modifier);
        dmabuf_export = true;
    } else {
        LOG_W("no suitable DRM modifier found, render targets will not support DMA-BUF export");
    }

    /* Verify external image format compatibility for DMA-BUF export */
    if (dmabuf_export) {
        VkPhysicalDeviceExternalImageFormatInfo ext_img_fmt = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT drm_mod_query = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
            .pNext = &ext_img_fmt,
            .drmFormatModifier = chosen_modifier,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkPhysicalDeviceImageFormatInfo2 fmt_info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
            .pNext = &drm_mod_query,
            .format = format,
            .type = VK_IMAGE_TYPE_2D,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = usage,
        };
        VkExternalImageFormatProperties ext_img_props = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
        };
        VkImageFormatProperties2 img_fmt_props = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
            .pNext = &ext_img_props,
        };

        VkResult check = vkGetPhysicalDeviceImageFormatProperties2(
            comp->physical_device, &fmt_info, &img_fmt_props);
        if (check != VK_SUCCESS) {
            LOG_W("DMA-BUF export not supported for this format/modifier (err=%d)", check);
            dmabuf_export = false;
        } else {
            VkExternalMemoryFeatureFlags feat =
                ext_img_props.externalMemoryProperties.externalMemoryFeatures;
            LOG_I("external memory features: 0x%x (exportable=%d, importable=%d, dedicated=%d)",
                  (unsigned)feat,
                  !!(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
                  !!(feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
                  !!(feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT));

            if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
                LOG_W("DMA-BUF export not exportable for this configuration");
                dmabuf_export = false;
            }
        }
    }

    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        comp->targets[i].format = format;
        comp->targets[i].dmabuf_fd = -1;
        comp->targets[i].layout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkResult res;

        if (dmabuf_export) {
            VkImageDrmFormatModifierListCreateInfoEXT drm_mod = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
                .drmFormatModifierCount = 1,
                .pDrmFormatModifiers = &chosen_modifier,
            };

            VkExternalMemoryImageCreateInfo emi = {
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                .pNext = &drm_mod,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            };

            VkImageCreateInfo ici = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = &emi,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = { comp->width, comp->height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                .usage = usage,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };

            res = vkCreateImage(comp->device, &ici, NULL, &comp->targets[i].image);
            if (res != VK_SUCCESS) {
                LOG_E("vkCreateImage[%d] (DRM modifier) failed: %d, falling back", i, res);
                dmabuf_export = false;
            }
        }

        if (!dmabuf_export) {
            VkImageCreateInfo ici = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = format,
                .extent = { comp->width, comp->height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = usage,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            };

            res = vkCreateImage(comp->device, &ici, NULL, &comp->targets[i].image);
            if (res != VK_SUCCESS) {
                LOG_E("vkCreateImage[%d] fallback failed: %d", i, res);
                return -1;
            }
            if (i == 0)
                LOG_W("render targets created without DMA-BUF export capability");
        }

        VkMemoryDedicatedRequirements ded_reqs = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
        };
        VkMemoryRequirements2 mem_req2 = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
            .pNext = &ded_reqs,
        };
        VkImageMemoryRequirementsInfo2 img_mem_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
            .image = comp->targets[i].image,
        };
        vkGetImageMemoryRequirements2(comp->device, &img_mem_info, &mem_req2);

        VkMemoryRequirements *mem_req = &mem_req2.memoryRequirements;
        bool need_dedicated = ded_reqs.requiresDedicatedAllocation ||
                              ded_reqs.prefersDedicatedAllocation;

        if (i == 0) {
            LOG_I("memory requirements: size=%lu, alignment=%lu, typeBits=0x%x",
                  (unsigned long)mem_req->size,
                  (unsigned long)mem_req->alignment,
                  mem_req->memoryTypeBits);
            LOG_I("dedicated allocation: required=%d, preferred=%d",
                  ded_reqs.requiresDedicatedAllocation,
                  ded_reqs.prefersDedicatedAllocation);
        }

        uint32_t mem_type = find_memory_type(comp, mem_req->memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type == UINT32_MAX) {
            mem_type = find_memory_type(comp, mem_req->memoryTypeBits, 0);
        }
        if (mem_type == UINT32_MAX) {
            LOG_E("no suitable memory type for target %d", i);
            return -1;
        }
        if (i == 0) {
            LOG_I("selected memory type index: %u (flags=0x%x)",
                  mem_type,
                  (unsigned)comp->mem_props.memoryTypes[mem_type].propertyFlags);
        }

        VkMemoryDedicatedAllocateInfo ded_alloc = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .image = comp->targets[i].image,
        };

        VkExportMemoryAllocateInfo export_info = {
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = (dmabuf_export && need_dedicated) ? &ded_alloc : NULL,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = dmabuf_export ? &export_info : NULL,
            .allocationSize = mem_req->size,
            .memoryTypeIndex = mem_type,
        };

        res = vkAllocateMemory(comp->device, &mai, NULL, &comp->targets[i].memory);
        if (res != VK_SUCCESS) {
            LOG_E("vkAllocateMemory[%d] failed: %d", i, res);
            return -1;
        }

        vkBindImageMemory(comp->device, comp->targets[i].image,
                          comp->targets[i].memory, 0);

        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = comp->targets[i].image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        res = vkCreateImageView(comp->device, &ivci, NULL, &comp->targets[i].view);
        if (res != VK_SUCCESS) {
            LOG_E("vkCreateImageView[%d] failed: %d", i, res);
            return -1;
        }

        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        res = vkCreateFence(comp->device, &fci, NULL, &comp->targets[i].fence);
        if (res != VK_SUCCESS) {
            LOG_E("vkCreateFence[%d] failed: %d", i, res);
            return -1;
        }

        if (dmabuf_export) {
            PFN_vkGetMemoryFdKHR vkGetMemoryFd =
                (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(comp->device, "vkGetMemoryFdKHR");
            if (vkGetMemoryFd) {
                VkMemoryGetFdInfoKHR fd_info = {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                    .memory = comp->targets[i].memory,
                    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                };
                res = vkGetMemoryFd(comp->device, &fd_info, &comp->targets[i].dmabuf_fd);
                LOG_I("target[%d] DMA-BUF probe: vkResult=%d, fd=%d", i, res, comp->targets[i].dmabuf_fd);
                if (res != VK_SUCCESS || comp->targets[i].dmabuf_fd < 0) {
                    LOG_W("target[%d] DMA-BUF export probe failed — disabling export", i);
                    dmabuf_export = false;
                }
            }
        }
    }

    comp->dmabuf_export_available = dmabuf_export;
    LOG_I("%d render targets created (%ux%u, %s, dmabuf_export=%s)",
          SBS_RENDER_TARGET_COUNT, comp->width, comp->height, "RGBA8",
          dmabuf_export ? "yes" : "no");
    return 0;
}

/* ── Staging buffer for CPU readback (frame export) ───────────── */

static int create_export_surface(sbs_compositor_t *comp,
                                 sbs_export_surface_t *surface,
                                 uint32_t width,
                                 uint32_t height,
                                 const char *label)
{
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = vkCreateImage(comp->device, &ici, NULL, &surface->image);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateImage (%s) failed: %d", label, res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(comp->device, surface->image, &mem_req);

    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mem_type == UINT32_MAX) {
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (mem_type == UINT32_MAX) {
        LOG_E("no HOST_VISIBLE memory type for %s", label);
        vkDestroyImage(comp->device, surface->image, NULL);
        surface->image = VK_NULL_HANDLE;
        return -1;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };

    res = vkAllocateMemory(comp->device, &mai, NULL, &surface->memory);
    if (res != VK_SUCCESS) {
        LOG_E("vkAllocateMemory (%s) failed: %d", label, res);
        vkDestroyImage(comp->device, surface->image, NULL);
        surface->image = VK_NULL_HANDLE;
        return -1;
    }

    surface->size = mem_req.size;
    surface->width = width;
    surface->height = height;

    res = vkBindImageMemory(comp->device, surface->image, surface->memory, 0);
    if (res != VK_SUCCESS) {
        LOG_E("vkBindImageMemory (%s) failed: %d", label, res);
        return -1;
    }

    res = vkMapMemory(comp->device, surface->memory, 0,
                      mem_req.size, 0, &surface->mapped);
    if (res != VK_SUCCESS) {
        LOG_E("vkMapMemory (%s) failed: %d", label, res);
        return -1;
    }

    VkImageSubresource subres = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .arrayLayer = 0,
    };
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(comp->device, surface->image, &subres, &layout);
    surface->offset = layout.offset;
    surface->row_pitch = layout.rowPitch;
    surface->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    LOG_I("%s created (%ux%u, size=%lu, offset=%lu, row_pitch=%lu, memType=%u flags=0x%x)",
          label, width, height,
          (unsigned long)mem_req.size,
          (unsigned long)layout.offset,
          (unsigned long)layout.rowPitch,
          mem_type,
          (unsigned)comp->mem_props.memoryTypes[mem_type].propertyFlags);
    return 0;
}

static void destroy_export_surface(sbs_compositor_t *comp,
                                   sbs_export_surface_t *surface)
{
    if (!comp->device) return;

    if (surface->mapped) {
        vkUnmapMemory(comp->device, surface->memory);
        surface->mapped = NULL;
    }
    if (surface->memory) {
        vkFreeMemory(comp->device, surface->memory, NULL);
        surface->memory = VK_NULL_HANDLE;
    }
    if (surface->image) {
        vkDestroyImage(comp->device, surface->image, NULL);
        surface->image = VK_NULL_HANDLE;
    }
    memset(surface, 0, sizeof(*surface));
}

static int create_blit_target(sbs_compositor_t *comp,
                              sbs_blit_target_t *target,
                              uint32_t width,
                              uint32_t height,
                              const char *label)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = vkCreateImage(comp->device, &ici, NULL, &target->image);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateImage (%s) failed: %d", label, res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(comp->device, target->image, &mem_req);
    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX) {
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits, 0);
    }
    if (mem_type == UINT32_MAX) {
        LOG_E("no suitable memory type for %s", label);
        vkDestroyImage(comp->device, target->image, NULL);
        target->image = VK_NULL_HANDLE;
        return -1;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    res = vkAllocateMemory(comp->device, &mai, NULL, &target->memory);
    if (res != VK_SUCCESS) {
        LOG_E("vkAllocateMemory (%s) failed: %d", label, res);
        vkDestroyImage(comp->device, target->image, NULL);
        target->image = VK_NULL_HANDLE;
        return -1;
    }

    res = vkBindImageMemory(comp->device, target->image, target->memory, 0);
    if (res != VK_SUCCESS) {
        LOG_E("vkBindImageMemory (%s) failed: %d", label, res);
        return -1;
    }

    target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    target->width = width;
    target->height = height;
    LOG_I("%s created (%ux%u)", label, width, height);
    return 0;
}

static void destroy_blit_target(sbs_compositor_t *comp,
                                sbs_blit_target_t *target)
{
    if (!comp->device) return;
    if (target->memory) {
        vkFreeMemory(comp->device, target->memory, NULL);
        target->memory = VK_NULL_HANDLE;
    }
    if (target->image) {
        vkDestroyImage(comp->device, target->image, NULL);
        target->image = VK_NULL_HANDLE;
    }
    memset(target, 0, sizeof(*target));
}

static int create_staging_buffer(sbs_compositor_t *comp)
{
    return create_export_surface(comp, &comp->staging,
                                 comp->width, comp->height,
                                 "staging buffer");
}

static void destroy_staging_buffer(sbs_compositor_t *comp)
{
    destroy_export_surface(comp, &comp->staging);
}

static int ensure_preview_export_path(sbs_compositor_t *comp,
                                      uint32_t width,
                                      uint32_t height)
{
    if (comp->preview_target.image && comp->preview_staging.image &&
        comp->preview_target.width == width && comp->preview_target.height == height &&
        comp->preview_staging.width == width && comp->preview_staging.height == height) {
        return 0;
    }

    /* Clean up old preview compute resources */
    if (comp->preview_target_view) {
        vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
        comp->preview_target_view = VK_NULL_HANDLE;
    }
    if (comp->preview_nv21_buffer) {
        vkDestroyBuffer(comp->device, comp->preview_nv21_buffer, NULL);
        comp->preview_nv21_buffer = VK_NULL_HANDLE;
    }
    if (comp->preview_nv21_memory) {
        vkFreeMemory(comp->device, comp->preview_nv21_memory, NULL);
        comp->preview_nv21_memory = VK_NULL_HANDLE;
    }
    comp->preview_nv21_mapped = NULL;
    comp->preview_compute_available = false;

    destroy_blit_target(comp, &comp->preview_target);
    destroy_export_surface(comp, &comp->preview_staging);

    if (create_blit_target(comp, &comp->preview_target, width, height,
                           "preview blit target") != 0) {
        destroy_blit_target(comp, &comp->preview_target);
        return -1;
    }
    if (create_export_surface(comp, &comp->preview_staging, width, height,
                              "preview staging buffer") != 0) {
        destroy_blit_target(comp, &comp->preview_target);
        destroy_export_surface(comp, &comp->preview_staging);
        return -1;
    }

    /* Set up GPU compute path for preview NV21 export */
    if (comp->nv21_compute_available && comp->export_compute_ds_layout) {
        VkResult res;

        /* Create ImageView for preview_target (storage image) */
        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = comp->preview_target.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        res = vkCreateImageView(comp->device, &ivci, NULL, &comp->preview_target_view);
        if (res != VK_SUCCESS) {
            LOG_W("preview target ImageView creation failed: %d", res);
            return 0;  /* fall back to CPU path */
        }

        /* Allocate preview NV21 buffer */
        VkDeviceSize nv21_size = width * height * 3 / 2;
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = nv21_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        res = vkCreateBuffer(comp->device, &bci, NULL, &comp->preview_nv21_buffer);
        if (res != VK_SUCCESS) {
            LOG_W("preview NV21 buffer creation failed: %d", res);
            vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
            comp->preview_target_view = VK_NULL_HANDLE;
            return 0;
        }

        VkMemoryRequirements mem_req;
        vkGetBufferMemoryRequirements(comp->device, comp->preview_nv21_buffer, &mem_req);
        uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mem_type == UINT32_MAX) {
            LOG_W("no HOST_VISIBLE memory for preview NV21 buffer");
            vkDestroyBuffer(comp->device, comp->preview_nv21_buffer, NULL);
            comp->preview_nv21_buffer = VK_NULL_HANDLE;
            vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
            comp->preview_target_view = VK_NULL_HANDLE;
            return 0;
        }

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type,
        };
        res = vkAllocateMemory(comp->device, &mai, NULL, &comp->preview_nv21_memory);
        if (res != VK_SUCCESS) {
            LOG_W("preview NV21 memory alloc failed: %d", res);
            vkDestroyBuffer(comp->device, comp->preview_nv21_buffer, NULL);
            comp->preview_nv21_buffer = VK_NULL_HANDLE;
            vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
            comp->preview_target_view = VK_NULL_HANDLE;
            return 0;
        }
        vkBindBufferMemory(comp->device, comp->preview_nv21_buffer, comp->preview_nv21_memory, 0);
        vkMapMemory(comp->device, comp->preview_nv21_memory, 0, nv21_size, 0, &comp->preview_nv21_mapped);
        comp->preview_nv21_size = nv21_size;

        /* Allocate descriptor set from the export compute pool */
        VkDescriptorSetAllocateInfo dsai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = comp->export_compute_descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &comp->export_compute_ds_layout,
        };
        res = vkAllocateDescriptorSets(comp->device, &dsai, &comp->preview_compute_ds);
        if (res != VK_SUCCESS) {
            LOG_W("preview compute descriptor set alloc failed: %d", res);
            vkDestroyBuffer(comp->device, comp->preview_nv21_buffer, NULL);
            comp->preview_nv21_buffer = VK_NULL_HANDLE;
            vkFreeMemory(comp->device, comp->preview_nv21_memory, NULL);
            comp->preview_nv21_memory = VK_NULL_HANDLE;
            comp->preview_nv21_mapped = NULL;
            vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
            comp->preview_target_view = VK_NULL_HANDLE;
            return 0;
        }

        /* Update descriptor set: binding 0 = storage image, binding 1 = SSBO */
        VkDescriptorImageInfo img_info = {
            .imageView = comp->preview_target_view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkDescriptorBufferInfo buf_info = {
            .buffer = comp->preview_nv21_buffer,
            .offset = 0,
            .range = nv21_size,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->preview_compute_ds,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &img_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->preview_compute_ds,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buf_info,
            },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);

        comp->preview_compute_available = true;
        LOG_I("preview GPU compute NV21 export ready: %ux%u, %lu bytes",
              width, height, (unsigned long)nv21_size);
    }

    return 0;
}

/* export_rgba_surface_to_nv21_memfd — REMOVED (was CPU NEON path) */
static int export_target_nv21_compute(sbs_compositor_t *comp, uint32_t target_idx, int *fd);

/* ── Frame export (render target → NV21 memfd) ────────────────── */

int sbs_compositor_export_target_fd(sbs_compositor_t *comp, uint32_t target_idx, int *fd)
{
    if (target_idx >= SBS_RENDER_TARGET_COUNT || !fd)
        return -1;

    *fd = -1;

    /* Path 1: DMA-BUF direct export (if available) */
    if (comp->dmabuf_export_available) {
        if (comp->targets[target_idx].dmabuf_fd >= 0) {
            *fd = dup(comp->targets[target_idx].dmabuf_fd);
            if (*fd >= 0) {
                return 0;
            }
            LOG_W("DMA-BUF dup failed: fd=%d errno=%d, falling back to staging",
                  comp->targets[target_idx].dmabuf_fd, errno);
            *fd = -1;
        }
    }

    /* Path 2: GPU compute RGBA→NV21 (if available) */
    if (comp->nv21_compute_available) {
        return export_target_nv21_compute(comp, target_idx, fd);
    }

    /* Path 3 REMOVED — no CPU staging fallback. GPU compute is mandatory. */
    LOG_E("FATAL: no export path available (dmabuf_export=%d, nv21_compute=%d). "
          "GPU compute export pipeline must be initialized.",
          comp->dmabuf_export_available, comp->nv21_compute_available);
    return -1;
}

int sbs_compositor_export_target_preview_fd(sbs_compositor_t *comp,
                                            uint32_t target_idx,
                                            uint32_t width,
                                            uint32_t height,
                                            int *fd)
{
    /* REMOVED - legacy CPU staging + blit + memfd path.
     * Preview uses dest-export (sampler compute) + DMA-BUF handoff now. */
    (void)comp; (void)target_idx; (void)width; (void)height;
    if (fd) *fd = -1;
    LOG_E("FATAL: preview fd export called - this path is removed. "
          "Use dest-export + DMA-BUF handoff instead.");
    return -1;
}

/* ── Preview export — GPU blit + GPU compute NV21 (zero-copy ptr) ─ */
int sbs_compositor_export_target_preview_ptr(sbs_compositor_t *comp,
                                              uint32_t target_idx,
                                              uint32_t width,
                                              uint32_t height,
                                              const void **data_out,
                                              size_t *size_out)
{
    if (!comp || target_idx >= SBS_RENDER_TARGET_COUNT || !data_out || !size_out ||
        width == 0 || height == 0)
        return -1;

    if (ensure_preview_export_path(comp, width, height) != 0)
        return -1;

    if (!comp->preview_compute_available)
        return -1;  /* caller should fall back to fd path */

    struct timespec _ts_begin;
    clock_gettime(CLOCK_MONOTONIC, &_ts_begin);

    /* Wait for render to complete */
    VkResult res = vkWaitForFences(comp->device, 1,
        &comp->targets[target_idx].fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("preview ptr export: render fence timed out");
        return -1;
    }

    /* Wait for previous compute/export to complete (shared fence) */
    res = vkWaitForFences(comp->device, 1, &comp->export_fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("preview ptr export: export fence timed out");
        return -1;
    }
    vkResetFences(comp->device, 1, &comp->export_fence);

    /* Single command buffer: blit + compute */
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = comp->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    pthread_mutex_lock(&comp->command_pool_mutex);
    res = vkAllocateCommandBuffers(comp->device, &cbai, &cb);
    pthread_mutex_unlock(&comp->command_pool_mutex);
    if (res != VK_SUCCESS) {
        LOG_E("preview ptr export: cmd buffer alloc failed: %d", res);
        return -1;
    }

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &cbbi);

    /* Phase 1: Blit render target → preview_target */
    VkImageMemoryBarrier pre_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = comp->targets[target_idx].layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = comp->targets[target_idx].image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = comp->preview_target.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = comp->preview_target.image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 2, pre_barriers);

    VkImageBlit blit_region = {0};
    blit_region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit_region.srcSubresource.layerCount = 1;
    blit_region.srcOffsets[1].x = (int32_t)comp->width;
    blit_region.srcOffsets[1].y = (int32_t)comp->height;
    blit_region.srcOffsets[1].z = 1;
    blit_region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit_region.dstSubresource.layerCount = 1;
    blit_region.dstOffsets[1].x = (int32_t)width;
    blit_region.dstOffsets[1].y = (int32_t)height;
    blit_region.dstOffsets[1].z = 1;
    vkCmdBlitImage(cb,
        comp->targets[target_idx].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        comp->preview_target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit_region, VK_FILTER_LINEAR);

    /* Phase 2: Transition preview_target → GENERAL for compute read */
    VkImageMemoryBarrier blit_to_compute = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = comp->preview_target.image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &blit_to_compute);

    /* Zero the NV21 buffer */
    vkCmdFillBuffer(cb, comp->preview_nv21_buffer, 0, comp->preview_nv21_size, 0);

    VkMemoryBarrier fill_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &fill_barrier, 0, NULL, 0, NULL);

    /* Phase 3: Compute dispatch — RGBA→NV21 on preview_target */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      comp->export_compute_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            comp->export_compute_pipeline_layout, 0, 1,
                            &comp->preview_compute_ds, 0, NULL);

    uint32_t y_stride = width;
    uint32_t uv_stride = width;
    uint32_t uv_offset = width * height;
    uint32_t pc_data[5] = { width, height, y_stride, uv_stride, uv_offset };
    vkCmdPushConstants(cb, comp->export_compute_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc_data);

    uint32_t gx = (width + 15) / 16;
    uint32_t gy = (height + 15) / 16;
    vkCmdDispatch(cb, gx, gy, 1);

    /* Phase 4: Barriers — compute done, host readable, images restored */
    VkMemoryBarrier post_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    VkImageMemoryBarrier restore_barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = comp->preview_target.image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = comp->targets[target_idx].image,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 1, &post_barrier, 0, NULL, 2, restore_barriers);

    vkEndCommandBuffer(cb);

    /* Submit */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, comp->export_fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        LOG_E("preview ptr export submit failed: %d", res);
        pthread_mutex_lock(&comp->command_pool_mutex);
        vkFreeCommandBuffers(comp->device, comp->command_pool, 1, &cb);
        pthread_mutex_unlock(&comp->command_pool_mutex);
        return -1;
    }

    res = vkWaitForFences(comp->device, 1, &comp->export_fence, VK_TRUE, 100000000ULL);
    pthread_mutex_lock(&comp->command_pool_mutex);
    vkFreeCommandBuffers(comp->device, comp->command_pool, 1, &cb);
    pthread_mutex_unlock(&comp->command_pool_mutex);
    if (res != VK_SUCCESS) {
        LOG_W("preview ptr export fence timed out after submit");
        return -1;
    }

    comp->targets[target_idx].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    comp->preview_target.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

    *data_out = comp->preview_nv21_mapped;
    *size_out = (size_t)(width * height * 3 / 2);

    /* Timing */
    {
        static uint64_t _frame_ctr = 0;
        _frame_ctr++;
        if (_frame_ctr % 60 == 0) {
            struct timespec _ts_now;
            clock_gettime(CLOCK_MONOTONIC, &_ts_now);
            double ms = (_ts_now.tv_sec - _ts_begin.tv_sec) * 1000.0 +
                        (_ts_now.tv_nsec - _ts_begin.tv_nsec) / 1000000.0;
            LOG_I("PREVIEW_COMPUTE total=%.1fms (%ux%u)", ms, width, height);
        }
    }

    return 0;
}

/* ── Render pass ──────────────────────────────────────────────── */

static int create_render_pass(sbs_compositor_t *comp)
{
    /* Mali G52 optimization: Use COLOR_ATTACHMENT_OPTIMAL as initialLayout
     * instead of UNDEFINED to preserve AFBC/CRC transaction elimination
     * signatures.  loadOp=CLEAR still discards contents, but the "safe"
     * initial layout avoids signature buffer invalidation.
     *
     * For the very first frame each target is still UNDEFINED, so we issue
     * a one-time layout transition barrier in sbs_compositor_render_frame(). */
    VkAttachmentDescription color_att = {
        .format = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };

    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &color_att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = NULL,
    };

    VkResult res = vkCreateRenderPass(comp->device, &rpci, NULL, &comp->render_pass);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateRenderPass failed: %d", res);
        return -1;
    }
    LOG_I("render pass created (1 subpass: compose RGBA10)");
    return 0;
}

/* ── Framebuffers ─────────────────────────────────────────────── */

static int create_framebuffers(sbs_compositor_t *comp)
{
    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        VkFramebufferCreateInfo fbci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = comp->render_pass,
            .attachmentCount = 1,
            .pAttachments = &comp->targets[i].view,
            .width = comp->width,
            .height = comp->height,
            .layers = 1,
        };

        VkResult res = vkCreateFramebuffer(comp->device, &fbci, NULL,
                                            &comp->targets[i].framebuffer);
        if (res != VK_SUCCESS) {
            LOG_E("vkCreateFramebuffer[%d] failed: %d", i, res);
            return -1;
        }
    }
    LOG_I("framebuffers created");
    return 0;
}

/* ── Shader loading ───────────────────────────────────────────── */

static VkShaderModule load_shader(sbs_compositor_t *comp, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        LOG_E("cannot open shader: %s", path);
        return VK_NULL_HANDLE;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint32_t *code = malloc(size);
    if (!code) {
        fclose(f);
        return VK_NULL_HANDLE;
    }
    size_t n_read = fread(code, 1, size, f);
    fclose(f);
    if (n_read != (size_t)size) {
        free(code);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };

    VkShaderModule module;
    VkResult res = vkCreateShaderModule(comp->device, &smci, NULL, &module);
    free(code);

    if (res != VK_SUCCESS) {
        LOG_E("vkCreateShaderModule failed for %s: %d", path, res);
        return VK_NULL_HANDLE;
    }

    LOG_I("shader loaded: %s", path);
    return module;
}

/* ── Descriptor set layout + pool ─────────────────────────────── */

static int create_descriptor_set_layout(sbs_compositor_t *comp)
{
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };

    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->descriptor_set_layout);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateDescriptorSetLayout failed: %d", res);
        return -1;
    }
    return 0;
}

static int create_pipeline_layout(sbs_compositor_t *comp)
{
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = SBS_PUSH_CONSTANT_SIZE,
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };

    VkResult res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                           &comp->pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreatePipelineLayout failed: %d", res);
        return -1;
    }
    LOG_I("pipeline layout created (push constants: %d bytes)", SBS_PUSH_CONSTANT_SIZE);
    return 0;
}

static int create_descriptor_pool(sbs_compositor_t *comp)
{
    /* One source sampler and one LUT sampler per source texture slot. */
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = SBS_MAX_SOURCE_TEXTURES * 2,
    };

    /* OPT-5: Omit VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT.
     * On Mali G52, this flag forces the driver into a slower allocator
     * path that supports individual descriptor set freeing.  Since we
     * allocate all sets up front and never free them individually (the
     * pool itself is destroyed at shutdown), we can skip this flag
     * for a faster allocation path. */
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = 0,
        .maxSets = SBS_MAX_SOURCE_TEXTURES + 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };

    VkResult res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                           &comp->descriptor_pool);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateDescriptorPool failed: %d", res);
        return -1;
    }

    /* Allocate all descriptor sets up front */
    VkDescriptorSetLayout layouts[SBS_MAX_SOURCE_TEXTURES];
    for (int i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++)
        layouts[i] = comp->descriptor_set_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->descriptor_pool,
        .descriptorSetCount = SBS_MAX_SOURCE_TEXTURES,
        .pSetLayouts = layouts,
    };

    res = vkAllocateDescriptorSets(comp->device, &dsai, comp->descriptor_sets);
    if (res != VK_SUCCESS) {
        LOG_E("vkAllocateDescriptorSets failed: %d", res);
        return -1;
    }

    LOG_I("descriptor pool and %d sets allocated", SBS_MAX_SOURCE_TEXTURES);
    return 0;
}

/* ── Shared sampler ───────────────────────────────────────────── */

static int create_source_sampler(sbs_compositor_t *comp)
{
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };

    VkResult res = vkCreateSampler(comp->device, &sci, NULL, &comp->source_sampler);
    if (res != VK_SUCCESS) {
        LOG_E("vkCreateSampler failed: %d", res);
        return -1;
    }
    return 0;
}

/* ── Compute pipeline for GPU-based YUV->RGBA conversion ─────── */
static int create_compute_pipeline(sbs_compositor_t *comp, const char *shader_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/yuv_to_rgba.comp.spv", shader_dir);
    comp->compute_shader = load_shader(comp, path);
    if (comp->compute_shader == VK_NULL_HANDLE) {
        LOG_W("compute shader not found, DMA-BUF import will fall back to CPU");
        return -1;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->compute_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("compute descriptor set layout create failed: %d", res);
        return -1;
    }

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 28, /* 7 x uint32_t */
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->compute_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->compute_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("compute pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp->compute_shader,
            .pName = "main",
        },
        .layout = comp->compute_pipeline_layout,
    };
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->compute_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("compute pipeline create failed: %d", res);
        return -1;
    }

    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SBS_MAX_SOURCE_TEXTURES },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SBS_MAX_SOURCE_TEXTURES },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_MAX_SOURCE_TEXTURES,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->compute_descriptor_pool);
    if (res != VK_SUCCESS) {
        LOG_E("compute descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_MAX_SOURCE_TEXTURES];
    for (int i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++)
        layouts[i] = comp->compute_ds_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->compute_descriptor_pool,
        .descriptorSetCount = SBS_MAX_SOURCE_TEXTURES,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai, comp->compute_descriptor_sets);
    if (res != VK_SUCCESS) {
        LOG_E("compute descriptor set allocation failed: %d", res);
        return -1;
    }

    LOG_I("compute pipeline created for YUV->RGBA conversion");
    return 0;
}

/* ── Export compute pipeline for GPU RGBA→NV21 conversion ─────── */
static int create_export_compute_pipeline(sbs_compositor_t *comp, const char *shader_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/rgba_to_nv21.comp.spv", shader_dir);
    comp->export_compute_shader = load_shader(comp, path);
    if (comp->export_compute_shader == VK_NULL_HANDLE) {
        LOG_W("export compute shader not found, GPU RGBA→NV21 not available");
        return -1;
    }

    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->export_compute_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("export compute DS layout create failed: %d", res);
        return -1;
    }

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 20, /* 5 × uint32_t: width, height, y_stride, uv_stride, uv_offset */
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->export_compute_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->export_compute_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("export compute pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp->export_compute_shader,
            .pName = "main",
        },
        .layout = comp->export_compute_pipeline_layout,
    };
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->export_compute_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("export compute pipeline create failed: %d", res);
        return -1;
    }

    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  SBS_RENDER_TARGET_COUNT + 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SBS_RENDER_TARGET_COUNT + 1 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_RENDER_TARGET_COUNT + 1,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->export_compute_descriptor_pool);
    if (res != VK_SUCCESS) {
        LOG_E("export compute descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_RENDER_TARGET_COUNT];
    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++)
        layouts[i] = comp->export_compute_ds_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->export_compute_descriptor_pool,
        .descriptorSetCount = SBS_RENDER_TARGET_COUNT,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai, comp->export_compute_ds);
    if (res != VK_SUCCESS) {
        LOG_E("export compute DS allocation failed: %d", res);
        return -1;
    }

    LOG_I("export compute pipeline created for RGBA→NV21 conversion");
    return 0;
}

/* ── NV21 export buffers (HOST_VISIBLE, one per render target) ── */
static int create_nv21_export_buffers(sbs_compositor_t *comp)
{
    if (!comp->export_compute_pipeline) {
        LOG_W("export compute pipeline not available, skipping NV21 buffer creation");
        return -1;
    }

    VkDeviceSize nv21_size = comp->width * comp->height * 3 / 2;
    /* Round up to multiple of 4 for uint32 SSBO access */
    nv21_size = (nv21_size + 3) & ~3ULL;
    comp->nv21_size = nv21_size;

    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = nv21_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult res = vkCreateBuffer(comp->device, &bci, NULL, &comp->nv21_buffer[i]);
        if (res != VK_SUCCESS) {
            LOG_E("NV21 buffer[%d] create failed: %d", i, res);
            return -1;
        }

        VkMemoryRequirements mem_req;
        vkGetBufferMemoryRequirements(comp->device, comp->nv21_buffer[i], &mem_req);

        uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mem_type == UINT32_MAX) {
            LOG_E("no HOST_VISIBLE+HOST_COHERENT memory type for NV21 buffer");
            return -1;
        }

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type,
        };
        res = vkAllocateMemory(comp->device, &mai, NULL, &comp->nv21_memory[i]);
        if (res != VK_SUCCESS) {
            LOG_E("NV21 buffer[%d] memory alloc failed: %d", i, res);
            return -1;
        }

        vkBindBufferMemory(comp->device, comp->nv21_buffer[i], comp->nv21_memory[i], 0);
        vkMapMemory(comp->device, comp->nv21_memory[i], 0, nv21_size, 0, &comp->nv21_mapped[i]);

        /* Update descriptor set: bind render target image + NV21 buffer */
        VkDescriptorImageInfo img_info = {
            .imageView = comp->targets[i].view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkDescriptorBufferInfo buf_info = {
            .buffer = comp->nv21_buffer[i],
            .offset = 0,
            .range = nv21_size,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->export_compute_ds[i],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                .pImageInfo = &img_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->export_compute_ds[i],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buf_info,
            },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);
    }

    /* Allocate compute fence + command buffer */
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkResult res = vkCreateFence(comp->device, &fci, NULL, &comp->compute_fence);
    if (res != VK_SUCCESS) {
        LOG_E("compute fence create failed: %d", res);
        return -1;
    }

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = comp->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    res = vkAllocateCommandBuffers(comp->device, &cbai, &comp->compute_cb);
    if (res != VK_SUCCESS) {
        LOG_E("compute command buffer alloc failed: %d", res);
        return -1;
    }

    comp->nv21_compute_available = true;
    LOG_I("NV21 export buffers created: %u×%u, %lu bytes each, GPU compute RGBA→NV21 ready",
          comp->width, comp->height, (unsigned long)nv21_size);
    return 0;
}

static int create_p010_export_buffers(sbs_compositor_t *comp)
{
    VkDeviceSize p010_size = (VkDeviceSize)comp->width * comp->height * 4;
    p010_size = (p010_size + 3) & ~3ULL;
    comp->p010_size = p010_size;

    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = p010_size,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult res = vkCreateBuffer(comp->device, &bci, NULL, &comp->p010_buffer[i]);
        if (res != VK_SUCCESS) {
            LOG_E("P010 buffer[%d] create failed: %d", i, res);
            return -1;
        }

        VkMemoryRequirements mem_req;
        vkGetBufferMemoryRequirements(comp->device, comp->p010_buffer[i], &mem_req);

        uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mem_type == UINT32_MAX) {
            LOG_E("no HOST_VISIBLE+HOST_COHERENT memory type for P010 buffer");
            return -1;
        }

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type,
        };
        res = vkAllocateMemory(comp->device, &mai, NULL, &comp->p010_memory[i]);
        if (res != VK_SUCCESS) {
            LOG_E("P010 buffer[%d] memory alloc failed: %d", i, res);
            return -1;
        }

        vkBindBufferMemory(comp->device, comp->p010_buffer[i], comp->p010_memory[i], 0);
        vkMapMemory(comp->device, comp->p010_memory[i], 0, p010_size, 0, &comp->p010_mapped[i]);
    }

    LOG_I("P010 export buffers created: %ux%u, %lu bytes each",
          comp->width, comp->height, (unsigned long)p010_size);
    return 0;
}

/* ── P010 compute conversion pipeline ───────────────────────── */
/* Creates a compute pipeline + per-render-target descriptor sets
 * for converting the composed RGBA render target to P010 (BT.2020).
 * Dispatched after the render pass on the HOST_VISIBLE p010_buffer.
 * No atomics — each work item processes one 2x2 block exclusively. */

static int create_p010_conv_pipeline(sbs_compositor_t *comp)
{
    if (comp->p010_size == 0 || !comp->p010_dest_export_available)
        return -1;

    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->p010_conv_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("P010 conv DS layout create failed: %d", res);
        return -1;
    }

    VkDescriptorPoolSize pool_sizes[2] = {
        { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = SBS_RENDER_TARGET_COUNT },
        { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = SBS_RENDER_TARGET_COUNT },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_RENDER_TARGET_COUNT,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->p010_conv_ds_pool);
    if (res != VK_SUCCESS) {
        LOG_E("P010 conv descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_RENDER_TARGET_COUNT];
    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++)
        layouts[i] = comp->p010_conv_ds_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->p010_conv_ds_pool,
        .descriptorSetCount = SBS_RENDER_TARGET_COUNT,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai, comp->p010_conv_ds);
    if (res != VK_SUCCESS) {
        LOG_E("P010 conv descriptor sets alloc failed: %d", res);
        return -1;
    }

    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        VkDescriptorImageInfo img_info = {
            .sampler = comp->source_sampler,
            .imageView = comp->targets[i].view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorBufferInfo buf_info = {
            .buffer = comp->p010_buffer[i],
            .offset = 0,
            .range = comp->p010_size,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->p010_conv_ds[i],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &img_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->p010_conv_ds[i],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buf_info,
            },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);
    }

    comp->p010_conv_available = true;
    LOG_I("P010 compute conversion pipeline ready (sampler-based, no atomics)");
    return 0;
}

static int create_native_yuv_composite_pipeline(sbs_compositor_t *comp,
                                                const char *shader_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/native_yuv_sdr.comp.spv", shader_dir);
    comp->native_yuv_sdr_shader = load_shader(comp, path);
    snprintf(path, sizeof(path), "%s/native_yuv_hdr.comp.spv", shader_dir);
    comp->native_yuv_hdr_shader = load_shader(comp, path);
    if (comp->native_yuv_sdr_shader == VK_NULL_HANDLE ||
        comp->native_yuv_hdr_shader == VK_NULL_HANDLE) {
        LOG_W("native YUV composite shaders unavailable");
        return -1;
    }

    VkDescriptorSetLayoutBinding bindings[3] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->native_yuv_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV DS layout create failed: %d", res);
        return -1;
    }

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = SBS_NATIVE_YUV_PC_SIZE,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->native_yuv_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->native_yuv_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .pName = "main",
        },
        .layout = comp->native_yuv_pipeline_layout,
    };
    cpci.stage.module = comp->native_yuv_sdr_shader;
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->native_yuv_sdr_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV SDR pipeline create failed: %d", res);
        return -1;
    }
    cpci.stage.module = comp->native_yuv_hdr_shader;
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->native_yuv_hdr_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV HDR pipeline create failed: %d", res);
        return -1;
    }

    uint32_t native_layer_set_count = SBS_NATIVE_CANVAS_RING_SIZE *
        SBS_NATIVE_LAYER_DESCRIPTOR_SETS;
    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS * 2 },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->native_yuv_ds_pool);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_NATIVE_CANVAS_RING_SIZE * SBS_NATIVE_LAYER_DESCRIPTOR_SETS];
    for (uint32_t i = 0; i < native_layer_set_count; i++)
        layouts[i] = comp->native_yuv_ds_layout;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->native_yuv_ds_pool,
        .descriptorSetCount = native_layer_set_count,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai, &comp->native_yuv_ds[0][0]);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV descriptor allocation failed: %d", res);
        return -1;
    }

    res = vkAllocateDescriptorSets(comp->device, &dsai,
                                   &comp->native_yuv_preview_ds[0][0]);
    if (res != VK_SUCCESS) {
        LOG_E("native YUV preview descriptor allocation failed: %d", res);
        return -1;
    }

    LOG_I("native YUV composite pipelines created");
    return 0;
}

static int create_native_p010_direct_pipeline(sbs_compositor_t *comp,
                                              const char *shader_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/native_p010_direct.comp.spv", shader_dir);
    comp->native_p010_direct_shader = load_shader(comp, path);
    if (comp->native_p010_direct_shader == VK_NULL_HANDLE) {
        LOG_W("native P010 direct shader unavailable");
        return -1;
    }

    VkDescriptorSetLayoutBinding bindings[4] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->native_p010_direct_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct DS layout create failed: %d", res);
        return -1;
    }

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = SBS_NATIVE_P010_DIRECT_PC_SIZE,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->native_p010_direct_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->native_p010_direct_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp->native_p010_direct_shader,
            .pName = "main",
        },
        .layout = comp->native_p010_direct_pipeline_layout,
    };
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->native_p010_direct_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct pipeline create failed: %d", res);
        return -1;
    }

    snprintf(path, sizeof(path), "%s/native_p010_to_nv21.comp.spv", shader_dir);
    comp->native_p010_to_nv21_shader = load_shader(comp, path);
    if (comp->native_p010_to_nv21_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_p010_to_nv21_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_p010_to_nv21_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native P010->NV21 direct pipeline create failed: %d", res);
            comp->native_p010_to_nv21_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native P010->NV21 direct shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_yuv8_to_p010.comp.spv", shader_dir);
    comp->native_yuv8_to_p010_shader = load_shader(comp, path);
    if (comp->native_yuv8_to_p010_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_yuv8_to_p010_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_yuv8_to_p010_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native YUV8->P010 direct pipeline create failed: %d", res);
            comp->native_yuv8_to_p010_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native YUV8->P010 direct shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_amly_to_p010.comp.spv", shader_dir);
    comp->native_amly_to_p010_shader = load_shader(comp, path);
    if (comp->native_amly_to_p010_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_amly_to_p010_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_amly_to_p010_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native AMLY->P010 direct pipeline create failed: %d", res);
            comp->native_amly_to_p010_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native AMLY->P010 direct shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_amly_to_p010_src.comp.spv", shader_dir);
    comp->native_amly_to_p010_src_shader = load_shader(comp, path);
    if (comp->native_amly_to_p010_src_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_amly_to_p010_src_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_amly_to_p010_src_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native AMLY->P010 source-driven pipeline create failed: %d", res);
            comp->native_amly_to_p010_src_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native AMLY->P010 source-driven shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_yuv8_to_nv21.comp.spv", shader_dir);
    comp->native_yuv8_to_nv21_shader = load_shader(comp, path);
    if (comp->native_yuv8_to_nv21_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_yuv8_to_nv21_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_yuv8_to_nv21_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native YUV8->NV21 direct pipeline create failed: %d", res);
            comp->native_yuv8_to_nv21_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native YUV8->NV21 direct shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_amly_to_nv21.comp.spv", shader_dir);
    comp->native_amly_to_nv21_shader = load_shader(comp, path);
    if (comp->native_amly_to_nv21_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_amly_to_nv21_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_amly_to_nv21_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native AMLY->NV21 direct pipeline create failed: %d", res);
            comp->native_amly_to_nv21_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native AMLY->NV21 direct shader unavailable");
    }

    snprintf(path, sizeof(path), "%s/native_amly_to_nv21_src.comp.spv", shader_dir);
    comp->native_amly_to_nv21_src_shader = load_shader(comp, path);
    if (comp->native_amly_to_nv21_src_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_amly_to_nv21_src_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_amly_to_nv21_src_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native AMLY->NV21 source-driven pipeline create failed: %d", res);
            comp->native_amly_to_nv21_src_pipeline = VK_NULL_HANDLE;
        }
    } else {
        LOG_W("native AMLY->NV21 source-driven shader unavailable");
    }

    uint32_t native_layer_set_count = SBS_NATIVE_CANVAS_RING_SIZE *
        SBS_NATIVE_LAYER_DESCRIPTOR_SETS;
    VkDescriptorPoolSize pool_sizes[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS * 2 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_NATIVE_TOTAL_ENTRY_COUNT * SBS_NATIVE_LAYER_DESCRIPTOR_SETS,
        .poolSizeCount = 3,
        .pPoolSizes = pool_sizes,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->native_p010_direct_ds_pool);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_NATIVE_CANVAS_RING_SIZE * SBS_NATIVE_LAYER_DESCRIPTOR_SETS];
    for (uint32_t i = 0; i < native_layer_set_count; i++)
        layouts[i] = comp->native_p010_direct_ds_layout;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->native_p010_direct_ds_pool,
        .descriptorSetCount = native_layer_set_count,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai,
                                   &comp->native_p010_direct_ds[0][0]);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct descriptor allocation failed: %d", res);
        return -1;
    }
    res = vkAllocateDescriptorSets(comp->device, &dsai,
                                   &comp->native_p010_direct_preview_ds[0][0]);
    if (res != VK_SUCCESS) {
        LOG_E("native P010 direct preview descriptor allocation failed: %d", res);
        return -1;
    }

    LOG_I("native direct YUV pipelines created (p010=%d p010_nv21=%d yuv8_p010=%d amly_p010=%d amly_p010_src=%d yuv8_nv21=%d amly_nv21=%d amly_src=%d)",
          comp->native_p010_direct_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_p010_to_nv21_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_yuv8_to_p010_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_amly_to_p010_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_amly_to_p010_src_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_yuv8_to_nv21_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_amly_to_nv21_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_amly_to_nv21_src_pipeline != VK_NULL_HANDLE ? 1 : 0);
    return 0;
}

static int create_native_downscale_pipeline(sbs_compositor_t *comp,
                                            const char *shader_dir)
{
    char path[512];

    snprintf(path, sizeof(path), "%s/native_downscale_nv21.comp.spv", shader_dir);
    comp->native_downscale_sdr_shader = load_shader(comp, path);
    snprintf(path, sizeof(path), "%s/native_downscale_p010.comp.spv", shader_dir);
    comp->native_downscale_hdr_shader = load_shader(comp, path);
    if (comp->native_downscale_sdr_shader == VK_NULL_HANDLE &&
        comp->native_downscale_hdr_shader == VK_NULL_HANDLE) {
        LOG_W("native preview downscale shaders unavailable");
        return -1;
    }

    VkDescriptorSetLayoutBinding bindings[4] = {
        { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 3, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 4,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->native_downscale_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native preview downscale DS layout create failed: %d", res);
        return -1;
    }

    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = SBS_NATIVE_DOWNSCALE_PC_SIZE,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->native_downscale_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->native_downscale_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("native preview downscale pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .pName = "main",
        },
        .layout = comp->native_downscale_pipeline_layout,
    };
    if (comp->native_downscale_sdr_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_downscale_sdr_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_downscale_sdr_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native NV21 preview downscale pipeline create failed: %d", res);
            comp->native_downscale_sdr_pipeline = VK_NULL_HANDLE;
        }
    }
    if (comp->native_downscale_hdr_shader != VK_NULL_HANDLE) {
        cpci.stage.module = comp->native_downscale_hdr_shader;
        res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                        NULL, &comp->native_downscale_hdr_pipeline);
        if (res != VK_SUCCESS) {
            LOG_W("native P010 preview downscale pipeline create failed: %d", res);
            comp->native_downscale_hdr_pipeline = VK_NULL_HANDLE;
        }
    }
    if (comp->native_downscale_sdr_pipeline == VK_NULL_HANDLE &&
        comp->native_downscale_hdr_pipeline == VK_NULL_HANDLE)
        return -1;

    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = SBS_NATIVE_CANVAS_RING_SIZE * 4u,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_NATIVE_CANVAS_RING_SIZE,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    res = vkCreateDescriptorPool(comp->device, &dpci, NULL,
                                  &comp->native_downscale_ds_pool);
    if (res != VK_SUCCESS) {
        LOG_E("native preview downscale descriptor pool create failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_NATIVE_CANVAS_RING_SIZE];
    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++)
        layouts[i] = comp->native_downscale_ds_layout;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->native_downscale_ds_pool,
        .descriptorSetCount = SBS_NATIVE_CANVAS_RING_SIZE,
        .pSetLayouts = layouts,
    };
    res = vkAllocateDescriptorSets(comp->device, &dsai,
                                   comp->native_downscale_ds);
    if (res != VK_SUCCESS) {
        LOG_E("native preview downscale descriptor allocation failed: %d", res);
        return -1;
    }

    LOG_I("native preview downscale pipelines created (nv21=%d p010=%d)",
          comp->native_downscale_sdr_pipeline != VK_NULL_HANDLE ? 1 : 0,
          comp->native_downscale_hdr_pipeline != VK_NULL_HANDLE ? 1 : 0);
    return 0;
}

/* ── YCbCr direct sampling pipeline (VK_KHR_sampler_ycbcr_conversion) ── */

static int create_ycbcr_pipeline(sbs_compositor_t *comp)
{
    /* Step 1: Create YCbCr conversion object */
    VkSamplerYcbcrConversionCreateInfo ycbcr_ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .xChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
        .yChromaOffset = VK_CHROMA_LOCATION_COSITED_EVEN,
        .chromaFilter = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
    };

    PFN_vkCreateSamplerYcbcrConversionKHR pfnCreate =
        (PFN_vkCreateSamplerYcbcrConversionKHR)vkGetDeviceProcAddr(
            comp->device, "vkCreateSamplerYcbcrConversionKHR");
    if (!pfnCreate) {
        LOG_W("vkCreateSamplerYcbcrConversionKHR not available");
        return -1;
    }

    VkResult res = pfnCreate(comp->device, &ycbcr_ci, NULL, &comp->ycbcr_conversion);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreateSamplerYcbcrConversion failed: %d", res);
        return -1;
    }

    /* Step 2: Create immutable sampler with ycbcr conversion */
    VkSamplerYcbcrConversionInfo ycbcr_info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .conversion = comp->ycbcr_conversion,
    };

    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &ycbcr_info,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };

    res = vkCreateSampler(comp->device, &sci, NULL, &comp->ycbcr_sampler);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreateSampler for ycbcr failed: %d", res);
        return -1;
    }

    /* Step 3: Descriptor set layout with immutable ycbcr sampler plus LUT. */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
            .pImmutableSamplers = &comp->ycbcr_sampler,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };

    res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL, &comp->ycbcr_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreateDescriptorSetLayout for ycbcr failed: %d", res);
        return -1;
    }

    /* Step 4: Pipeline layout (same push constants as regular composition) */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = SBS_PUSH_CONSTANT_SIZE,
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->ycbcr_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };

    res = vkCreatePipelineLayout(comp->device, &plci, NULL, &comp->ycbcr_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreatePipelineLayout for ycbcr failed: %d", res);
        return -1;
    }

    /* Step 5: Descriptor pool and sets */
    VkDescriptorPoolSize pool_size = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = SBS_MAX_SOURCE_TEXTURES * 2,
    };

    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = 0,
        .maxSets = SBS_MAX_SOURCE_TEXTURES,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };

    res = vkCreateDescriptorPool(comp->device, &dpci, NULL, &comp->ycbcr_descriptor_pool);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreateDescriptorPool for ycbcr failed: %d", res);
        return -1;
    }

    VkDescriptorSetLayout layouts[SBS_MAX_SOURCE_TEXTURES];
    for (int i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++)
        layouts[i] = comp->ycbcr_ds_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = comp->ycbcr_descriptor_pool,
        .descriptorSetCount = SBS_MAX_SOURCE_TEXTURES,
        .pSetLayouts = layouts,
    };

    res = vkAllocateDescriptorSets(comp->device, &dsai, comp->ycbcr_descriptor_sets);
    if (res != VK_SUCCESS) {
        LOG_W("vkAllocateDescriptorSets for ycbcr failed: %d", res);
        return -1;
    }

    if (comp->hdr_lut_view != VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++) {
            VkDescriptorImageInfo lut_info = {
                .sampler = comp->source_sampler,
                .imageView = comp->hdr_lut_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkWriteDescriptorSet write = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->ycbcr_descriptor_sets[i],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &lut_info,
            };
            vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
            comp->sources[i].ycbcr_lut_descriptor_view = comp->hdr_lut_view;
        }
    }

    /* Note: ycbcr pipeline (graphics) is created later in load_shaders
     * when we have the shader modules available. Mark as partially ready. */
    LOG_I("ycbcr conversion + sampler + descriptors created");
    return 0;
}

/* Create ycbcr graphics pipeline — must be called after main graphics pipeline
 * creation since it reuses the same shader modules pattern. */
static int create_ycbcr_graphics_pipeline(sbs_compositor_t *comp,
                                           VkShaderModule vert, VkShaderModule frag)
{
    if (comp->ycbcr_pipeline_layout == VK_NULL_HANDLE)
        return -1;

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_att,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state,
        .layout = comp->ycbcr_pipeline_layout,
        .renderPass = comp->render_pass,
        .subpass = 0,
    };

    VkResult res = vkCreateGraphicsPipelines(comp->device, comp->pipeline_cache, 1, &gpci,
                                              NULL, &comp->ycbcr_pipeline);
    if (res != VK_SUCCESS) {
        LOG_W("vkCreateGraphicsPipelines for ycbcr failed: %d", res);
        return -1;
    }

    comp->ycbcr_available = true;
    LOG_I("ycbcr graphics pipeline created");
    return 0;
}

/* ── Destination-oriented export pipeline (sampler-based) ─────── */

static int create_dest_export_pipeline(sbs_compositor_t *comp, const char *shader_dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/export_nv21.comp.spv", shader_dir);
    comp->dest_export_shader = load_shader(comp, path);
    if (comp->dest_export_shader == VK_NULL_HANDLE) {
        LOG_W("dest export shader not found, destination-oriented export not available");
        return -1;
    }

    /* Descriptor set layout: binding 0 = combined image sampler, binding 1 = SSBO */
    VkDescriptorSetLayoutBinding bindings[2] = {
        {
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
        {
            .binding = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        },
    };

    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bindings,
    };
    VkResult res = vkCreateDescriptorSetLayout(comp->device, &dslci, NULL,
                                                &comp->dest_export_ds_layout);
    if (res != VK_SUCCESS) {
        LOG_E("dest export DS layout create failed: %d", res);
        return -1;
    }

    /* Push constants: dst_width, dst_height, y_stride, uv_stride, uv_offset,
     * inv_dst_width, inv_dst_height = 5 uint + 2 float = 28 bytes */
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = 28,
    };

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &comp->dest_export_ds_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range,
    };
    res = vkCreatePipelineLayout(comp->device, &plci, NULL,
                                  &comp->dest_export_pipeline_layout);
    if (res != VK_SUCCESS) {
        LOG_E("dest export pipeline layout create failed: %d", res);
        return -1;
    }

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = comp->dest_export_shader,
            .pName = "main",
        },
        .layout = comp->dest_export_pipeline_layout,
    };
    res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1, &cpci,
                                    NULL, &comp->dest_export_pipeline);
    if (res != VK_SUCCESS) {
        LOG_E("dest export compute pipeline create failed: %d", res);
        return -1;
    }

    comp->dest_export_available = true;
    LOG_I("destination export pipeline created (sampler-based RGBA→NV21)");

    /* Create P010 dest-export pipeline (same descriptor layout, different shader) */
    snprintf(path, sizeof(path), "%s/p010_export.comp.spv", shader_dir);
    comp->p010_dest_export_shader = load_shader(comp, path);
    if (comp->p010_dest_export_shader != VK_NULL_HANDLE) {
        VkPushConstantRange p010_pc = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .offset = 0,
            .size = 28,
        };
        VkPipelineLayoutCreateInfo p010_plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &comp->dest_export_ds_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &p010_pc,
        };
        res = vkCreatePipelineLayout(comp->device, &p010_plci, NULL,
                                      &comp->p010_dest_export_pipeline_layout);
        if (res == VK_SUCCESS) {
            VkComputePipelineCreateInfo p010_cpci = {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = comp->p010_dest_export_shader,
                    .pName = "main",
                },
                .layout = comp->p010_dest_export_pipeline_layout,
            };
            res = vkCreateComputePipelines(comp->device, comp->pipeline_cache, 1,
                                            &p010_cpci, NULL,
                                            &comp->p010_dest_export_pipeline);
            if (res == VK_SUCCESS) {
                comp->p010_dest_export_available = true;
                LOG_I("P010 dest-export pipeline created (sampler-based RGBA→P010)");
            } else {
                LOG_W("P010 dest-export compute pipeline create failed: %d", res);
            }
        }
    } else {
        LOG_W("p010_export.comp.spv not found, P010 dest-export not available");
    }

    return 0;
}

/* Initialize per-slot descriptor sets for a destination */
static int init_dest_export_slots(sbs_compositor_t *comp, sbs_export_dest_t *dest)
{
    /* Create per-destination descriptor pool */
    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SBS_EXPORT_SLOT_COUNT },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         SBS_EXPORT_SLOT_COUNT },
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = SBS_EXPORT_SLOT_COUNT,
        .poolSizeCount = 2,
        .pPoolSizes = pool_sizes,
    };
    VkResult res = vkCreateDescriptorPool(comp->device, &dpci, NULL, &dest->export_ds_pool);
    if (res != VK_SUCCESS) {
        LOG_E("dest export descriptor pool create failed: %d", res);
        return -1;
    }

    /* Allocate descriptor sets */
    VkDescriptorSetLayout layouts[SBS_EXPORT_SLOT_COUNT];
    for (int i = 0; i < SBS_EXPORT_SLOT_COUNT; i++)
        layouts[i] = comp->dest_export_ds_layout;

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dest->export_ds_pool,
        .descriptorSetCount = SBS_EXPORT_SLOT_COUNT,
        .pSetLayouts = layouts,
    };

    VkDescriptorSet sets[SBS_EXPORT_SLOT_COUNT];
    res = vkAllocateDescriptorSets(comp->device, &dsai, sets);
    if (res != VK_SUCCESS) {
        LOG_E("dest export DS allocation failed: %d", res);
        return -1;
    }

    /* Create sampler for sampling the composed RGBA render target */
    if (dest->source_sampler == VK_NULL_HANDLE) {
        VkSamplerCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };
        res = vkCreateSampler(comp->device, &sci, NULL, &dest->source_sampler);
        if (res != VK_SUCCESS) {
            LOG_E("dest export sampler create failed: %d", res);
            return -1;
        }
    }

    for (int i = 0; i < SBS_EXPORT_SLOT_COUNT; i++) {
        dest->slots[i].descriptor_set = sets[i];

        /* Update descriptor: source texture sampler + NV21 output buffer.
         * The source image view will be updated at submit time (varies per render target). */
        VkDescriptorBufferInfo buf_info = {
            .buffer = dest->slots[i].nv21_buffer,
            .offset = 0,
            .range  = dest->slots[i].nv21_size,
        };
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = sets[i],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buf_info,
        };
        vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
    }

    /* Store shared pipeline refs in destination */
    dest->export_pipeline_layout = comp->dest_export_pipeline_layout;
    dest->export_pipeline = comp->dest_export_pipeline;
    dest->export_ds_layout = comp->dest_export_ds_layout;

    return 0;
}

/* ── Destination Export API ───────────────────────────────────── */

int sbs_compositor_init_export_dest(sbs_compositor_t *comp,
                                    sbs_export_dest_type_t type,
                                    uint32_t width, uint32_t height,
                                    sbs_export_color_mode_t color_mode)
{
    if (type >= SBS_EXPORT_DEST_COUNT) return -1;
    if (!comp->dest_export_available) {
        LOG_E("cannot init %s export dest: pipeline not available",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output");
        return -1;
    }

    sbs_export_dest_t *dest = &comp->export_dests[type];

    /* Tear down existing if dimensions or color_mode changed */
    if (dest->initialized && (dest->width != width || dest->height != height ||
                               dest->color_mode != color_mode)) {
        LOG_I("export dest %s: teardown due to %s change (w=%u→%u h=%u→%u color=%d→%d)",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output",
              (dest->color_mode != color_mode) ? "color_mode" : "dimensions",
              dest->width, width, dest->height, height, dest->color_mode, color_mode);
        /* Wait for GPU to finish any in-flight work using these resources.
         * Must hold queue_mutex since vkQueueWaitIdle requires external sync on the queue. */
        pthread_mutex_lock(&comp->queue_mutex);
        vkQueueWaitIdle(comp->graphics_queue);
        pthread_mutex_unlock(&comp->queue_mutex);

        /* Clear dangling retire_slot pointers on all render targets — they
         * reference slots in this dest that are about to be freed. */
        pthread_mutex_lock(&comp->retire_slot_mutex);
        for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
            if (comp->targets[i].retire_slot) {
                /* Walk the dest's slot array to see if this retire_slot
                 * belongs to this dest. If so, null it out. */
                for (int s = 0; s < SBS_EXPORT_SLOT_COUNT; s++) {
                    if (comp->targets[i].retire_slot == &dest->slots[s]) {
                        comp->targets[i].retire_slot = NULL;
                        break;
                    }
                }
            }
        }
        pthread_mutex_unlock(&comp->retire_slot_mutex);
        sbs_export_dest_destroy(dest, comp->device);
    }

    if (dest->initialized) return 0; /* Already set up at correct dimensions */

    if (sbs_export_dest_init(dest, comp->device, &comp->mem_props,
                             type, width, height, color_mode) != 0) {
        LOG_E("export dest init failed for %s",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output");
        return -1;
    }

    if (sbs_export_dest_init_slots(dest, comp->device, &comp->mem_props,
                                   comp->command_pool) != 0) {
        LOG_E("export dest slot init failed for %s",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output");
        return -1;
    }

    if (init_dest_export_slots(comp, dest) != 0) {
        LOG_E("export dest descriptor init failed for %s",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output");
        sbs_export_dest_destroy(dest, comp->device);
        return -1;
    }

    dest->active = true;
    LOG_I("export dest %s initialized: %ux%u",
          type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output", width, height);
    return 0;
}

static void release_retire_slot_if_done(sbs_export_slot_t **slot_ptr);

int sbs_compositor_export_dest_submit(sbs_compositor_t *comp,
                                      sbs_export_dest_type_t type,
                                      uint32_t render_target_idx,
                                      uint64_t frame_number)
{
    if (type >= SBS_EXPORT_DEST_COUNT) return -1;
    sbs_export_dest_t *dest = &comp->export_dests[type];
    if (!dest->initialized || !dest->active) return -1;

    /* Acquire a free slot */
    sbs_export_slot_t *slot = sbs_export_dest_acquire_slot(dest);
    if (!slot) {
        LOG_W("export dest %s: ring full, dropping frame %lu",
              type == SBS_EXPORT_DEST_PREVIEW ? "preview" : "output",
              (unsigned long)frame_number);
        return -1;
    }

    /* P010 path: use vkCmdCopyBuffer from subpass1 HOST_VISIBLE output to
     * DMA-BUF-backed slot buffer. Mali G52 compute hangs on large DMA-BUF
     * SSBO writes, but buffer copies work fine. */
    bool is_hdr10 = (dest->color_mode == SBS_EXPORT_COLOR_HDR10);
    bool use_p010_copy = is_hdr10 && comp->p010_conv_available && comp->p010_size > 0;

    /* Update descriptor set for SDR compute path (HDR10 uses copy, no descriptors needed) */
    if (!use_p010_copy) {
        VkDescriptorImageInfo img_info = {
            .sampler = dest->source_sampler,
            .imageView = comp->targets[render_target_idx].view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorBufferInfo buf_info = {
            .buffer = slot->nv21_buffer,
            .offset = 0,
            .range  = slot->nv21_size,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot->descriptor_set,
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &img_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = slot->descriptor_set,
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &buf_info,
            },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);
    }

    /* Reset fence */
    vkResetFences(comp->device, 1, &slot->fence);

    /* Record command buffer */
    vkResetCommandBuffer(slot->cmd_buffer, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(slot->cmd_buffer, &cbbi);

    if (use_p010_copy) {
        /* ── HDR10 P010: copy from HOST_VISIBLE buffer to DMA-BUF slot ── */

        /* Barrier: wait for compute shader writes to p010_buffer */
        VkBufferMemoryBarrier p010_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .buffer = comp->p010_buffer[render_target_idx],
            .offset = 0,
            .size = comp->p010_size,
        };
        vkCmdPipelineBarrier(slot->cmd_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 1, &p010_barrier, 0, NULL);

        /* Copy P010 data from subpass1 output to DMA-BUF slot */
        VkDeviceSize copy_size = slot->nv21_size < comp->p010_size ? slot->nv21_size : comp->p010_size;
        VkBufferCopy copy_region = {
            .srcOffset = 0,
            .dstOffset = 0,
            .size = copy_size,
        };
        vkCmdCopyBuffer(slot->cmd_buffer,
                        comp->p010_buffer[render_target_idx],
                        slot->nv21_buffer,
                        1, &copy_region);

        /* Barrier: copy writes visible before handing DMA-BUF to consumers */
        VkBufferMemoryBarrier copy_done_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .buffer = slot->nv21_buffer,
            .offset = 0,
            .size = copy_size,
        };
        vkCmdPipelineBarrier(slot->cmd_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 1, &copy_done_barrier, 0, NULL);

        {
            static uint64_t copy_count = 0;
            copy_count++;
            if (copy_count <= 5) {
                LOG_I("P010 EXPORT COPY slot=%p frame=%lu size=%lu",
                      (void*)slot, (unsigned long)frame_number, (unsigned long)copy_size);
            }
        }
    } else {
        /* ── SDR NV21: compute dispatch from render target to slot ── */

        /* Transition render target to SHADER_READ_ONLY_OPTIMAL for sampling.
         * After composition it's in COLOR_ATTACHMENT_OPTIMAL. */
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = comp->targets[render_target_idx].layout,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = comp->targets[render_target_idx].image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(slot->cmd_buffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier);

        /* Bind pipeline and dispatch */
        vkCmdBindPipeline(slot->cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          comp->dest_export_pipeline);
        vkCmdBindDescriptorSets(slot->cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                comp->dest_export_pipeline_layout, 0, 1,
                                &slot->descriptor_set, 0, NULL);

        /* Push constants */
        struct {
            uint32_t dst_width;
            uint32_t dst_height;
            uint32_t y_stride;
            uint32_t uv_stride;
            uint32_t uv_offset;
            float    inv_dst_width;
            float    inv_dst_height;
        } pc;
        pc.dst_width      = dest->width;
        pc.dst_height     = dest->height;
        pc.inv_dst_width  = 1.0f / (float)dest->width;
        pc.inv_dst_height = 1.0f / (float)dest->height;
        pc.y_stride   = dest->width;
        pc.uv_stride  = dest->width;
        pc.uv_offset  = dest->width * dest->height;
        vkCmdPushConstants(slot->cmd_buffer,
                           comp->dest_export_pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

        uint32_t gx = (dest->width + 15) / 16;
        uint32_t gy = (dest->height + 15) / 16;
        vkCmdDispatch(slot->cmd_buffer, gx, gy, 1);

        /* Barrier: compute writes visible before handing the DMA-BUF to consumers. */
        VkBufferMemoryBarrier host_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .buffer = slot->nv21_buffer,
            .offset = 0,
            .size = slot->nv21_size,
        };
        vkCmdPipelineBarrier(slot->cmd_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 1, &host_barrier, 0, NULL);

        /* Transition render target back to COLOR_ATTACHMENT_OPTIMAL */
        VkImageMemoryBarrier barrier_back = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = comp->targets[render_target_idx].image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(slot->cmd_buffer,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0, 0, NULL, 0, NULL, 1, &barrier_back);
    }

    vkEndCommandBuffer(slot->cmd_buffer);

    /* Submit */
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot->cmd_buffer,
    };

    pthread_mutex_lock(&comp->queue_mutex);
    VkResult res = vkQueueSubmit(comp->graphics_queue, 1, &submit_info, slot->fence);
    pthread_mutex_unlock(&comp->queue_mutex);

    if (res != VK_SUCCESS) {
        LOG_E("export dest submit failed: %d", res);
        atomic_store(&slot->state, SBS_EXPORT_SLOT_FREE);
        return -1;
    }
    {
        static uint64_t submit_ok_count = 0;
        submit_ok_count++;
        if (submit_ok_count <= 5) {
            LOG_I("export dest submit OK slot=%p frame=%lu %s",
                  (void*)slot, (unsigned long)frame_number,
                  use_p010_copy ? "(P010 copy)" : "(NV21 compute)");
        }
    }
    if (comp->targets[render_target_idx].retire_slot &&
        comp->targets[render_target_idx].retire_slot != slot) {
        release_retire_slot_if_done(&comp->targets[render_target_idx].retire_slot);
    }
    pthread_mutex_lock(&comp->retire_slot_mutex);
    comp->targets[render_target_idx].retire_slot = slot;
    pthread_mutex_unlock(&comp->retire_slot_mutex);
    atomic_fetch_add(&slot->retire_refs, 1);

    sbs_export_slot_mark_submitted(slot, frame_number);
    slot->render_target_idx = render_target_idx;
    atomic_fetch_add(&dest->diag.frames_exported, 1);

    return 0;
}

sbs_export_slot_t *sbs_compositor_export_dest_acquire(sbs_compositor_t *comp,
                                                       sbs_export_dest_type_t type)
{
    if (type >= SBS_EXPORT_DEST_COUNT) return NULL;
    sbs_export_dest_t *dest = &comp->export_dests[type];
    if (!dest->initialized) return NULL;
    return sbs_export_dest_acquire_ready(dest, comp->device);
}

void sbs_compositor_export_dest_release(sbs_compositor_t *comp,
                                         sbs_export_dest_type_t type,
                                         sbs_export_slot_t *slot)
{
    (void)type;
    sbs_export_slot_release(slot, comp->device);
}

bool sbs_compositor_dest_export_available(const sbs_compositor_t *comp,
                                          sbs_export_dest_type_t type)
{
    if (type >= SBS_EXPORT_DEST_COUNT) return false;
    return comp->dest_export_available && comp->export_dests[type].initialized;
}

static void release_retire_slot_if_done(sbs_export_slot_t **slot_ptr)
{
    sbs_export_slot_t *slot = *slot_ptr;
    if (!slot)
        return;

    uint32_t refs = atomic_load(&slot->retire_refs);
    if (refs > 0)
        refs = atomic_fetch_sub(&slot->retire_refs, 1) - 1;
    if (refs == 0 && slot->release_pending_free)
        atomic_store(&slot->state, SBS_EXPORT_SLOT_FREE);
    *slot_ptr = NULL;
}

/* ── GPU compute RGBA→NV21 export (replaces CPU NEON path) ────── */
static int export_target_nv21_compute(sbs_compositor_t *comp, uint32_t target_idx, int *fd)
{
    struct timespec _ts_begin, _ts_fence, _ts_dispatch, _ts_wait, _ts_memcpy;
    clock_gettime(CLOCK_MONOTONIC, &_ts_begin);

    /* Wait for render to complete */
    VkResult res = vkWaitForFences(comp->device, 1,
        &comp->targets[target_idx].fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("compute export: render fence wait timed out for target %u", target_idx);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_fence);

    /* Wait for previous compute to complete */
    res = vkWaitForFences(comp->device, 1, &comp->compute_fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("compute export: compute fence wait timed out");
        return -1;
    }
    vkResetFences(comp->device, 1, &comp->compute_fence);

    /* Record compute command buffer */
    vkResetCommandBuffer(comp->compute_cb, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(comp->compute_cb, &cbbi);

    /* Transition render target: COLOR_ATTACHMENT_OPTIMAL → GENERAL (for storage image read) */
    VkImageMemoryBarrier pre_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = comp->targets[target_idx].layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = comp->targets[target_idx].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &pre_barrier);

    /* Zero the NV21 buffer (required for atomicOr byte packing) */
    vkCmdFillBuffer(comp->compute_cb, comp->nv21_buffer[target_idx], 0, comp->nv21_size, 0);

    /* Memory barrier: TRANSFER_WRITE (fill) → SHADER_WRITE */
    VkMemoryBarrier fill_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &fill_barrier, 0, NULL, 0, NULL);

    /* Bind pipeline + descriptor set */
    vkCmdBindPipeline(comp->compute_cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      comp->export_compute_pipeline);
    vkCmdBindDescriptorSets(comp->compute_cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            comp->export_compute_pipeline_layout, 0, 1,
                            &comp->export_compute_ds[target_idx], 0, NULL);

    /* Push constants */
    uint32_t y_stride = comp->width;
    uint32_t uv_stride = comp->width;
    uint32_t uv_offset = comp->width * comp->height;
    uint32_t pc_data[5] = { comp->width, comp->height, y_stride, uv_stride, uv_offset };
    vkCmdPushConstants(comp->compute_cb, comp->export_compute_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc_data);

    /* Dispatch: one thread per pixel, workgroup 16×16 */
    uint32_t gx = (comp->width + 15) / 16;
    uint32_t gy = (comp->height + 15) / 16;
    vkCmdDispatch(comp->compute_cb, gx, gy, 1);

    /* Memory barrier: SHADER_WRITE → HOST_READ */
    VkMemoryBarrier post_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &post_barrier, 0, NULL, 0, NULL);

    /* Transition render target back: GENERAL → COLOR_ATTACHMENT_OPTIMAL */
    VkImageMemoryBarrier rt_back = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = comp->targets[target_idx].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &rt_back);

    vkEndCommandBuffer(comp->compute_cb);

    /* Submit */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &comp->compute_cb,
    };
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, comp->compute_fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        LOG_E("compute export submit failed: %d", res);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_dispatch);

    /* Wait for compute to complete */
    res = vkWaitForFences(comp->device, 1, &comp->compute_fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("compute export fence wait timed out after dispatch");
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_wait);

    /* Create memfd and copy NV21 data out */
    VkDeviceSize frame_size = comp->width * comp->height * 3 / 2;
    int mfd = memfd_create("sbs-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (mfd < 0) {
        LOG_E("memfd_create failed in compute export");
        return -1;
    }
    if (ftruncate(mfd, frame_size) != 0) {
        close(mfd);
        return -1;
    }
    void *dst = mmap(NULL, frame_size, PROT_WRITE, MAP_SHARED, mfd, 0);
    if (dst == MAP_FAILED) {
        close(mfd);
        return -1;
    }
    memcpy(dst, comp->nv21_mapped[target_idx], frame_size);
    munmap(dst, frame_size);
    *fd = mfd;
    clock_gettime(CLOCK_MONOTONIC, &_ts_memcpy);

    /* Timing instrumentation */
    {
        static uint64_t _frame_ctr = 0;
        _frame_ctr++;
        if (_frame_ctr % 60 == 0) {
            #define _TS_MS(a,b) (((b).tv_sec-(a).tv_sec)*1000.0 + ((b).tv_nsec-(a).tv_nsec)/1000000.0)
            LOG_I("COMPUTE_EXPORT fence=%.1fms dispatch=%.1fms gpu_wait=%.1fms memcpy=%.1fms total=%.1fms",
                  _TS_MS(_ts_begin, _ts_fence),
                  _TS_MS(_ts_fence, _ts_dispatch),
                  _TS_MS(_ts_dispatch, _ts_wait),
                  _TS_MS(_ts_wait, _ts_memcpy),
                  _TS_MS(_ts_begin, _ts_memcpy));
            #undef _TS_MS
        }
    }

    return 0;
}

/* ── GPU compute RGBA→NV21 export — returns pointer (zero-copy) ─ */
int sbs_compositor_export_target_nv21_ptr(sbs_compositor_t *comp,
                                           uint32_t target_idx,
                                           const void **data_out,
                                           size_t *size_out,
                                           uint32_t *target_idx_out)
{
    if (!comp->nv21_compute_available || target_idx >= SBS_RENDER_TARGET_COUNT)
        return -1;

    struct timespec _ts_begin, _ts_fence, _ts_dispatch, _ts_wait;
    clock_gettime(CLOCK_MONOTONIC, &_ts_begin);

    /* Wait for render to complete */
    VkResult res = vkWaitForFences(comp->device, 1,
        &comp->targets[target_idx].fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("ptr export: render fence wait timed out for target %u", target_idx);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_fence);

    /* Wait for previous compute to complete */
    res = vkWaitForFences(comp->device, 1, &comp->compute_fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("ptr export: compute fence wait timed out");
        return -1;
    }
    vkResetFences(comp->device, 1, &comp->compute_fence);

    /* Record compute command buffer */
    vkResetCommandBuffer(comp->compute_cb, 0);
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(comp->compute_cb, &cbbi);

    VkImageMemoryBarrier pre_barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout = comp->targets[target_idx].layout,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .image = comp->targets[target_idx].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, NULL, 0, NULL, 1, &pre_barrier);

    vkCmdFillBuffer(comp->compute_cb, comp->nv21_buffer[target_idx], 0, comp->nv21_size, 0);

    VkMemoryBarrier fill_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &fill_barrier, 0, NULL, 0, NULL);

    vkCmdBindPipeline(comp->compute_cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      comp->export_compute_pipeline);
    vkCmdBindDescriptorSets(comp->compute_cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            comp->export_compute_pipeline_layout, 0, 1,
                            &comp->export_compute_ds[target_idx], 0, NULL);

    uint32_t y_stride = comp->width;
    uint32_t uv_stride = comp->width;
    uint32_t uv_offset = comp->width * comp->height;
    uint32_t pc_data[5] = { comp->width, comp->height, y_stride, uv_stride, uv_offset };
    vkCmdPushConstants(comp->compute_cb, comp->export_compute_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 20, pc_data);

    uint32_t gx = (comp->width + 15) / 16;
    uint32_t gy = (comp->height + 15) / 16;
    vkCmdDispatch(comp->compute_cb, gx, gy, 1);

    VkMemoryBarrier post_barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &post_barrier, 0, NULL, 0, NULL);

    VkImageMemoryBarrier rt_back = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = comp->targets[target_idx].image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(comp->compute_cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, NULL, 0, NULL, 1, &rt_back);

    vkEndCommandBuffer(comp->compute_cb);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &comp->compute_cb,
    };
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, comp->compute_fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        LOG_E("ptr export submit failed: %d", res);
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_dispatch);

    res = vkWaitForFences(comp->device, 1, &comp->compute_fence, VK_TRUE, 100000000ULL);
    if (res != VK_SUCCESS) {
        LOG_W("ptr export fence wait timed out after dispatch");
        return -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &_ts_wait);

    VkDeviceSize frame_size = comp->width * comp->height * 3 / 2;
    *data_out = comp->nv21_mapped[target_idx];
    *size_out = (size_t)frame_size;
    *target_idx_out = target_idx;

    /* Timing instrumentation */
    {
        static uint64_t _frame_ctr = 0;
        _frame_ctr++;
        if (_frame_ctr % 60 == 0) {
            #define _TS_MS(a,b) (((b).tv_sec-(a).tv_sec)*1000.0 + ((b).tv_nsec-(a).tv_nsec)/1000000.0)
            LOG_I("COMPUTE_PTR fence=%.1fms dispatch=%.1fms gpu_wait=%.1fms total=%.1fms",
                  _TS_MS(_ts_begin, _ts_fence),
                  _TS_MS(_ts_fence, _ts_dispatch),
                  _TS_MS(_ts_dispatch, _ts_wait),
                  _TS_MS(_ts_begin, _ts_wait));
            #undef _TS_MS
        }
    }

    return 0;
}

/* ── Placeholder texture (1x1 magenta) ────────────────────────── */
static int create_placeholder_texture(sbs_compositor_t *comp)
{
    sbs_source_texture_t *ph = &comp->placeholder;
    uint32_t w = 1, h = 1;
    VkFormat format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

    /* Create device-local image */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = vkCreateImage(comp->device, &ici, NULL, &ph->image);
    if (res != VK_SUCCESS) {
        LOG_E("placeholder image create failed: %d", res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(comp->device, ph->image, &mem_req);
    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits, 0);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    res = vkAllocateMemory(comp->device, &mai, NULL, &ph->memory);
    if (res != VK_SUCCESS) return -1;
    vkBindImageMemory(comp->device, ph->image, ph->memory, 0);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = ph->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    res = vkCreateImageView(comp->device, &ivci, NULL, &ph->view);
    if (res != VK_SUCCESS) return -1;

    /* Upload 1 magenta pixel via a staging buffer */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    VkBuffer staging_buf;
    VkDeviceMemory staging_mem;
    res = vkCreateBuffer(comp->device, &bci, NULL, &staging_buf);
    if (res != VK_SUCCESS) return -1;

    VkMemoryRequirements buf_req;
    vkGetBufferMemoryRequirements(comp->device, staging_buf, &buf_req);
    uint32_t buf_mem_type = find_memory_type(comp, buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_req.size,
        .memoryTypeIndex = buf_mem_type,
    };
    res = vkAllocateMemory(comp->device, &bmai, NULL, &staging_mem);
    if (res != VK_SUCCESS) { vkDestroyBuffer(comp->device, staging_buf, NULL); return -1; }
    vkBindBufferMemory(comp->device, staging_buf, staging_mem, 0);

    void *mapped;
    vkMapMemory(comp->device, staging_mem, 0, 4, 0, &mapped);
    uint32_t magenta_a2b10g10r10 = (3u << 30) | (1023u << 20) | (0u << 10) | 1023u;
    memcpy(mapped, &magenta_a2b10g10r10, 4);
    vkUnmapMemory(comp->device, staging_mem);

    /* Copy to image via command buffer */
    VkCommandBuffer cb = comp->upload_cb;
    vkWaitForFences(comp->device, 1, &comp->upload_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(comp->device, 1, &comp->upload_fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cb, &cbbi);

    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = ph->image,
        .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
    };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &barrier);

    VkBufferImageCopy copy = {
        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
        .imageExtent = { 1, 1, 1 },
    };
    vkCmdCopyBufferToImage(cb, staging_buf, ph->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 0, NULL, 0, NULL, 1, &barrier);

    vkEndCommandBuffer(cb);

    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    pthread_mutex_lock(&comp->queue_mutex);
    vkQueueSubmit(comp->graphics_queue, 1, &si, comp->upload_fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    vkWaitForFences(comp->device, 1, &comp->upload_fence, VK_TRUE, UINT64_MAX);

    vkDestroyBuffer(comp->device, staging_buf, NULL);
    vkFreeMemory(comp->device, staging_mem, NULL);

    ph->width = w;
    ph->height = h;
    ph->allocated = true;
    ph->has_content = true;

    /* Write placeholder into all descriptor sets so they start valid */
    for (uint32_t i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++) {
        VkDescriptorImageInfo img_info = {
            .sampler = comp->source_sampler,
            .imageView = ph->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkDescriptorImageInfo lut_info = {
            .sampler = comp->source_sampler,
            .imageView = comp->hdr_lut_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet writes[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->descriptor_sets[i],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &img_info,
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = comp->descriptor_sets[i],
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &lut_info,
            },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);
        comp->sources[i].lut_descriptor_view = comp->hdr_lut_view;
    }

    LOG_I("placeholder texture created (1x1 magenta)");
    return 0;
}

/* ── Pipeline creation ────────────────────────────────────────── */

static int create_pipeline(sbs_compositor_t *comp)
{
    if (create_descriptor_set_layout(comp) != 0)
        return -1;
    if (create_pipeline_layout(comp) != 0)
        return -1;
    if (create_descriptor_pool(comp) != 0)
        return -1;
    if (create_source_sampler(comp) != 0)
        return -1;
    return 0;
}

/* ── Source texture allocation ────────────────────────────────── */

static void destroy_source_texture(sbs_compositor_t *comp, sbs_source_texture_t *tex)
{
    if (!comp->device)
        return;
    destroy_lut_texture(comp, &tex->lut_image, &tex->lut_memory, &tex->lut_view);
    if (!tex->allocated)
        return;
    if (tex->staging_mapped) {
        vkUnmapMemory(comp->device, tex->staging_mem);
    }
    if (tex->staging_mem)
        vkFreeMemory(comp->device, tex->staging_mem, NULL);
    if (tex->staging_buf)
        vkDestroyBuffer(comp->device, tex->staging_buf, NULL);
    if (tex->dmabuf_imported) {
        /* y_buf/dmabuf_memory are aliases of the active cache entry below.
         * The cache owns those Vulkan objects; destroying them here would
         * double-free the same buffer/memory when the cache is cleared. */
        tex->y_buf = VK_NULL_HANDLE;
        tex->dmabuf_memory = VK_NULL_HANDLE;
        tex->dmabuf_imported = false;
    }
    /* Destroy all DMA-BUF buffer cache entries */
    for (uint32_t c = 0; c < SBS_DMABUF_BUF_CACHE_SIZE; c++) {
        struct sbs_dmabuf_buf_cache_entry *e = &tex->dmabuf_buf_cache[c];
        if (!e->valid) continue;
        if (e->is_ycbcr) {
            if (e->ycbcr_view) vkDestroyImageView(comp->device, e->ycbcr_view, NULL);
            if (e->ycbcr_image) vkDestroyImage(comp->device, e->ycbcr_image, NULL);
        } else {
            if (e->buf) vkDestroyBuffer(comp->device, e->buf, NULL);
        }
        if (e->memory) vkFreeMemory(comp->device, e->memory, NULL);
        memset(e, 0, sizeof(*e));
    }
    tex->dmabuf_buf_active_idx = -1;
    if (tex->dmabuf_image_imported) {
        if (tex->dmabuf_image_view)
            vkDestroyImageView(comp->device, tex->dmabuf_image_view, NULL);
        if (tex->dmabuf_image)
            vkDestroyImage(comp->device, tex->dmabuf_image, NULL);
        tex->dmabuf_image_view = VK_NULL_HANDLE;
        tex->dmabuf_image = VK_NULL_HANDLE;
        tex->dmabuf_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        tex->dmabuf_image_imported = false;
    }
    /* Destroy all DMA-BUF image cache entries */
    for (uint32_t c = 0; c < SBS_DMABUF_IMAGE_CACHE_SIZE; c++) {
        struct sbs_dmabuf_image_cache_entry *e = &tex->dmabuf_image_cache[c];
        if (!e->valid) continue;
        if (e->view) vkDestroyImageView(comp->device, e->view, NULL);
        if (e->image) vkDestroyImage(comp->device, e->image, NULL);
        if (e->memory) vkFreeMemory(comp->device, e->memory, NULL);
        memset(e, 0, sizeof(*e));
    }
    tex->dmabuf_image_active_idx = -1;
    if (tex->view)
        vkDestroyImageView(comp->device, tex->view, NULL);
    if (tex->memory)
        vkFreeMemory(comp->device, tex->memory, NULL);
    if (tex->image)
        vkDestroyImage(comp->device, tex->image, NULL);
    memset(tex, 0, sizeof(*tex));
}

static void wait_for_source_texture_idle(sbs_compositor_t *comp)
{
    if (comp && comp->device)
        vkDeviceWaitIdle(comp->device);
}

static void destroy_imported_dmabuf_buffers(sbs_compositor_t *comp, sbs_source_texture_t *tex)
{
    if (!tex->dmabuf_imported)
        return;
    if (tex->y_buf)
        vkDestroyBuffer(comp->device, tex->y_buf, NULL);
    if (tex->dmabuf_memory)
        vkFreeMemory(comp->device, tex->dmabuf_memory, NULL);
    tex->y_buf = VK_NULL_HANDLE;
    tex->uv_buf = VK_NULL_HANDLE;
    tex->dmabuf_memory = VK_NULL_HANDLE;
    tex->dmabuf_imported = false;
}

static void destroy_imported_dmabuf_image(sbs_compositor_t *comp, sbs_source_texture_t *tex)
{
    (void)comp;
    if (!tex->dmabuf_image_imported)
        return;
    /* No fence wait needed — the active cache entry is NOT destroyed here,
     * it stays alive in the cache for reuse. We just clear the "active" alias. */
    tex->dmabuf_image_view = VK_NULL_HANDLE;
    tex->dmabuf_image = VK_NULL_HANDLE;
    tex->dmabuf_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex->dmabuf_modifier = 0;
    tex->dmabuf_image_imported = false;
    tex->dmabuf_image_active_idx = -1;
}

void sbs_compositor_release_source_dmabuf_import(sbs_compositor_t *comp,
                                                 uint32_t slot,
                                                 int cache_idx,
                                                 ino_t inode)
{
    if (!comp || !comp->device || slot >= SBS_MAX_SOURCE_TEXTURES ||
        cache_idx < 0 || cache_idx >= SBS_DMABUF_BUF_CACHE_SIZE)
        return;

    sbs_source_texture_t *tex = &comp->sources[slot];
    struct sbs_dmabuf_buf_cache_entry *e = &tex->dmabuf_buf_cache[cache_idx];
    if (!e->valid || e->inode != inode || e->is_ycbcr ||
        e->drm_format != SBS_DRM_FORMAT_AMLY)
        return;

    vkDeviceWaitIdle(comp->device);

    bool was_active = tex->dmabuf_buf_active_idx == cache_idx && tex->y_buf == e->buf;
    if (e->buf)
        vkDestroyBuffer(comp->device, e->buf, NULL);
    if (e->memory)
        vkFreeMemory(comp->device, e->memory, NULL);
    memset(e, 0, sizeof(*e));

    if (was_active) {
        tex->y_buf = VK_NULL_HANDLE;
        tex->uv_buf = VK_NULL_HANDLE;
        tex->dmabuf_memory = VK_NULL_HANDLE;
        tex->dmabuf_imported = false;
        tex->upload_pending = false;
        tex->has_content = false;
        tex->dmabuf_buf_active_idx = -1;
    }
}

static void bind_source_descriptor_view(sbs_compositor_t *comp, uint32_t slot,
                                        sbs_source_texture_t *tex, VkImageView view)
{
    if (slot >= SBS_MAX_SOURCE_TEXTURES || view == VK_NULL_HANDLE)
        return;
    if (tex->descriptor_view == view)
        return;

    VkDescriptorImageInfo img_info = {
        .sampler = comp->source_sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = comp->descriptor_sets[slot],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &img_info,
    };
    vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
    tex->descriptor_view = view;
}

/* Bind a ycbcr image view to the ycbcr descriptor set for a slot.
 * The sampler is immutable (baked into layout), only the image view changes. */
static void bind_ycbcr_descriptor_view(sbs_compositor_t *comp, uint32_t slot,
                                         sbs_source_texture_t *tex, VkImageView view)
{
    if (slot >= SBS_MAX_SOURCE_TEXTURES || view == VK_NULL_HANDLE ||
        !comp->ycbcr_available)
        return;

    VkDescriptorImageInfo img_info = {
        .sampler = comp->ycbcr_sampler,  /* must match immutable sampler */
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet write = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = comp->ycbcr_descriptor_sets[slot],
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &img_info,
    };
    vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
}

static void bind_lut_descriptor_view(sbs_compositor_t *comp, uint32_t slot,
                                     sbs_source_texture_t *tex, VkImageView view)
{
    VkDescriptorImageInfo lut_info = {
        .sampler = comp->source_sampler,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };

    if (slot >= SBS_MAX_SOURCE_TEXTURES || view == VK_NULL_HANDLE)
        return;

    if (tex->lut_descriptor_view != view) {
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = comp->descriptor_sets[slot],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &lut_info,
        };
        vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
        tex->lut_descriptor_view = view;
    }

    if (comp->ycbcr_available && tex->ycbcr_lut_descriptor_view != view) {
        VkWriteDescriptorSet write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = comp->ycbcr_descriptor_sets[slot],
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &lut_info,
        };
        vkUpdateDescriptorSets(comp->device, 1, &write, 0, NULL);
        tex->ycbcr_lut_descriptor_view = view;
    }
}

static VkImageView resolve_item_lut_view(sbs_compositor_t *comp, uint32_t slot,
                                         const sbs_comp_scene_item_t *item)
{
    sbs_source_texture_t *tex;
    const char *path;

    if (slot >= SBS_MAX_SOURCE_TEXTURES || !item)
        return comp->hdr_lut_view;

    tex = &comp->sources[slot];
    path = item->lut_path;
    if (((item->filter_flags & (SBS_COMP_FILTER_HDR_TO_SDR_LUT | SBS_COMP_FILTER_LUT)) == 0) || !path[0]) {
        if (tex->lut_view != VK_NULL_HANDLE) {
            destroy_lut_texture(comp, &tex->lut_image, &tex->lut_memory, &tex->lut_view);
            tex->lut_size = 0;
            tex->lut_path[0] = '\0';
        }
        return comp->hdr_lut_view;
    }

    if (tex->lut_view != VK_NULL_HANDLE && strcmp(tex->lut_path, path) == 0)
        return tex->lut_view;
    if (tex->lut_view == VK_NULL_HANDLE && tex->lut_size == UINT32_MAX &&
        strcmp(tex->lut_path, path) == 0)
        return comp->hdr_lut_view;

    destroy_lut_texture(comp, &tex->lut_image, &tex->lut_memory, &tex->lut_view);
    tex->lut_size = 0;
    tex->lut_path[0] = '\0';

    uint8_t *data = NULL;
    uint32_t size = 0;
    if (parse_cube_lut(path, &data, &size) != 0) {
        LOG_W("source[%u] custom LUT load failed: %s", slot, path);
        snprintf(tex->lut_path, sizeof(tex->lut_path), "%s", path);
        tex->lut_size = UINT32_MAX;
        return comp->hdr_lut_view;
    }

    if (create_lut_texture_from_rgba8(comp, data, size,
                                      &tex->lut_image,
                                      &tex->lut_memory,
                                      &tex->lut_view) != 0) {
        LOG_W("source[%u] custom LUT upload failed: %s", slot, path);
        free(data);
        snprintf(tex->lut_path, sizeof(tex->lut_path), "%s", path);
        tex->lut_size = UINT32_MAX;
        return comp->hdr_lut_view;
    }

    free(data);
    tex->lut_size = size;
    snprintf(tex->lut_path, sizeof(tex->lut_path), "%s", path);
    LOG_I("source[%u] custom LUT loaded: %s (%u³)", slot, path, size);
    return tex->lut_view;
}

static void prepare_luts_for_items(sbs_compositor_t *comp,
                                   const sbs_comp_scene_item_t *items,
                                   uint32_t count)
{
    for (uint32_t i = 0; i < count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        VkImageView view = resolve_item_lut_view(comp, i, &items[i]);
        bind_lut_descriptor_view(comp, i, &comp->sources[i], view);
    }
}

static void source_alpha_bounds_set_full(sbs_source_texture_t *tex,
                                         uint32_t width,
                                         uint32_t height);

int sbs_compositor_alloc_source(sbs_compositor_t *comp, uint32_t slot,
                                uint32_t width, uint32_t height)
{
    if (slot >= SBS_MAX_SOURCE_TEXTURES)
        return -1;

    sbs_source_texture_t *tex = &comp->sources[slot];

    /* If already the right size with a sampleable RGBA image, nothing to do. */
    if (tex->allocated && tex->width == width && tex->height == height &&
        tex->image != VK_NULL_HANDLE && tex->view != VK_NULL_HANDLE)
        return 0;

    /* Destroy old allocation if dimensions changed */
    if (tex->allocated) {
        wait_for_source_texture_idle(comp);
        destroy_source_texture(comp, tex);
    }

    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;

    /* Create device-local image for sampling */
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkResult res = vkCreateImage(comp->device, &ici, NULL, &tex->image);
    if (res != VK_SUCCESS) {
        LOG_E("source[%u] image create failed: %d", slot, res);
        return -1;
    }

    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(comp->device, tex->image, &mem_req);
    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits, 0);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    res = vkAllocateMemory(comp->device, &mai, NULL, &tex->memory);
    if (res != VK_SUCCESS) {
        LOG_E("source[%u] alloc failed: %d", slot, res);
        vkDestroyImage(comp->device, tex->image, NULL);
        tex->image = VK_NULL_HANDLE;
        return -1;
    }
    vkBindImageMemory(comp->device, tex->image, tex->memory, 0);

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = tex->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1, .layerCount = 1,
        },
    };
    res = vkCreateImageView(comp->device, &ivci, NULL, &tex->view);
    if (res != VK_SUCCESS) {
        LOG_E("source[%u] view create failed: %d", slot, res);
        return -1;
    }

    tex->width = width;
    tex->height = height;
    source_alpha_bounds_set_full(tex, width, height);
    tex->staging_size = 0;
    tex->allocated = true;
    tex->has_content = false;

    LOG_I("source texture slot %u allocated (%ux%u)", slot, width, height);
    return 0;
}

static int sbs_compositor_alloc_direct_source(sbs_compositor_t *comp,
                                              uint32_t slot,
                                              uint32_t width,
                                              uint32_t height)
{
    if (slot >= SBS_MAX_SOURCE_TEXTURES)
        return -1;

    sbs_source_texture_t *tex = &comp->sources[slot];
    if (tex->allocated && tex->width == width && tex->height == height &&
        tex->image == VK_NULL_HANDLE && tex->view == VK_NULL_HANDLE)
        return 0;

    if (tex->allocated) {
        wait_for_source_texture_idle(comp);
        destroy_source_texture(comp, tex);
    }

    tex->width = width;
    tex->height = height;
    source_alpha_bounds_set_full(tex, width, height);
    tex->allocated = true;
    tex->has_content = false;
    tex->dmabuf_buf_active_idx = -1;
    tex->dmabuf_image_active_idx = -1;

    LOG_I("source direct slot %u allocated (%ux%u)", slot, width, height);
    return 0;
}

/* ── NV21 → RGBA conversion (CPU, used for source upload) ─────── */

/* nv21_to_rgba — REMOVED (was CPU NEON NV21→RGBA path) */

/* ── DMA-BUF import for GPU-based YUV→RGBA conversion (CACHED) ── */
static bool ycbcr_direct_import_enabled(void)
{
    static int initialized = 0;
    static bool enabled = false;

    if (!initialized) {
        const char *env = getenv("SBS_ENABLE_YCBCR_IMPORT");
        enabled = env && *env && strcmp(env, "0") != 0;
        if (!enabled) {
            LOG_I("YCbCr direct import disabled; using compute YUV->RGBA path");
        }
        initialized = 1;
    }
    return enabled;
}

static bool source_drm_format_is_rgba(uint32_t drm_format)
{
    return drm_format == DRM_FORMAT_ABGR8888;
}

static void source_alpha_bounds_set_full(sbs_source_texture_t *tex,
                                         uint32_t width,
                                         uint32_t height)
{
    if (!tex)
        return;
    tex->alpha_x = 0;
    tex->alpha_y = 0;
    tex->alpha_w = width;
    tex->alpha_h = height;
    tex->alpha_rect_opaque = false;
}

static void source_alpha_bounds_scan_rgba_fd(int fd,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t stride,
                                             uint32_t *out_x,
                                             uint32_t *out_y,
                                             uint32_t *out_w,
                                             uint32_t *out_h,
                                             bool *out_rect_opaque)
{
    uint32_t min_x = width;
    uint32_t min_y = height;
    uint32_t max_x = 0;
    uint32_t max_y = 0;
    uint64_t nonzero_count = 0;
    uint64_t opaque_count = 0;
    size_t size;
    const uint8_t *ptr;

    if (out_x) *out_x = 0;
    if (out_y) *out_y = 0;
    if (out_w) *out_w = width;
    if (out_h) *out_h = height;
    if (out_rect_opaque) *out_rect_opaque = false;
    if (fd < 0 || width == 0 || height == 0 || stride < width * 4u)
        return;

    size = (size_t)stride * height;
    ptr = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
        return;

    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = ptr + (size_t)y * stride;
        for (uint32_t x = 0; x < width; x++) {
            uint8_t alpha = row[(size_t)x * 4u + 3u];
            if (alpha == 0)
                continue;
            nonzero_count++;
            if (alpha == 255)
                opaque_count++;
            if (x < min_x) min_x = x;
            if (y < min_y) min_y = y;
            if (x > max_x) max_x = x;
            if (y > max_y) max_y = y;
        }
    }

    munmap((void *)ptr, size);

    if (min_x > max_x || min_y > max_y) {
        if (out_x) *out_x = 0;
        if (out_y) *out_y = 0;
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }

    if (out_x) *out_x = min_x;
    if (out_y) *out_y = min_y;
    if (out_w) *out_w = max_x - min_x + 1u;
    if (out_h) *out_h = max_y - min_y + 1u;
    if (out_rect_opaque) {
        uint64_t bounds_area = (uint64_t)(max_x - min_x + 1u) *
                               (uint64_t)(max_y - min_y + 1u);
        *out_rect_opaque = nonzero_count == bounds_area &&
                           opaque_count == bounds_area;
    }
}

static bool source_texture_matches_item(const sbs_source_texture_t *tex,
                                        const sbs_comp_scene_item_t *item)
{
    return tex && item && item->source_id[0] != '\0' &&
           tex->source_id[0] != '\0' &&
           strcmp(tex->source_id, item->source_id) == 0;
}

static int import_dmabuf_source(sbs_compositor_t *comp, uint32_t slot,
                                int fd, uint32_t width, uint32_t height,
                                uint32_t drm_format,
                                const uint32_t plane_offset[2],
                                const uint32_t plane_stride[2],
                                bool force_compute)
{
    sbs_source_texture_t *tex = &comp->sources[slot];

    bool rgba_input = source_drm_format_is_rgba(drm_format);

    /* RGBA only needs transfer copy; YUV requires ycbcr or compute conversion. */
    if (!rgba_input && comp->compute_pipeline == VK_NULL_HANDLE && !comp->ycbcr_available)
        return -1;

    /* Get DMA-BUF identity via inode */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        LOG_W("fstat(fd=%d) failed for DMA-BUF buffer import", fd);
        return -1;
    }
    ino_t inode = st.st_ino;

    uint32_t y_stride = plane_stride[0] ? plane_stride[0] : width;
    uint32_t uv_stride = plane_stride[1] ? plane_stride[1] : width;
    uint32_t uv_offset = plane_offset[1] ? plane_offset[1] : width * height;

    if (drm_format == DRM_FORMAT_P010) {
        y_stride = plane_stride[0] ? plane_stride[0] : width * 2;
        uv_stride = plane_stride[1] ? plane_stride[1] : width * 2;
        uv_offset = plane_offset[1] ? plane_offset[1] : y_stride * height;
    } else if (drm_format == SBS_DRM_FORMAT_AMLY) {
        y_stride = plane_stride[0] ? plane_stride[0] : ((width + 1u) / 2u) * 5u;
        uv_stride = 0;
        uv_offset = 0;
    } else if (rgba_input) {
        y_stride = plane_stride[0] ? plane_stride[0] : width * 4u;
        uv_stride = 0;
        uv_offset = 0;
    }

    tex->dmabuf_buf_frame_counter++;

    /* Cache lookup: same inode = same backing buffer */
    for (int i = 0; i < SBS_DMABUF_BUF_CACHE_SIZE; i++) {
        struct sbs_dmabuf_buf_cache_entry *e = &tex->dmabuf_buf_cache[i];
        if (!e->valid) continue;
        if (e->inode == inode && e->width == width && e->height == height &&
            e->drm_format == drm_format) {
            if (force_compute && e->is_ycbcr)
                continue;
            /* Cache hit */
            e->age = tex->dmabuf_buf_frame_counter;
            if (e->is_ycbcr) {
                /* YCbCr path: NV12 sampled directly, no compute needed */
                tex->ycbcr_imported = true;
                tex->ycbcr_active_view = e->ycbcr_view;
                tex->dmabuf_imported = false;
                tex->upload_pending = false;
            } else {
                /* Legacy compute path */
                tex->y_buf = e->buf;
                tex->dmabuf_memory = e->memory;
                tex->y_stride = e->y_stride;
                tex->uv_stride = e->uv_stride;
                tex->uv_offset = e->uv_offset;
                tex->alpha_x = e->alpha_x;
                tex->alpha_y = e->alpha_y;
                tex->alpha_w = e->alpha_w;
                tex->alpha_h = e->alpha_h;
                tex->alpha_rect_opaque = e->alpha_rect_opaque;
                tex->drm_format = drm_format;
                tex->dmabuf_imported = true;
                tex->ycbcr_imported = false;
                tex->upload_pending = true;
            }
            tex->dmabuf_buf_active_idx = i;
            return 0;
        }
    }

    /* Cache miss — find free or evict LRU */
    int target_idx = -1;
    uint32_t oldest_age = UINT32_MAX;

    /* First pass: find a free slot */
    for (int i = 0; i < SBS_DMABUF_BUF_CACHE_SIZE; i++) {
        if (!tex->dmabuf_buf_cache[i].valid) {
            target_idx = i;
            break;
        }
    }

    /* Second pass: if no free slot, find the oldest entry */
    if (target_idx < 0) {
        for (int i = 0; i < SBS_DMABUF_BUF_CACHE_SIZE; i++) {
            if (tex->dmabuf_buf_cache[i].age < oldest_age) {
                oldest_age = tex->dmabuf_buf_cache[i].age;
                target_idx = i;
            }
        }
    }

    struct sbs_dmabuf_buf_cache_entry *ce = &tex->dmabuf_buf_cache[target_idx];

    /* Evict old entry */
    if (ce->valid) {
        wait_for_source_texture_idle(comp);
        if (ce->is_ycbcr) {
            if (ce->ycbcr_view) vkDestroyImageView(comp->device, ce->ycbcr_view, NULL);
            if (ce->ycbcr_image) vkDestroyImage(comp->device, ce->ycbcr_image, NULL);
        } else {
            if (ce->buf) vkDestroyBuffer(comp->device, ce->buf, NULL);
        }
        if (ce->memory) vkFreeMemory(comp->device, ce->memory, NULL);
        memset(ce, 0, sizeof(*ce));
    }

    /* Clean up legacy active alias (but don't destroy cache entries) */
    tex->y_buf = VK_NULL_HANDLE;
    tex->dmabuf_memory = VK_NULL_HANDLE;
    tex->dmabuf_imported = false;
    tex->ycbcr_imported = false;
    tex->ycbcr_active_view = VK_NULL_HANDLE;

    /* Size validation (cache miss only — skip lseek on cache hit for perf) */
    off_t total_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    uint32_t expected_size;
    if (drm_format == DRM_FORMAT_P010) {
        expected_size = uv_offset + uv_stride * (height / 2);
    } else if (drm_format == SBS_DRM_FORMAT_AMLY) {
        expected_size = y_stride * height;
    } else if (rgba_input) {
        expected_size = y_stride * height;
    } else {
        expected_size = width * height * 3 / 2;
    }
    if (total_size < (off_t)expected_size) {
        LOG_W("DMA-BUF fd size %ld < expected %u (fmt=%.4s)",
              (long)total_size, expected_size,
              (const char *)&drm_format);
        return -1;
    }

    /* Try YCbCr VkImage import path first (NV12 only, not P010 yet) */
    if (!force_compute && ycbcr_direct_import_enabled() && comp->ycbcr_available &&
        (drm_format == DRM_FORMAT_NV12 || drm_format == DRM_FORMAT_NV21)) {
        int dup_fd = dup(fd);
        if (dup_fd < 0) {
            LOG_W("dup(fd) failed for ycbcr import");
            return -1;
        }

        /* Create NV12 VkImage with DRM format modifier */
        VkSubresourceLayout plane_layouts[2] = {
            {   /* Y plane */
                .offset = 0,
                .rowPitch = y_stride,
                .size = 0,
                .depthPitch = 0,
                .arrayPitch = 0,
            },
            {   /* UV plane */
                .offset = uv_offset,
                .rowPitch = uv_stride,
                .size = 0,
                .depthPitch = 0,
                .arrayPitch = 0,
            },
        };

        VkImageDrmFormatModifierExplicitCreateInfoEXT drm_ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
            .drmFormatModifier = 0,  /* DRM_FORMAT_MOD_LINEAR */
            .drmFormatModifierPlaneCount = 2,
            .pPlaneLayouts = plane_layouts,
        };

        VkExternalMemoryImageCreateInfo ext_ci = {
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = &drm_ci,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        };

        VkImageCreateInfo ici = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = &ext_ci,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .extent = { width, height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        VkImage ycbcr_image = VK_NULL_HANDLE;
        VkResult res = vkCreateImage(comp->device, &ici, NULL, &ycbcr_image);
        if (res != VK_SUCCESS) {
            LOG_W("vkCreateImage for ycbcr NV12 failed: %d (falling back to compute)", res);
            close(dup_fd);
            goto try_compute_path;
        }

        VkMemoryRequirements mem_req;
        vkGetImageMemoryRequirements(comp->device, ycbcr_image, &mem_req);

        VkImportMemoryFdInfoKHR import_info = {
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
            .fd = dup_fd,
        };

        VkMemoryDedicatedAllocateInfo dedicated = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
            .pNext = &import_info,
            .image = ycbcr_image,
        };

        uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mem_type == UINT32_MAX)
            mem_type = find_memory_type(comp, mem_req.memoryTypeBits, 0);

        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicated,
            .allocationSize = mem_req.size,
            .memoryTypeIndex = mem_type,
        };

        VkDeviceMemory ycbcr_mem = VK_NULL_HANDLE;
        res = vkAllocateMemory(comp->device, &mai, NULL, &ycbcr_mem);
        if (res != VK_SUCCESS) {
            LOG_W("vkAllocateMemory for ycbcr NV12 failed: %d", res);
            vkDestroyImage(comp->device, ycbcr_image, NULL);
            /* fd consumed by import_info on failure? Close if still valid. */
            goto try_compute_path;
        }

        res = vkBindImageMemory(comp->device, ycbcr_image, ycbcr_mem, 0);
        if (res != VK_SUCCESS) {
            LOG_W("vkBindImageMemory for ycbcr failed: %d", res);
            vkDestroyImage(comp->device, ycbcr_image, NULL);
            vkFreeMemory(comp->device, ycbcr_mem, NULL);
            goto try_compute_path;
        }

        /* Create image view with ycbcr conversion */
        VkSamplerYcbcrConversionInfo ycbcr_view_info = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
            .conversion = comp->ycbcr_conversion,
        };

        VkImageViewCreateInfo ivci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &ycbcr_view_info,
            .image = ycbcr_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };

        VkImageView ycbcr_view = VK_NULL_HANDLE;
        res = vkCreateImageView(comp->device, &ivci, NULL, &ycbcr_view);
        if (res != VK_SUCCESS) {
            LOG_W("vkCreateImageView for ycbcr failed: %d", res);
            vkDestroyImage(comp->device, ycbcr_image, NULL);
            vkFreeMemory(comp->device, ycbcr_mem, NULL);
            goto try_compute_path;
        }

        /* Store in cache as ycbcr entry */
        ce->ycbcr_image = ycbcr_image;
        ce->ycbcr_view = ycbcr_view;
        ce->memory = ycbcr_mem;
        ce->buf = VK_NULL_HANDLE;
        ce->inode = inode;
        ce->width = width;
        ce->height = height;
        ce->drm_format = drm_format;
        ce->y_stride = y_stride;
        ce->uv_stride = uv_stride;
        ce->uv_offset = uv_offset;
        ce->age = tex->dmabuf_buf_frame_counter;
        ce->valid = true;
        ce->is_ycbcr = true;

        /* Set active alias */
        tex->ycbcr_imported = true;
        tex->ycbcr_active_view = ycbcr_view;
        tex->dmabuf_imported = false;
        tex->upload_pending = false;  /* no compute upload needed! */
        tex->dmabuf_buf_active_idx = target_idx;
        {
            static uint32_t ycbcr_log_counter = 0;
            if (ycbcr_log_counter++ % 60 == 0) {
                LOG_I("YCbCr NV12 import slot=%u cache[%d] %ux%u inode=%lu (zero compute!)",
                      slot, target_idx, width, height, (unsigned long)inode);
            }
        }
        return 0;
    }

try_compute_path:
    /* Fallback: compute shader YUV→RGBA via VkBuffer import */
    if (!rgba_input && comp->compute_pipeline == VK_NULL_HANDLE) {
        LOG_E("FATAL: ycbcr import failed and compute pipeline not available");
        return -1;
    }

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup(fd),
    };
    if (import_info.fd < 0) {
        LOG_W("dup(fd) failed for DMA-BUF import");
        return -1;
    }

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = expected_size,
        .usage = rgba_input
            ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
            : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    };
    VkBuffer dummy_buf;
    VkResult res = vkCreateBuffer(comp->device, &bci, NULL, &dummy_buf);
    if (res != VK_SUCCESS) {
        close(import_info.fd);
        return -1;
    }
    VkMemoryRequirements mem_req;
    vkGetBufferMemoryRequirements(comp->device, dummy_buf, &mem_req);
    vkDestroyBuffer(comp->device, dummy_buf, NULL);

    uint32_t mem_type = find_memory_type(comp, mem_req.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        mem_type = find_memory_type(comp, mem_req.memoryTypeBits, 0);

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = expected_size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    res = vkAllocateMemory(comp->device, &mai, NULL, &new_memory);
    if (res != VK_SUCCESS) {
        close(import_info.fd);
        LOG_W("vkAllocateMemory for DMA-BUF import failed: %d", res);
        return -1;
    }

    VkBuffer new_buf = VK_NULL_HANDLE;
    bci.size = expected_size;
    res = vkCreateBuffer(comp->device, &bci, NULL, &new_buf);
    if (res != VK_SUCCESS) goto fail;
    res = vkBindBufferMemory(comp->device, new_buf, new_memory, 0);
    if (res != VK_SUCCESS) goto fail;

    /* Store in cache */
    ce->buf = new_buf;
    ce->memory = new_memory;
    ce->inode = inode;
    ce->width = width;
    ce->height = height;
    ce->drm_format = drm_format;
    ce->y_stride = y_stride;
    ce->uv_stride = uv_stride;
    ce->uv_offset = uv_offset;
    if (rgba_input) {
        source_alpha_bounds_scan_rgba_fd(fd, width, height, y_stride,
                                         &ce->alpha_x, &ce->alpha_y,
                                         &ce->alpha_w, &ce->alpha_h,
                                         &ce->alpha_rect_opaque);
        if (ce->alpha_w != width || ce->alpha_h != height ||
            ce->alpha_x != 0 || ce->alpha_y != 0) {
            LOG_I("RGBA alpha bounds slot=%u cache[%d] %ux%u+%u+%u from %ux%u",
                  slot, target_idx, ce->alpha_w, ce->alpha_h,
                  ce->alpha_x, ce->alpha_y, width, height);
        }
    } else {
        ce->alpha_x = 0;
        ce->alpha_y = 0;
        ce->alpha_w = width;
        ce->alpha_h = height;
        ce->alpha_rect_opaque = true;
    }
    ce->age = tex->dmabuf_buf_frame_counter;
    ce->valid = true;

    /* Set active alias */
    tex->y_buf = new_buf;
    tex->dmabuf_memory = new_memory;
    tex->y_stride = y_stride;
    tex->uv_stride = uv_stride;
    tex->uv_offset = uv_offset;
    tex->alpha_x = ce->alpha_x;
    tex->alpha_y = ce->alpha_y;
    tex->alpha_w = ce->alpha_w;
    tex->alpha_h = ce->alpha_h;
    tex->alpha_rect_opaque = ce->alpha_rect_opaque;
    tex->drm_format = drm_format;
    tex->dmabuf_imported = true;
    tex->dmabuf_buf_active_idx = target_idx;
    tex->upload_pending = true;
    {
        static uint32_t import_log_counter = 0;
        if (import_log_counter++ % 60 == 0) {
            LOG_I("DMA-BUF import slot=%u cache[%d] %ux%u fmt=%.4s inode=%lu",
                  slot, target_idx, width, height, (const char *)&drm_format,
                  (unsigned long)inode);
        }
    }
    return 0;

fail:
    if (new_buf) vkDestroyBuffer(comp->device, new_buf, NULL);
    if (new_memory) vkFreeMemory(comp->device, new_memory, NULL);
    return -1;
}

/* ── DMA-BUF image import for AFBC RGBA direct sampling (CACHED) ─ */
static int import_dmabuf_source_image(sbs_compositor_t *comp, uint32_t slot,
                                       int fd, uint32_t width, uint32_t height,
                                       uint64_t modifier)
{
    sbs_source_texture_t *tex = &comp->sources[slot];

    /* Get DMA-BUF identity via inode */
    struct stat st;
    if (fstat(fd, &st) < 0) {
        LOG_W("fstat(fd=%d) failed for DMA-BUF image import", fd);
        return -1;
    }
    ino_t inode = st.st_ino;

    tex->dmabuf_image_frame_counter++;

    /* Cache lookup: same inode + dimensions + modifier = same buffer, reuse */
    for (int i = 0; i < SBS_DMABUF_IMAGE_CACHE_SIZE; i++) {
        struct sbs_dmabuf_image_cache_entry *e = &tex->dmabuf_image_cache[i];
        if (!e->valid) continue;
        if (e->inode == inode && e->width == width && e->height == height &&
            e->modifier == modifier) {
            /* Cache hit — just point the active alias at this entry */
            e->age = tex->dmabuf_image_frame_counter;
            tex->dmabuf_image = e->image;
            tex->dmabuf_image_view = e->view;
            tex->dmabuf_image_layout = e->layout;
            tex->dmabuf_modifier = modifier;
            tex->dmabuf_image_imported = true;
            tex->dmabuf_image_active_idx = i;
            tex->upload_pending = false;
            return 0;
        }
    }

    /* Cache miss — find a free slot or evict LRU */
    int target_idx = -1;
    uint32_t oldest_age = UINT32_MAX;
    for (int i = 0; i < SBS_DMABUF_IMAGE_CACHE_SIZE; i++) {
        if (!tex->dmabuf_image_cache[i].valid) {
            target_idx = i;
            break;
        }
        if (tex->dmabuf_image_cache[i].age < oldest_age) {
            oldest_age = tex->dmabuf_image_cache[i].age;
            target_idx = i;
        }
    }

    struct sbs_dmabuf_image_cache_entry *ce = &tex->dmabuf_image_cache[target_idx];

    /* Evict old entry — must wait for all renders using it to complete */
    if (ce->valid) {
        wait_for_source_texture_idle(comp);
        if (ce->view) vkDestroyImageView(comp->device, ce->view, NULL);
        if (ce->image) vkDestroyImage(comp->device, ce->image, NULL);
        if (ce->memory) vkFreeMemory(comp->device, ce->memory, NULL);
        memset(ce, 0, sizeof(*ce));
    }

    /* Create new VkImage + import DMA-BUF memory */
    VkFormat format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;

    VkImageDrmFormatModifierListCreateInfoEXT drm_mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1,
        .pDrmFormatModifiers = &modifier,
    };

    VkExternalMemoryImageCreateInfo emi = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &drm_mod,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &emi,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { width, height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    VkImage new_image = VK_NULL_HANDLE;
    VkResult res = vkCreateImage(comp->device, &ici, NULL, &new_image);
    if (res != VK_SUCCESS) {
        LOG_W("DMA-BUF image import vkCreateImage failed: %d", res);
        return -1;
    }

    VkMemoryDedicatedRequirements ded_reqs = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,
    };
    VkMemoryRequirements2 mem_req2 = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
        .pNext = &ded_reqs,
    };
    VkImageMemoryRequirementsInfo2 img_mem_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = new_image,
    };
    vkGetImageMemoryRequirements2(comp->device, &img_mem_info, &mem_req2);

    VkImportMemoryFdInfoKHR import_info = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
        .fd = dup(fd),
    };
    if (import_info.fd < 0) {
        LOG_W("dup(fd) failed for DMA-BUF image import");
        vkDestroyImage(comp->device, new_image, NULL);
        return -1;
    }

    uint32_t mem_type = find_memory_type(comp, mem_req2.memoryRequirements.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type == UINT32_MAX)
        mem_type = find_memory_type(comp, mem_req2.memoryRequirements.memoryTypeBits, 0);

    VkMemoryDedicatedAllocateInfo ded_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = new_image,
    };

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_info,
        .allocationSize = mem_req2.memoryRequirements.size,
        .memoryTypeIndex = mem_type,
    };

    if (ded_reqs.requiresDedicatedAllocation || ded_reqs.prefersDedicatedAllocation) {
        ded_alloc.pNext = import_info.pNext;
        import_info.pNext = &ded_alloc;
    }

    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    res = vkAllocateMemory(comp->device, &mai, NULL, &new_memory);
    if (res != VK_SUCCESS) {
        close(import_info.fd);
        vkDestroyImage(comp->device, new_image, NULL);
        LOG_W("vkAllocateMemory for DMA-BUF image import failed: %d", res);
        return -1;
    }

    res = vkBindImageMemory(comp->device, new_image, new_memory, 0);
    if (res != VK_SUCCESS) {
        vkFreeMemory(comp->device, new_memory, NULL);
        vkDestroyImage(comp->device, new_image, NULL);
        LOG_W("vkBindImageMemory for DMA-BUF image import failed: %d", res);
        return -1;
    }

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = new_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    VkImageView new_view = VK_NULL_HANDLE;
    res = vkCreateImageView(comp->device, &ivci, NULL, &new_view);
    if (res != VK_SUCCESS) {
        vkFreeMemory(comp->device, new_memory, NULL);
        vkDestroyImage(comp->device, new_image, NULL);
        LOG_W("vkCreateImageView for DMA-BUF image import failed: %d", res);
        return -1;
    }

    /* Store in cache */
    ce->image = new_image;
    ce->memory = new_memory;
    ce->view = new_view;
    ce->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    ce->inode = inode;
    ce->width = width;
    ce->height = height;
    ce->modifier = modifier;
    ce->age = tex->dmabuf_image_frame_counter;
    ce->valid = true;

    /* Set active alias */
    tex->dmabuf_image = new_image;
    tex->dmabuf_image_view = new_view;
    tex->dmabuf_image_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    tex->dmabuf_modifier = modifier;
    tex->dmabuf_image_imported = true;
    tex->dmabuf_image_active_idx = target_idx;
    tex->upload_pending = false;

    LOG_D("DMA-BUF image import slot=%u cache[%d] %ux%u modifier=0x%lx inode=%lu",
          slot, target_idx, width, height, (unsigned long)modifier, (unsigned long)inode);
    return 0;
}

/* ── Upload YUV frame data into a source texture slot ──────────── */
static bool native_canvas_can_direct_upload_format(sbs_compositor_t *comp,
                                                   uint32_t drm_format)
{
    if (!comp || !comp->native_canvas.initialized ||
        comp->native_p010_direct_pipeline_layout == VK_NULL_HANDLE)
        return false;
    sbs_export_color_mode_t mode = comp->native_canvas.entries[0].color_mode;
    if (mode == SBS_EXPORT_COLOR_HDR10) {
        if (drm_format == DRM_FORMAT_P010)
            return comp->native_p010_direct_pipeline != VK_NULL_HANDLE;
        if (drm_format == DRM_FORMAT_NV12 || drm_format == DRM_FORMAT_NV21)
            return comp->native_yuv8_to_p010_pipeline != VK_NULL_HANDLE &&
                !ycbcr_direct_import_enabled();
        if (drm_format == SBS_DRM_FORMAT_AMLY)
            return comp->native_amly_to_p010_pipeline != VK_NULL_HANDLE;
    } else {
        if (drm_format == DRM_FORMAT_P010)
            return comp->native_p010_to_nv21_pipeline != VK_NULL_HANDLE;
        if (drm_format == DRM_FORMAT_NV12 || drm_format == DRM_FORMAT_NV21)
            return comp->native_yuv8_to_nv21_pipeline != VK_NULL_HANDLE &&
                !ycbcr_direct_import_enabled();
        if (drm_format == SBS_DRM_FORMAT_AMLY)
            return comp->native_amly_to_nv21_pipeline != VK_NULL_HANDLE;
    }
    return false;
}

int sbs_compositor_upload_source(sbs_compositor_t *comp, uint32_t slot,
                                  const char *source_id,
                                  int fd, int fd2, uint32_t width, uint32_t height,
                                  uint32_t drm_format, uint64_t drm_modifier,
                                  const uint32_t plane_offset[2],
                                  const uint32_t plane_stride[2],
                                  bool force_sampleable,
                                  uint32_t filter_flags)
{
    (void)fd2;
    if (slot >= SBS_MAX_SOURCE_TEXTURES || fd < 0)
        return -1;

    if (drm_format == DRM_FORMAT_ABGR2101010 && drm_modifier != 0) {
        LOG_W("source[%u] AFBC RGBA input ignored; vfmcap must provide linear NV12/P010", slot);
        return -1;
    }

    sbs_source_texture_t *tex = &comp->sources[slot];
    if (tex->allocated && source_id && source_id[0] && tex->source_id[0] &&
        strcmp(tex->source_id, source_id) != 0) {
        LOG_I("source[%u] reassigned from %s to %s; clearing stale texture",
              slot, tex->source_id, source_id);
        wait_for_source_texture_idle(comp);
        destroy_source_texture(comp, tex);
    }

    bool direct_upload = !force_sampleable &&
        native_canvas_can_direct_upload_format(comp, drm_format);
    if (direct_upload) {
        if (sbs_compositor_alloc_direct_source(comp, slot, width, height) != 0)
            return -1;
    } else if (sbs_compositor_alloc_source(comp, slot, width, height) != 0) {
        return -1;
    }

    if (source_id)
        snprintf(tex->source_id, sizeof(tex->source_id), "%s", source_id);
    else
        tex->source_id[0] = '\0';
    tex->upload_filter_flags = filter_flags;

    /* GPU DMA-BUF buffer import for linear YUV formats */
    if (import_dmabuf_source(comp, slot, fd, width, height,
                             drm_format, plane_offset, plane_stride,
                             force_sampleable) == 0) {
        destroy_imported_dmabuf_image(comp, tex);
        if (tex->ycbcr_imported) {
            /* YCbCr path: bind ycbcr descriptor, skip compute upload */
            bind_ycbcr_descriptor_view(comp, slot, tex, tex->ycbcr_active_view);
        } else {
            /* Compute path: bind regular descriptor (intermediate RGBA image) */
            bind_source_descriptor_view(comp, slot, tex, tex->view);
        }
        /* Direct/Ycbcr imports are immediately renderable. Sampleable RGBA
         * imports become renderable after native_record_source_uploads(). */
        if (!tex->upload_pending || tex->image == VK_NULL_HANDLE ||
            tex->view == VK_NULL_HANDLE) {
            tex->has_content = true;
        }
        return 0;
    }

    /* CPU fallback REMOVED — GPU DMA-BUF import is mandatory. */
    LOG_E("FATAL: source[%u] linear YUV GPU import failed. No CPU fallback. fd=%d %ux%u fmt=0x%x",
          slot, fd, width, height, drm_format);
    return -1;
}

/* ── Shader loading + graphics pipeline ───────────────────────── */

int sbs_compositor_load_shaders(sbs_compositor_t *comp, const char *shader_dir)
{
    /* Load compute shader for DMA-BUF YUV import (non-fatal if missing) */
    create_compute_pipeline(comp, shader_dir);

    /* Load export compute shader for GPU RGBA→NV21 conversion (non-fatal) */
    if (create_export_compute_pipeline(comp, shader_dir) == 0) {
        create_nv21_export_buffers(comp);
    }

    create_p010_export_buffers(comp);

    /* Load destination-oriented export pipeline (sampler-based, non-fatal) */
    create_dest_export_pipeline(comp, shader_dir);

    /* P010 compute conversion pipeline needs p010_dest_export_pipeline
     * (created by create_dest_export_pipeline above) */
    create_p010_conv_pipeline(comp);

    create_native_yuv_composite_pipeline(comp, shader_dir);
    create_native_p010_direct_pipeline(comp, shader_dir);
    create_native_downscale_pipeline(comp, shader_dir);

    /* Create YCbCr conversion pipeline (non-fatal) */
    create_ycbcr_pipeline(comp);

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;

    char path[512];
    snprintf(path, sizeof(path), "%s/composite.vert.spv", shader_dir);
    vert = load_shader(comp, path);
    if (vert == VK_NULL_HANDLE)
        return -1;

    snprintf(path, sizeof(path), "%s/composite.frag.spv", shader_dir);
    frag = load_shader(comp, path);
    if (frag == VK_NULL_HANDLE) {
        vkDestroyShaderModule(comp->device, vert, NULL);
        return -1;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vert,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = frag,
            .pName = "main",
        },
    };

    VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };

    /* OPT-4: Dynamic viewport/scissor — avoid baking dimensions into the
     * pipeline so we never need to recreate on resolution changes.
     * We set pViewports/pScissors = NULL with count=1; actual values are
     * provided at draw time via vkCmdSetViewport/vkCmdSetScissor. */
    VkPipelineViewportStateCreateInfo viewport_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = NULL,
        .scissorCount = 1,
        .pScissors = NULL,
    };

    VkDynamicState dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };

    VkPipelineRasterizationStateCreateInfo rasterizer = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };

    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };

    VkPipelineColorBlendAttachmentState blend_att = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };

    VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend_att,
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisample,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic_state,
        .layout = comp->pipeline_layout,
        .renderPass = comp->render_pass,
        .subpass = 0,
    };

    VkResult res = vkCreateGraphicsPipelines(comp->device, comp->pipeline_cache, 1, &gpci,
                                              NULL, &comp->pipeline);

    /* Create ycbcr graphics pipeline with same shaders before destroying modules */
    if (res == VK_SUCCESS && comp->ycbcr_pipeline_layout != VK_NULL_HANDLE) {
        create_ycbcr_graphics_pipeline(comp, vert, frag);
    }

    vkDestroyShaderModule(comp->device, vert, NULL);
    vkDestroyShaderModule(comp->device, frag, NULL);

    if (res != VK_SUCCESS) {
        LOG_E("vkCreateGraphicsPipelines failed: %d", res);
        return -1;
    }

    LOG_I("graphics pipeline created");
    return 0;
}

/* ── Frame rendering ──────────────────────────────────────────── */

/* Push constant structure — must match the shader layout exactly.
 * Total size: 176 bytes (SBS_PUSH_CONSTANT_SIZE). */
typedef struct sbs_push_constants {
    float transform[16];       /*  0: mat4 (64 bytes) */
    float crop[4];             /* 64: vec4 (16 bytes) */
    float opacity;             /* 80: float */
    float pad0;                /* 84: float */
    float source_size[2];      /* 88: vec2 (8 bytes) */
    float tint[4];             /* 96: vec4 (16 bytes) */
    uint32_t filter_flags;     /* 112: uint */
    float filter_pad[3];       /* 116: padding (12 bytes) */
    float filter_params_a[4];  /* 128: vec4 (16 bytes) */
    float filter_params_b[4];  /* 144: vec4 (16 bytes) */
    uint32_t has_texture;      /* 160: uint (1 = sample texture, 0 = tint only) */
    uint32_t _pad1[3];         /* 164: padding to 176 bytes */
} sbs_push_constants_t;

_Static_assert(sizeof(sbs_push_constants_t) == SBS_PUSH_CONSTANT_SIZE,
               "push constants must be exactly SBS_PUSH_CONSTANT_SIZE bytes");

static uint32_t source_texture_slot_for_item(const sbs_comp_scene_item_t *items,
                                             uint32_t count,
                                             uint32_t item_idx)
{
    if (!items || item_idx >= count)
        return item_idx;

    const sbs_comp_scene_item_t *item = &items[item_idx];
    for (uint32_t i = 0; i < item_idx && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *prev = &items[i];
        if (prev->visible && prev->frame_slot &&
            strcmp(prev->source_id, item->source_id) == 0) {
            return i;
        }
    }
    return item_idx;
}

static uint32_t source_texture_slot_for_item_with_reference(const sbs_comp_scene_item_t *items,
                                                            uint32_t count,
                                                            uint32_t item_idx,
                                                            const sbs_comp_scene_item_t *ref_items,
                                                            uint32_t ref_count)
{
    if (!items || item_idx >= count)
        return item_idx;
    if (!ref_items || ref_count == 0)
        return source_texture_slot_for_item(items, count, item_idx);

    const sbs_comp_scene_item_t *item = &items[item_idx];
    for (uint32_t i = 0; i < ref_count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *ref = &ref_items[i];
        if (ref->visible && ref->frame_slot &&
            strcmp(ref->source_id, item->source_id) == 0) {
            return i;
        }
    }

    uint32_t slot = ref_count;
    for (uint32_t i = 0; i < item_idx && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *prev = &items[i];
        bool existed_in_ref = false;
        if (!prev->visible || !prev->frame_slot)
            continue;
        for (uint32_t j = 0; j < ref_count && j < SBS_MAX_SOURCE_TEXTURES; j++) {
            const sbs_comp_scene_item_t *ref = &ref_items[j];
            if (ref->visible && ref->frame_slot &&
                strcmp(ref->source_id, prev->source_id) == 0) {
                existed_in_ref = true;
                break;
            }
        }
        if (!existed_in_ref)
            slot++;
    }
    return slot;
}

static uint32_t native_source_texture_slot_for_item(sbs_compositor_t *comp,
                                                    const sbs_comp_scene_item_t *items,
                                                    uint32_t count,
                                                    uint32_t item_idx,
                                                    const sbs_comp_scene_item_t *ref_items,
                                                    uint32_t ref_count)
{
    uint32_t slot = source_texture_slot_for_item_with_reference(items, count, item_idx,
                                                               ref_items, ref_count);
    const sbs_comp_scene_item_t *item;

    if (!comp || !items || item_idx >= count || slot >= SBS_MAX_SOURCE_TEXTURES)
        return slot;

    item = &items[item_idx];
    if (item->source_id[0] == '\0')
        return slot;
    if (comp->sources[slot].source_id[0] == '\0' ||
        strcmp(comp->sources[slot].source_id, item->source_id) == 0)
        return slot;

    for (uint32_t i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++) {
        if (strcmp(comp->sources[i].source_id, item->source_id) == 0)
            return i;
    }
    return slot;
}

static void render_items(sbs_compositor_t *comp, VkCommandBuffer cb,
                         const sbs_comp_scene_item_t *items, uint32_t count,
                         float opacity_scale,
                         const sbs_comp_scene_item_t *ref_items,
                         uint32_t ref_count)
{
    for (uint32_t i = 0; i < count; i++) {
        const sbs_comp_scene_item_t *item = &items[i];
        uint32_t slot = native_source_texture_slot_for_item(comp, items, count, i,
                                                            ref_items, ref_count);
        if (!item->visible)
            continue;

        sbs_push_constants_t pc;
        memset(&pc, 0, sizeof(pc));
        memcpy(pc.transform, item->transform, sizeof(pc.transform));
        memcpy(pc.crop, item->crop, sizeof(pc.crop));
        pc.opacity = item->opacity * opacity_scale;
        pc.source_size[0] = (float)comp->width;
        pc.source_size[1] = (float)comp->height;
        memcpy(pc.tint, item->tint, sizeof(pc.tint));
        pc.filter_flags = item->filter_flags;
        memcpy(pc.filter_params_a, item->filter_params, sizeof(float) * 4);
        memcpy(pc.filter_params_b, item->filter_params + 4, sizeof(float) * 4);

        /* Determine which descriptor set to bind for this item.
         * If the source has a live texture with content, use its slot. */
        bool has_texture = (slot < SBS_MAX_SOURCE_TEXTURES &&
                            comp->sources[slot].allocated &&
                            comp->sources[slot].has_content &&
                            source_texture_matches_item(&comp->sources[slot], item));
        pc.has_texture = has_texture ? 1 : 0;

        /* Check if this source uses ycbcr sampling */
        bool use_ycbcr = (has_texture && comp->ycbcr_available &&
                          comp->sources[slot].ycbcr_imported);

        VkPipelineLayout active_layout = use_ycbcr
            ? comp->ycbcr_pipeline_layout : comp->pipeline_layout;

        vkCmdPushConstants(cb, active_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);

        if (use_ycbcr) {
            /* Switch to ycbcr pipeline + descriptor for this source */
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, comp->ycbcr_pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    comp->ycbcr_pipeline_layout, 0, 1,
                                    &comp->ycbcr_descriptor_sets[slot],
                                    0, NULL);
        } else {
            /* Regular pipeline + descriptor */
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, comp->pipeline);
            /* Bind the descriptor set for this item's source texture */
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    comp->pipeline_layout, 0, 1,
                                    &comp->descriptor_sets[slot < SBS_MAX_SOURCE_TEXTURES ? slot : 0],
                                    0, NULL);
        }

        vkCmdDraw(cb, 6, 1, 0, 0);
    }
}

static float native_clampf01(float v)
{
    if (v < 0.0f)
        return 0.0f;
    if (v > 1.0f)
        return 1.0f;
    return v;
}

static bool native_item_has_crop(const sbs_comp_scene_item_t *item)
{
    if (!item)
        return false;
    return item->crop[0] > 0.000001f || item->crop[1] > 0.000001f ||
           item->crop[2] > 0.000001f || item->crop[3] > 0.000001f;
}

static uint32_t native_rotation_quadrant(float degrees)
{
    int quadrant = (int)lroundf(degrees / 90.0f);
    quadrant = ((quadrant % 4) + 4) % 4;
    return (uint32_t)quadrant;
}

static uint32_t native_transform_flags(const sbs_comp_scene_item_t *item)
{
    uint32_t flags;

    if (!item)
        return 0;
    flags = (native_rotation_quadrant(item->rotation_deg) << SBS_NATIVE_ROTATION_SHIFT) &
        SBS_NATIVE_ROTATION_MASK;
    if (item->flip_horizontal)
        flags |= SBS_NATIVE_FLIP_HORIZONTAL;
    if (item->flip_vertical)
        flags |= SBS_NATIVE_FLIP_VERTICAL;
    return flags;
}

static bool native_apply_item_crop(const sbs_comp_scene_item_t *item,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t *src_x,
                                   uint32_t *src_y,
                                   uint32_t *src_w,
                                   uint32_t *src_h)
{
    uint32_t left = 0, top = 0, right = 0, bottom = 0;

    if (!src_x || !src_y || !src_w || !src_h || width == 0 || height == 0)
        return false;

    if (item) {
        left = (uint32_t)lroundf(native_clampf01(item->crop[0]) * (float)width);
        top = (uint32_t)lroundf(native_clampf01(item->crop[1]) * (float)height);
        right = (uint32_t)lroundf(native_clampf01(item->crop[2]) * (float)width);
        bottom = (uint32_t)lroundf(native_clampf01(item->crop[3]) * (float)height);
    }

    if (left > width) left = width;
    if (top > height) top = height;
    if (right > width) right = width;
    if (bottom > height) bottom = height;

    if (right > width - left)
        right = width - left;
    if (bottom > height - top)
        bottom = height - top;

    *src_x = left;
    *src_y = top;
    *src_w = width - left - right;
    *src_h = height - top - bottom;
    return *src_w > 0 && *src_h > 0;
}

static void rgb_to_bt709_yuv(float r, float g, float b,
                             float *y_out, float *u_out, float *v_out)
{
    r = native_clampf01(r);
    g = native_clampf01(g);
    b = native_clampf01(b);

    float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float u = 0.5f + (b - y) * 0.5389f;
    float v = 0.5f + (r - y) * 0.6350f;
    if (y_out) *y_out = native_clampf01((16.0f / 255.0f) + y * (219.0f / 255.0f));
    if (u_out) *u_out = native_clampf01((16.0f / 255.0f) + u * (224.0f / 255.0f));
    if (v_out) *v_out = native_clampf01((16.0f / 255.0f) + v * (224.0f / 255.0f));
}

static void rgb_to_bt2020_yuv(float r, float g, float b,
                              float *y_out, float *u_out, float *v_out)
{
    r = native_clampf01(r);
    g = native_clampf01(g);
    b = native_clampf01(b);

    float y = 0.2627f * r + 0.6780f * g + 0.0593f * b;
    float u = 0.5f + (b - y) * 0.5315f;
    float v = 0.5f + (r - y) * 0.6782f;
    if (y_out) *y_out = native_clampf01((64.0f / 1023.0f) + y * (876.0f / 1023.0f));
    if (u_out) *u_out = native_clampf01((64.0f / 1023.0f) + u * (896.0f / 1023.0f));
    if (v_out) *v_out = native_clampf01((64.0f / 1023.0f) + v * (896.0f / 1023.0f));
}

typedef struct sbs_native_yuv_pc {
    uint32_t canvas_width;
    uint32_t canvas_height;
    int32_t  dst_x;
    int32_t  dst_y;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t src_w;
    uint32_t src_h;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t src_rect_w;
    uint32_t src_rect_h;
    float    opacity;
    uint32_t flags;
    uint32_t filter_flags;
    uint32_t filter_pad;
    float    filter_params_a[4];
    float    filter_params_b[4];
} sbs_native_yuv_pc_t;

typedef struct sbs_native_p010_direct_pc {
    uint32_t canvas_width;
    uint32_t canvas_height;
    int32_t  dst_x;
    int32_t  dst_y;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t src_w;
    uint32_t src_h;
    float    opacity;
    uint32_t flags;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t uv_offset;
    float    y_scale_x;
    float    y_scale_y;
    float    uv_scale_x;
    float    uv_scale_y;
    float    bg_y;
    float    bg_u;
    float    bg_v;
    uint32_t filter_flags;
    float    hdr_to_sdr_amount;
    float    hdr_saturation;
    float    hdr_brightness;
    float    hdr_hue_cos;
    float    hdr_hue_sin;
    float    filter_params[4];
    uint32_t src_offset_x;
    uint32_t src_offset_y;
} sbs_native_p010_direct_pc_t;

typedef struct sbs_native_downscale_pc {
    uint32_t src_w;
    uint32_t src_h;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t factor;
} sbs_native_downscale_pc_t;

_Static_assert(sizeof(sbs_native_yuv_pc_t) == SBS_NATIVE_YUV_PC_SIZE,
               "native YUV push constants size mismatch");
_Static_assert(sizeof(sbs_native_p010_direct_pc_t) == SBS_NATIVE_P010_DIRECT_PC_SIZE,
               "native P010 direct push constants size mismatch");
_Static_assert(sizeof(sbs_native_downscale_pc_t) == SBS_NATIVE_DOWNSCALE_PC_SIZE,
               "native downscale push constants size mismatch");

#define SBS_NATIVE_P010_DIRECT_FULL_CANVAS 1u
#define SBS_NATIVE_P010_DIRECT_SOURCE_NV12 2u
#define SBS_NATIVE_P010_DIRECT_SOURCE_NV21 4u
#define SBS_NATIVE_P010_DIRECT_DST_IN_BOUNDS 8u
#define SBS_NATIVE_P010_DIRECT_SRC_OFFSET 16u
#define SBS_NATIVE_P010_DIRECT_SRC_RECT_OFFSET 32u
#define SBS_NATIVE_P010_DIRECT_SOURCE_DRIVEN 64u

static bool native_item_filters_direct_yuv_compatible(const sbs_comp_scene_item_t *item,
                                                      sbs_export_color_mode_t color_mode)
{
    uint32_t direct_filter_mask = SBS_COMP_FILTER_GRAYSCALE |
        SBS_COMP_FILTER_BRIGHTNESS | SBS_COMP_FILTER_CONTRAST |
        SBS_COMP_FILTER_HDR_TO_SDR_LUT | SBS_COMP_FILTER_COLOR_CORRECTION;
    if (color_mode == SBS_EXPORT_COLOR_SDR)
        direct_filter_mask |= SBS_COMP_FILTER_LUMA_KEY;
    if (!item || item->filter_flags == 0)
        return true;
    if ((item->filter_flags & SBS_COMP_FILTER_LUMA_KEY) &&
        (item->filter_flags & SBS_COMP_FILTER_HDR_TO_SDR_LUT))
        return false;
    if ((item->filter_flags & ~direct_filter_mask) == 0 && item->lut_path[0] == '\0')
        return true;
    return false;
}

static bool native_source_can_direct_yuv(const sbs_compositor_t *comp,
                                         const sbs_native_canvas_entry_t *entry,
                                         const sbs_source_texture_t *tex,
                                         const sbs_comp_scene_item_t *item)
{
    if (!comp || !entry || !tex ||
        comp->native_p010_direct_pipeline_layout == VK_NULL_HANDLE ||
        !tex->dmabuf_imported || tex->y_buf == VK_NULL_HANDLE)
        return false;
    if (!native_item_filters_direct_yuv_compatible(item, entry->color_mode))
        return false;
    if (entry->color_mode == SBS_EXPORT_COLOR_HDR10) {
        if (tex->drm_format == DRM_FORMAT_P010)
            return comp->native_p010_direct_pipeline != VK_NULL_HANDLE;
        if (tex->drm_format == DRM_FORMAT_NV12 || tex->drm_format == DRM_FORMAT_NV21)
            return comp->native_yuv8_to_p010_pipeline != VK_NULL_HANDLE;
        if (tex->drm_format == SBS_DRM_FORMAT_AMLY)
            return comp->native_amly_to_p010_pipeline != VK_NULL_HANDLE;
    } else {
        if (tex->drm_format == DRM_FORMAT_P010)
            return comp->native_p010_to_nv21_pipeline != VK_NULL_HANDLE;
        if (tex->drm_format == DRM_FORMAT_NV12 || tex->drm_format == DRM_FORMAT_NV21)
            return comp->native_yuv8_to_nv21_pipeline != VK_NULL_HANDLE;
        if (tex->drm_format == SBS_DRM_FORMAT_AMLY)
            return comp->native_amly_to_nv21_pipeline != VK_NULL_HANDLE;
    }
    return false;
}

static bool native_scene_can_full_canvas_direct_yuv(sbs_compositor_t *comp,
                                                    sbs_native_canvas_entry_t *entry,
                                                    const sbs_comp_scene_state_t *scene)
{
    if (!comp || !entry || !scene || scene->transition_active)
        return false;
    if (scene->active_item_count <= 1)
        return false;

    for (uint32_t i = 0; i < scene->active_item_count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *item = &scene->active_items[i];
        uint32_t slot;
        if (!item->visible || item->opacity < 0.999f ||
            native_rotation_quadrant(item->rotation_deg) != 0 ||
            item->render_width <= 0 || item->render_height <= 0)
            continue;

        slot = native_source_texture_slot_for_item(comp, scene->active_items,
                                                   scene->active_item_count, i,
                                                   NULL, 0);
        if (slot >= SBS_MAX_SOURCE_TEXTURES)
            return false;

        if (!source_texture_matches_item(&comp->sources[slot], item))
            return false;

        bool direct_yuv = native_source_can_direct_yuv(comp, entry,
                                                       &comp->sources[slot], item);
        if (!direct_yuv)
            return false;

        int32_t dst_x = item->render_x;
        int32_t dst_y = item->render_y;
        uint32_t dst_w = (uint32_t)item->render_width;
        uint32_t dst_h = (uint32_t)item->render_height;
        if (dst_w == 0 || dst_h == 0 || dst_x > 0 ||
            dst_x + (int32_t)dst_w < (int32_t)comp->width)
            return false;

        bool covers_canvas_height = dst_y + (int32_t)dst_h >= (int32_t)comp->height;
        bool source_driven_amly_with_bg_fill =
            !native_item_has_crop(item) &&
            !item->flip_horizontal && !item->flip_vertical &&
            comp->sources[slot].drm_format == SBS_DRM_FORMAT_AMLY &&
            ((entry->color_mode == SBS_EXPORT_COLOR_SDR &&
              comp->native_amly_to_nv21_src_pipeline != VK_NULL_HANDLE) ||
             (entry->color_mode == SBS_EXPORT_COLOR_HDR10 &&
              comp->native_amly_to_p010_src_pipeline != VK_NULL_HANDLE)) &&
            dst_y < (int32_t)comp->height &&
            dst_y + (int32_t)dst_h > 0 &&
            (item->filter_flags & ~SBS_COMP_FILTER_HDR_TO_SDR_LUT) == 0;
        return covers_canvas_height || source_driven_amly_with_bg_fill;
    }

    return false;
}

static bool native_scene_can_targeted_bg_fill_yuv(sbs_compositor_t *comp,
                                                  sbs_native_canvas_entry_t *entry,
                                                  const sbs_comp_scene_state_t *scene)
{
    if (!comp || !entry || !scene || scene->transition_active)
        return false;

    VkPipeline amly_source_pipeline = entry->color_mode == SBS_EXPORT_COLOR_HDR10
        ? comp->native_amly_to_p010_src_pipeline
        : comp->native_amly_to_nv21_src_pipeline;
    if (amly_source_pipeline == VK_NULL_HANDLE)
        return false;

    for (uint32_t i = 0; i < scene->active_item_count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *item = &scene->active_items[i];
        uint32_t slot;
        sbs_source_texture_t *tex;
        int32_t dst_x, dst_y;
        uint32_t dst_w, dst_h;

        if (!item->visible || item->render_width <= 0 || item->render_height <= 0)
            continue;
        if (item->opacity < 0.999f || item->rotation_deg != 0.0f ||
            item->flip_horizontal || item->flip_vertical ||
            (item->filter_flags & SBS_COMP_FILTER_LUMA_KEY) != 0 ||
            native_item_has_crop(item))
            return false;

        slot = native_source_texture_slot_for_item(comp, scene->active_items,
                                                   scene->active_item_count, i,
                                                   NULL, 0);
        if (slot >= SBS_MAX_SOURCE_TEXTURES)
            return false;
        tex = &comp->sources[slot];
        if (!source_texture_matches_item(tex, item) ||
            tex->drm_format != SBS_DRM_FORMAT_AMLY ||
            !native_source_can_direct_yuv(comp, entry, tex, item))
            return false;

        dst_x = item->render_x;
        dst_y = item->render_y;
        dst_w = (uint32_t)item->render_width;
        dst_h = (uint32_t)item->render_height;
        return dst_x >= 0 && dst_y >= 0 &&
            dst_x + (int32_t)dst_w <= (int32_t)comp->width &&
            dst_y + (int32_t)dst_h <= (int32_t)comp->height;
    }

    return false;
}

static void native_record_source_uploads(sbs_compositor_t *comp, VkCommandBuffer cb)
{
    VkImageMemoryBarrier transfer_pre[SBS_MAX_SOURCE_TEXTURES];
    VkImageMemoryBarrier transfer_post[SBS_MAX_SOURCE_TEXTURES];
    uint32_t transfer_slots[SBS_MAX_SOURCE_TEXTURES];
    uint32_t transfer_count = 0;

    VkImageMemoryBarrier compute_pre[SBS_MAX_SOURCE_TEXTURES];
    VkImageMemoryBarrier compute_post[SBS_MAX_SOURCE_TEXTURES];
    uint32_t compute_slots[SBS_MAX_SOURCE_TEXTURES];
    uint32_t compute_count = 0;

    if (!comp || cb == VK_NULL_HANDLE)
        return;

    for (uint32_t s = 0; s < SBS_MAX_SOURCE_TEXTURES; s++) {
        sbs_source_texture_t *tex = &comp->sources[s];
        if (!tex->allocated || !tex->upload_pending ||
            tex->image == VK_NULL_HANDLE || tex->view == VK_NULL_HANDLE)
            continue;

        if ((tex->dmabuf_imported && source_drm_format_is_rgba(tex->drm_format) &&
             tex->y_buf != VK_NULL_HANDLE) || tex->staging_buf != VK_NULL_HANDLE) {
            transfer_slots[transfer_count] = s;
            transfer_pre[transfer_count] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = tex->has_content
                    ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    : VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .image = tex->image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            transfer_count++;
            continue;
        }

        if (!tex->dmabuf_imported || tex->y_buf == VK_NULL_HANDLE ||
            comp->compute_pipeline == VK_NULL_HANDLE)
            continue;

        compute_slots[compute_count] = s;
        compute_pre[compute_count] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = tex->has_content
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = tex->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        compute_count++;
    }

    if (transfer_count > 0) {
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, transfer_count, transfer_pre);

        for (uint32_t p = 0; p < transfer_count; p++) {
            sbs_source_texture_t *tex = &comp->sources[transfer_slots[p]];
            VkBuffer src = source_drm_format_is_rgba(tex->drm_format) && tex->y_buf != VK_NULL_HANDLE
                ? tex->y_buf : tex->staging_buf;
            VkBufferImageCopy copy = {
                .bufferRowLength = source_drm_format_is_rgba(tex->drm_format) && tex->y_stride > 0
                    ? tex->y_stride / 4u : 0,
                .imageSubresource = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .layerCount = 1,
                },
                .imageExtent = { tex->width, tex->height, 1 },
            };
            vkCmdCopyBufferToImage(cb, src, tex->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        for (uint32_t p = 0; p < transfer_count; p++) {
            sbs_source_texture_t *tex = &comp->sources[transfer_slots[p]];
            transfer_post[p] = (VkImageMemoryBarrier){
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = tex->image,
                .subresourceRange = {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
            };
            tex->upload_pending = false;
            tex->has_content = true;
        }
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, NULL, 0, NULL, transfer_count, transfer_post);
    }

    if (compute_count == 0)
        return;

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, compute_count, compute_pre);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp->compute_pipeline);
    for (uint32_t p = 0; p < compute_count; p++) {
        uint32_t slot = compute_slots[p];
        sbs_source_texture_t *tex = &comp->sources[slot];
        VkDescriptorImageInfo img_info = {
            .imageView = tex->view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkDescriptorBufferInfo buf_info = {
            .buffer = tex->y_buf,
            .offset = 0,
            .range = VK_WHOLE_SIZE,
        };
        VkWriteDescriptorSet writes[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->compute_descriptor_sets[slot], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img_info },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->compute_descriptor_sets[slot], .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &buf_info },
        };
        vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                comp->compute_pipeline_layout, 0, 1,
                                &comp->compute_descriptor_sets[slot], 0, NULL);

        uint32_t format = 1u;
        if (tex->drm_format == DRM_FORMAT_P010)
            format = 2u;
        else if (tex->drm_format == DRM_FORMAT_NV21)
            format = 0u;
        uint32_t flags = 0u;
        if (tex->drm_format == DRM_FORMAT_P010 &&
            (tex->upload_filter_flags & SBS_COMP_FILTER_HDR_TO_SDR_LUT) != 0)
            flags |= 1u;
        uint32_t push[7] = {
            tex->width, tex->height, tex->y_stride, tex->uv_stride,
            tex->uv_offset, format, flags,
        };
        vkCmdPushConstants(cb, comp->compute_pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
        vkCmdDispatch(cb, (tex->width + 15u) / 16u, (tex->height + 15u) / 16u, 1);
    }

    for (uint32_t p = 0; p < compute_count; p++) {
        sbs_source_texture_t *tex = &comp->sources[compute_slots[p]];
        compute_post[p] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = tex->image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
        };
        tex->upload_pending = false;
        tex->has_content = true;
    }
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, compute_count, compute_post);
}

static int32_t native_scale_i32(int32_t value, float scale)
{
    return (int32_t)lroundf((float)value * scale);
}

static uint32_t native_scale_u32(int32_t value, float scale)
{
    long scaled;
    if (value <= 0)
        return 0;
    scaled = lroundf((float)value * scale);
    return scaled > 0 ? (uint32_t)scaled : 1u;
}

static bool native_items_need_rgba_upload(sbs_compositor_t *comp,
                                          sbs_native_canvas_entry_t *entry,
                                          const sbs_comp_scene_item_t *items,
                                          uint32_t count)
{
    if (!comp || !entry || !items)
        return false;

    for (uint32_t i = 0; i < count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *item = &items[i];
        uint32_t slot = native_source_texture_slot_for_item(comp, items, count, i,
                                                            NULL, 0);
        if (slot >= SBS_MAX_SOURCE_TEXTURES)
            continue;
        sbs_source_texture_t *tex = &comp->sources[slot];

        if (!item->visible || !tex->allocated ||
            !source_texture_matches_item(tex, item) ||
            item->render_width <= 0 || item->render_height <= 0)
            continue;
        if (!native_source_can_direct_yuv(comp, entry, tex, item) &&
            tex->upload_pending && tex->dmabuf_imported &&
            tex->image != VK_NULL_HANDLE && tex->view != VK_NULL_HANDLE &&
            tex->y_buf != VK_NULL_HANDLE)
            return true;
    }
    return false;
}

static bool native_scene_needs_rgba_upload(sbs_compositor_t *comp,
                                           sbs_native_canvas_entry_t *entry,
                                           const sbs_comp_scene_state_t *scene)
{
    if (!scene)
        return false;
    if (native_items_need_rgba_upload(comp, entry, scene->active_items,
                                      scene->active_item_count))
        return true;
    return false;
}

typedef struct sbs_native_timing_ctx {
    uint32_t base;
    uint32_t next_mark;
    sbs_native_canvas_entry_t *entry;
} sbs_native_timing_ctx_t;

static void native_timing_record_layer_end(sbs_compositor_t *comp,
                                            VkCommandBuffer cb,
                                            sbs_native_timing_ctx_t *timing,
                                            const sbs_comp_scene_item_t *item,
                                           const sbs_source_texture_t *tex,
                                           uint32_t dst_w,
                                           uint32_t dst_h,
                                           int32_t dst_x,
                                           int32_t dst_y,
                                           bool direct)
{
    if (!comp || !comp->native_timing_detail || !timing || !timing->entry ||
        timing->base == UINT32_MAX || cb == VK_NULL_HANDLE ||
        timing->next_mark >= SBS_NATIVE_TIMING_QUERY_MARKS - 2u)
        return;

    uint32_t layer_idx = timing->entry->timing_layer_count;
    if (layer_idx < SBS_NATIVE_TIMING_MAX_LAYERS) {
        snprintf(timing->entry->timing_layer_labels[layer_idx],
                 SBS_NATIVE_TIMING_LABEL_MAX,
                 "%.31s %.4s %ux%u+%d+%d %s",
                 item ? item->source_id : "?",
                 tex ? (const char *)&tex->drm_format : "????",
                 dst_w, dst_h, dst_x, dst_y,
                 direct ? "direct" : "rgba");
    }
    timing->entry->timing_layer_count++;
    vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        comp->native_timing_query_pool,
                        timing->base + timing->next_mark);
    timing->next_mark++;
}

static void native_canvas_layer_barrier(VkCommandBuffer cb,
                                        const sbs_native_canvas_entry_t *entry)
{
    if (cb == VK_NULL_HANDLE || !entry)
        return;

    VkImageMemoryBarrier barriers[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, NULL, 0, NULL, 2, barriers);
}

static bool native_crop_rgba_layer_to_alpha_bounds(const sbs_source_texture_t *tex,
                                                   int32_t *dst_x,
                                                   int32_t *dst_y,
                                                   uint32_t *dst_w,
                                                   uint32_t *dst_h,
                                                   uint32_t *src_x,
                                                   uint32_t *src_y,
                                                   uint32_t *src_w,
                                                   uint32_t *src_h)
{
    uint32_t ax, ay, aw, ah;
    uint64_t x0, y0, x1, y1;

    if (!tex || !dst_x || !dst_y || !dst_w || !dst_h ||
        !src_x || !src_y || !src_w || !src_h ||
        tex->width == 0 || tex->height == 0 || *dst_w == 0 || *dst_h == 0)
        return false;

    ax = tex->alpha_x;
    ay = tex->alpha_y;
    aw = tex->alpha_w;
    ah = tex->alpha_h;
    if (aw == 0 || ah == 0)
        return false;
    if (ax >= tex->width || ay >= tex->height)
        return false;
    if (ax + aw > tex->width)
        aw = tex->width - ax;
    if (ay + ah > tex->height)
        ah = tex->height - ay;

    x0 = ((uint64_t)(*dst_w) * ax) / tex->width;
    y0 = ((uint64_t)(*dst_h) * ay) / tex->height;
    x1 = ((uint64_t)(*dst_w) * (ax + aw) + tex->width - 1u) / tex->width;
    y1 = ((uint64_t)(*dst_h) * (ay + ah) + tex->height - 1u) / tex->height;
    if (x1 <= x0 || y1 <= y0)
        return false;

    *dst_x += (int32_t)x0;
    *dst_y += (int32_t)y0;
    *dst_w = (uint32_t)(x1 - x0);
    *dst_h = (uint32_t)(y1 - y0);
    *src_x = ax;
    *src_y = ay;
    *src_w = aw;
    *src_h = ah;
    return true;
}

typedef struct sbs_native_rect {
    int32_t x;
    int32_t y;
    int32_t x1;
    int32_t y1;
} sbs_native_rect_t;

static bool native_rect_clip_canvas(sbs_native_rect_t *r,
                                    uint32_t canvas_width,
                                    uint32_t canvas_height)
{
    if (!r)
        return false;
    if (r->x < 0) r->x = 0;
    if (r->y < 0) r->y = 0;
    if (r->x1 > (int32_t)canvas_width) r->x1 = (int32_t)canvas_width;
    if (r->y1 > (int32_t)canvas_height) r->y1 = (int32_t)canvas_height;
    return r->x1 > r->x && r->y1 > r->y;
}

static bool native_rect_align_full_chroma_cells(sbs_native_rect_t *r)
{
    if (!r)
        return false;
    r->x = (r->x + 1) & ~1;
    r->y = (r->y + 1) & ~1;
    r->x1 &= ~1;
    r->y1 &= ~1;
    return r->x1 > r->x && r->y1 > r->y;
}

static bool native_rect_subtract_one(sbs_native_rect_t *rects,
                                     uint32_t *rect_count,
                                     sbs_native_rect_t occ)
{
    uint32_t out_count = 0;
    sbs_native_rect_t out[SBS_NATIVE_OCCLUSION_MAX_RECTS];

    if (!rects || !rect_count)
        return false;

    for (uint32_t i = 0; i < *rect_count; i++) {
        sbs_native_rect_t r = rects[i];
        int32_t ix0 = r.x > occ.x ? r.x : occ.x;
        int32_t iy0 = r.y > occ.y ? r.y : occ.y;
        int32_t ix1 = r.x1 < occ.x1 ? r.x1 : occ.x1;
        int32_t iy1 = r.y1 < occ.y1 ? r.y1 : occ.y1;

        if (ix1 <= ix0 || iy1 <= iy0) {
            if (out_count >= SBS_NATIVE_OCCLUSION_MAX_RECTS)
                return false;
            out[out_count++] = r;
            continue;
        }

        sbs_native_rect_t pieces[4] = {
            { r.x, r.y, r.x1, iy0 },
            { r.x, iy1, r.x1, r.y1 },
            { r.x, iy0, ix0, iy1 },
            { ix1, iy0, r.x1, iy1 },
        };
        for (uint32_t p = 0; p < 4; p++) {
            if (pieces[p].x1 <= pieces[p].x || pieces[p].y1 <= pieces[p].y)
                continue;
            if (out_count >= SBS_NATIVE_OCCLUSION_MAX_RECTS)
                return false;
            out[out_count++] = pieces[p];
        }
    }

    memcpy(rects, out, sizeof(sbs_native_rect_t) * out_count);
    *rect_count = out_count;
    return true;
}

static bool native_source_opaque_occluder_rect(sbs_compositor_t *comp,
                                               sbs_native_canvas_entry_t *entry,
                                               const sbs_comp_scene_item_t *items,
                                               uint32_t count,
                                               uint32_t item_index,
                                               float opacity_scale,
                                               float scale_x,
                                               float scale_y,
                                               uint32_t canvas_width,
                                               uint32_t canvas_height,
                                               const sbs_comp_scene_item_t *ref_items,
                                               uint32_t ref_count,
                                               sbs_native_rect_t *out)
{
    const sbs_comp_scene_item_t *item;
    sbs_source_texture_t *tex;
    uint32_t slot;
    int32_t dst_x, dst_y, dst_x1, dst_y1;
    uint32_t dst_w, dst_h;

    if (!comp || !entry || !items || item_index >= count || !out)
        return false;
    item = &items[item_index];
    if (!item->visible || item->render_width <= 0 || item->render_height <= 0 ||
        native_clampf01(item->opacity * opacity_scale) < 0.999f ||
        item->rotation_deg != 0.0f)
        return false;

    slot = native_source_texture_slot_for_item(comp, items, count, item_index,
                                               ref_items, ref_count);
    if (slot >= SBS_MAX_SOURCE_TEXTURES)
        return false;
    tex = &comp->sources[slot];
    if (!tex->allocated || !tex->has_content ||
        !source_texture_matches_item(tex, item))
        return false;

    dst_x = native_scale_i32(item->render_x, scale_x);
    dst_y = native_scale_i32(item->render_y, scale_y);
    dst_w = native_scale_u32(item->render_width, scale_x);
    dst_h = native_scale_u32(item->render_height, scale_y);
    if (dst_w == 0 || dst_h == 0)
        return false;

    if (source_drm_format_is_rgba(tex->drm_format)) {
        uint32_t ax = tex->alpha_x;
        uint32_t ay = tex->alpha_y;
        uint32_t aw = tex->alpha_w;
        uint32_t ah = tex->alpha_h;
        uint64_t ox0, oy0, ox1, oy1;

        if (!tex->alpha_rect_opaque || ax >= tex->width || ay >= tex->height ||
            aw == 0 || ah == 0)
            return false;
        if (ax + aw > tex->width)
            aw = tex->width - ax;
        if (ay + ah > tex->height)
            ah = tex->height - ay;

        ox0 = ((uint64_t)dst_w * ax) / tex->width;
        oy0 = ((uint64_t)dst_h * ay) / tex->height;
        ox1 = ((uint64_t)dst_w * (ax + aw) + tex->width - 1u) / tex->width;
        oy1 = ((uint64_t)dst_h * (ay + ah) + tex->height - 1u) / tex->height;
        if (ox1 <= ox0 || oy1 <= oy0)
            return false;
        if (item->flip_horizontal) {
            uint64_t fx0 = dst_w - ox1;
            uint64_t fx1 = dst_w - ox0;
            ox0 = fx0;
            ox1 = fx1;
        }
        if (item->flip_vertical) {
            uint64_t fy0 = dst_h - oy1;
            uint64_t fy1 = dst_h - oy0;
            oy0 = fy0;
            oy1 = fy1;
        }
        dst_x += (int32_t)ox0;
        dst_y += (int32_t)oy0;
        dst_x1 = dst_x + (int32_t)(ox1 - ox0);
        dst_y1 = dst_y + (int32_t)(oy1 - oy0);
    } else {
        if (!native_source_can_direct_yuv(comp, entry, tex, item))
            return false;
        dst_x1 = dst_x + (int32_t)dst_w;
        dst_y1 = dst_y + (int32_t)dst_h;
    }

    *out = (sbs_native_rect_t){ dst_x, dst_y, dst_x1, dst_y1 };
    if (!native_rect_clip_canvas(out, canvas_width, canvas_height))
        return false;
    return native_rect_align_full_chroma_cells(out);
}

static bool native_build_base_visible_rects(sbs_compositor_t *comp,
                                            sbs_native_canvas_entry_t *entry,
                                            const sbs_comp_scene_item_t *items,
                                            uint32_t count,
                                            uint32_t base_index,
                                            float opacity_scale,
                                            float scale_x,
                                            float scale_y,
                                            uint32_t canvas_width,
                                            uint32_t canvas_height,
                                            const sbs_comp_scene_item_t *ref_items,
                                            uint32_t ref_count,
                                            sbs_native_rect_t *rects,
                                            uint32_t *rect_count)
{
    if (!rects || !rect_count || base_index >= count)
        return false;

    rects[0] = (sbs_native_rect_t){ 0, 0, (int32_t)canvas_width, (int32_t)canvas_height };
    *rect_count = 1;

    for (uint32_t j = base_index + 1; j < count && j < SBS_MAX_SOURCE_TEXTURES; j++) {
        sbs_native_rect_t occ;
        if (!native_source_opaque_occluder_rect(comp, entry, items, count, j,
                opacity_scale, scale_x, scale_y, canvas_width, canvas_height,
                ref_items, ref_count, &occ))
            continue;
        if (!native_rect_subtract_one(rects, rect_count, occ))
            return false;
        if (*rect_count == 0)
            return true;
    }

    return *rect_count > 1;
}

static bool native_build_layer_visible_rects(sbs_compositor_t *comp,
                                             sbs_native_canvas_entry_t *entry,
                                             const sbs_comp_scene_item_t *items,
                                             uint32_t count,
                                             uint32_t base_index,
                                             sbs_native_rect_t base_rect,
                                             float opacity_scale,
                                             float scale_x,
                                             float scale_y,
                                             uint32_t canvas_width,
                                             uint32_t canvas_height,
                                             const sbs_comp_scene_item_t *ref_items,
                                             uint32_t ref_count,
                                             sbs_native_rect_t *rects,
                                             uint32_t *rect_count)
{
    sbs_native_rect_t original_rect = base_rect;

    if (!rects || !rect_count || base_index >= count)
        return false;
    if (!native_rect_clip_canvas(&base_rect, canvas_width, canvas_height))
        return false;

    rects[0] = base_rect;
    *rect_count = 1;

    for (uint32_t j = base_index + 1; j < count && j < SBS_MAX_SOURCE_TEXTURES; j++) {
        sbs_native_rect_t occ;
        if (!native_source_opaque_occluder_rect(comp, entry, items, count, j,
                opacity_scale, scale_x, scale_y, canvas_width, canvas_height,
                ref_items, ref_count, &occ))
            continue;
        if (!native_rect_subtract_one(rects, rect_count, occ))
            return false;
        if (*rect_count == 0)
            return true;
    }

    return *rect_count != 1 || rects[0].x != original_rect.x ||
        rects[0].y != original_rect.y || rects[0].x1 != original_rect.x1 ||
        rects[0].y1 != original_rect.y1 || rects[0].x != base_rect.x ||
        rects[0].y != base_rect.y || rects[0].x1 != base_rect.x1 ||
        rects[0].y1 != base_rect.y1;
}

static void native_dispatch_source_layers(sbs_compositor_t *comp,
                                           VkCommandBuffer cb,
                                           uint32_t log_idx,
                                           VkDescriptorSet *descriptor_sets,
                                           VkDescriptorSet *p010_direct_sets,
                                           sbs_native_canvas_entry_t *entry,
                                           uint32_t canvas_width,
                                           uint32_t canvas_height,
                                           const sbs_comp_scene_item_t *items,
                                           uint32_t count,
                                           float opacity_scale,
                                           float scale_x,
                                           float scale_y,
                                            bool full_canvas_direct,
                                            bool targeted_bg_fill,
                                            float bg_y,
                                            float bg_u,
                                            float bg_v,
                                            uint32_t *descriptor_index,
                                            sbs_native_timing_ctx_t *timing,
                                            const sbs_comp_scene_item_t *ref_items,
                                            uint32_t ref_count)
{
    VkPipeline pipeline = entry->color_mode == SBS_EXPORT_COLOR_HDR10
        ? comp->native_yuv_hdr_pipeline : comp->native_yuv_sdr_pipeline;
    bool have_rgba_pipeline = pipeline != VK_NULL_HANDLE &&
        descriptor_sets != NULL &&
        comp->native_yuv_pipeline_layout != VK_NULL_HANDLE;
    bool have_direct_pipeline = (comp->native_p010_direct_pipeline != VK_NULL_HANDLE ||
        comp->native_p010_to_nv21_pipeline != VK_NULL_HANDLE ||
        comp->native_yuv8_to_p010_pipeline != VK_NULL_HANDLE ||
        comp->native_amly_to_p010_pipeline != VK_NULL_HANDLE ||
        comp->native_amly_to_p010_src_pipeline != VK_NULL_HANDLE ||
        comp->native_yuv8_to_nv21_pipeline != VK_NULL_HANDLE ||
        comp->native_amly_to_nv21_pipeline != VK_NULL_HANDLE ||
        comp->native_amly_to_nv21_src_pipeline != VK_NULL_HANDLE) &&
        p010_direct_sets != NULL &&
        comp->native_p010_direct_pipeline_layout != VK_NULL_HANDLE;

    if (!have_rgba_pipeline && !have_direct_pipeline)
        return;

    bool base_layer_used = false;
    for (uint32_t i = 0; i < count && i < SBS_MAX_SOURCE_TEXTURES; i++) {
        const sbs_comp_scene_item_t *item = &items[i];
        uint32_t slot = native_source_texture_slot_for_item(comp, items, count, i,
                                                            ref_items, ref_count);
        uint32_t ds_idx = descriptor_index ? (*descriptor_index)++ : i;
        if (slot >= SBS_MAX_SOURCE_TEXTURES)
            continue;
        if (ds_idx >= SBS_NATIVE_LAYER_DESCRIPTOR_SETS)
            continue;
        sbs_source_texture_t *tex = &comp->sources[slot];
        VkDescriptorSet descriptor_set = descriptor_sets ? descriptor_sets[ds_idx] : VK_NULL_HANDLE;
        VkDescriptorSet p010_direct_set = p010_direct_sets ? p010_direct_sets[ds_idx] : VK_NULL_HANDLE;
        uint32_t src_x = 0;
        uint32_t src_y = 0;
        uint32_t src_rect_w = tex->width;
        uint32_t src_rect_h = tex->height;
        bool item_has_crop = native_item_has_crop(item);
        bool direct_yuv = false;

        if (!item->visible || !tex->allocated || !tex->has_content ||
            !source_texture_matches_item(tex, item) ||
            item->render_width <= 0 || item->render_height <= 0)
            continue;

        if (!native_apply_item_crop(item, tex->width, tex->height,
                                    &src_x, &src_y, &src_rect_w, &src_rect_h))
            continue;

        direct_yuv = have_direct_pipeline &&
            p010_direct_set != VK_NULL_HANDLE &&
            native_source_can_direct_yuv(comp, entry, tex, item);
        if (!direct_yuv && (!have_rgba_pipeline ||
                            descriptor_set == VK_NULL_HANDLE ||
                            tex->view == VK_NULL_HANDLE))
            continue;

        int32_t dst_x = native_scale_i32(item->render_x, scale_x);
        int32_t dst_y = native_scale_i32(item->render_y, scale_y);
        uint32_t dst_w = native_scale_u32(item->render_width, scale_x);
        uint32_t dst_h = native_scale_u32(item->render_height, scale_y);
        if (dst_w == 0 || dst_h == 0)
            continue;

        if (!direct_yuv && !item_has_crop &&
            !native_crop_rgba_layer_to_alpha_bounds(tex,
                &dst_x, &dst_y, &dst_w, &dst_h,
                &src_x, &src_y, &src_rect_w, &src_rect_h))
            continue;

        VkDescriptorImageInfo y_info = {
            .imageView = entry->y.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };
        VkDescriptorImageInfo uv_info = {
            .imageView = entry->uv.view,
            .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        };

        if (direct_yuv) {
            bool item_base_direct = !base_layer_used;
            bool item_full_canvas_direct = full_canvas_direct && item_base_direct;
            float layer_opacity = native_clampf01(item->opacity * opacity_scale);
            uint32_t item_rotation = native_rotation_quadrant(item->rotation_deg);
            bool item_flipped = item->flip_horizontal || item->flip_vertical;
            uint32_t source_scale_dst_w = dst_w;
            uint32_t source_scale_dst_h = dst_h;
            sbs_native_rect_t visible_rects[SBS_NATIVE_OCCLUSION_MAX_RECTS];
            uint32_t visible_rect_count = 0;
            bool split_full_canvas_direct = false;
            bool split_direct_layer = false;
            bool dst_in_bounds = dst_x >= 0 && dst_y >= 0 &&
                dst_x + (int32_t)dst_w <= (int32_t)canvas_width &&
                dst_y + (int32_t)dst_h <= (int32_t)canvas_height;
            VkPipeline direct_pipeline = VK_NULL_HANDLE;
            if (entry->color_mode == SBS_EXPORT_COLOR_HDR10) {
                if (tex->drm_format == DRM_FORMAT_P010)
                    direct_pipeline = comp->native_p010_direct_pipeline;
                else if (tex->drm_format == SBS_DRM_FORMAT_AMLY)
                    direct_pipeline = comp->native_amly_to_p010_pipeline;
                else
                    direct_pipeline = comp->native_yuv8_to_p010_pipeline;
            } else {
                if (tex->drm_format == DRM_FORMAT_P010)
                    direct_pipeline = comp->native_p010_to_nv21_pipeline;
                else if (tex->drm_format == DRM_FORMAT_NV12 ||
                         tex->drm_format == DRM_FORMAT_NV21)
                    direct_pipeline = comp->native_yuv8_to_nv21_pipeline;
                else if (tex->drm_format == SBS_DRM_FORMAT_AMLY)
                    direct_pipeline = comp->native_amly_to_nv21_pipeline;
            }
            if (direct_pipeline == VK_NULL_HANDLE)
                continue;

            bool source_driven_amly = false;
            bool source_driven_yuv8 = false;
            VkPipeline amly_source_pipeline = entry->color_mode == SBS_EXPORT_COLOR_HDR10
                ? comp->native_amly_to_p010_src_pipeline
                : comp->native_amly_to_nv21_src_pipeline;
            if (!item_has_crop && !item_flipped && item_rotation == 0 &&
                tex->drm_format == SBS_DRM_FORMAT_AMLY &&
                amly_source_pipeline != VK_NULL_HANDLE &&
                item_base_direct && layer_opacity >= 0.999f &&
                (item->filter_flags & ~SBS_COMP_FILTER_HDR_TO_SDR_LUT) == 0 &&
                dst_x <= 0 &&
                dst_x + (int32_t)dst_w >= (int32_t)canvas_width &&
                dst_y < (int32_t)canvas_height &&
                dst_y + (int32_t)dst_h > 0) {
                direct_pipeline = amly_source_pipeline;
                source_driven_amly = true;
            }
            if (!item_has_crop && !item_flipped && item_rotation == 0 &&
                (tex->drm_format == DRM_FORMAT_NV12 || tex->drm_format == DRM_FORMAT_NV21) &&
                (direct_pipeline == comp->native_yuv8_to_nv21_pipeline ||
                 direct_pipeline == comp->native_yuv8_to_p010_pipeline) &&
                layer_opacity >= 0.999f && item->filter_flags == 0 &&
                dst_in_bounds && (dst_x & 1) == 0 && (dst_y & 1) == 0) {
                uint64_t src_blocks = (((uint64_t)tex->width + 1u) >> 1) *
                    (uint64_t)tex->height;
                uint64_t dst_blocks = (((uint64_t)dst_w + 1u) >> 1) *
                    (((uint64_t)dst_h + 1u) >> 1);
                source_driven_yuv8 = src_blocks < dst_blocks;
            }

            if (item_rotation == 0 && !item_flipped && item_full_canvas_direct && layer_opacity >= 0.999f &&
                dst_x <= 0 && dst_y <= 0 &&
                dst_x + (int32_t)dst_w >= (int32_t)canvas_width &&
                dst_y + (int32_t)dst_h >= (int32_t)canvas_height &&
                !source_driven_amly) {
                split_full_canvas_direct = native_build_base_visible_rects(
                    comp, entry, items, count, i, opacity_scale, scale_x, scale_y,
                    canvas_width, canvas_height, ref_items, ref_count,
                    visible_rects, &visible_rect_count);
            }
            if (item_rotation == 0 && !item_flipped && !item_has_crop && !split_full_canvas_direct &&
                ((item_base_direct && tex->drm_format == SBS_DRM_FORMAT_AMLY &&
                  amly_source_pipeline != VK_NULL_HANDLE) ||
                 (entry->color_mode == SBS_EXPORT_COLOR_SDR &&
                  (tex->drm_format == DRM_FORMAT_NV12 ||
                   tex->drm_format == DRM_FORMAT_NV21))) &&
                layer_opacity >= 0.999f &&
                dst_x < (int32_t)canvas_width && dst_y < (int32_t)canvas_height &&
                dst_x + (int32_t)dst_w > 0 && dst_y + (int32_t)dst_h > 0 &&
                (item->filter_flags & SBS_COMP_FILTER_LUMA_KEY) == 0) {
                split_direct_layer = native_build_layer_visible_rects(
                    comp, entry, items, count, i,
                    (sbs_native_rect_t){ dst_x, dst_y,
                                         dst_x + (int32_t)dst_w,
                                         dst_y + (int32_t)dst_h },
                    opacity_scale, scale_x, scale_y, canvas_width, canvas_height,
                    ref_items, ref_count, visible_rects, &visible_rect_count);
            }

            VkDescriptorBufferInfo src_buf = {
                .buffer = tex->y_buf,
                .offset = 0,
                .range = VK_WHOLE_SIZE,
            };
            VkDescriptorImageInfo lut_info = {
                .sampler = comp->hdr_ycbcr_lut_sampler != VK_NULL_HANDLE
                    ? comp->hdr_ycbcr_lut_sampler : comp->source_sampler,
                .imageView = comp->hdr_ycbcr_lut_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            };
            VkWriteDescriptorSet writes[4] = {
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p010_direct_set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &src_buf },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p010_direct_set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &y_info },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p010_direct_set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &uv_info },
                { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = p010_direct_set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &lut_info },
            };
            vkUpdateDescriptorSets(comp->device, 4, writes, 0, NULL);

            uint32_t flags = item_full_canvas_direct
                ? SBS_NATIVE_P010_DIRECT_FULL_CANVAS : 0u;
            if (tex->drm_format == DRM_FORMAT_NV12)
                flags |= SBS_NATIVE_P010_DIRECT_SOURCE_NV12;
            else if (tex->drm_format == DRM_FORMAT_NV21)
                flags |= SBS_NATIVE_P010_DIRECT_SOURCE_NV21;
            if (dst_in_bounds)
                flags |= SBS_NATIVE_P010_DIRECT_DST_IN_BOUNDS;
            if (source_driven_yuv8)
                flags |= SBS_NATIVE_P010_DIRECT_SOURCE_DRIVEN;
            flags |= native_transform_flags(item);
            if (split_full_canvas_direct)
                flags = (flags & ~SBS_NATIVE_P010_DIRECT_FULL_CANVAS) |
                    SBS_NATIVE_P010_DIRECT_SRC_OFFSET |
                    SBS_NATIVE_P010_DIRECT_DST_IN_BOUNDS;

            bool hdr_to_sdr_filter =
                (item->filter_flags & SBS_COMP_FILTER_HDR_TO_SDR_LUT) != 0;
            float hdr_hue_rad = item->hdr_to_sdr_hue_deg * 0.01745329252f;

            sbs_native_p010_direct_pc_t pc = {
                .canvas_width = canvas_width,
                .canvas_height = canvas_height,
                .dst_x = dst_x,
                .dst_y = dst_y,
                .dst_w = dst_w,
                .dst_h = dst_h,
                .src_w = src_rect_w,
                .src_h = src_rect_h,
                .opacity = layer_opacity,
                .flags = flags,
                .y_stride = tex->y_stride,
                .uv_stride = tex->uv_stride,
                .uv_offset = tex->drm_format == SBS_DRM_FORMAT_AMLY ? tex->width : tex->uv_offset,
                .y_scale_x = (float)src_rect_w / (float)source_scale_dst_w,
                .y_scale_y = (float)src_rect_h / (float)source_scale_dst_h,
                .uv_scale_x = (float)((src_rect_w + 1u) >> 1) /
                               (float)((source_scale_dst_w + 1u) >> 1),
                .uv_scale_y = (float)((src_rect_h + 1u) >> 1) /
                               (float)((source_scale_dst_h + 1u) >> 1),
                .bg_y = bg_y,
                .bg_u = bg_u,
                .bg_v = bg_v,
                .filter_flags = item->filter_flags,
                .hdr_to_sdr_amount = item->filter_params[5],
                .hdr_saturation = hdr_to_sdr_filter ? item->hdr_to_sdr_saturation : item->filter_params[4],
                .hdr_brightness = hdr_to_sdr_filter ? item->hdr_to_sdr_brightness : item->filter_params[5],
                .hdr_hue_cos = hdr_to_sdr_filter ? cosf(hdr_hue_rad) : item->filter_params[6],
                .hdr_hue_sin = hdr_to_sdr_filter ? sinf(hdr_hue_rad) : item->filter_params[7],
                .src_offset_x = src_x,
                .src_offset_y = src_y,
            };
            memcpy(pc.filter_params, item->filter_params, sizeof(pc.filter_params));
            if (source_driven_amly) {
                int32_t local_x0 = -dst_x;
                int32_t local_x1 = (int32_t)canvas_width - dst_x;
                if (local_x0 < 0) local_x0 = 0;
                if (local_x1 > (int32_t)dst_w) local_x1 = (int32_t)dst_w;
                uint32_t src_x0 = (uint32_t)floorf((float)local_x0 * pc.y_scale_x);
                uint32_t src_x1 = (uint32_t)ceilf((float)local_x1 * pc.y_scale_x) + 1u;
                if (src_x1 > tex->width) src_x1 = tex->width;
                pc.y_stride = 0;
                pc.uv_stride = (src_x0 >> 1);
                pc.uv_offset = src_x1 > (pc.uv_stride << 1)
                    ? (((src_x1 + 1u) >> 1) - pc.uv_stride) : 0u;
            }

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                              direct_pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    comp->native_p010_direct_pipeline_layout, 0, 1,
                                    &p010_direct_set, 0, NULL);
            vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

            if (split_full_canvas_direct) {
                for (uint32_t r = 0; r < visible_rect_count; r++) {
                    sbs_native_rect_t vr = visible_rects[r];
                    uint32_t block_w;
                    uint32_t block_h;
                    uint32_t wg_x;
                    uint32_t wg_y;
                    if (vr.x1 <= vr.x || vr.y1 <= vr.y)
                        continue;
                    pc.dst_x = vr.x;
                    pc.dst_y = vr.y;
                    pc.dst_w = (uint32_t)(vr.x1 - vr.x);
                    pc.dst_h = (uint32_t)(vr.y1 - vr.y);
                    vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                    block_w = (pc.dst_w + 1u) / 2u;
                    block_h = (pc.dst_h + 1u) / 2u;
                    wg_x = direct_pipeline == comp->native_amly_to_nv21_pipeline ? 16u : 8u;
                    wg_y = direct_pipeline == comp->native_amly_to_nv21_pipeline ? 16u : 8u;
                    vkCmdDispatch(cb, (block_w + wg_x - 1u) / wg_x,
                                  (block_h + wg_y - 1u) / wg_y, 1);
                }
            } else if (split_direct_layer) {
                bool use_source_split = tex->drm_format == SBS_DRM_FORMAT_AMLY &&
                    amly_source_pipeline != VK_NULL_HANDLE;
                if (use_source_split)
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                      amly_source_pipeline);
                if (use_source_split && item_base_direct &&
                    (targeted_bg_fill || (source_driven_amly && full_canvas_direct))) {
                    sbs_native_p010_direct_pc_t bg_pc = pc;
                    bg_pc.src_h = 0;
                    if (dst_y > 0) {
                        uint32_t rows = (uint32_t)dst_y;
                        bg_pc.y_stride = 0;
                        bg_pc.uv_stride = 0;
                        bg_pc.uv_offset = (canvas_width + 1u) >> 1;
                        vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                        vkCmdDispatch(cb, (bg_pc.uv_offset + 15u) / 16u,
                                      (((rows + 1u) >> 1) + 7u) / 8u, 1);
                    }
                    if (dst_y + (int32_t)dst_h < (int32_t)canvas_height) {
                        uint32_t y0 = (uint32_t)(dst_y + (int32_t)dst_h);
                        uint32_t rows = canvas_height - y0;
                        bg_pc.y_stride = y0;
                        bg_pc.uv_stride = 0;
                        bg_pc.uv_offset = (canvas_width + 1u) >> 1;
                        vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                        vkCmdDispatch(cb, (bg_pc.uv_offset + 15u) / 16u,
                                      (((rows + 1u) >> 1) + 7u) / 8u, 1);
                    }
                    if (dst_x > 0) {
                        uint32_t pairs = ((uint32_t)dst_x + 1u) >> 1;
                        bg_pc.y_stride = 0;
                        bg_pc.uv_stride = 0;
                        bg_pc.uv_offset = pairs;
                        vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                        vkCmdDispatch(cb, (pairs + 15u) / 16u,
                                      (((canvas_height + 1u) >> 1) + 7u) / 8u, 1);
                    }
                    if (dst_x + (int32_t)dst_w < (int32_t)canvas_width) {
                        uint32_t x0 = (uint32_t)(dst_x + (int32_t)dst_w);
                        uint32_t pairs = ((canvas_width - x0) + 1u) >> 1;
                        bg_pc.y_stride = 0;
                        bg_pc.uv_stride = x0;
                        bg_pc.uv_offset = pairs;
                        vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                        vkCmdDispatch(cb, (pairs + 15u) / 16u,
                                      (((canvas_height + 1u) >> 1) + 7u) / 8u, 1);
                    }
                }
                for (uint32_t r = 0; r < visible_rect_count; r++) {
                    sbs_native_rect_t vr = visible_rects[r];
                    sbs_native_p010_direct_pc_t sub_pc = pc;
                    uint32_t block_w;
                    uint32_t block_h;
                    uint32_t wg_x = tex->drm_format == SBS_DRM_FORMAT_AMLY ? 16u : 8u;
                    uint32_t wg_y = tex->drm_format == SBS_DRM_FORMAT_AMLY ? 16u : 8u;

                    if (vr.x1 <= vr.x || vr.y1 <= vr.y ||
                        vr.x < dst_x || vr.y < dst_y)
                        continue;
                    if (use_source_split) {
                        uint32_t local_x0 = (uint32_t)(vr.x - dst_x);
                        uint32_t local_y0 = (uint32_t)(vr.y - dst_y);
                        uint32_t local_x1 = (uint32_t)(vr.x1 - dst_x);
                        uint32_t local_y1 = (uint32_t)(vr.y1 - dst_y);
                        uint32_t src_x0 = (uint32_t)floorf((float)local_x0 * pc.y_scale_x);
                        uint32_t src_y0 = (uint32_t)floorf((float)local_y0 * pc.y_scale_y);
                        uint32_t src_x1 = (uint32_t)ceilf((float)local_x1 * pc.y_scale_x) + 1u;
                        uint32_t src_y1 = (uint32_t)ceilf((float)local_y1 * pc.y_scale_y) + 1u;

                        if (src_x1 > tex->width) src_x1 = tex->width;
                        if (src_y1 > tex->height) src_y1 = tex->height;
                        if (src_x1 <= src_x0 || src_y1 <= src_y0)
                            continue;
                        sub_pc.y_stride = src_y0;
                        sub_pc.uv_stride = src_x0 >> 1;
                        sub_pc.uv_offset = src_x1 > (sub_pc.uv_stride << 1)
                            ? (((src_x1 + 1u) >> 1) - sub_pc.uv_stride) : 0u;
                        block_w = sub_pc.uv_offset;
                        block_h = src_y1 - src_y0;
                        wg_y = 8u;
                    } else {
                        sub_pc.dst_x = vr.x;
                        sub_pc.dst_y = vr.y;
                        sub_pc.dst_w = (uint32_t)(vr.x1 - vr.x);
                        sub_pc.dst_h = (uint32_t)(vr.y1 - vr.y);
                        sub_pc.flags |= SBS_NATIVE_P010_DIRECT_DST_IN_BOUNDS |
                            SBS_NATIVE_P010_DIRECT_SRC_RECT_OFFSET;
                        sub_pc.src_offset_x = (uint32_t)(vr.x - dst_x);
                        sub_pc.src_offset_y = (uint32_t)(vr.y - dst_y);
                        if (tex->drm_format == SBS_DRM_FORMAT_AMLY) {
                            sub_pc.y_stride = sub_pc.src_offset_x;
                            sub_pc.uv_stride = sub_pc.src_offset_y;
                        }
                        block_w = (sub_pc.dst_w + 1u) / 2u;
                        block_h = (sub_pc.dst_h + 1u) / 2u;
                    }
                    vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sub_pc), &sub_pc);
                    vkCmdDispatch(cb, (block_w + wg_x - 1u) / wg_x,
                                  (block_h + wg_y - 1u) / wg_y, 1);
                }
            } else {
                uint32_t block_w = (pc.dst_w + 1u) / 2u;
                uint32_t block_h = (pc.dst_h + 1u) / 2u;
                uint32_t wg_x = direct_pipeline == comp->native_amly_to_nv21_pipeline ? 16u : 8u;
                uint32_t wg_y = direct_pipeline == comp->native_amly_to_nv21_pipeline ? 16u : 8u;
                if (source_driven_amly) {
                    block_w = pc.uv_offset;
                    block_h = pc.src_h;
                    wg_x = 16u;
                    wg_y = 8u;
                } else if (item_full_canvas_direct) {
                    block_w = (canvas_width + 1u) / 2u;
                    block_h = (canvas_height + 1u) / 2u;
                    if (tex->drm_format == SBS_DRM_FORMAT_AMLY &&
                        pc.dst_x <= 0 && pc.dst_y == 0 &&
                        pc.dst_x + (int32_t)pc.dst_w >= (int32_t)canvas_width &&
                        pc.dst_h < canvas_height)
                        block_h = (pc.dst_h + 1u) / 2u;
                } else if (source_driven_yuv8) {
                    pc.src_offset_x = 0;
                    pc.src_offset_y = (pc.src_w + 1u) >> 1;
                    block_w = pc.src_offset_y;
                    block_h = pc.src_h;
                    vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
                }
                vkCmdDispatch(cb, (block_w + wg_x - 1u) / wg_x,
                              (block_h + wg_y - 1u) / wg_y, 1);
                if (source_driven_amly && full_canvas_direct) {
                    uint32_t bg_pairs = (canvas_width + 1u) >> 1;
                    sbs_native_p010_direct_pc_t bg_pc = pc;
                    bg_pc.src_h = 0;
                    bg_pc.uv_stride = 0;
                    bg_pc.uv_offset = bg_pairs;
                    if (pc.dst_y > 0) {
                        uint32_t bg_rows = (uint32_t)pc.dst_y < canvas_height
                            ? (uint32_t)pc.dst_y : canvas_height;
                        bg_pc.y_stride = 0;
                        vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                        vkCmdDispatch(cb, (bg_pairs + 15u) / 16u,
                                      (((bg_rows + 1u) >> 1) + 7u) / 8u, 1);
                    }
                    if (pc.dst_y + (int32_t)pc.dst_h < (int32_t)canvas_height) {
                        int32_t bg_start_i = pc.dst_y + (int32_t)pc.dst_h;
                        uint32_t bg_start = bg_start_i > 0 ? (uint32_t)bg_start_i : 0u;
                        uint32_t bg_rows = bg_start < canvas_height ? canvas_height - bg_start : 0u;
                        if (bg_rows > 0) {
                            bg_pc.y_stride = bg_start;
                            vkCmdPushConstants(cb, comp->native_p010_direct_pipeline_layout,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(bg_pc), &bg_pc);
                            vkCmdDispatch(cb, (bg_pairs + 15u) / 16u,
                                          (((bg_rows + 1u) >> 1) + 7u) / 8u, 1);
                        }
                    }
                }
            }
            native_timing_record_layer_end(comp, cb, timing, item, tex,
                                            split_full_canvas_direct ? canvas_width : pc.dst_w,
                                            split_full_canvas_direct ? canvas_height : pc.dst_h,
                                            split_full_canvas_direct ? 0 : pc.dst_x,
                                            split_full_canvas_direct ? 0 : pc.dst_y,
                                            true);
            native_canvas_layer_barrier(cb, entry);
            base_layer_used = true;
            tex->upload_pending = false;
            {
                static uint64_t direct_layer_count = 0;
                direct_layer_count++;
                if (direct_layer_count <= 5) {
                    LOG_I("native direct YUV dispatch entry=%u item=%s dst=%dx%d+%d+%d src=%ux%u fmt=%.4s stride=%u opacity=%.3f mode=%s",
                          log_idx, item->source_id, (int)pc.dst_w, (int)pc.dst_h,
                          pc.dst_x, pc.dst_y, pc.src_w, pc.src_h,
                          (const char *)&tex->drm_format,
                          pc.y_stride, pc.opacity,
                          entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr");
                }
            }
            continue;
        }

        VkDescriptorImageInfo src_info = {
            .sampler = comp->source_sampler,
            .imageView = tex->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet writes[3] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &src_info },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &y_info },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor_set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &uv_info },
        };
        vkUpdateDescriptorSets(comp->device, 3, writes, 0, NULL);

        sbs_native_yuv_pc_t pc = {
            .canvas_width = canvas_width,
            .canvas_height = canvas_height,
            .dst_x = dst_x,
            .dst_y = dst_y,
            .dst_w = dst_w,
            .dst_h = dst_h,
            .src_w = tex->width,
            .src_h = tex->height,
            .src_x = src_x,
            .src_y = src_y,
            .src_rect_w = src_rect_w,
            .src_rect_h = src_rect_h,
            .opacity = native_clampf01(item->opacity * opacity_scale),
            .flags = native_transform_flags(item),
            .filter_flags = item->filter_flags,
        };
        memcpy(pc.filter_params_a, item->filter_params, sizeof(float) * 4);
        memcpy(pc.filter_params_b, item->filter_params + 4, sizeof(float) * 4);

        sbs_native_rect_t visible_rects[SBS_NATIVE_OCCLUSION_MAX_RECTS];
        uint32_t visible_rect_count = 0;
        bool split_rgba_layer = native_rotation_quadrant(item->rotation_deg) == 0 &&
            !item->flip_horizontal && !item->flip_vertical &&
            native_build_layer_visible_rects(
            comp, entry, items, count, i,
            (sbs_native_rect_t){ dst_x, dst_y,
                                 dst_x + (int32_t)dst_w,
                                 dst_y + (int32_t)dst_h },
            opacity_scale, scale_x, scale_y, canvas_width, canvas_height,
            ref_items, ref_count, visible_rects, &visible_rect_count);

        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                comp->native_yuv_pipeline_layout, 0, 1,
                                &descriptor_set, 0, NULL);

        if (split_rgba_layer) {
            for (uint32_t r = 0; r < visible_rect_count; r++) {
                sbs_native_rect_t vr = visible_rects[r];
                uint32_t rel_x0, rel_y0, rel_x1, rel_y1;
                uint32_t src_x0, src_y0, src_x1, src_y1;
                sbs_native_yuv_pc_t sub_pc = pc;

                if (vr.x1 <= vr.x || vr.y1 <= vr.y)
                    continue;
                rel_x0 = (uint32_t)(vr.x - dst_x);
                rel_y0 = (uint32_t)(vr.y - dst_y);
                rel_x1 = (uint32_t)(vr.x1 - dst_x);
                rel_y1 = (uint32_t)(vr.y1 - dst_y);
                src_x0 = src_x + (uint32_t)(((uint64_t)rel_x0 * src_rect_w) / dst_w);
                src_y0 = src_y + (uint32_t)(((uint64_t)rel_y0 * src_rect_h) / dst_h);
                src_x1 = src_x + (uint32_t)(((uint64_t)rel_x1 * src_rect_w + dst_w - 1u) / dst_w);
                src_y1 = src_y + (uint32_t)(((uint64_t)rel_y1 * src_rect_h + dst_h - 1u) / dst_h);
                if (src_x1 <= src_x0 || src_y1 <= src_y0)
                    continue;

                sub_pc.dst_x = vr.x;
                sub_pc.dst_y = vr.y;
                sub_pc.dst_w = (uint32_t)(vr.x1 - vr.x);
                sub_pc.dst_h = (uint32_t)(vr.y1 - vr.y);
                sub_pc.src_x = src_x0;
                sub_pc.src_y = src_y0;
                sub_pc.src_rect_w = src_x1 - src_x0;
                sub_pc.src_rect_h = src_y1 - src_y0;
                vkCmdPushConstants(cb, comp->native_yuv_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(sub_pc), &sub_pc);
                uint32_t wg_px_x = 32u;
                uint32_t wg_px_y = 16u;
                vkCmdDispatch(cb, (sub_pc.dst_w + wg_px_x - 1u) / wg_px_x,
                              (sub_pc.dst_h + wg_px_y - 1u) / wg_px_y, 1);
            }
            if (visible_rect_count > 0) {
                native_timing_record_layer_end(comp, cb, timing, item, tex,
                                                pc.dst_w, pc.dst_h, pc.dst_x, pc.dst_y,
                                                false);
                native_canvas_layer_barrier(cb, entry);
            }
        } else {
            vkCmdPushConstants(cb, comp->native_yuv_pipeline_layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            uint32_t wg_px_x = 32u;
            uint32_t wg_px_y = 16u;
            vkCmdDispatch(cb, (pc.dst_w + wg_px_x - 1u) / wg_px_x,
                          (pc.dst_h + wg_px_y - 1u) / wg_px_y, 1);
            native_timing_record_layer_end(comp, cb, timing, item, tex,
                                            pc.dst_w, pc.dst_h, pc.dst_x, pc.dst_y,
                                            false);
            native_canvas_layer_barrier(cb, entry);
            base_layer_used = true;
        }
        {
            static uint64_t native_layer_count = 0;
            native_layer_count++;
            if (native_layer_count <= 5) {
                LOG_I("native YUV layer dispatch entry=%u item=%s dst=%dx%d+%d+%d opacity=%.3f mode=%s",
                      log_idx, item->source_id, (int)pc.dst_w, (int)pc.dst_h,
                      pc.dst_x, pc.dst_y, pc.opacity,
                      entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr");
            }
        }
    }
}

static void native_record_encoder_copy(VkCommandBuffer cb,
                                       sbs_native_canvas_entry_t *entry)
{
    if (!entry || entry->encoder_buffer == VK_NULL_HANDLE ||
        entry->encoder_size == 0)
        return;

    VkImageMemoryBarrier pre[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = entry->y.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = entry->uv.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, pre);

    uint32_t y_bpp = entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? 2u : 1u;
    uint32_t uv_bpp = entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? 4u : 2u;

    VkBufferImageCopy y_copy = {
        .bufferOffset = entry->y.offset,
        .bufferRowLength = (uint32_t)(entry->y.stride / y_bpp),
        .bufferImageHeight = entry->y.height,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageExtent = { entry->y.width, entry->y.height, 1 },
    };
    VkBufferImageCopy uv_copy = {
        .bufferOffset = entry->uv.offset,
        .bufferRowLength = (uint32_t)(entry->uv.stride / uv_bpp),
        .bufferImageHeight = entry->uv.height,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageExtent = { entry->uv.width, entry->uv.height, 1 },
    };

    vkCmdCopyImageToBuffer(cb, entry->y.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           entry->encoder_buffer, 1, &y_copy);
    vkCmdCopyImageToBuffer(cb, entry->uv.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           entry->encoder_buffer, 1, &uv_copy);

    VkImageMemoryBarrier post_img[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };

    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 2, post_img);
    entry->y.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    entry->uv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = entry->encoder_buffer,
        .offset = 0,
        .size = entry->encoder_size,
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT,
                          0, 0, NULL, 1, &barrier, 0, NULL);
}

static VkPipeline native_downscale_pipeline_for_mode(sbs_compositor_t *comp,
                                                     sbs_export_color_mode_t color_mode)
{
    if (!comp)
        return VK_NULL_HANDLE;
    return color_mode == SBS_EXPORT_COLOR_HDR10
        ? comp->native_downscale_hdr_pipeline
        : comp->native_downscale_sdr_pipeline;
}

static bool native_record_cached_canvas_copy(sbs_compositor_t *comp,
                                             VkCommandBuffer cb,
                                             sbs_native_canvas_entry_t *src_entry,
                                             sbs_native_canvas_entry_t *dst_entry)
{
    if (!comp || cb == VK_NULL_HANDLE || !src_entry || !dst_entry ||
        src_entry == dst_entry || src_entry->color_mode != dst_entry->color_mode)
        return false;

    VkImageMemoryBarrier pre[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = src_entry->y.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = src_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = src_entry->uv.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .image = src_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = dst_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = dst_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 4, pre);

    VkImageCopy y_copy = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { dst_entry->y.width, dst_entry->y.height, 1 },
    };
    VkImageCopy uv_copy = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { dst_entry->uv.width, dst_entry->uv.height, 1 },
    };
    vkCmdCopyImage(cb,
                   src_entry->y.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst_entry->y.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &y_copy);
    vkCmdCopyImage(cb,
                   src_entry->uv.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst_entry->uv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &uv_copy);

    VkImageMemoryBarrier post[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = src_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = src_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = dst_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = dst_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 4, post);
    src_entry->y.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src_entry->uv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_entry->y.layout = VK_IMAGE_LAYOUT_GENERAL;
    dst_entry->uv.layout = VK_IMAGE_LAYOUT_GENERAL;
    return true;
}

static int sbs_compositor_submit_native_preview_from_entry(sbs_compositor_t *comp,
                                                           sbs_native_canvas_entry_t *src_entry,
                                                           uint64_t frame_number,
                                                           uint64_t content_frame_number)
{
    uint32_t idx = 0;
    sbs_native_canvas_entry_t *dst_entry = NULL;

    if (!comp || !src_entry || !comp->native_preview.initialized)
        return -1;
    if (src_entry->color_mode != comp->native_preview.color_mode)
        return -1;

    VkPipeline pipeline = native_downscale_pipeline_for_mode(comp, src_entry->color_mode);
    if (pipeline == VK_NULL_HANDLE ||
        comp->native_downscale_pipeline_layout == VK_NULL_HANDLE ||
        comp->native_downscale_ds_layout == VK_NULL_HANDLE) {
        static uint32_t warn_counter = 0;
        if (warn_counter++ % 300 == 0) {
            LOG_W("native preview downscale pipeline unavailable for %s",
                  src_entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr");
        }
        return -1;
    }

    if (sbs_compositor_native_preview_acquire(comp, &idx, &dst_entry) != 0)
        return -1;
    if (!dst_entry || idx >= SBS_NATIVE_CANVAS_RING_SIZE ||
        dst_entry->cmd_buffer == VK_NULL_HANDLE ||
        comp->native_downscale_ds[idx] == VK_NULL_HANDLE) {
        if (dst_entry) {
            atomic_store_explicit(&dst_entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                                  memory_order_release);
        }
        return -1;
    }

    VkCommandBuffer cb = dst_entry->cmd_buffer;
    vkResetFences(comp->device, 1, &dst_entry->fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = vkBeginCommandBuffer(cb, &cbbi);
    if (res != VK_SUCCESS) {
        LOG_W("native preview downscale command buffer begin failed: %d", res);
        atomic_store_explicit(&dst_entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                              memory_order_release);
        return -1;
    }

    VkDescriptorImageInfo img_infos[4] = {
        { .imageView = src_entry->y.view,  .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = src_entry->uv.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = dst_entry->y.view,  .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
        { .imageView = dst_entry->uv.view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL },
    };
    VkWriteDescriptorSet writes[4] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->native_downscale_ds[idx], .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img_infos[0] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->native_downscale_ds[idx], .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img_infos[1] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->native_downscale_ds[idx], .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img_infos[2] },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = comp->native_downscale_ds[idx], .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &img_infos[3] },
    };
    vkUpdateDescriptorSets(comp->device, 4, writes, 0, NULL);

    VkAccessFlags dst_old_access = dst_entry->y.layout == VK_IMAGE_LAYOUT_UNDEFINED
        ? 0
        : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageMemoryBarrier pre[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = src_entry->y.layout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = src_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = src_entry->uv.layout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = src_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = dst_old_access,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = dst_entry->y.layout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = dst_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = dst_old_access,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = dst_entry->uv.layout,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .image = dst_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 4, pre);

    sbs_native_downscale_pc_t pc = {
        .src_w = src_entry->y.width,
        .src_h = src_entry->y.height,
        .dst_w = dst_entry->y.width,
        .dst_h = dst_entry->y.height,
        .factor = 1u,
    };
    if (pc.dst_w > 0 && pc.dst_h > 0 &&
        pc.src_w % pc.dst_w == 0 && pc.src_h % pc.dst_h == 0) {
        uint32_t factor_x = pc.src_w / pc.dst_w;
        uint32_t factor_y = pc.src_h / pc.dst_h;
        if (factor_x == factor_y &&
            (factor_x == 1u || factor_x == 2u || factor_x == 4u || factor_x == 8u)) {
            pc.factor = factor_x;
        }
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                            comp->native_downscale_pipeline_layout, 0, 1,
                            &comp->native_downscale_ds[idx], 0, NULL);
    vkCmdPushConstants(cb, comp->native_downscale_pipeline_layout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    uint32_t block_w = (pc.dst_w + 1u) / 2u;
    uint32_t block_h = (pc.dst_h + 1u) / 2u;
    vkCmdDispatch(cb, (block_w + 7u) / 8u, (block_h + 7u) / 8u, 1);

    VkImageMemoryBarrier post[4] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = src_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = src_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = dst_entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = dst_entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 4, post);
    src_entry->y.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    src_entry->uv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_entry->y.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    dst_entry->uv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (dst_entry->color_mode != SBS_EXPORT_COLOR_HDR10)
        native_record_encoder_copy(cb, dst_entry);

    res = vkEndCommandBuffer(cb);
    if (res != VK_SUCCESS) {
        LOG_W("native preview downscale command buffer end failed: %d", res);
        atomic_store_explicit(&dst_entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                              memory_order_release);
        return -1;
    }

    dst_entry->frame_number = frame_number;
    dst_entry->content_frame_number = content_frame_number != 0
        ? content_frame_number : frame_number;
    dst_entry->submit_time_us = monotonic_us();
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, dst_entry->fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        LOG_W("native preview downscale submit failed: %d", res);
        atomic_store_explicit(&dst_entry->state, SBS_NATIVE_CANVAS_ENTRY_FREE,
                              memory_order_release);
        return -1;
    }

    {
        static uint64_t submit_count = 0;
        submit_count++;
        if (submit_count <= 5 || (submit_count % 300) == 0) {
            LOG_I("native preview downscale submit entry=%u frame=%lu %ux%u->%ux%u mode=%s",
                  idx, (unsigned long)frame_number,
                  src_entry->y.width, src_entry->y.height,
                  dst_entry->y.width, dst_entry->y.height,
                  src_entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr");
        }
    }
    return 0;
}

int sbs_compositor_render_native_frame(sbs_compositor_t *comp,
                                        const sbs_comp_scene_state_t *scene,
                                        uint64_t frame_number,
                                        uint64_t content_frame_number,
                                        uint32_t transition_prev_entry_idx,
                                        uint32_t *entry_idx_out)
{
    uint32_t idx = 0;
    sbs_native_canvas_entry_t *entry = NULL;
    float r = scene ? scene->background_rgba[0] : 0.1f;
    float g = scene ? scene->background_rgba[1] : 0.1f;
    float b = scene ? scene->background_rgba[2] : 0.1f;
    float y = 0.0f, u = 0.5f, v = 0.5f;

    if (entry_idx_out)
        *entry_idx_out = UINT32_MAX;

    if (!comp || !comp->native_canvas.initialized)
        return -1;

    sbs_compositor_native_canvas_poll_ready(comp);
    sbs_compositor_native_preview_poll_ready(comp);

    if (sbs_compositor_native_canvas_acquire(comp, &idx, &entry) != 0)
        return -1;

    if (entry->color_mode == SBS_EXPORT_COLOR_HDR10)
        rgb_to_bt2020_yuv(r, g, b, &y, &u, &v);
    else
        rgb_to_bt709_yuv(r, g, b, &y, &u, &v);

    VkCommandBuffer cb = entry->cmd_buffer;
    if (cb == VK_NULL_HANDLE) {
        sbs_compositor_native_canvas_release_failed(comp, idx);
        return -1;
    }

    vkResetFences(comp->device, 1, &entry->fence);
    vkResetCommandBuffer(cb, 0);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = vkBeginCommandBuffer(cb, &cbbi);
    if (res != VK_SUCCESS) {
        LOG_W("native canvas command buffer begin failed: %d", res);
        sbs_compositor_native_canvas_release_failed(comp, idx);
        return -1;
    }

    uint32_t timing_base = UINT32_MAX;
    entry->timing_query_valid = false;
    entry->timing_mark_count = 0;
    entry->timing_layer_count = 0;
    memset(entry->timing_layer_labels, 0, sizeof(entry->timing_layer_labels));
    if (comp->native_timing_available &&
        comp->native_timing_query_pool != VK_NULL_HANDLE) {
        timing_base = native_timing_query_base(idx);
        if (timing_base != UINT32_MAX) {
            vkCmdResetQueryPool(cb, comp->native_timing_query_pool,
                                timing_base, SBS_NATIVE_TIMING_QUERY_MARKS);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                comp->native_timing_query_pool, timing_base + 0u);
            entry->timing_query_base = timing_base;
            entry->timing_mark_count = 4u;
            entry->timing_query_valid = true;
        }
    }
    sbs_native_timing_ctx_t timing_ctx = {
        .base = timing_base,
        .next_mark = 2u,
        .entry = entry,
    };

    sbs_native_canvas_entry_t *transition_prev_entry = NULL;
    if (scene && scene->transition_active &&
        transition_prev_entry_idx < SBS_NATIVE_CANVAS_RING_SIZE) {
        sbs_native_canvas_entry_t *candidate = native_canvas_lookup_entry(
            comp, transition_prev_entry_idx);
        if (candidate && candidate != entry && candidate->allocated &&
            candidate->color_mode == entry->color_mode &&
            atomic_load_explicit(&candidate->state, memory_order_acquire) ==
                SBS_NATIVE_CANVAS_ENTRY_READY) {
            transition_prev_entry = candidate;
        }
    }

    bool preview_will_render = false;
    if (comp->native_preview.initialized) {
        uint32_t interval = comp->native_preview.frame_interval > 0
            ? comp->native_preview.frame_interval : 1u;
        preview_will_render = interval <= 1u ||
            comp->native_preview.last_submit_frame == 0 ||
            frame_number - comp->native_preview.last_submit_frame >= interval;
    }

    bool need_rgba_upload = native_scene_needs_rgba_upload(comp, entry, scene);
    if (need_rgba_upload)
        native_record_source_uploads(comp, cb);

    bool have_active_native_layers = scene && scene->active_item_count > 0 &&
        comp->native_yuv_pipeline_layout != VK_NULL_HANDLE;
    bool have_cached_transition = transition_prev_entry != NULL;
    bool have_native_layers = have_active_native_layers || have_cached_transition;
    bool full_canvas_direct = native_scene_can_full_canvas_direct_yuv(comp, entry, scene);
    bool targeted_bg_fill = !full_canvas_direct &&
        native_scene_can_targeted_bg_fill_yuv(comp, entry, scene);

    VkImageMemoryBarrier pre[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = (full_canvas_direct || targeted_bg_fill)
                ? VK_ACCESS_SHADER_WRITE_BIT
                : VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = entry->y.layout,
            .newLayout = (full_canvas_direct || targeted_bg_fill)
                ? VK_IMAGE_LAYOUT_GENERAL
                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = entry->y.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = (full_canvas_direct || targeted_bg_fill)
                ? VK_ACCESS_SHADER_WRITE_BIT
                : VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = entry->uv.layout,
            .newLayout = (full_canvas_direct || targeted_bg_fill)
                ? VK_IMAGE_LAYOUT_GENERAL
                : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image = entry->uv.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(cb,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         (full_canvas_direct || targeted_bg_fill)
                            ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                            : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 2, pre);

    if (!full_canvas_direct && !targeted_bg_fill) {
        VkClearColorValue y_clear = { .float32 = { y, 0.0f, 0.0f, 1.0f } };
        /* SDR canvas currently follows the existing NV21-family output order:
         * R=V, G=U. HDR10 uses P010 CbCr order: R=U, G=V. */
        VkClearColorValue uv_clear = entry->color_mode == SBS_EXPORT_COLOR_HDR10
            ? (VkClearColorValue){ .float32 = { u, v, 0.0f, 1.0f } }
            : (VkClearColorValue){ .float32 = { v, u, 0.0f, 1.0f } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cb, entry->y.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &y_clear, 1, &range);
        vkCmdClearColorImage(cb, entry->uv.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &uv_clear, 1, &range);

        VkImageMemoryBarrier to_general[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = have_native_layers
                    ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
                    : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT),
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = have_native_layers
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = entry->y.image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = have_native_layers
                    ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
                    : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT),
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = have_native_layers
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = entry->uv.image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
        };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             have_native_layers
                                ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                : (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT),
                             0, 0, NULL, 0, NULL, 2, to_general);
    }
    if (timing_base != UINT32_MAX) {
        vkCmdWriteTimestamp(cb, (full_canvas_direct || targeted_bg_fill)
                                 ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                 : VK_PIPELINE_STAGE_TRANSFER_BIT,
                            comp->native_timing_query_pool, timing_base + 1u);
    }

    if (have_native_layers) {
        uint32_t layer_descriptor_idx = 0;
        entry->y.layout = VK_IMAGE_LAYOUT_GENERAL;
        entry->uv.layout = VK_IMAGE_LAYOUT_GENERAL;
        if (have_cached_transition) {
            (void)native_record_cached_canvas_copy(
                comp, cb, transition_prev_entry, entry);
        }
        if (have_active_native_layers) {
            native_dispatch_source_layers(comp, cb, idx,
                                          comp->native_yuv_ds[idx],
                                          comp->native_p010_direct_ds[idx], entry,
                                          comp->width, comp->height,
                                          scene->active_items,
                                          scene->active_item_count,
                                           scene->transition_active ? scene->transition_progress : 1.0f,
                                           1.0f, 1.0f,
                                           full_canvas_direct, targeted_bg_fill, y, u, v,
                                          &layer_descriptor_idx,
                                          timing_base != UINT32_MAX ? &timing_ctx : NULL,
                                          NULL, 0);
        }

        VkImageMemoryBarrier post[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = entry->y.image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .image = entry->uv.image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
        };
        vkCmdPipelineBarrier(cb,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 2, post);
    }
    if (timing_base != UINT32_MAX) {
        uint32_t layers_done_mark = comp->native_timing_detail
            ? timing_ctx.next_mark : 2u;
        vkCmdWriteTimestamp(cb, have_native_layers
                                ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                : VK_PIPELINE_STAGE_TRANSFER_BIT,
                            comp->native_timing_query_pool,
                            timing_base + layers_done_mark);
        timing_ctx.next_mark = layers_done_mark + 1u;
    }
    entry->y.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    entry->uv.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    native_record_encoder_copy(cb, entry);
    if (timing_base != UINT32_MAX) {
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            comp->native_timing_query_pool,
                            timing_base + timing_ctx.next_mark);
        entry->timing_mark_count = timing_ctx.next_mark + 1u;
    }

    res = vkEndCommandBuffer(cb);
    if (res != VK_SUCCESS) {
        LOG_W("native canvas command buffer end failed: %d", res);
        sbs_compositor_native_canvas_release_failed(comp, idx);
        return -1;
    }

    sbs_compositor_native_canvas_mark_rendering(comp, idx);
    entry->frame_number = frame_number;
    entry->content_frame_number = content_frame_number != 0
        ? content_frame_number : frame_number;

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    entry->submit_time_us = monotonic_us();
    pthread_mutex_lock(&comp->queue_mutex);
    res = vkQueueSubmit(comp->graphics_queue, 1, &si, entry->fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        sbs_compositor_native_canvas_release_failed(comp, idx);
        LOG_W("native canvas submit failed: %d", res);
        return -1;
    }

    if (entry_idx_out)
        *entry_idx_out = idx;

    if (preview_will_render &&
        sbs_compositor_submit_native_preview_from_entry(
            comp, entry, frame_number, entry->content_frame_number) == 0) {
        comp->native_preview.last_submit_frame = frame_number;
    }

    {
        static uint64_t native_submit_count = 0;
        native_submit_count++;
        if (native_submit_count <= 5) {
            LOG_I("native canvas submit entry=%u frame=%lu mode=%s y=%.4f u=%.4f v=%.4f",
                  idx, (unsigned long)frame_number,
                  entry->color_mode == SBS_EXPORT_COLOR_HDR10 ? "hdr10" : "sdr",
                  y, u, v);
        }
    }

    return 0;
}

int sbs_compositor_render_frame(sbs_compositor_t *comp, const sbs_comp_scene_state_t *scene)
{
    uint32_t idx = comp->current_target;
    VkCommandBuffer cb = comp->targets[idx].cmd_buffer;

    if (scene)
        prepare_luts_for_items(comp, scene->active_items, scene->active_item_count);

    vkResetFences(comp->device, 1, &comp->targets[idx].fence);

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &cbbi);

    /* OPT-3: Batch deferred source texture uploads into the render command
     * buffer.  For each source with upload_pending=true, record a layout
     * transition → upload → transition back, all synchronized via pipeline
     * barriers instead of separate fence stalls.
     *
     * Two upload paths:
     *   - DMA-BUF imported: compute shader YUV→RGBA conversion
     *   - CPU staging: vkCmdCopyBufferToImage
     */
    {
        /* Separate staging and compute pending sources */
        VkImageMemoryBarrier staging_pre_barriers[SBS_MAX_SOURCE_TEXTURES];
        VkImageMemoryBarrier staging_post_barriers[SBS_MAX_SOURCE_TEXTURES];
        uint32_t staging_slots[SBS_MAX_SOURCE_TEXTURES];
        uint32_t staging_count = 0;

        VkImageMemoryBarrier compute_pre_barriers[SBS_MAX_SOURCE_TEXTURES];
        VkImageMemoryBarrier compute_post_barriers[SBS_MAX_SOURCE_TEXTURES];
        uint32_t compute_slots[SBS_MAX_SOURCE_TEXTURES];
        uint32_t compute_count = 0;

        for (uint32_t s = 0; s < SBS_MAX_SOURCE_TEXTURES; s++) {
            sbs_source_texture_t *tex = &comp->sources[s];
            if (!tex->allocated || !tex->upload_pending)
                continue;

            if (tex->dmabuf_imported && source_drm_format_is_rgba(tex->drm_format) &&
                tex->y_buf != VK_NULL_HANDLE) {
                staging_slots[staging_count] = s;
                staging_pre_barriers[staging_count] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = tex->has_content
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .image = tex->image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1, .layerCount = 1,
                    },
                };
                staging_count++;
            } else if (tex->dmabuf_imported && comp->compute_pipeline != VK_NULL_HANDLE) {
                compute_slots[compute_count] = s;
                compute_pre_barriers[compute_count] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = tex->has_content
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .image = tex->image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1, .layerCount = 1,
                    },
                };
                compute_count++;
            } else if (tex->staging_buf != VK_NULL_HANDLE) {
                staging_slots[staging_count] = s;
                staging_pre_barriers[staging_count] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = tex->has_content
                        ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .image = tex->image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1, .layerCount = 1,
                    },
                };
                staging_count++;
            } else {
                tex->upload_pending = false;
            }
        }

        /* Staging buffer upload path */
        if (staging_count > 0) {
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, staging_count, staging_pre_barriers);

            for (uint32_t p = 0; p < staging_count; p++) {
                sbs_source_texture_t *tex = &comp->sources[staging_slots[p]];
                VkBuffer src = source_drm_format_is_rgba(tex->drm_format) && tex->y_buf != VK_NULL_HANDLE
                    ? tex->y_buf : tex->staging_buf;
                VkBufferImageCopy copy = {
                    .bufferRowLength = source_drm_format_is_rgba(tex->drm_format) && tex->y_stride > 0
                        ? tex->y_stride / 4u : 0,
                    .imageSubresource = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .layerCount = 1,
                    },
                    .imageExtent = { tex->width, tex->height, 1 },
                };
                vkCmdCopyBufferToImage(cb, src, tex->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            }

            for (uint32_t p = 0; p < staging_count; p++) {
                sbs_source_texture_t *tex = &comp->sources[staging_slots[p]];
                staging_post_barriers[p] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .image = tex->image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1, .layerCount = 1,
                    },
                };
                tex->upload_pending = false;
                tex->has_content = true;
            }
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, NULL, 0, NULL, staging_count, staging_post_barriers);
        }

        /* Compute shader upload path (DMA-BUF import) */
        if (compute_count > 0) {
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, NULL, 0, NULL, compute_count, compute_pre_barriers);

            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, comp->compute_pipeline);

            for (uint32_t p = 0; p < compute_count; p++) {
                uint32_t slot = compute_slots[p];
                sbs_source_texture_t *tex = &comp->sources[slot];

                /* Update compute descriptor set for this slot */
                VkDescriptorImageInfo img_info = {
                    .imageView = tex->view,
                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                };
                VkDescriptorBufferInfo buf_info = {
                    .buffer = tex->y_buf,
                    .offset = 0,
                    .range = VK_WHOLE_SIZE,
                };
                VkWriteDescriptorSet writes[2] = {
                    {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = comp->compute_descriptor_sets[slot],
                        .dstBinding = 0,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &img_info,
                    },
                    {
                        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = comp->compute_descriptor_sets[slot],
                        .dstBinding = 1,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                        .pBufferInfo = &buf_info,
                    },
                };
                vkUpdateDescriptorSets(comp->device, 2, writes, 0, NULL);

                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                        comp->compute_pipeline_layout, 0, 1,
                                        &comp->compute_descriptor_sets[slot], 0, NULL);

                uint32_t format;
                if (tex->drm_format == DRM_FORMAT_P010) {
                    format = 2u; /* 10-bit semi-planar (P010/NV12_10LE) */
                } else if (tex->drm_format == DRM_FORMAT_NV21) {
                    format = 0u; /* NV21 8-bit (VU order) */
                } else {
                    format = 1u; /* NV12 8-bit (UV order) */
                }
                uint32_t flags = 0u;
                if (tex->drm_format == DRM_FORMAT_P010 &&
                    (tex->upload_filter_flags & SBS_COMP_FILTER_HDR_TO_SDR_LUT) != 0)
                    flags |= 1u;
                uint32_t push[7] = {
                    tex->width,
                    tex->height,
                    tex->y_stride,
                    tex->uv_stride,
                    tex->uv_offset,
                    format,
                    flags,
                };
                vkCmdPushConstants(cb, comp->compute_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);

                uint32_t groups_x = (tex->width + 15) / 16;
                uint32_t groups_y = (tex->height + 15) / 16;
                vkCmdDispatch(cb, groups_x, groups_y, 1);
            }

            for (uint32_t p = 0; p < compute_count; p++) {
                sbs_source_texture_t *tex = &comp->sources[compute_slots[p]];
                compute_post_barriers[p] = (VkImageMemoryBarrier){
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    .image = tex->image,
                    .subresourceRange = {
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .levelCount = 1, .layerCount = 1,
                    },
                };
                tex->upload_pending = false;
            }
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, NULL, 0, NULL, compute_count, compute_post_barriers);
        }
    }

    {
        VkImageMemoryBarrier imported_image_barriers[SBS_MAX_SOURCE_TEXTURES];
        uint32_t imported_image_count = 0;

        for (uint32_t s = 0; s < SBS_MAX_SOURCE_TEXTURES; s++) {
            sbs_source_texture_t *tex = &comp->sources[s];

            /* AFBC RGBA image imports */
            if (tex->dmabuf_image_imported && tex->dmabuf_image != VK_NULL_HANDLE) {
                if (tex->dmabuf_image_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                    imported_image_barriers[imported_image_count++] = (VkImageMemoryBarrier){
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = 0,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                        .oldLayout = tex->dmabuf_image_layout,
                        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .image = tex->dmabuf_image,
                        .subresourceRange = {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = 1,
                            .layerCount = 1,
                        },
                    };
                    tex->dmabuf_image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    /* Sync layout to cache entry */
                    if (tex->dmabuf_image_active_idx >= 0 &&
                        tex->dmabuf_image_active_idx < SBS_DMABUF_IMAGE_CACHE_SIZE) {
                        tex->dmabuf_image_cache[tex->dmabuf_image_active_idx].layout =
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    }
                }
                continue;
            }

            /* YCbCr NV12 image imports — need layout transition on first use */
            if (tex->ycbcr_imported && tex->ycbcr_active_view != VK_NULL_HANDLE &&
                tex->dmabuf_buf_active_idx >= 0) {
                struct sbs_dmabuf_buf_cache_entry *ce =
                    &tex->dmabuf_buf_cache[tex->dmabuf_buf_active_idx];
                if (ce->is_ycbcr && ce->ycbcr_image != VK_NULL_HANDLE) {
                    /* Always transition from UNDEFINED — external DMA-BUF images
                     * don't retain layout across frames. */
                    imported_image_barriers[imported_image_count++] = (VkImageMemoryBarrier){
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .srcAccessMask = 0,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        .image = ce->ycbcr_image,
                        .subresourceRange = {
                            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                            .levelCount = 1,
                            .layerCount = 1,
                        },
                    };
                }
            }
        }

        if (imported_image_count > 0) {
            vkCmdPipelineBarrier(cb,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, NULL, 0, NULL, imported_image_count, imported_image_barriers);
        }
    }

    /* OPT-1: Mali G52 transaction elimination — only transition from
     * UNDEFINED on the very first use of this target.  After that, the
     * target stays in COLOR_ATTACHMENT_OPTIMAL (a "safe" layout on Mali
     * that preserves AFBC/CRC signatures), and the render pass
     * initialLayout=COLOR_ATTACHMENT_OPTIMAL matches without needing an
     * explicit barrier. */
    if (comp->targets[idx].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = comp->targets[idx].layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = comp->targets[idx].image,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                              0, 0, NULL, 0, NULL, 1, &barrier);
        comp->targets[idx].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    VkClearValue clear = {
        .color = {
            .float32 = {
                scene ? scene->background_rgba[0] : 0.1f,
                scene ? scene->background_rgba[1] : 0.1f,
                scene ? scene->background_rgba[2] : 0.1f,
                scene ? scene->background_rgba[3] : 1.0f,
            }
        }
    };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = comp->render_pass,
        .framebuffer = comp->targets[idx].framebuffer,
        .renderArea = { { 0, 0 }, { comp->width, comp->height } },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };

    vkCmdBeginRenderPass(cb, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    if (comp->pipeline) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, comp->pipeline);

        VkViewport dyn_vp = {
            .width = (float)comp->width,
            .height = (float)comp->height,
            .maxDepth = 1.0f,
        };
        VkRect2D dyn_scissor = {
            .extent = { comp->width, comp->height },
        };
        vkCmdSetViewport(cb, 0, 1, &dyn_vp);
        vkCmdSetScissor(cb, 0, 1, &dyn_scissor);

        if (scene && scene->transition_active) {
            render_items(comp, cb, scene->previous_items, scene->previous_item_count,
                         1.0f - scene->transition_progress,
                         NULL, 0);
        }

        if (scene) {
            render_items(comp, cb, scene->active_items, scene->active_item_count,
                         scene->transition_active ? scene->transition_progress : 1.0f,
                         scene->transition_active ? scene->previous_items : NULL,
                         scene->transition_active ? scene->previous_item_count : 0);
        }
    }

    vkCmdEndRenderPass(cb);

    /* ── P010 compute conversion (post-render-pass) ─────────────── */
    /* After the render pass completes, dispatch a compute shader to
     * convert the composed RGBA render target to P010 (BT.2020 HDR10).
     * Each work item processes a 2x2 block — no atomics needed.
     * This runs on the HOST_VISIBLE p010_buffer (not DMA-BUF),
     * so Mali G52 compute does not hang. */
    if (comp->p010_conv_available && comp->p010_size > 0) {
        /* Transition render target: COLOR_ATTACHMENT → SHADER_READ_ONLY */
        VkImageMemoryBarrier img_barrier = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = comp->targets[idx].layout,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .image = comp->targets[idx].image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &img_barrier);
        comp->targets[idx].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        /* Zero P010 buffer before compute write */
        vkCmdFillBuffer(cb, comp->p010_buffer[idx], 0, comp->p010_size, 0);
        VkBufferMemoryBarrier fill_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .buffer = comp->p010_buffer[idx],
            .offset = 0,
            .size = comp->p010_size,
        };
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 1, &fill_barrier, 0, NULL);

        /* Dispatch P010 compute */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                          comp->p010_dest_export_pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                comp->p010_dest_export_pipeline_layout, 0, 1,
                                &comp->p010_conv_ds[idx], 0, NULL);

        uint32_t y_stride = comp->width;
        uint32_t uv_stride = comp->width;
        uint32_t uv_offset = comp->width * comp->height * 2;
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t y_stride;
            uint32_t uv_stride;
            uint32_t uv_offset;
            float    inv_width;
            float    inv_height;
        } pc_data;
        pc_data.width = comp->width;
        pc_data.height = comp->height;
        pc_data.y_stride = y_stride;
        pc_data.uv_stride = uv_stride;
        pc_data.uv_offset = uv_offset;
        pc_data.inv_width = 1.0f / (float)comp->width;
        pc_data.inv_height = 1.0f / (float)comp->height;
        vkCmdPushConstants(cb, comp->p010_dest_export_pipeline_layout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc_data), &pc_data);

        uint32_t block_x = ((comp->width + 1u) / 2u + 7u) / 8u;
        uint32_t block_y = ((comp->height + 1u) / 2u + 7u) / 8u;
        vkCmdDispatch(cb, block_x, block_y, 1);

        /* Barrier: compute writes visible for transfer read + host read */
        VkBufferMemoryBarrier compute_barrier = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            .buffer = comp->p010_buffer[idx],
            .offset = 0,
            .size = comp->p010_size,
        };
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 1, &compute_barrier, 0, NULL);

        /* Transition render target back to COLOR_ATTACHMENT for next frame */
        VkImageMemoryBarrier img_back = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = comp->targets[idx].image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cb,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0, 0, NULL, 0, NULL, 1, &img_back);
        comp->targets[idx].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        {
            static uint64_t p010_conv_count = 0;
            p010_conv_count++;
            if (p010_conv_count <= 5) {
                LOG_I("P010 COMPUTE CONV target=%u blocks=%ux%u",
                      idx, block_x, block_y);
            }
        }
    }

    vkEndCommandBuffer(cb);

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &comp->targets[idx].cmd_buffer,
    };

    pthread_mutex_lock(&comp->queue_mutex);
    VkResult res = vkQueueSubmit(comp->graphics_queue, 1, &si, comp->targets[idx].fence);
    pthread_mutex_unlock(&comp->queue_mutex);
    if (res != VK_SUCCESS) {
        LOG_E("vkQueueSubmit failed: %d", res);
        return -1;
    }

    comp->current_target = (idx + 1) % SBS_RENDER_TARGET_COUNT;
    atomic_store(&comp->last_rendered_target, idx);
    return 0;
}

/* ── Init / Destroy ───────────────────────────────────────────── */

int sbs_compositor_init(sbs_compositor_t *comp, uint32_t width, uint32_t height,
                        sbs_export_color_mode_t native_color_mode)
{
    memset(comp, 0, sizeof(*comp));
    pthread_mutex_init(&comp->queue_mutex, NULL);
    pthread_mutex_init(&comp->command_pool_mutex, NULL);
    pthread_mutex_init(&comp->retire_slot_mutex, NULL);
    comp->width = width;
    comp->height = height;

    /* Initialize DMA-BUF cache indices to "no active entry" */
    for (uint32_t s = 0; s < SBS_MAX_SOURCE_TEXTURES; s++) {
        comp->sources[s].dmabuf_image_active_idx = -1;
        comp->sources[s].dmabuf_buf_active_idx = -1;
    }
    for (uint32_t i = 0; i < SBS_NATIVE_CANVAS_RING_SIZE; i++) {
        comp->native_preview.entries[i].backing_fd = -1;
        comp->native_preview.entries[i].y.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        comp->native_preview.entries[i].uv.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    if (create_instance(comp) != 0)
        return -1;
    if (select_physical_device(comp) != 0)
        goto fail;
    if (create_device(comp) != 0)
        goto fail;
    if (check_native_yuv_capabilities(comp) != 0)
        goto fail;
    if (create_native_canvas_ring(comp) != 0)
        goto fail;
    if (allocate_native_canvas_entries(comp, native_color_mode) != 0)
        goto fail;
    if (create_command_pool(comp) != 0)
        goto fail;
    if (create_native_timing_query_pool(comp) != 0)
        goto fail;
    if (create_render_targets(comp) != 0)
        goto fail;
    if (create_staging_buffer(comp) != 0)
        goto fail;
    if (create_render_pass(comp) != 0)
        goto fail;
    if (create_framebuffers(comp) != 0)
        goto fail;
    if (create_pipeline(comp) != 0)
        goto fail;

    /* OPT-8: Create pipeline cache — on Mali G52 this avoids redundant
     * shader compilation if load_shaders() is called multiple times
     * (e.g. on hot-reload).  The cache is in-memory only; for a fixed
     * embedded pipeline, disk serialization adds complexity without
     * meaningful benefit. */
    {
        VkPipelineCacheCreateInfo pcci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .initialDataSize = 0,
            .pInitialData = NULL,
        };
        VkResult cache_res = vkCreatePipelineCache(comp->device, &pcci, NULL,
                                                    &comp->pipeline_cache);
        if (cache_res != VK_SUCCESS) {
            LOG_W("vkCreatePipelineCache failed: %d (non-fatal)", cache_res);
            comp->pipeline_cache = VK_NULL_HANDLE;
        } else {
            LOG_I("pipeline cache created");
        }
    }

    if (create_builtin_hdr_lut(comp) != 0)
        goto fail;

    if (create_placeholder_texture(comp) != 0)
        goto fail;

    comp->initialized = true;
    LOG_I("compositor initialized (%ux%u)", width, height);
    return 0;

fail:
    sbs_compositor_destroy(comp);
    return -1;
}

void sbs_compositor_destroy(sbs_compositor_t *comp)
{
    if (!comp || !comp->device)
        return;

    vkDeviceWaitIdle(comp->device);

    destroy_native_preview_ring(comp);
    destroy_native_canvas_ring(comp);

    /* Destroy source textures */
    for (uint32_t i = 0; i < SBS_MAX_SOURCE_TEXTURES; i++) {
        destroy_source_texture(comp, &comp->sources[i]);
    }

    /* Destroy placeholder texture */
    if (comp->placeholder.allocated) {
        if (comp->placeholder.view)
            vkDestroyImageView(comp->device, comp->placeholder.view, NULL);
        if (comp->placeholder.memory)
            vkFreeMemory(comp->device, comp->placeholder.memory, NULL);
        if (comp->placeholder.image)
            vkDestroyImage(comp->device, comp->placeholder.image, NULL);
    }

    destroy_lut_texture(comp, &comp->hdr_lut_image,
                        &comp->hdr_lut_memory,
                        &comp->hdr_lut_view);
    destroy_lut_texture(comp, &comp->hdr_ycbcr_lut_image,
                        &comp->hdr_ycbcr_lut_memory,
                        &comp->hdr_ycbcr_lut_view);
    if (comp->hdr_ycbcr_lut_sampler)
        vkDestroySampler(comp->device, comp->hdr_ycbcr_lut_sampler, NULL);

    if (comp->source_sampler)
        vkDestroySampler(comp->device, comp->source_sampler, NULL);

    destroy_staging_buffer(comp);

    /* Preview compute resources */
    if (comp->preview_target_view)
        vkDestroyImageView(comp->device, comp->preview_target_view, NULL);
    if (comp->preview_nv21_mapped)
        vkUnmapMemory(comp->device, comp->preview_nv21_memory);
    if (comp->preview_nv21_buffer)
        vkDestroyBuffer(comp->device, comp->preview_nv21_buffer, NULL);
    if (comp->preview_nv21_memory)
        vkFreeMemory(comp->device, comp->preview_nv21_memory, NULL);

    destroy_blit_target(comp, &comp->preview_target);
    destroy_export_surface(comp, &comp->preview_staging);

    if (comp->pipeline)
        vkDestroyPipeline(comp->device, comp->pipeline, NULL);
    if (comp->native_timing_query_pool)
        vkDestroyQueryPool(comp->device, comp->native_timing_query_pool, NULL);
    if (comp->pipeline_cache)
        vkDestroyPipelineCache(comp->device, comp->pipeline_cache, NULL);
    if (comp->pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->pipeline_layout, NULL);
    if (comp->render_pass)
        vkDestroyRenderPass(comp->device, comp->render_pass, NULL);
    if (comp->descriptor_set_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->descriptor_set_layout, NULL);
    if (comp->descriptor_pool)
        vkDestroyDescriptorPool(comp->device, comp->descriptor_pool, NULL);

    /* YCbCr pipeline cleanup */
    if (comp->ycbcr_pipeline)
        vkDestroyPipeline(comp->device, comp->ycbcr_pipeline, NULL);
    if (comp->ycbcr_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->ycbcr_pipeline_layout, NULL);
    if (comp->ycbcr_descriptor_pool)
        vkDestroyDescriptorPool(comp->device, comp->ycbcr_descriptor_pool, NULL);
    if (comp->ycbcr_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->ycbcr_ds_layout, NULL);
    if (comp->ycbcr_sampler)
        vkDestroySampler(comp->device, comp->ycbcr_sampler, NULL);
    if (comp->ycbcr_conversion) {
        PFN_vkDestroySamplerYcbcrConversionKHR pfnDestroy =
            (PFN_vkDestroySamplerYcbcrConversionKHR)vkGetDeviceProcAddr(
                comp->device, "vkDestroySamplerYcbcrConversionKHR");
        if (pfnDestroy)
            pfnDestroy(comp->device, comp->ycbcr_conversion, NULL);
    }

    if (comp->native_yuv_sdr_pipeline)
        vkDestroyPipeline(comp->device, comp->native_yuv_sdr_pipeline, NULL);
    if (comp->native_yuv_hdr_pipeline)
        vkDestroyPipeline(comp->device, comp->native_yuv_hdr_pipeline, NULL);
    if (comp->native_yuv_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->native_yuv_pipeline_layout, NULL);
    if (comp->native_yuv_ds_pool)
        vkDestroyDescriptorPool(comp->device, comp->native_yuv_ds_pool, NULL);
    if (comp->native_yuv_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->native_yuv_ds_layout, NULL);
    if (comp->native_yuv_sdr_shader)
        vkDestroyShaderModule(comp->device, comp->native_yuv_sdr_shader, NULL);
    if (comp->native_yuv_hdr_shader)
        vkDestroyShaderModule(comp->device, comp->native_yuv_hdr_shader, NULL);

    if (comp->native_p010_direct_pipeline)
        vkDestroyPipeline(comp->device, comp->native_p010_direct_pipeline, NULL);
    if (comp->native_p010_to_nv21_pipeline)
        vkDestroyPipeline(comp->device, comp->native_p010_to_nv21_pipeline, NULL);
    if (comp->native_yuv8_to_p010_pipeline)
        vkDestroyPipeline(comp->device, comp->native_yuv8_to_p010_pipeline, NULL);
    if (comp->native_amly_to_p010_pipeline)
        vkDestroyPipeline(comp->device, comp->native_amly_to_p010_pipeline, NULL);
    if (comp->native_amly_to_p010_src_pipeline)
        vkDestroyPipeline(comp->device, comp->native_amly_to_p010_src_pipeline, NULL);
    if (comp->native_yuv8_to_nv21_pipeline)
        vkDestroyPipeline(comp->device, comp->native_yuv8_to_nv21_pipeline, NULL);
    if (comp->native_amly_to_nv21_pipeline)
        vkDestroyPipeline(comp->device, comp->native_amly_to_nv21_pipeline, NULL);
    if (comp->native_amly_to_nv21_src_pipeline)
        vkDestroyPipeline(comp->device, comp->native_amly_to_nv21_src_pipeline, NULL);
    if (comp->native_p010_direct_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->native_p010_direct_pipeline_layout, NULL);
    if (comp->native_p010_direct_ds_pool)
        vkDestroyDescriptorPool(comp->device, comp->native_p010_direct_ds_pool, NULL);
    if (comp->native_p010_direct_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->native_p010_direct_ds_layout, NULL);
    if (comp->native_p010_direct_shader)
        vkDestroyShaderModule(comp->device, comp->native_p010_direct_shader, NULL);
    if (comp->native_p010_to_nv21_shader)
        vkDestroyShaderModule(comp->device, comp->native_p010_to_nv21_shader, NULL);
    if (comp->native_yuv8_to_p010_shader)
        vkDestroyShaderModule(comp->device, comp->native_yuv8_to_p010_shader, NULL);
    if (comp->native_amly_to_p010_shader)
        vkDestroyShaderModule(comp->device, comp->native_amly_to_p010_shader, NULL);
    if (comp->native_amly_to_p010_src_shader)
        vkDestroyShaderModule(comp->device, comp->native_amly_to_p010_src_shader, NULL);
    if (comp->native_yuv8_to_nv21_shader)
        vkDestroyShaderModule(comp->device, comp->native_yuv8_to_nv21_shader, NULL);
    if (comp->native_amly_to_nv21_shader)
        vkDestroyShaderModule(comp->device, comp->native_amly_to_nv21_shader, NULL);
    if (comp->native_amly_to_nv21_src_shader)
        vkDestroyShaderModule(comp->device, comp->native_amly_to_nv21_src_shader, NULL);

    if (comp->native_downscale_sdr_pipeline)
        vkDestroyPipeline(comp->device, comp->native_downscale_sdr_pipeline, NULL);
    if (comp->native_downscale_hdr_pipeline)
        vkDestroyPipeline(comp->device, comp->native_downscale_hdr_pipeline, NULL);
    if (comp->native_downscale_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->native_downscale_pipeline_layout, NULL);
    if (comp->native_downscale_ds_pool)
        vkDestroyDescriptorPool(comp->device, comp->native_downscale_ds_pool, NULL);
    if (comp->native_downscale_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->native_downscale_ds_layout, NULL);
    if (comp->native_downscale_sdr_shader)
        vkDestroyShaderModule(comp->device, comp->native_downscale_sdr_shader, NULL);
    if (comp->native_downscale_hdr_shader)
        vkDestroyShaderModule(comp->device, comp->native_downscale_hdr_shader, NULL);

    if (comp->upload_fence)
        vkDestroyFence(comp->device, comp->upload_fence, NULL);
    if (comp->export_fence)
        vkDestroyFence(comp->device, comp->export_fence, NULL);

    /* Compute pipeline cleanup */
    if (comp->compute_pipeline)
        vkDestroyPipeline(comp->device, comp->compute_pipeline, NULL);
    if (comp->compute_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->compute_pipeline_layout, NULL);
    if (comp->compute_descriptor_pool)
        vkDestroyDescriptorPool(comp->device, comp->compute_descriptor_pool, NULL);
    if (comp->compute_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->compute_ds_layout, NULL);
    if (comp->compute_shader)
        vkDestroyShaderModule(comp->device, comp->compute_shader, NULL);

    /* Export compute pipeline cleanup */
    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        if (comp->nv21_mapped[i])
            vkUnmapMemory(comp->device, comp->nv21_memory[i]);
        if (comp->nv21_buffer[i])
            vkDestroyBuffer(comp->device, comp->nv21_buffer[i], NULL);
        if (comp->nv21_memory[i])
            vkFreeMemory(comp->device, comp->nv21_memory[i], NULL);

        if (comp->p010_mapped[i])
            vkUnmapMemory(comp->device, comp->p010_memory[i]);
        if (comp->p010_buffer[i])
            vkDestroyBuffer(comp->device, comp->p010_buffer[i], NULL);
        if (comp->p010_memory[i])
            vkFreeMemory(comp->device, comp->p010_memory[i], NULL);
    }
    if (comp->p010_conv_ds_pool)
        vkDestroyDescriptorPool(comp->device, comp->p010_conv_ds_pool, NULL);
    if (comp->p010_conv_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->p010_conv_ds_layout, NULL);
    if (comp->compute_fence)
        vkDestroyFence(comp->device, comp->compute_fence, NULL);
    if (comp->export_compute_pipeline)
        vkDestroyPipeline(comp->device, comp->export_compute_pipeline, NULL);
    if (comp->export_compute_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->export_compute_pipeline_layout, NULL);
    if (comp->export_compute_descriptor_pool)
        vkDestroyDescriptorPool(comp->device, comp->export_compute_descriptor_pool, NULL);
    if (comp->export_compute_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->export_compute_ds_layout, NULL);
    if (comp->export_compute_shader)
        vkDestroyShaderModule(comp->device, comp->export_compute_shader, NULL);

    /* Destination export pipeline cleanup */
    for (int i = 0; i < SBS_EXPORT_DEST_COUNT; i++) {
        sbs_export_dest_destroy(&comp->export_dests[i], comp->device);
    }
    if (comp->dest_export_pipeline)
        vkDestroyPipeline(comp->device, comp->dest_export_pipeline, NULL);
    if (comp->dest_export_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->dest_export_pipeline_layout, NULL);
    if (comp->dest_export_ds_layout)
        vkDestroyDescriptorSetLayout(comp->device, comp->dest_export_ds_layout, NULL);
    if (comp->dest_export_shader)
        vkDestroyShaderModule(comp->device, comp->dest_export_shader, NULL);
    if (comp->p010_dest_export_pipeline)
        vkDestroyPipeline(comp->device, comp->p010_dest_export_pipeline, NULL);
    if (comp->p010_dest_export_pipeline_layout)
        vkDestroyPipelineLayout(comp->device, comp->p010_dest_export_pipeline_layout, NULL);
    if (comp->p010_dest_export_shader)
        vkDestroyShaderModule(comp->device, comp->p010_dest_export_shader, NULL);

    for (int i = 0; i < SBS_RENDER_TARGET_COUNT; i++) {
        if (comp->targets[i].framebuffer)
            vkDestroyFramebuffer(comp->device, comp->targets[i].framebuffer, NULL);
        if (comp->targets[i].fence)
            vkDestroyFence(comp->device, comp->targets[i].fence, NULL);
        if (comp->targets[i].view)
            vkDestroyImageView(comp->device, comp->targets[i].view, NULL);
        if (comp->targets[i].image)
            vkDestroyImage(comp->device, comp->targets[i].image, NULL);
        if (comp->targets[i].memory)
            vkFreeMemory(comp->device, comp->targets[i].memory, NULL);
        if (comp->targets[i].dmabuf_fd >= 0)
            close(comp->targets[i].dmabuf_fd);
    }

    if (comp->command_pool) {
        vkDestroyCommandPool(comp->device, comp->command_pool, NULL);
    }
    if (comp->device)
        vkDestroyDevice(comp->device, NULL);
    if (comp->instance)
        vkDestroyInstance(comp->instance, NULL);

    pthread_mutex_destroy(&comp->queue_mutex);
    pthread_mutex_destroy(&comp->command_pool_mutex);
    pthread_mutex_destroy(&comp->retire_slot_mutex);
    memset(comp, 0, sizeof(*comp));
}

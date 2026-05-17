#define SBS_LOG_COMP "api-v4l2"

#include "sbs/v4l2_discovery.h"

#include "sbs/log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool video_node_name(const char *name)
{
    if (!name || strncmp(name, "video", 5) != 0 || !name[5])
        return false;
    for (const char *p = name + 5; *p; p++) {
        if (!g_ascii_isdigit(*p))
            return false;
    }
    return true;
}

static bool video_node_is_uvc(const char *name)
{
    char path[PATH_MAX];
    char link_target[PATH_MAX];
    ssize_t len;
    const char *base;

    if (!video_node_name(name))
        return false;

    g_snprintf(path, sizeof(path), "/sys/class/video4linux/%s/device/driver", name);
    len = readlink(path, link_target, sizeof(link_target) - 1);
    if (len <= 0)
        return false;
    link_target[len] = '\0';

    base = strrchr(link_target, '/');
    base = base ? base + 1 : link_target;
    return strcmp(base, "uvcvideo") == 0;
}

static char *v4l2_field_string(const uint8_t *field, size_t max_len)
{
    size_t len = 0;
    while (len < max_len && field[len] != '\0')
        len++;
    return g_strndup((const char *)field, len);
}

static void fourcc_string(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xffu);
    out[1] = (char)((fourcc >> 8) & 0xffu);
    out[2] = (char)((fourcc >> 16) & 0xffu);
    out[3] = (char)((fourcc >> 24) & 0xffu);
    out[4] = '\0';
    for (uint32_t i = 0; i < 4; i++) {
        if (!isprint((unsigned char)out[i]))
            out[i] = ' ';
    }
}

static const char *media_type_for_format(uint32_t pixfmt, bool compressed)
{
    switch (pixfmt) {
    case V4L2_PIX_FMT_MJPEG:
    case V4L2_PIX_FMT_JPEG:
        return "mjpeg";
    case V4L2_PIX_FMT_H264:
#ifdef V4L2_PIX_FMT_H264_NO_SC
    case V4L2_PIX_FMT_H264_NO_SC:
#endif
        return "h264";
#ifdef V4L2_PIX_FMT_HEVC
    case V4L2_PIX_FMT_HEVC:
        return "h265";
#endif
    default:
        return compressed ? "compressed" : "raw";
    }
}

static bool software_decode_available(const char *media_type)
{
    return g_strcmp0(media_type, "mjpeg") == 0 ||
           g_strcmp0(media_type, "h264") == 0;
}

static bool gst_element_available(const char *name)
{
    gchar *inspect_path = NULL;
    gint status = 0;
    gboolean ok;

    if (!name || !name[0])
        return false;

    inspect_path = g_find_program_in_path("gst-inspect-1.0");
    if (!inspect_path && g_file_test("/usr/bin/gst-inspect-1.0", G_FILE_TEST_IS_EXECUTABLE))
        inspect_path = g_strdup("/usr/bin/gst-inspect-1.0");
    if (!inspect_path)
        return false;

    gchar *argv[] = { inspect_path, (gchar *)name, NULL };
    ok = g_spawn_sync(NULL, (gchar **)argv, NULL,
                      G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL, &status, NULL);
    g_free(inspect_path);
    return ok && status == 0;
}

static bool hardware_decode_available(const char *media_type)
{
    static int have_aml_jpeg = -1;
    static int have_aml_h264 = -1;

    if (g_strcmp0(media_type, "mjpeg") == 0) {
        if (have_aml_jpeg < 0)
            have_aml_jpeg = gst_element_available("amlv4l2jpegdec") ? 1 : 0;
        return have_aml_jpeg == 1;
    }
    if (g_strcmp0(media_type, "h264") == 0) {
        if (have_aml_h264 < 0)
            have_aml_h264 = gst_element_available("amlv4l2h264dec") ? 1 : 0;
        return have_aml_h264 == 1;
    }
    return false;
}

static void add_frame_intervals(int fd, uint32_t pixfmt, uint32_t width,
                                uint32_t height, cJSON *resolution)
{
    cJSON *intervals = cJSON_CreateArray();

    for (uint32_t i = 0;; i++) {
        struct v4l2_frmivalenum ival;
        memset(&ival, 0, sizeof(ival));
        ival.index = i;
        ival.pixel_format = pixfmt;
        ival.width = width;
        ival.height = height;

        if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &ival) != 0)
            break;

        cJSON *entry = cJSON_CreateObject();
        if (ival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            uint32_t num = ival.discrete.numerator;
            uint32_t den = ival.discrete.denominator;
            cJSON_AddStringToObject(entry, "type", "discrete");
            cJSON_AddNumberToObject(entry, "numerator", num);
            cJSON_AddNumberToObject(entry, "denominator", den);
            if (num > 0)
                cJSON_AddNumberToObject(entry, "fps", (double)den / (double)num);
        } else {
            const char *type = ival.type == V4L2_FRMIVAL_TYPE_CONTINUOUS ?
                "continuous" : "stepwise";
            cJSON_AddStringToObject(entry, "type", type);
            cJSON_AddNumberToObject(entry, "min_numerator", ival.stepwise.min.numerator);
            cJSON_AddNumberToObject(entry, "min_denominator", ival.stepwise.min.denominator);
            cJSON_AddNumberToObject(entry, "max_numerator", ival.stepwise.max.numerator);
            cJSON_AddNumberToObject(entry, "max_denominator", ival.stepwise.max.denominator);
            cJSON_AddNumberToObject(entry, "step_numerator", ival.stepwise.step.numerator);
            cJSON_AddNumberToObject(entry, "step_denominator", ival.stepwise.step.denominator);
        }
        cJSON_AddItemToArray(intervals, entry);
    }

    cJSON_AddItemToObject(resolution, "frame_intervals", intervals);
}

static void add_frame_sizes(int fd, uint32_t pixfmt, cJSON *format)
{
    cJSON *resolutions = cJSON_CreateArray();

    for (uint32_t i = 0;; i++) {
        struct v4l2_frmsizeenum size;
        memset(&size, 0, sizeof(size));
        size.index = i;
        size.pixel_format = pixfmt;

        if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0)
            break;

        cJSON *entry = cJSON_CreateObject();
        if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            cJSON_AddStringToObject(entry, "type", "discrete");
            cJSON_AddNumberToObject(entry, "width", size.discrete.width);
            cJSON_AddNumberToObject(entry, "height", size.discrete.height);
            add_frame_intervals(fd, pixfmt, size.discrete.width, size.discrete.height, entry);
        } else {
            const char *type = size.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ?
                "continuous" : "stepwise";
            cJSON_AddStringToObject(entry, "type", type);
            cJSON_AddNumberToObject(entry, "min_width", size.stepwise.min_width);
            cJSON_AddNumberToObject(entry, "min_height", size.stepwise.min_height);
            cJSON_AddNumberToObject(entry, "max_width", size.stepwise.max_width);
            cJSON_AddNumberToObject(entry, "max_height", size.stepwise.max_height);
            cJSON_AddNumberToObject(entry, "step_width", size.stepwise.step_width);
            cJSON_AddNumberToObject(entry, "step_height", size.stepwise.step_height);
            add_frame_intervals(fd, pixfmt, size.stepwise.max_width, size.stepwise.max_height, entry);
        }
        cJSON_AddItemToArray(resolutions, entry);

        if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
            break;
    }

    cJSON_AddItemToObject(format, "resolutions", resolutions);
}

static void add_formats(int fd, uint32_t caps, cJSON *device)
{
    enum v4l2_buf_type type = (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) ?
        V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cJSON *formats = cJSON_CreateArray();

    for (uint32_t i = 0;; i++) {
        struct v4l2_fmtdesc fmt;
        char fourcc[5];
        bool compressed;
        const char *media_type;

        memset(&fmt, 0, sizeof(fmt));
        fmt.index = i;
        fmt.type = type;

        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0)
            break;

        fourcc_string(fmt.pixelformat, fourcc);
        compressed = (fmt.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;
        media_type = media_type_for_format(fmt.pixelformat, compressed);
        compressed = compressed || g_strcmp0(media_type, "raw") != 0;

        cJSON *entry = cJSON_CreateObject();
        char *description = v4l2_field_string(fmt.description, sizeof(fmt.description));
        cJSON_AddStringToObject(entry, "fourcc", fourcc);
        cJSON_AddStringToObject(entry, "description", description ? description : "");
        cJSON_AddStringToObject(entry, "media_type", media_type);
        cJSON_AddBoolToObject(entry, "compressed", compressed);
        cJSON_AddBoolToObject(entry, "zero_copy_expected", !compressed);
        cJSON_AddBoolToObject(entry, "software_decode_available",
                              software_decode_available(media_type));
        cJSON_AddBoolToObject(entry, "hardware_decode_available",
                              hardware_decode_available(media_type));
        add_frame_sizes(fd, fmt.pixelformat, entry);
        cJSON_AddItemToArray(formats, entry);
        g_free(description);
    }

    cJSON_AddItemToObject(device, "formats", formats);
}

static cJSON *create_device_json(const char *path)
{
    int fd;
    struct v4l2_capability cap;
    uint32_t caps;
    char *card;
    char *driver;
    char *bus_info;
    char *id_seed;
    char *hash;
    char *id;
    cJSON *device;
    cJSON *hints;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return NULL;

    memset(&cap, 0, sizeof(cap));
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        close(fd);
        return NULL;
    }

    caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if ((caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE)) == 0) {
        close(fd);
        return NULL;
    }

    card = v4l2_field_string(cap.card, sizeof(cap.card));
    driver = v4l2_field_string(cap.driver, sizeof(cap.driver));
    bus_info = v4l2_field_string(cap.bus_info, sizeof(cap.bus_info));
    bool uvc_device = (driver && g_strrstr(driver, "uvc")) ||
        (bus_info && g_strrstr(bus_info, "usb"));
    if (!uvc_device) {
        g_free(card);
        g_free(driver);
        g_free(bus_info);
        close(fd);
        return NULL;
    }

    id_seed = g_strdup_printf("%s|%s|%s",
                              bus_info && bus_info[0] ? bus_info : path,
                              driver ? driver : "",
                              card ? card : "");
    hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, id_seed, -1);
    id = g_strdup_printf("v4l2-%.*s", 12, hash ? hash : "unknown");

    device = cJSON_CreateObject();
    cJSON_AddStringToObject(device, "id", id);
    cJSON_AddStringToObject(device, "display_name", card && card[0] ? card : path);
    cJSON_AddStringToObject(device, "name", card && card[0] ? card : path);
    cJSON_AddStringToObject(device, "path", path);
    cJSON_AddStringToObject(device, "driver", driver ? driver : "");
    cJSON_AddStringToObject(device, "bus_info", bus_info ? bus_info : "");
    cJSON_AddBoolToObject(device, "streaming", (caps & V4L2_CAP_STREAMING) != 0);
    cJSON_AddBoolToObject(device, "readwrite", (caps & V4L2_CAP_READWRITE) != 0);

    hints = cJSON_CreateArray();
    if (uvc_device) {
        cJSON_AddItemToArray(hints, cJSON_CreateString("uvc"));
    }
    if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
        cJSON_AddItemToArray(hints, cJSON_CreateString("multi-plane"));
    cJSON_AddItemToObject(device, "type_hints", hints);

    add_formats(fd, caps, device);

    g_free(card);
    g_free(driver);
    g_free(bus_info);
    g_free(id_seed);
    g_free(hash);
    g_free(id);
    close(fd);
    return device;
}

cJSON *sbs_v4l2_discovery_list_devices(void)
{
    cJSON *devices = cJSON_CreateArray();
    GDir *dir = g_dir_open("/dev", 0, NULL);
    const char *name;

    if (!dir) {
        LOG_W("unable to open /dev for V4L2 discovery: %s", g_strerror(errno));
        return devices;
    }

    while ((name = g_dir_read_name(dir)) != NULL) {
        if (!video_node_name(name))
            continue;
        if (!video_node_is_uvc(name))
            continue;

        char *path = g_build_filename("/dev", name, NULL);
        cJSON *device = create_device_json(path);
        if (device)
            cJSON_AddItemToArray(devices, device);
        g_free(path);
    }

    g_dir_close(dir);
    return devices;
}

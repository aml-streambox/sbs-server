#define SBS_LOG_COMP "audio"

#include "sbs/audio_mixer.h"
#include "sbs/log.h"
#include "cjson/cJSON.h"

#include <errno.h>
#include <string.h>

#ifdef SBS_HAVE_GSTREAMER_AUDIO
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#endif

static bool path_exists(const char *path)
{
    return path && g_file_test(path, G_FILE_TEST_EXISTS);
}

static char *read_text_file_stripped(const char *path)
{
    char *contents = NULL;
    gsize len = 0;
    if (!path || !g_file_get_contents(path, &contents, &len, NULL)) {
        return NULL;
    }
    g_strstrip(contents);
    return contents;
}

static void add_device_node_status(cJSON *parent, const char *name, const char *path)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "path", path);
    cJSON_AddBoolToObject(obj, "exists", path_exists(path));
    cJSON_AddItemToObject(parent, name, obj);
}

static void add_asound_pcm_lines(cJSON *parent)
{
    cJSON *devices = cJSON_CreateArray();
    char *contents = read_text_file_stripped("/proc/asound/pcm");
    if (contents && contents[0]) {
        char **lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines && lines[i]; i++) {
            char *line = g_strstrip(lines[i]);
            if (line[0]) {
                cJSON_AddItemToArray(devices, cJSON_CreateString(line));
            }
        }
        g_strfreev(lines);
    }
    cJSON_AddItemToObject(parent, "alsa_pcm", devices);
    g_free(contents);
}

static cJSON *serialize_hifi_probe(void)
{
    cJSON *hifi = cJSON_CreateObject();
    cJSON *nodes = cJSON_CreateObject();
    char *clock = read_text_file_stripped("/sys/kernel/debug/dspboota/clk_rate");
    char *health = read_text_file_stripped("/sys/kernel/debug/dspboota/health_cnt");
    bool hifi_dev = path_exists("/dev/hifi4dsp0");
    bool dsp_dev = path_exists("/dev/dsp_dev");
    bool mbox = path_exists("/sys/class/mbox_devfs/dsp_dev");

    cJSON_AddBoolToObject(hifi, "available", hifi_dev && dsp_dev);
    cJSON_AddBoolToObject(hifi, "hifi_device", hifi_dev);
    cJSON_AddBoolToObject(hifi, "mailbox_class", mbox);
    cJSON_AddStringToObject(hifi, "integration", "alsa-hifi-bridge");
    if (clock && clock[0]) {
        cJSON_AddNumberToObject(hifi, "dspa_clock_khz", g_ascii_strtoll(clock, NULL, 10));
    }
    if (health && health[0]) {
        cJSON_AddStringToObject(hifi, "dspa_health", health);
    }

    add_device_node_status(nodes, "hifi4dsp0", "/dev/hifi4dsp0");
    add_device_node_status(nodes, "audiodsp0", "/dev/audiodsp0");
    add_device_node_status(nodes, "dsp_dev", "/dev/dsp_dev");
    add_device_node_status(nodes, "dspa2ree", "/dev/dspa2ree");
    add_device_node_status(nodes, "dspb_dev", "/dev/dspb_dev");
    add_device_node_status(nodes, "dspb2ree", "/dev/dspb2ree");
    add_device_node_status(nodes, "ree2aocpu", "/dev/ree2aocpu");
    cJSON_AddItemToObject(hifi, "device_nodes", nodes);
    add_asound_pcm_lines(hifi);

    g_free(clock);
    g_free(health);
    return hifi;
}

static const char *active_audio_backend_name(void)
{
#ifdef SBS_HAVE_GSTREAMER_AUDIO
    return "gstreamer";
#else
    return "disabled";
#endif
}

#ifndef SBS_HAVE_GSTREAMER_AUDIO

struct sbs_audio_mixer {
    sbs_scene_graph_t *graph;
    double master_volume_value;
    double master_left_gain;
    double master_right_gain;
    double master_eq_bands[10];
    bool master_mute;
};

sbs_audio_mixer_t *sbs_audio_mixer_new(sbs_scene_graph_t *graph)
{
    sbs_audio_mixer_t *audio = g_new0(sbs_audio_mixer_t, 1);
    audio->graph = graph;
    audio->master_volume_value = 1.0;
    audio->master_left_gain = 1.0;
    audio->master_right_gain = 1.0;
    return audio;
}

void sbs_audio_mixer_free(sbs_audio_mixer_t *audio) { g_free(audio); }
int sbs_audio_mixer_start(sbs_audio_mixer_t *audio) { (void)audio; return SBS_ERR_NOT_FOUND; }
void sbs_audio_mixer_stop(sbs_audio_mixer_t *audio) { (void)audio; }
void sbs_audio_mixer_on_scene_change(sbs_audio_mixer_t *audio, const char *active_scene_id) { (void)audio; (void)active_scene_id; }
int sbs_audio_mixer_set_source_state(sbs_audio_mixer_t *audio, sbs_source_state_t *source) { (void)audio; (void)source; return SBS_OK; }
int sbs_audio_mixer_set_scene_item_state(sbs_audio_mixer_t *audio, sbs_scene_item_state_t *item) { (void)audio; (void)item; return SBS_OK; }
int sbs_audio_mixer_set_master(sbs_audio_mixer_t *audio, double volume, bool mute, double left_gain, double right_gain, const double *eq_bands, uint32_t eq_band_count) { if (audio) { audio->master_volume_value = volume; audio->master_mute = mute; audio->master_left_gain = left_gain; audio->master_right_gain = right_gain; for (uint32_t i = 0; eq_bands && i < G_N_ELEMENTS(audio->master_eq_bands) && i < eq_band_count; i++) audio->master_eq_bands[i] = eq_bands[i]; } return SBS_OK; }
cJSON *sbs_audio_mixer_serialize_levels(sbs_audio_mixer_t *audio) { (void)audio; cJSON *obj=cJSON_CreateObject(); cJSON_AddItemToObject(obj,"sources",cJSON_CreateObject()); cJSON_AddItemToObject(obj,"master",cJSON_CreateObject()); return obj; }
cJSON *sbs_audio_mixer_serialize_state(sbs_audio_mixer_t *audio) { cJSON *obj=cJSON_CreateObject(); cJSON *eq=cJSON_CreateArray(); cJSON_AddStringToObject(obj,"device","hw:0,2"); cJSON_AddStringToObject(obj,"backend", active_audio_backend_name()); cJSON_AddStringToObject(obj,"preferred_backend", "hifi"); cJSON_AddNumberToObject(obj,"master_volume", audio ? audio->master_volume_value : 1.0); cJSON_AddNumberToObject(obj,"master_left_gain", audio ? audio->master_left_gain : 1.0); cJSON_AddNumberToObject(obj,"master_right_gain", audio ? audio->master_right_gain : 1.0); for (uint32_t i = 0; i < 10; i++) cJSON_AddItemToArray(eq, cJSON_CreateNumber(audio ? audio->master_eq_bands[i] : 0.0)); cJSON_AddItemToObject(obj,"master_eq_bands", eq); cJSON_AddBoolToObject(obj,"master_mute", audio ? audio->master_mute : false); cJSON_AddItemToObject(obj,"hifi", serialize_hifi_probe()); cJSON_AddItemToObject(obj,"levels", sbs_audio_mixer_serialize_levels(audio)); return obj; }
uint32_t sbs_audio_mixer_pending_depth(sbs_audio_mixer_t *audio) { (void)audio; return 0; }
sbs_audio_buffer_t *sbs_audio_mixer_take_latest_buffer(sbs_audio_mixer_t *audio) { (void)audio; return NULL; }

#else

#define SBS_AUDIO_PENDING_MAX 256u
#define SBS_AUDIO_SILENCE_SAMPLE_RATE 48000u
#define SBS_AUDIO_SILENCE_CHANNELS 2u
#define SBS_AUDIO_SILENCE_SAMPLES 480u
#define SBS_AUDIO_SILENCE_INTERVAL_US 10000u
#define SBS_AUDIO_SILENCE_DURATION_NS 10000000ull
#define SBS_AUDIO_SILENCE_DATA_SIZE \
    (SBS_AUDIO_SILENCE_SAMPLES * SBS_AUDIO_SILENCE_CHANNELS * sizeof(int16_t))

static void ensure_gstreamer_audio_ready(void)
{
    static gsize initialized = 0;
    if (g_once_init_enter(&initialized)) {
        gst_init(NULL, NULL);
        g_once_init_leave(&initialized, 1);
    }
}

typedef struct sbs_audio_branch {
    char *source_id;
    bool monitor;
    bool active_in_scene;
    bool effective_mute;
    GstElement *source;
    GstElement *queue;
    GstElement *convert;
    GstElement *resample;
    GstElement *capsfilter;
    GstElement *delay;
    GstElement *panorama;
    GstElement *equalizer;
    GstElement *volume;
    GstElement *level;
    GstPad *mixer_pad;
    sbs_audio_level_info_t meter;
    bool muted;
} sbs_audio_branch_t;

static double clamp_double(double value, double min_value, double max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return value;
}

static double audio_binding_volume(const sbs_audio_binding_t *binding)
{
    return binding && binding->volume >= 0.0 ? binding->volume : 1.0;
}

static void configure_branch_audio_controls(sbs_audio_branch_t *branch,
                                            const sbs_audio_binding_t *binding)
{
    double left = binding ? binding->left_gain : 1.0;
    double right = binding ? binding->right_gain : 1.0;
    int32_t delay_ms = binding ? binding->delay_ms : 0;
    double pan = 0.0;

    left = clamp_double(left >= 0.0 ? left : 1.0, 0.0, 2.0);
    right = clamp_double(right >= 0.0 ? right : 1.0, 0.0, 2.0);
    delay_ms = CLAMP(delay_ms, 0, 5000);
    if (left + right > 0.0) {
        pan = clamp_double((right - left) / (left + right), -1.0, 1.0);
    }

    if (branch->delay) {
        g_object_set(branch->delay, "ts-offset", (gint64)delay_ms * GST_MSECOND, NULL);
    }
    if (branch->panorama) {
        g_object_set(branch->panorama, "panorama", pan, NULL);
    }
    if (branch->equalizer && binding) {
        for (uint32_t i = 0; i < G_N_ELEMENTS(binding->eq_bands); i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "band%u", i);
            g_object_set(branch->equalizer, prop,
                         clamp_double(binding->eq_bands[i], -24.0, 12.0), NULL);
        }
    }
}

static bool link_branch_element(GstElement **prev, GstElement *next)
{
    if (!next) return true;
    if (!gst_element_link(*prev, next)) return false;
    *prev = next;
    return true;
}

struct sbs_audio_mixer {
    sbs_scene_graph_t *graph;
    GstElement *pipeline;
    GstElement *mixer;
    GstElement *master_panorama;
    GstElement *master_equalizer;
    GstElement *master_volume;
    GstElement *master_level;
    GstElement *appsink;
    GHashTable *branches;
    sbs_audio_level_info_t master_meter;
    char *active_scene_id;
    GMutex lock;
    GQueue *pending_buffers;
    uint64_t pending_samples;
    uint64_t queued_buffers;
    uint64_t popped_buffers;
    uint64_t dropped_buffers;
    uint64_t dropped_samples;
    uint64_t silence_buffers_generated;
    gint64 silence_next_emit_us;
    gint64 last_queue_log_us;
    double master_volume_value;
    double master_left_gain;
    double master_right_gain;
    double master_eq_bands[10];
    bool master_mute;
};

static void configure_master_audio_controls(sbs_audio_mixer_t *audio)
{
    double left = audio ? audio->master_left_gain : 1.0;
    double right = audio ? audio->master_right_gain : 1.0;
    double pan = 0.0;

    if (!audio) return;
    left = clamp_double(left >= 0.0 ? left : 1.0, 0.0, 2.0);
    right = clamp_double(right >= 0.0 ? right : 1.0, 0.0, 2.0);
    if (left + right > 0.0) {
        pan = clamp_double((right - left) / (left + right), -1.0, 1.0);
    }

    if (audio->master_panorama) {
        g_object_set(audio->master_panorama, "panorama", pan, NULL);
    }
    if (audio->master_equalizer) {
        for (uint32_t i = 0; i < G_N_ELEMENTS(audio->master_eq_bands); i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "band%u", i);
            g_object_set(audio->master_equalizer, prop,
                         clamp_double(audio->master_eq_bands[i], -24.0, 12.0), NULL);
        }
    }
}

static void branch_free(gpointer data)
{
    sbs_audio_branch_t *branch = data;
    if (!branch) return;
    g_free(branch->source_id);
    g_free(branch);
}

static sbs_audio_branch_t *find_branch_by_level_name(sbs_audio_mixer_t *audio, const char *name)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, audio->branches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_audio_branch_t *branch = value;
        if (branch->level && g_strcmp0(GST_OBJECT_NAME(branch->level), name) == 0) {
            return branch;
        }
    }
    return NULL;
}

static double level_value_first_double(const GValue *value)
{
    GValueArray *array;
    if (!value) {
        return -120.0;
    }
    if (G_VALUE_HOLDS_DOUBLE(value)) {
        return g_value_get_double(value);
    }
    if (G_VALUE_HOLDS_FLOAT(value)) {
        return g_value_get_float(value);
    }
    if (g_strcmp0(G_VALUE_TYPE_NAME(value), "GValueArray") == 0) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        array = g_value_get_boxed(value);
        if (array && array->n_values > 0) {
            return level_value_first_double(g_value_array_get_nth(array, 0));
        }
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
    if (GST_VALUE_HOLDS_LIST(value)) {
        guint n = gst_value_list_get_size(value);
        if (n > 0) {
            return level_value_first_double(gst_value_list_get_value(value, 0));
        }
    }
    if (GST_VALUE_HOLDS_ARRAY(value)) {
        guint n = gst_value_array_get_size(value);
        if (n > 0) {
            return level_value_first_double(gst_value_array_get_value(value, 0));
        }
    }
    return -120.0;
}

static gboolean on_bus_message(GstBus *bus, GstMessage *msg, gpointer user_data)
{
    sbs_audio_mixer_t *audio = user_data;
    (void)bus;

    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_error(msg, &err, &debug);
        LOG_E("audio pipeline error from %s: %s%s%s",
              GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
              err ? err->message : "unknown error",
              debug ? " debug=" : "",
              debug ? debug : "");
        g_clear_error(&err);
        g_free(debug);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
        GError *err = NULL;
        gchar *debug = NULL;
        gst_message_parse_warning(msg, &err, &debug);
        LOG_W("audio pipeline warning from %s: %s%s%s",
              GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)),
              err ? err->message : "unknown warning",
              debug ? " debug=" : "",
              debug ? debug : "");
        g_clear_error(&err);
        g_free(debug);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ELEMENT) {
        const GstStructure *s = gst_message_get_structure(msg);
        if (s && gst_structure_has_name(s, "level")) {
            const gchar *name = GST_OBJECT_NAME(GST_MESSAGE_SRC(msg));
            const GValue *rms_arr = gst_structure_get_value(s, "rms");
            const GValue *peak_arr = gst_structure_get_value(s, "peak");
            double rms_db = level_value_first_double(rms_arr);
            double peak_db = level_value_first_double(peak_arr);
            int64_t now = g_get_monotonic_time();

            if (g_strcmp0(name, "master_level") == 0) {
                audio->master_meter.level_db = rms_db;
                audio->master_meter.peak_db = peak_db;
                audio->master_meter.timestamp_us = now;
            } else {
                sbs_audio_branch_t *branch = find_branch_by_level_name(audio, name);
                if (branch) {
                    branch->meter.level_db = rms_db;
                    branch->meter.peak_db = peak_db;
                    branch->meter.timestamp_us = now;
                }
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static GstFlowReturn on_audio_sample(GstAppSink *sink, gpointer user_data)
{
    sbs_audio_mixer_t *audio = user_data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    GstBuffer *buffer;
    GstMapInfo map;
    sbs_audio_buffer_t *abuf, *dropped = NULL;

    if (!sample) return GST_FLOW_OK;
    buffer = gst_sample_get_buffer(sample);
    if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (sample) gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    abuf = g_malloc(sizeof(*abuf) + map.size);
    abuf->msg.header.msg_type = SBS_IPC_MSG_AUDIO_BUFFER;
    abuf->msg.header.payload_size = sizeof(abuf->msg) - sizeof(abuf->msg.header) + map.size;
    abuf->msg.pts_ns = GST_BUFFER_PTS(buffer);
    abuf->msg.duration_ns = GST_BUFFER_DURATION(buffer);
    abuf->msg.sample_rate = 48000;
    abuf->msg.channels = 2;
    abuf->msg.format = SBS_AUDIO_FORMAT_S16LE;
    abuf->msg.n_samples = map.size / (2 * sizeof(int16_t));
    abuf->msg.data_size = map.size;
    memcpy(abuf->data, map.data, map.size);

    g_mutex_lock(&audio->lock);
    if (audio->pending_buffers) {
        if (g_queue_get_length(audio->pending_buffers) >= SBS_AUDIO_PENDING_MAX) {
            dropped = g_queue_pop_head(audio->pending_buffers);
            if (dropped) {
                audio->pending_samples -= MIN(audio->pending_samples,
                                              (uint64_t)dropped->msg.n_samples);
                audio->dropped_buffers++;
                audio->dropped_samples += dropped->msg.n_samples;
            }
        }
        g_queue_push_tail(audio->pending_buffers, abuf);
        audio->pending_samples += abuf->msg.n_samples;
        audio->queued_buffers++;
        if (audio->dropped_buffers > 0) {
            gint64 now_us = g_get_monotonic_time();
            if (audio->last_queue_log_us == 0 ||
                now_us - audio->last_queue_log_us >= 1000000) {
                uint32_t depth = g_queue_get_length(audio->pending_buffers);
                uint32_t pending_ms = audio->pending_samples > 0
                    ? (uint32_t)((audio->pending_samples * 1000u) / 48000u)
                    : 0;
                LOG_W("audio FIFO overrun: dropped=%lu queued=%lu popped=%lu depth=%u pending=%ums",
                      (unsigned long)audio->dropped_buffers,
                      (unsigned long)audio->queued_buffers,
                      (unsigned long)audio->popped_buffers,
                      depth,
                      pending_ms);
                audio->last_queue_log_us = now_us;
            }
        }
        abuf = NULL;
    }
    g_mutex_unlock(&audio->lock);
    g_free(dropped);
    g_free(abuf);

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static sbs_audio_buffer_t *make_silent_master_buffer(void)
{
    sbs_audio_buffer_t *abuf = g_malloc0(sizeof(*abuf) + SBS_AUDIO_SILENCE_DATA_SIZE);
    abuf->msg.header.msg_type = SBS_IPC_MSG_AUDIO_BUFFER;
    abuf->msg.header.payload_size = sizeof(abuf->msg) - sizeof(abuf->msg.header) +
                                     SBS_AUDIO_SILENCE_DATA_SIZE;
    abuf->msg.pts_ns = UINT64_MAX;
    abuf->msg.duration_ns = SBS_AUDIO_SILENCE_DURATION_NS;
    abuf->msg.sample_rate = SBS_AUDIO_SILENCE_SAMPLE_RATE;
    abuf->msg.channels = SBS_AUDIO_SILENCE_CHANNELS;
    abuf->msg.format = SBS_AUDIO_FORMAT_S16LE;
    abuf->msg.n_samples = SBS_AUDIO_SILENCE_SAMPLES;
    abuf->msg.data_size = SBS_AUDIO_SILENCE_DATA_SIZE;
    return abuf;
}

static bool should_emit_silent_master_buffer(sbs_audio_mixer_t *audio, gint64 now_us)
{
    if (!audio)
        return false;

    if (audio->branches && g_hash_table_size(audio->branches) > 0) {
        audio->silence_next_emit_us = 0;
        return false;
    }

    if (audio->silence_next_emit_us == 0 ||
        now_us > audio->silence_next_emit_us + 10 * SBS_AUDIO_SILENCE_INTERVAL_US) {
        audio->silence_next_emit_us = now_us;
    }

    if (now_us < audio->silence_next_emit_us)
        return false;

    audio->silence_next_emit_us += SBS_AUDIO_SILENCE_INTERVAL_US;
    audio->silence_buffers_generated++;
    audio->master_meter.level_db = -120.0;
    audio->master_meter.peak_db = -120.0;
    audio->master_meter.timestamp_us = now_us;
    return true;
}

static sbs_audio_branch_t *add_branch_for_binding(sbs_audio_mixer_t *audio,
                                                  sbs_source_state_t *source,
                                                  const sbs_audio_binding_t *binding)
{
    sbs_audio_branch_t *branch;
    GstPad *src_pad;
    GstCaps *caps;
    char *level_name;
    GstElement *last;

    branch = g_new0(sbs_audio_branch_t, 1);
    branch->source_id = g_strdup(source->id);
    branch->monitor = binding ? binding->monitor : false;
    branch->active_in_scene = true;
    branch->muted = binding ? binding->mute : false;
    branch->effective_mute = branch->muted || !(binding && binding->enabled);
    branch->meter.level_db = -120.0;
    branch->meter.peak_db = -120.0;
    branch->source = gst_element_factory_make("alsasrc", NULL);
    branch->queue = gst_element_factory_make("queue", NULL);
    branch->convert = gst_element_factory_make("audioconvert", NULL);
    branch->resample = gst_element_factory_make("audioresample", NULL);
    branch->capsfilter = gst_element_factory_make("capsfilter", NULL);
    branch->delay = gst_element_factory_make("identity", NULL);
    branch->panorama = gst_element_factory_make("audiopanorama", NULL);
    branch->equalizer = gst_element_factory_make("equalizer-10bands", NULL);
    branch->volume = gst_element_factory_make("volume", NULL);
    branch->level = gst_element_factory_make("level", NULL);
    if (!branch->source || !branch->queue || !branch->convert || !branch->resample ||
        !branch->capsfilter || !branch->delay || !branch->volume || !branch->level) {
        LOG_W("failed to create audio branch elements for source %s", source->id);
        branch_free(branch);
        return NULL;
    }

    g_object_set(branch->source,
        "device", binding && binding->device ? binding->device : "hw:0,2",
        "buffer-time", (gint64)200000,
        "latency-time", (gint64)20000,
        "do-timestamp", TRUE,
        "provide-clock", TRUE,
        "slave-method", 0,
        NULL);
    g_object_set(branch->queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", (guint64)(2 * GST_SECOND),
        "leaky", 2,
        NULL);
    g_object_set(branch->volume,
        "volume", (gdouble)audio_binding_volume(binding),
        "mute", branch->effective_mute,
        NULL);
    g_object_set(branch->level,
        "interval", (guint64)(100 * GST_MSECOND),
        "peak-ttl", (guint64)(1000 * GST_MSECOND),
        "post-messages", TRUE,
        NULL);
    configure_branch_audio_controls(branch, binding);
    level_name = g_strdup_printf("level_%s", source->id);
    gst_object_set_name(GST_OBJECT(branch->level), level_name);
    g_free(level_name);

    caps = gst_caps_new_simple("audio/x-raw",
        "rate", G_TYPE_INT, 48000,
        "channels", G_TYPE_INT, 2,
        "format", G_TYPE_STRING, "S16LE",
        NULL);
    g_object_set(branch->capsfilter, "caps", caps, NULL);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(audio->pipeline), branch->source, branch->queue, branch->convert,
                     branch->resample, branch->capsfilter, branch->delay, branch->volume,
                     branch->level, NULL);
    if (branch->panorama) {
        gst_bin_add(GST_BIN(audio->pipeline), branch->panorama);
    } else {
        LOG_W("audiopanorama unavailable; left/right gain control disabled for source %s", source->id);
    }
    if (branch->equalizer) {
        gst_bin_add(GST_BIN(audio->pipeline), branch->equalizer);
    } else {
        LOG_W("equalizer-10bands unavailable; EQ control disabled for source %s", source->id);
    }

    last = branch->source;
    if (!link_branch_element(&last, branch->queue) ||
        !link_branch_element(&last, branch->convert) ||
        !link_branch_element(&last, branch->resample) ||
        !link_branch_element(&last, branch->capsfilter) ||
        !link_branch_element(&last, branch->delay) ||
        !link_branch_element(&last, branch->panorama) ||
        !link_branch_element(&last, branch->equalizer) ||
        !link_branch_element(&last, branch->volume) ||
        !link_branch_element(&last, branch->level)) {
        LOG_W("failed to link audio branch for source %s", source->id);
        branch_free(branch);
        return NULL;
    }

    branch->mixer_pad = gst_element_request_pad_simple(audio->mixer, "sink_%u");
    src_pad = gst_element_get_static_pad(branch->level, "src");
    if (gst_pad_link(src_pad, branch->mixer_pad) != GST_PAD_LINK_OK) {
        LOG_W("failed to link source %s level output into audiomixer", source->id);
        gst_object_unref(src_pad);
        branch_free(branch);
        return NULL;
    }
    gst_object_unref(src_pad);

    gst_element_sync_state_with_parent(branch->source);
    gst_element_sync_state_with_parent(branch->queue);
    gst_element_sync_state_with_parent(branch->convert);
    gst_element_sync_state_with_parent(branch->resample);
    gst_element_sync_state_with_parent(branch->capsfilter);
    gst_element_sync_state_with_parent(branch->delay);
    if (branch->panorama) gst_element_sync_state_with_parent(branch->panorama);
    if (branch->equalizer) gst_element_sync_state_with_parent(branch->equalizer);
    gst_element_sync_state_with_parent(branch->volume);
    gst_element_sync_state_with_parent(branch->level);

    g_hash_table_insert(audio->branches, branch->source_id, branch);
    LOG_I("audio branch added for source %s (%s)", source->id,
          binding && binding->device ? binding->device : "hw:0,2");
    return branch;
}

static sbs_audio_branch_t *add_branch(sbs_audio_mixer_t *audio, sbs_source_state_t *source)
{
    return add_branch_for_binding(audio, source, source ? &source->audio : NULL);
}

sbs_audio_mixer_t *sbs_audio_mixer_new(sbs_scene_graph_t *graph)
{
    sbs_audio_mixer_t *audio = g_new0(sbs_audio_mixer_t, 1);
    audio->graph = graph;
    audio->branches = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, branch_free);
    audio->pending_buffers = g_queue_new();
    g_mutex_init(&audio->lock);
    audio->master_volume_value = 1.0;
    audio->master_left_gain = 1.0;
    audio->master_right_gain = 1.0;
    audio->master_meter.level_db = -120.0;
    audio->master_meter.peak_db = -120.0;
    return audio;
}

void sbs_audio_mixer_free(sbs_audio_mixer_t *audio)
{
    if (!audio) return;
    sbs_audio_mixer_stop(audio);
    g_hash_table_destroy(audio->branches);
    if (audio->pending_buffers) {
        sbs_audio_buffer_t *buf;
        while ((buf = g_queue_pop_head(audio->pending_buffers)) != NULL) {
            g_free(buf);
        }
        g_queue_free(audio->pending_buffers);
    }
    g_mutex_clear(&audio->lock);
    g_free(audio->active_scene_id);
    g_free(audio);
}

int sbs_audio_mixer_start(sbs_audio_mixer_t *audio)
{
    GHashTableIter iter;
    gpointer key, value;
    GstBus *bus;
    char *dspa_clock;
    if (!audio) return SBS_ERR_INVAL;

    ensure_gstreamer_audio_ready();

    dspa_clock = read_text_file_stripped("/sys/kernel/debug/dspboota/clk_rate");
    LOG_I("audio backend=%s preferred=hifi hifi4dsp0=%s dsp_dev=%s dspa_clock_khz=%s",
          active_audio_backend_name(),
          path_exists("/dev/hifi4dsp0") ? "present" : "missing",
          path_exists("/dev/dsp_dev") ? "present" : "missing",
          dspa_clock && dspa_clock[0] ? dspa_clock : "unknown");
    g_free(dspa_clock);

    audio->pipeline = gst_pipeline_new("audio_pipeline");
    audio->mixer = gst_element_factory_make("audiomixer", "audio_mixer");
    GstElement *post_queue = gst_element_factory_make("queue", "audio_post_queue");
    audio->master_panorama = gst_element_factory_make("audiopanorama", "master_panorama");
    audio->master_equalizer = gst_element_factory_make("equalizer-10bands", "master_equalizer");
    audio->master_volume = gst_element_factory_make("volume", "master_volume");
    audio->master_level = gst_element_factory_make("level", "master_level");
    audio->appsink = gst_element_factory_make("appsink", "audio_out_tap");
    if (!audio->pipeline || !audio->mixer || !post_queue || !audio->master_volume || !audio->master_level || !audio->appsink) {
        LOG_E("failed to create audio mixer pipeline elements");
        return SBS_ERR_IO;
    }

    g_object_set(audio->mixer,
        "latency", (guint64)(150 * GST_MSECOND),
        NULL);
    g_object_set(post_queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", (guint64)(2 * GST_SECOND),
        "leaky", 0,
        NULL);

    g_object_set(audio->master_volume, "volume", audio->master_volume_value, "mute", audio->master_mute, NULL);
    configure_master_audio_controls(audio);
    g_object_set(audio->master_level,
        "interval", (guint64)(100 * GST_MSECOND),
        "peak-ttl", (guint64)(1000 * GST_MSECOND),
        "post-messages", TRUE,
        NULL);
    g_object_set(audio->appsink,
        "emit-signals", TRUE,
        "sync", FALSE,
        "max-buffers", 256,
        "drop", FALSE,
        NULL);
    g_signal_connect(audio->appsink, "new-sample", G_CALLBACK(on_audio_sample), audio);

    gst_bin_add_many(GST_BIN(audio->pipeline), audio->mixer, post_queue, audio->master_volume, audio->master_level, audio->appsink, NULL);
    if (audio->master_panorama) {
        gst_bin_add(GST_BIN(audio->pipeline), audio->master_panorama);
    } else {
        LOG_W("audiopanorama unavailable; master left/right gain disabled");
    }
    if (audio->master_equalizer) {
        gst_bin_add(GST_BIN(audio->pipeline), audio->master_equalizer);
    } else {
        LOG_W("equalizer-10bands unavailable; master EQ disabled");
    }
    GstElement *last = audio->mixer;
    if (!link_branch_element(&last, post_queue) ||
        !link_branch_element(&last, audio->master_panorama) ||
        !link_branch_element(&last, audio->master_equalizer) ||
        !link_branch_element(&last, audio->master_volume) ||
        !link_branch_element(&last, audio->master_level) ||
        !link_branch_element(&last, audio->appsink)) {
        LOG_E("failed to link audio mixer core path");
        return SBS_ERR_IO;
    }

    g_hash_table_iter_init(&iter, audio->graph->sources);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_source_state_t *source = value;
        if (source->audio.enabled && source->audio.device) {
            add_branch(audio, source);
        }
    }

    bus = gst_pipeline_get_bus(GST_PIPELINE(audio->pipeline));
    gst_bus_add_watch(bus, on_bus_message, audio);
    gst_object_unref(bus);

    if (gst_element_set_state(audio->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOG_E("audio mixer pipeline failed to enter PLAYING");
        return SBS_ERR_IO;
    }
    LOG_I("audio mixer pipeline PLAYING using ALSA hw:0,2");
    return SBS_OK;
}

void sbs_audio_mixer_stop(sbs_audio_mixer_t *audio)
{
    sbs_audio_buffer_t *buf;
    if (!audio || !audio->pipeline) return;
    gst_element_set_state(audio->pipeline, GST_STATE_NULL);
    gst_object_unref(audio->pipeline);
    audio->pipeline = NULL;
    audio->mixer = audio->master_panorama = audio->master_equalizer = audio->master_volume = audio->master_level = audio->appsink = NULL;
    g_mutex_lock(&audio->lock);
    while (audio->pending_buffers && (buf = g_queue_pop_head(audio->pending_buffers)) != NULL) {
        g_free(buf);
    }
    g_mutex_unlock(&audio->lock);
}

void sbs_audio_mixer_on_scene_change(sbs_audio_mixer_t *audio,
                                     const char *active_scene_id)
{
    GHashTable *needed;
    GHashTableIter iter;
    gpointer key, value;
    sbs_scene_state_t *scene;
    guint i;
    if (!audio || !audio->pipeline) return;
    g_free(audio->active_scene_id);
    audio->active_scene_id = g_strdup(active_scene_id);
    scene = sbs_scene_graph_get_scene(audio->graph, active_scene_id);
    needed = g_hash_table_new(g_str_hash, g_str_equal);
    if (scene) {
        for (i = 0; i < scene->items->len; i++) {
            sbs_scene_item_state_t *item = g_ptr_array_index(scene->items, i);
            sbs_source_state_t *source;
            if (!item->visible) {
                continue;
            }
            g_hash_table_replace(needed, item->source_id, &item->audio);
            source = sbs_scene_graph_get_source(audio->graph, item->source_id);
            if (source && item->audio.enabled && item->audio.device &&
                !g_hash_table_lookup(audio->branches, item->source_id)) {
                add_branch_for_binding(audio, source, &item->audio);
            }
        }
    }
    g_hash_table_iter_init(&iter, audio->branches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_audio_branch_t *branch = value;
        sbs_audio_binding_t *binding = g_hash_table_lookup(needed, branch->source_id);
        if (binding) {
            branch->monitor = binding->monitor;
            branch->muted = binding->mute;
            branch->active_in_scene = binding->enabled;
            branch->effective_mute = !binding->enabled || binding->mute;
            g_object_set(branch->volume,
                         "volume", audio_binding_volume(binding),
                         "mute", branch->effective_mute,
                         NULL);
            configure_branch_audio_controls(branch, binding);
        } else {
            sbs_source_state_t *source = sbs_scene_graph_get_source(audio->graph, branch->source_id);
            branch->monitor = source ? source->audio.monitor : false;
            branch->muted = source ? source->audio.mute : true;
            branch->active_in_scene = false;
            branch->effective_mute = true;
            g_object_set(branch->volume,
                         "volume", source ? audio_binding_volume(&source->audio) : 1.0,
                         "mute", branch->effective_mute,
                         NULL);
            if (source) {
                configure_branch_audio_controls(branch, &source->audio);
            }
        }
    }
    g_hash_table_destroy(needed);
}

int sbs_audio_mixer_set_source_state(sbs_audio_mixer_t *audio,
                                     sbs_source_state_t *source)
{
    sbs_audio_branch_t *branch;
    if (!audio || !source) return SBS_ERR_INVAL;
    branch = g_hash_table_lookup(audio->branches, source->id);
    if (!source->audio.enabled) {
        if (branch) {
            branch->muted = true;
            branch->effective_mute = true;
            g_object_set(branch->volume, "mute", TRUE, NULL);
        }
        return SBS_OK;
    }
    if (!source->audio.device) {
        source->audio.device = g_strdup("hw:0,2");
    }
    if (!branch) {
        if (!audio->pipeline) return SBS_ERR_NOT_FOUND;
        branch = add_branch(audio, source);
        if (!branch) return SBS_ERR_IO;
        if (audio->active_scene_id) {
            sbs_audio_mixer_on_scene_change(audio, audio->active_scene_id);
        }
    }
    branch->monitor = source->audio.monitor;
    branch->muted = source->audio.mute;
    branch->effective_mute = branch->muted || !branch->active_in_scene;
    configure_branch_audio_controls(branch, &source->audio);
    g_object_set(branch->volume, "volume", audio_binding_volume(&source->audio), "mute", branch->effective_mute, NULL);
    return SBS_OK;
}

int sbs_audio_mixer_set_scene_item_state(sbs_audio_mixer_t *audio,
                                         sbs_scene_item_state_t *item)
{
    sbs_source_state_t *source;
    sbs_audio_branch_t *branch;
    bool active_scene_item = false;

    if (!audio || !item) return SBS_ERR_INVAL;
    source = sbs_scene_graph_get_source(audio->graph, item->source_id);
    if (!source) return SBS_ERR_NOT_FOUND;

    branch = g_hash_table_lookup(audio->branches, item->source_id);
    if (!branch && item->audio.enabled && item->audio.device) {
        if (!audio->pipeline) return SBS_ERR_NOT_FOUND;
        branch = add_branch_for_binding(audio, source, &item->audio);
        if (!branch) return SBS_ERR_IO;
    }
    if (!branch) return SBS_OK;

    active_scene_item = audio->active_scene_id &&
        sbs_scene_graph_get_item(audio->graph, audio->active_scene_id, item->id) == item;
    if (active_scene_item) {
        branch->monitor = item->audio.monitor;
        branch->muted = item->audio.mute;
        branch->active_in_scene = item->audio.enabled;
        branch->effective_mute = !item->audio.enabled || item->audio.mute;
        g_object_set(branch->volume,
                     "volume", audio_binding_volume(&item->audio),
                     "mute", branch->effective_mute,
                     NULL);
        configure_branch_audio_controls(branch, &item->audio);
    }
    return SBS_OK;
}

int sbs_audio_mixer_set_master(sbs_audio_mixer_t *audio,
                               double volume,
                               bool mute,
                               double left_gain,
                               double right_gain,
                               const double *eq_bands,
                               uint32_t eq_band_count)
{
    if (!audio || !audio->master_volume) return SBS_ERR_INVAL;
    audio->master_volume_value = volume;
    audio->master_mute = mute;
    audio->master_left_gain = left_gain >= 0.0 ? left_gain : 1.0;
    audio->master_right_gain = right_gain >= 0.0 ? right_gain : 1.0;
    for (uint32_t i = 0; eq_bands && i < G_N_ELEMENTS(audio->master_eq_bands) && i < eq_band_count; i++) {
        audio->master_eq_bands[i] = eq_bands[i];
    }
    configure_master_audio_controls(audio);
    g_object_set(audio->master_volume, "volume", volume, "mute", mute, NULL);
    return SBS_OK;
}

cJSON *sbs_audio_mixer_serialize_levels(sbs_audio_mixer_t *audio)
{
    GHashTableIter iter;
    gpointer key, value;
    cJSON *obj = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *master = cJSON_CreateObject();
    if (!audio) {
        cJSON_AddItemToObject(obj, "sources", sources);
        cJSON_AddItemToObject(obj, "master", master);
        return obj;
    }
    g_hash_table_iter_init(&iter, audio->branches);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        sbs_audio_branch_t *branch = value;
        cJSON *lvl = cJSON_CreateObject();
        cJSON_AddNumberToObject(lvl, "level_db", branch->meter.level_db);
        cJSON_AddNumberToObject(lvl, "peak_db", branch->meter.peak_db);
        cJSON_AddNumberToObject(lvl, "timestamp_us", (double)branch->meter.timestamp_us);
        cJSON_AddBoolToObject(lvl, "monitor", branch->monitor);
        cJSON_AddBoolToObject(lvl, "active_in_scene", branch->active_in_scene);
        cJSON_AddBoolToObject(lvl, "effective_mute", branch->effective_mute);
        cJSON_AddItemToObject(sources, branch->source_id, lvl);
    }
    cJSON_AddNumberToObject(master, "level_db", audio->master_meter.level_db);
    cJSON_AddNumberToObject(master, "peak_db", audio->master_meter.peak_db);
    cJSON_AddNumberToObject(master, "timestamp_us", (double)audio->master_meter.timestamp_us);
    cJSON_AddItemToObject(obj, "sources", sources);
    cJSON_AddItemToObject(obj, "master", master);
    return obj;
}

cJSON *sbs_audio_mixer_serialize_state(sbs_audio_mixer_t *audio)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *eq = cJSON_CreateArray();
    cJSON_AddStringToObject(obj, "device", "hw:0,2");
    cJSON_AddStringToObject(obj, "backend", active_audio_backend_name());
    cJSON_AddStringToObject(obj, "preferred_backend", "hifi");
    cJSON_AddNumberToObject(obj, "master_volume", audio ? audio->master_volume_value : 1.0);
    cJSON_AddNumberToObject(obj, "master_left_gain", audio ? audio->master_left_gain : 1.0);
    cJSON_AddNumberToObject(obj, "master_right_gain", audio ? audio->master_right_gain : 1.0);
    for (uint32_t i = 0; i < 10; i++) {
        cJSON_AddItemToArray(eq, cJSON_CreateNumber(audio ? audio->master_eq_bands[i] : 0.0));
    }
    cJSON_AddItemToObject(obj, "master_eq_bands", eq);
    cJSON_AddBoolToObject(obj, "master_mute", audio ? audio->master_mute : false);
    cJSON_AddItemToObject(obj, "hifi", serialize_hifi_probe());
    cJSON_AddItemToObject(obj, "levels", sbs_audio_mixer_serialize_levels(audio));
    return obj;
}

sbs_audio_buffer_t *sbs_audio_mixer_take_latest_buffer(sbs_audio_mixer_t *audio)
{
    sbs_audio_buffer_t *buf;
    bool emit_silence = false;
    if (!audio) return NULL;
    g_mutex_lock(&audio->lock);
    buf = audio->pending_buffers ? g_queue_pop_head(audio->pending_buffers) : NULL;
    if (buf) {
        audio->pending_samples -= MIN(audio->pending_samples,
                                      (uint64_t)buf->msg.n_samples);
        audio->popped_buffers++;
    } else {
        emit_silence = should_emit_silent_master_buffer(audio, g_get_monotonic_time());
    }
    g_mutex_unlock(&audio->lock);
    if (emit_silence)
        return make_silent_master_buffer();
    return buf;
}

uint32_t sbs_audio_mixer_pending_depth(sbs_audio_mixer_t *audio)
{
    uint32_t depth = 0;
    if (!audio) return 0;
    g_mutex_lock(&audio->lock);
    depth = audio->pending_buffers ? g_queue_get_length(audio->pending_buffers) : 0;
    g_mutex_unlock(&audio->lock);
    return depth;
}

#endif

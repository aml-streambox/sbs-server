#define _GNU_SOURCE
#define SBS_LOG_COMP "server"

#include "sbs/log.h"
#include "sbs/types.h"
#include "sbs_version.h"
#include "sbs/compositor_thread.h"
#include "sbs/source_supervisor.h"
#include "sbs/output_supervisor.h"
#include "sbs/output_router.h"
#include "sbs/frame_slot.h"
#include "sbs/scene_graph.h"
#include "sbs/api_server.h"
#include "sbs/audio_mixer.h"
#include "sbs/config_manager.h"
#include "sbs/instance_manager.h"
#include "sbs/encoder_manager.h"

#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <glib/gstdio.h>

#ifdef SBS_HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

static gboolean opt_version  = FALSE;
static gchar   *opt_config   = NULL;
static gchar   *opt_log_level = NULL;
static gchar   *opt_instance_socket = NULL;
static gint     opt_instance_id = -1;
static gint     opt_api_port = -1;

static GOptionEntry entries[] = {
    { "version",   'v', 0, G_OPTION_ARG_NONE,   &opt_version,
      "Print version and exit", NULL },
    { "config-dir", 'c', 0, G_OPTION_ARG_STRING, &opt_config,
      "Configuration directory", "DIR" },
    { "log-level", 'l', 0, G_OPTION_ARG_STRING,  &opt_log_level,
      "Log level (fatal,error,warn,info,debug,trace)", "LEVEL" },
    { "instance-socket", 0, 0, G_OPTION_ARG_STRING, &opt_instance_socket,
      "Internal instance control socket path", "PATH" },
    { "instance-id", 0, 0, G_OPTION_ARG_INT, &opt_instance_id,
      "Internal instance identifier", "ID" },
    { "api-port", 0, 0, G_OPTION_ARG_INT, &opt_api_port,
      "API port override", "PORT" },
    { NULL }
};

static GMainLoop *main_loop = NULL;
static sbs_compositor_thread_t *comp_thread = NULL;
static sbs_source_supervisor_t *source_sup = NULL;
static sbs_output_supervisor_t *output_sup = NULL;
static sbs_output_router_t *output_router = NULL;
static sbs_scene_graph_t *scene_graph = NULL;
static sbs_api_server_t *api_server = NULL;
static sbs_audio_mixer_t *audio_mixer = NULL;
static sbs_config_manager_t *config_mgr = NULL;
static sbs_instance_manager_t *instance_mgr = NULL;
static sbs_encoder_manager_t *encoder_mgr = NULL;

static gboolean shader_dir_valid(const char *path)
{
    char *vert;
    gboolean ok;
    if (!path) return FALSE;
    vert = g_build_filename(path, "composite.vert.spv", NULL);
    ok = g_file_test(vert, G_FILE_TEST_EXISTS);
    g_free(vert);
    return ok;
}

static void resolve_runtime_paths(const char *argv0,
                                  char **out_worker_bin,
                                  char **out_shader_dir,
                                  char **out_socket_dir)
{
    const char *env_worker = g_getenv("SBS_WORKER_BIN");
    const char *env_shaders = g_getenv("SBS_SHADER_DIR");
    const char *env_socket = g_getenv("SBS_SOCKET_DIR");
    char *exe_path = g_canonicalize_filename(argv0, NULL);
    char *exe_dir = g_path_get_dirname(exe_path);
    char *dev_worker = g_build_filename(exe_dir, "..", "worker", "sbs-worker", NULL);
    char *dev_shaders_a = g_build_filename(exe_dir, "..", "..", "..", "shaders", NULL);
    char *dev_shaders_b = g_build_filename(exe_dir, "..", "shaders", NULL);
    char *socket_dir = g_build_filename(g_get_tmp_dir(), "sbs-run", NULL);

    if (env_worker && *env_worker) {
        *out_worker_bin = g_strdup(env_worker);
    } else if (shader_dir_valid("/usr/share/sbs/shaders")) {
        *out_worker_bin = g_strdup("/usr/bin/sbs-worker");
    } else {
        *out_worker_bin = g_strdup(dev_worker);
    }

    if (env_shaders && *env_shaders) {
        *out_shader_dir = g_strdup(env_shaders);
    } else if (shader_dir_valid("/usr/share/sbs/shaders")) {
        *out_shader_dir = g_strdup("/usr/share/sbs/shaders");
    } else if (shader_dir_valid(dev_shaders_a)) {
        *out_shader_dir = g_strdup(dev_shaders_a);
    } else {
        *out_shader_dir = g_strdup(dev_shaders_b);
    }

    if (env_socket && *env_socket) {
        *out_socket_dir = g_strdup(env_socket);
    } else if (shader_dir_valid("/usr/share/sbs/shaders")) {
        *out_socket_dir = g_strdup("/run/sbs");
    } else {
        *out_socket_dir = g_strdup(socket_dir);
    }

    g_free(socket_dir);
    g_free(dev_worker);
    g_free(dev_shaders_a);
    g_free(dev_shaders_b);
    g_free(exe_dir);
    g_free(exe_path);
}

static void publish_current_scene(void)
{
    sbs_comp_scene_state_t scene_state;

    if (!scene_graph || !comp_thread) {
        return;
    }

    if (sbs_scene_graph_build_compositor_state(scene_graph, &scene_state) == SBS_OK) {
        sbs_compositor_thread_set_scene(comp_thread, &scene_state);
    }
}

static void crash_handler(int sig, siginfo_t *info, void *ctx)
{
    (void)ctx;
    void *bt[64];
    int n = backtrace(bt, 64);
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "\n=== SBS CRASH: signal=%d code=%d addr=%p pid=%d tid=%ld ===\n",
        sig, info ? info->si_code : -1, info ? info->si_addr : NULL,
        (int)getpid(), (long)syscall(SYS_gettid));
    (void)!write(STDERR_FILENO, hdr, hlen);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    (void)!write(STDERR_FILENO, "=== END BACKTRACE ===\n", 22);
    /* reraise with default handler */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static gboolean on_signal(gpointer user_data)
{
    (void)user_data;
    LOG_I("received shutdown signal");

#ifdef SBS_HAVE_SYSTEMD
    sd_notify(0, "STOPPING=1\nSTATUS=Shutting down...");
#endif

    if (instance_mgr) {
        sbs_instance_manager_shutdown_all(instance_mgr);
    } else {
        if (output_sup) {
            sbs_output_supervisor_shutdown_all(output_sup);
        }
        if (source_sup) {
            sbs_source_supervisor_shutdown_all(source_sup);
        }
    }

    if (main_loop)
        g_main_loop_quit(main_loop);

    return G_SOURCE_REMOVE;
}

static void cleanup_runtime_paths(char *worker_bin, char *shader_dir, char *socket_dir)
{
    g_free(worker_bin);
    g_free(shader_dir);
    g_free(socket_dir);
}

static int run_instance_daemon(const char *argv0, sbs_log_level_t level)
{
    char *worker_bin = NULL;
    char *shader_dir = NULL;
    char *socket_dir = NULL;
    uint16_t api_port = opt_api_port > 0 ? (uint16_t)opt_api_port : 10100;

    /* Force line-buffered stdout/stderr so crash diagnostics survive.
       Without this, libc fully-buffers when redirected to a file and
       a hard crash loses all unflushed output. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    install_crash_handler();

    sbs_log_init("instance", level);
    LOG_I("sbs-server instance %d starting (pid=%d)", opt_instance_id, (int)getpid());

    LOG_I("[init:1] resolving runtime paths");
    resolve_runtime_paths(argv0, &worker_bin, &shader_dir, &socket_dir);
    g_mkdir_with_parents(socket_dir, 0755);
    config_mgr = sbs_config_manager_new(opt_config ? opt_config : "/var/lib/sbs/instances/0");
    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);

    LOG_I("[init:2] creating scene graph");
    scene_graph = sbs_scene_graph_new_default();
    if (!scene_graph) goto cleanup;
    if (config_mgr) {
        sbs_config_manager_apply_system_overrides(config_mgr, scene_graph, NULL);
        sbs_config_manager_preload_canvas(config_mgr, scene_graph);
    }

    uint32_t canvas_w = scene_graph->canvas.width;
    uint32_t canvas_h = scene_graph->canvas.height;
    uint32_t canvas_fps = scene_graph->canvas.fps_num;
    sbs_export_color_mode_t canvas_export_color =
        scene_graph->canvas.color_mode == SBS_SCENE_COLOR_MODE_HDR10
            ? SBS_EXPORT_COLOR_HDR10 : SBS_EXPORT_COLOR_SDR;

    LOG_I("[init:3] creating compositor thread (%ux%u@%u)", canvas_w, canvas_h, canvas_fps);
    comp_thread = sbs_compositor_thread_new(canvas_w, canvas_h, canvas_fps,
                                            canvas_export_color, shader_dir);
    if (!comp_thread || sbs_compositor_thread_start(comp_thread) != 0) goto cleanup;
    LOG_I("[init:4] compositor thread started");

    LOG_I("[init:5] creating output supervisor");
    output_sup = sbs_output_supervisor_new(worker_bin, socket_dir);
    if (output_sup) {
        output_router = sbs_output_router_new(comp_thread, output_sup, canvas_w, canvas_h, canvas_fps);
    }
    LOG_I("[init:6] output supervisor ready");

    /* Create shared encoder manager */
    {
        sbs_encoder_config_t enc_cfg = {
            .codec        = "h265",
            .bitrate_kbps = 10000,
            .gop_size     = canvas_fps,
            .gop_pattern  = 0,
            .rc_mode      = 0,
            .encoder      = NULL,
            .hdr10        = (canvas_export_color == SBS_EXPORT_COLOR_HDR10),
        };
        encoder_mgr = sbs_encoder_manager_new(canvas_w, canvas_h, canvas_fps, 1, &enc_cfg);
        if (encoder_mgr) {
            sbs_output_router_set_encoder_manager(output_router, encoder_mgr);
            sbs_output_router_set_color_mode(output_router,
                canvas_export_color);
            LOG_I("[init:6.1] encoder manager created and wired to output router (hdr10=%d)", enc_cfg.hdr10);
        } else {
            LOG_W("failed to create encoder manager, falling back to supervisor-only");
        }
    }

    /* The export thread inside the output router watches the compositor
     * eventfd directly — no need for a GLib fd watch on the main thread.
     * This keeps the main loop free from blocking GPU export work. */

    LOG_I("[init:7] creating source supervisor");
    source_sup = sbs_source_supervisor_new(worker_bin, socket_dir);
    if (source_sup) {
        sbs_source_supervisor_set_scene_graph(source_sup, scene_graph);
    }
    LOG_I("[init:8] creating API server");
    api_server = sbs_api_server_new(scene_graph, source_sup, output_sup, comp_thread, output_router);
    if (!api_server) goto cleanup;
    if (source_sup) {
        sbs_source_supervisor_set_api_server(source_sup, api_server);
    }
    api_server->audio = audio_mixer;
    audio_mixer = NULL;
    api_server->encoder_mgr = encoder_mgr;
    api_server->config = config_mgr;
    api_server->auth = sbs_auth_manager_new(sbs_config_manager_config_dir(config_mgr));
    config_mgr = NULL;
    sbs_api_server_set_instance_id(api_server, opt_instance_id >= 0 ? (uint32_t)opt_instance_id : 0U);

    LOG_I("[init:9] loading persisted config bundle");
    if (api_server->config) {
        cJSON *bundle = NULL;
        int load_rc = sbs_config_manager_load_bundle(api_server->config, &bundle);
        if (load_rc == SBS_OK && bundle) {
            if (sbs_config_manager_apply_bundle(api_server->config, api_server, bundle) != SBS_OK) {
                LOG_W("failed to apply persisted config bundle; continuing with defaults");
                sbs_config_manager_start_runtime(api_server);
            }
            cJSON_Delete(bundle);
        } else {
            sbs_config_manager_start_runtime(api_server);
        }
    } else {
        sbs_config_manager_start_runtime(api_server);
    }

    LOG_I("[init:10] starting API server on port %u", (unsigned)api_port);
    if (sbs_api_server_start(api_server, api_port) != SBS_OK) goto cleanup;
    if (opt_instance_socket && sbs_api_server_start_unix(api_server, opt_instance_socket) != SBS_OK) goto cleanup;

    LOG_I("[init:DONE] instance %d entering main loop", opt_instance_id);
    g_main_loop_run(main_loop);

cleanup:
    if (output_router) {
        sbs_output_router_free(output_router);
        output_router = NULL;
    }
    if (encoder_mgr) {
        sbs_encoder_manager_free(encoder_mgr);
        encoder_mgr = NULL;
    }
    if (output_sup) {
        sbs_output_supervisor_free(output_sup);
        output_sup = NULL;
    }
    if (source_sup) {
        sbs_source_supervisor_free(source_sup);
        source_sup = NULL;
    }
    if (api_server) {
        sbs_api_server_free(api_server);
        api_server = NULL;
    }
    if (config_mgr) {
        sbs_config_manager_free(config_mgr);
        config_mgr = NULL;
    }
    if (audio_mixer) {
        sbs_audio_mixer_free(audio_mixer);
        audio_mixer = NULL;
    }
    if (scene_graph) {
        sbs_scene_graph_free(scene_graph);
        scene_graph = NULL;
    }
    sbs_compositor_thread_free(comp_thread);
    comp_thread = NULL;
    if (main_loop) {
        g_main_loop_unref(main_loop);
        main_loop = NULL;
    }
    cleanup_runtime_paths(worker_bin, shader_dir, socket_dir);
    return EXIT_SUCCESS;
}

static int run_controller(const char *argv0, sbs_log_level_t level)
{
    char *worker_bin = NULL;
    char *shader_dir = NULL;
    char *socket_dir = NULL;
    uint16_t api_port = opt_api_port > 0 ? (uint16_t)opt_api_port : 10086;

    sbs_log_init("server", level);
    LOG_I("sbs-server controller starting");
    resolve_runtime_paths(argv0, &worker_bin, &shader_dir, &socket_dir);
    g_mkdir_with_parents(socket_dir, 0755);
    main_loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_signal, NULL);
    g_unix_signal_add(SIGTERM, on_signal, NULL);

    /* Use /proc/self/exe to get the real binary path. g_canonicalize_filename(argv0)
       fails when argv0 is a bare name (e.g. "sbs-server") resolved via PATH — it would
       resolve relative to CWD instead of /usr/bin. */
    {
        char *self_exe = g_file_read_link("/proc/self/exe", NULL);
        if (!self_exe) self_exe = g_canonicalize_filename(argv0, NULL);
        instance_mgr = sbs_instance_manager_new(self_exe,
                                                opt_config ? opt_config : "/var/lib/sbs",
                                                socket_dir,
                                                opt_log_level ? opt_log_level : "info");
        g_free(self_exe);
    }
    if (!instance_mgr) goto cleanup;
    sbs_instance_manager_load(instance_mgr);
    sbs_instance_manager_ensure_default(instance_mgr);
    sbs_instance_manager_start_desired(instance_mgr);

    api_server = sbs_api_server_new(NULL, NULL, NULL, NULL, NULL);
    if (!api_server) goto cleanup;
    sbs_api_server_configure_controller(api_server, instance_mgr);
    if (sbs_api_server_start(api_server, api_port) != SBS_OK) goto cleanup;

    LOG_I("controller entering main loop");
    g_main_loop_run(main_loop);

cleanup:
    if (api_server) {
        sbs_api_server_free(api_server);
        api_server = NULL;
    }
    if (instance_mgr) {
        sbs_instance_manager_free(instance_mgr);
        instance_mgr = NULL;
    }
    if (main_loop) {
        g_main_loop_unref(main_loop);
        main_loop = NULL;
    }
    cleanup_runtime_paths(worker_bin, shader_dir, socket_dir);
    return EXIT_SUCCESS;
}

#ifdef SBS_HAVE_SYSTEMD
static gboolean watchdog_cb(gpointer user_data)
{
    (void)user_data;
    sd_notify(0, "WATCHDOG=1");
    return G_SOURCE_CONTINUE;
}
#endif

int main(int argc, char **argv)
{
    GError *error = NULL;
    GOptionContext *ctx = g_option_context_new("- StreamBox Broadcast System");
    g_option_context_add_main_entries(ctx, entries, NULL);

    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        fprintf(stderr, "sbs-server: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(ctx);
        return EXIT_FAILURE;
    }
    g_option_context_free(ctx);

    if (opt_version) {
        printf("sbs-server %s (built %s)\n", SBS_VERSION_STRING, SBS_BUILD_DATE);
        return EXIT_SUCCESS;
    }

    sbs_log_level_t level = SBS_LOG_INFO;
    if (opt_log_level) {
        level = sbs_log_level_from_string(opt_log_level);
    }

    if (opt_instance_socket && opt_instance_id >= 0) {
        return run_instance_daemon(argv[0], level);
    }
    return run_controller(argv[0], level);
}

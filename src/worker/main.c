/*
 * SBS - StreamBox Broadcast System
 * sbs-worker: Child process entry point
 *
 * The worker binary runs in two modes:
 *   --mode=source   Source capture pipeline (GStreamer → IPC)
 *   --mode=output   Output encoding/streaming pipeline (IPC → GStreamer)
 *
 * Configuration is passed as JSON on stdin by the supervisor.
 */
#define _GNU_SOURCE
#define SBS_LOG_COMP "worker"

#include "sbs/log.h"
#include "sbs/types.h"
#include "sbs_version.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SBS_HAVE_GSTREAMER
#include "worker_common.h"

/* Forward declarations for mode-specific entry points (tasks 5.x, 7.x) */
extern int source_worker_run(sbs_worker_ctx_t *ctx);
extern int output_worker_run(sbs_worker_ctx_t *ctx);
#endif /* SBS_HAVE_GSTREAMER */

/* ── Command-line options ─────────────────────────────────────── */

static gboolean opt_version = FALSE;
static gchar   *opt_mode    = NULL;
static gchar   *opt_id      = NULL;
static gchar   *opt_socket  = NULL;

static GOptionEntry entries[] = {
    { "version", 'v', 0, G_OPTION_ARG_NONE,   &opt_version,
      "Print version and exit", NULL },
    { "mode", 'm', 0, G_OPTION_ARG_STRING, &opt_mode,
      "Worker mode (source or output)", "MODE" },
    { "id", 'i', 0, G_OPTION_ARG_STRING, &opt_id,
      "Worker identifier (UUID)", "ID" },
    { "socket", 's', 0, G_OPTION_ARG_STRING, &opt_socket,
      "IPC socket path", "PATH" },
    { NULL }
};

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    GError *error = NULL;
    int exit_code = EXIT_FAILURE;
    GOptionContext *ctx = g_option_context_new("- SBS Worker Process");
    g_option_context_add_main_entries(ctx, entries, NULL);

    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        fprintf(stderr, "sbs-worker: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(ctx);
        return EXIT_FAILURE;
    }
    g_option_context_free(ctx);

    if (opt_version) {
        printf("sbs-worker %s (built %s)\n", SBS_VERSION_STRING, SBS_BUILD_DATE);
        return EXIT_SUCCESS;
    }

    /* Validate required arguments */
    if (!opt_mode) {
        fprintf(stderr, "sbs-worker: --mode is required (source or output)\n");
        return EXIT_FAILURE;
    }
    if (strcmp(opt_mode, "source") != 0 && strcmp(opt_mode, "output") != 0) {
        fprintf(stderr, "sbs-worker: --mode must be 'source' or 'output'\n");
        return EXIT_FAILURE;
    }
    if (!opt_id) {
        fprintf(stderr, "sbs-worker: --id is required\n");
        return EXIT_FAILURE;
    }
    if (!opt_socket) {
        fprintf(stderr, "sbs-worker: --socket is required\n");
        return EXIT_FAILURE;
    }

    /* Initialize logging (worker uses its mode as component) */
    char comp[64];
    snprintf(comp, sizeof(comp), "worker:%s", opt_mode);
    sbs_log_init(comp, SBS_LOG_INFO);

    LOG_I("sbs-worker %s starting (mode=%s, id=%s)", SBS_VERSION_STRING, opt_mode, opt_id);
    LOG_I("IPC socket: %s", opt_socket);

#ifdef SBS_HAVE_GSTREAMER
    /* Initialize worker context (GStreamer, JSON config, IPC connect, signals) */
    sbs_worker_ctx_t wctx;
    int rc = sbs_worker_init(&wctx, &argc, &argv, opt_mode, opt_id, opt_socket);
    if (rc != SBS_OK) {
        LOG_E("worker init failed: %d", rc);
        goto cleanup_opts;
    }

    /* Send READY status to supervisor */
    sbs_worker_send_status(&wctx, SBS_WORKER_STATE_READY);

    /* Dispatch to mode-specific implementation */
    if (strcmp(opt_mode, "source") == 0) {
        rc = source_worker_run(&wctx);
    } else {
        rc = output_worker_run(&wctx);
    }

    /* Send final status */
    if (!wctx.shutting_down) {
        sbs_worker_send_status(&wctx, SBS_WORKER_STATE_EOS);
    }

    LOG_I("sbs-worker exiting (rc=%d)", rc);
    exit_code = (rc == SBS_OK) ? EXIT_SUCCESS : EXIT_FAILURE;
    sbs_worker_cleanup(&wctx);

cleanup_opts:
#else
    /* Stub build — no GStreamer available */
    LOG_E("sbs-worker cannot run requested mode '%s': GStreamer not available", opt_mode);
#endif

    g_free(opt_mode);
    g_free(opt_id);
    g_free(opt_socket);

    return exit_code;
}

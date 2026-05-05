/*
 * SBS - StreamBox Broadcast System
 * sbs-cli: Command-line client (Phase 0 skeleton)
 */
#define SBS_LOG_COMP "cli"

#include "sbs/log.h"
#include "sbs/types.h"
#include "sbs_version.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Command-line options ─────────────────────────────────────── */

static gboolean opt_version = FALSE;
static gchar   *opt_host    = NULL;
static gint     opt_port    = 10086;

static GOptionEntry entries[] = {
    { "version", 'v', 0, G_OPTION_ARG_NONE,   &opt_version,
      "Print version and exit", NULL },
    { "host", 'h', 0, G_OPTION_ARG_STRING,  &opt_host,
      "SBS server hostname", "HOST" },
    { "port", 'p', 0, G_OPTION_ARG_INT,     &opt_port,
      "SBS server port (default: 10086)", "PORT" },
    { NULL }
};

/* ── Main ─────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    GError *error = NULL;
    GOptionContext *ctx = g_option_context_new("COMMAND [ARGS] - SBS CLI Client");
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_option_context_set_strict_posix(ctx, TRUE);

    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        fprintf(stderr, "sbs-cli: %s\n", error->message);
        g_error_free(error);
        g_option_context_free(ctx);
        return EXIT_FAILURE;
    }
    g_option_context_free(ctx);

    if (opt_version) {
        printf("sbs-cli %s (built %s)\n", SBS_VERSION_STRING, SBS_BUILD_DATE);
        return EXIT_SUCCESS;
    }

    /* Remaining args are the command + params */
    if (argc < 2) {
        fprintf(stderr, "sbs-cli: no command specified\n");
        fprintf(stderr, "Usage: sbs-cli [options] <command> [params]\n");
        fprintf(stderr, "\nCommands:\n");
        fprintf(stderr, "  system.getInfo       Get server info\n");
        fprintf(stderr, "  system.getStatus     Get server status\n");
        fprintf(stderr, "  source.list          List sources\n");
        fprintf(stderr, "  scene.list           List scenes\n");
        fprintf(stderr, "  output.list          List output groups\n");
        fprintf(stderr, "\nRun 'sbs-cli --help' for options.\n");
        return EXIT_FAILURE;
    }

    const char *command = argv[1];
    const char *host = opt_host ? opt_host : "127.0.0.1";

    printf("sbs-cli %s\n", SBS_VERSION_STRING);
    printf("Connecting to ws://%s:%d/api ...\n", host, opt_port);
    printf("Command: %s\n", command);

    /* TODO Phase 3: WebSocket connect, send JSON-RPC, print result */
    fprintf(stderr, "sbs-cli: not yet implemented (skeleton)\n");

    g_free(opt_host);

    return EXIT_FAILURE; /* Skeleton always "fails" */
}

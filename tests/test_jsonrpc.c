#include "sbs_test.h"
#include "sbs/api_server.h"
#include "sbs/instance_manager.h"

#include <glib/gstdio.h>
#include <sys/mman.h>
#include <unistd.h>

static sbs_scene_graph_t *graph;
static sbs_api_server_t *server;
static sbs_api_client_t *client;
static sbs_instance_manager_t *instance_mgr;
static char *api_config_dir;

void test_api_stubs_reset_output_start(void);
void test_api_stubs_set_output_start_result(int result);

static void remove_tree(const char *path)
{
    GDir *dir;
    const char *name;

    if (!path) {
        return;
    }
    dir = g_dir_open(path, 0, NULL);
    if (!dir) {
        g_remove(path);
        return;
    }
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *child = g_build_filename(path, name, NULL);
        remove_tree(child);
        g_free(child);
    }
    g_dir_close(dir);
    g_rmdir(path);
}

static void setup_api(void)
{
    GError *gerr = NULL;

    graph = sbs_scene_graph_new_default();
    server = sbs_api_server_new(graph, NULL, NULL, NULL, NULL);
    api_config_dir = g_dir_make_tmp("sbs-test-api-XXXXXX", &gerr);
    SBS_ASSERT_NOT_NULL(api_config_dir);
    if (gerr) {
        g_error_free(gerr);
    }
    server->config = sbs_config_manager_new(api_config_dir);
    SBS_ASSERT_NOT_NULL(server->config);
    client = sbs_api_client_new(1);
    sbs_api_server_add_client(server, client);
}

static void teardown_api(void)
{
    if (instance_mgr) {
        sbs_instance_manager_free(instance_mgr);
        instance_mgr = NULL;
    }
    if (server && server->config) {
        sbs_config_manager_free(server->config);
        server->config = NULL;
    }
    sbs_api_server_free(server);
    sbs_scene_graph_free(graph);
    remove_tree(api_config_dir);
    g_free(api_config_dir);
    api_config_dir = NULL;
    server = NULL;
    graph = NULL;
    client = NULL;
}

static void setup_controller_api(void)
{
    char tmpl[] = "/tmp/sbs-test-controller-XXXXXX";
    char *dir = mkdtemp(tmpl);
    SBS_ASSERT_NOT_NULL(dir);
    graph = NULL;
    server = sbs_api_server_new(NULL, NULL, NULL, NULL, NULL);
    instance_mgr = sbs_instance_manager_new("/bin/true", dir, "/tmp", "info");
    SBS_ASSERT_NOT_NULL(instance_mgr);
    SBS_ASSERT_EQ(sbs_instance_manager_load(instance_mgr), SBS_OK);
    SBS_ASSERT_EQ(sbs_instance_manager_ensure_default(instance_mgr), SBS_OK);
    sbs_api_server_configure_controller(server, instance_mgr);
    client = sbs_api_client_new(1);
    sbs_api_server_add_client(server, client);
}

SBS_TEST_FIXTURE(jsonrpc, dispatches_system_get_info, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"system.getInfo\"}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "result") != NULL);
    SBS_ASSERT(strstr(response, "version") != NULL);
    SBS_ASSERT(strstr(response, "hostname") != NULL);
    SBS_ASSERT(strstr(response, "runtime") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, unknown_method_returns_error, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"bogus.method\"}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "Method not found") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, command_execute_system_info, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"command.execute\",\"params\":{\"command\":\"system get-info\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "protocol_version") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, system_state_includes_runtime_health, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"system.getState\"}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"runtime\"") != NULL);
    SBS_ASSERT(strstr(response, "\"api_running\"") != NULL);
    SBS_ASSERT(strstr(response, "\"sources\"") != NULL);
    SBS_ASSERT(strstr(response, "\"outputs\"") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_catalog_lists_supported_kinds, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"source.listKinds\"}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "videotestsrc") != NULL);
    SBS_ASSERT(strstr(response, "streamboxsrc") == NULL);
    SBS_ASSERT(strstr(response, "text") != NULL);
    SBS_ASSERT(strstr(response, "vfmcap") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_describe_kind_returns_fields, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"source.describeKind\",\"params\":{\"kind\":\"videotestsrc\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"fields\"") != NULL);
    SBS_ASSERT(strstr(response, "pattern") != NULL);
    SBS_ASSERT(strstr(response, "options") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_describe_text_kind_returns_font_fields, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"source.describeKind\",\"params\":{\"kind\":\"text\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "font_family") != NULL);
    SBS_ASSERT(strstr(response, "Liberation Sans") != NULL);
    SBS_ASSERT(strstr(response, "font_path") != NULL);
    SBS_ASSERT(strstr(response, "asset_kind") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_describe_unknown_kind_returns_error, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"source.describeKind\",\"params\":{\"kind\":\"not-real\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Unsupported source kind") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_upload_rejects_invalid_asset_kind, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":27,\"method\":\"source.uploadAsset\",\"params\":{\"asset_kind\":\"script\",\"filename\":\"x.js\",\"data_base64\":\"aGk=\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Unsupported asset kind") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_upload_requires_data, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":28,\"method\":\"source.uploadAsset\",\"params\":{\"asset_kind\":\"image\",\"filename\":\"x.png\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Upload data is required") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_upload_requires_config_manager, setup_api, teardown_api)
{
    char *response = NULL;
    sbs_config_manager_free(server->config);
    server->config = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":29,\"method\":\"source.uploadAsset\",\"params\":{\"asset_kind\":\"image\",\"filename\":\"x.png\",\"data_base64\":\"aGk=\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "config manager not initialized") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_upload_sanitizes_path_traversal_filename, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":29,\"method\":\"source.uploadAsset\",\"params\":{\"asset_kind\":\"image\",\"filename\":\"../../bad name.png\",\"data_base64\":\"aGk=\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"result\"") != NULL);
    SBS_ASSERT(strstr(response, "bad_name.png") != NULL);
    SBS_ASSERT(strstr(response, "../") == NULL);
    SBS_ASSERT(strstr(response, "/assets/image/") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, structured_source_create_and_update, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"source.create\",\"params\":{\"id\":\"source-test\",\"name\":\"Pattern\",\"type\":\"videotestsrc\",\"config\":{\"pattern\":\"ball\",\"fps\":30}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "source-test") != NULL);
    SBS_ASSERT(strstr(response, "ball") != NULL);
    free(response);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"source.update\",\"params\":{\"id\":\"source-test\",\"config\":{\"pattern\":\"snow\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "snow") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, output_rtmp_passcode_redacted, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"output.create\",\"params\":{\"id\":\"output-rtmp\",\"name\":\"RTMP\",\"encoder\":{\"sink_type\":\"rtmp\",\"rtmp_uri\":\"rtmp://example.com/live\",\"rtmp_passcode\":\"stream-key\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"rtmp_passcode\":") == NULL);
    SBS_ASSERT(strstr(response, "rtmp_passcode_set") != NULL);
    SBS_ASSERT(strstr(response, "stream-key") == NULL);
    free(response);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"output.update\",\"params\":{\"id\":\"output-rtmp\",\"encoder\":{\"sink_type\":\"rtmp\",\"rtmp_uri\":\"rtmp://example.com/live\",\"rtmp_passcode\":\"new-key\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"rtmp_passcode\":") == NULL);
    SBS_ASSERT(strstr(response, "rtmp_passcode_set") != NULL);
    SBS_ASSERT(strstr(response, "new-key") == NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, output_update_restart_failure_marks_error, setup_api, teardown_api)
{
    sbs_output_state_t *output = NULL;
    char *response = NULL;

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"output.create\",\"params\":{\"id\":\"output-fail\",\"name\":\"Output\",\"encoder\":{\"sink_type\":\"fakesink\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    free(response);

    output = sbs_scene_graph_get_output(server->scene_graph, "output-fail");
    SBS_ASSERT_NOT_NULL(output);
    output->running = true;
    g_free(output->runtime_state);
    output->runtime_state = g_strdup("running");

    server->output_sup = (sbs_output_supervisor_t *)0x1;
    test_api_stubs_reset_output_start();
    test_api_stubs_set_output_start_result(-1);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"output.update\",\"params\":{\"id\":\"output-fail\",\"encoder\":{\"sink_type\":\"fakesink\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "\"state\":\"error\"") != NULL);
    free(response);
    server->output_sup = NULL;
}

SBS_TEST_FIXTURE(jsonrpc, obs_style_filters_round_trip, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"source.create\",\"params\":{\"id\":\"source-filter\",\"name\":\"Filter Source\",\"type\":\"videotestsrc\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    free(response);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"filter.add\",\"params\":{\"source_id\":\"source-filter\",\"id\":\"cc\",\"type\":\"color_correction\",\"params\":{\"saturation\":1.25,\"brightness\":0.1,\"contrast\":1.1,\"gamma\":0.95,\"hue\":15}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "color_correction") != NULL);
    SBS_ASSERT(strstr(response, "saturation") != NULL);
    free(response);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"filter.add\",\"params\":{\"source_id\":\"source-filter\",\"id\":\"ck\",\"type\":\"chroma_key\",\"params\":{\"color\":\"#00ff00\",\"similarity\":0.3,\"smoothness\":0.1,\"spill\":0.2}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "chroma_key") != NULL);
    SBS_ASSERT(strstr(response, "#00ff00") != NULL);
    free(response);

    response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":35,\"method\":\"filter.add\",\"params\":{\"source_id\":\"source-filter\",\"id\":\"lk\",\"type\":\"luma_key\",\"params\":{\"min\":0.2,\"max\":0.9,\"smoothness\":0.05}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "luma_key") != NULL);
    SBS_ASSERT(strstr(response, "smoothness") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, unsupported_source_kind_returns_error, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"source.create\",\"params\":{\"id\":\"source-bad\",\"name\":\"Bad\",\"type\":\"not-real\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Unsupported source kind") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_create_rejects_unsafe_id, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"source.create\",\"params\":{\"id\":\"../source\",\"name\":\"Bad\",\"type\":\"videotestsrc\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Unable to create source") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, output_create_rejects_unsafe_id, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"output.create\",\"params\":{\"id\":\"bad/output\",\"name\":\"Bad\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Unable to create output") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, structured_text_source_preserves_font_config, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"source.create\",\"params\":{\"id\":\"source-text\",\"name\":\"Title\",\"type\":\"text\",\"config\":{\"text\":\"Hello\",\"font_family\":\"Liberation Serif\",\"font_path\":\"/tmp/custom.ttf\",\"font_size\":96,\"text_align\":\"center\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "source-text") != NULL);
    SBS_ASSERT(strstr(response, "Liberation Serif") != NULL);
    SBS_ASSERT(strstr(response, "/tmp/custom.ttf") != NULL);
    SBS_ASSERT(strstr(response, "center") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, source_update_unknown_id_returns_error, setup_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"source.update\",\"params\":{\"id\":\"missing\",\"config\":{\"pattern\":\"snow\"}}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Source not found") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, instance_list_returns_inventory, setup_controller_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"instance.list\"}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "instances") != NULL);
    SBS_ASSERT(strstr(response, "instance_id") != NULL);
    SBS_ASSERT(strstr(response, "Default") != NULL);
    SBS_ASSERT(strstr(response, "config_dir") == NULL);
    SBS_ASSERT(strstr(response, "control_socket_path") == NULL);
    SBS_ASSERT(strstr(response, "log_path") == NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, instance_call_requires_instance_id, setup_controller_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"instance.call\",\"params\":{\"method\":\"system.getState\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "instance_id is required") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, instance_create_returns_instance_id, setup_controller_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"instance.create\",\"params\":{\"name\":\"Verifier\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "instance_id") != NULL);
    SBS_ASSERT(strstr(response, "Verifier") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(jsonrpc, instance_update_renames_default_instance, setup_controller_api, teardown_api)
{
    char *response = NULL;
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"instance.update\",\"params\":{\"instance_id\":0,\"name\":\"Studio Main\"}}",
        &response), SBS_OK);
    SBS_ASSERT_NOT_NULL(response);
    SBS_ASSERT(strstr(response, "Studio Main") != NULL);
    free(response);
}

SBS_TEST_MAIN()

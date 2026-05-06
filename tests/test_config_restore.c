#include "sbs_test.h"
#include "sbs/api_server.h"
#include "sbs/config_manager.h"

static sbs_scene_graph_t *graph;
static sbs_api_server_t *server;
static sbs_config_manager_t *mgr;
static char *tmpdir;

void test_api_stubs_reset_last_source_start(void);
const char *test_api_stubs_last_source_output_format(void);
void test_api_stubs_set_source_start_result(int result);
unsigned test_api_stubs_source_start_count(void);
void test_api_stubs_reset_output_start(void);
void test_api_stubs_set_output_start_result(int result);
unsigned test_api_stubs_output_start_count(void);

static void setup_restore(void)
{
    graph = sbs_scene_graph_new_default();
    server = sbs_api_server_new(graph, NULL, NULL, NULL, NULL);
    tmpdir = g_dir_make_tmp("sbs-config-XXXXXX", NULL);
    mgr = sbs_config_manager_new(tmpdir);
    server->config = mgr;
}

static void teardown_restore(void)
{
    if (server) {
        server->config = NULL;
        sbs_api_server_free(server);
        server = NULL;
    }
    if (mgr) {
        sbs_config_manager_free(mgr);
        mgr = NULL;
    }
    g_clear_pointer(&tmpdir, g_free);
    graph = NULL;
}

SBS_TEST_FIXTURE(config_restore, save_load_apply_round_trip, setup_restore, teardown_restore)
{
    sbs_source_create_params_t src = { "src-a", "Source A", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    sbs_scene_create_params_t scene = { "scene-a", "Scene A" };
    sbs_scene_item_create_params_t item = {
        .id = "item-a",
        .source_id = "src-a",
        .visible = true,
        .locked = false,
        .z_order = 0,
        .transform = { .width = 640, .height = 360, .bounds_type = "stretch", .alignment = "center", .opacity = 1.0 }
    };
    cJSON *bundle = NULL;
    cJSON *loaded = NULL;

    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &src, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_add_item(graph, "scene-a", &item, NULL), SBS_OK);
    g_free(graph->active_scene_id);
    graph->active_scene_id = g_strdup("scene-a");

    bundle = sbs_config_manager_build_bundle(server);
    SBS_ASSERT_NOT_NULL(bundle);
    SBS_ASSERT_EQ(sbs_config_manager_save_bundle(mgr, bundle), SBS_OK);
    cJSON_Delete(bundle);

    SBS_ASSERT_EQ(sbs_config_manager_load_bundle(mgr, &loaded), SBS_OK);
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, loaded), SBS_OK);
    SBS_ASSERT_NOT_NULL(sbs_scene_graph_get_source(server->scene_graph, "src-a"));
    SBS_ASSERT_NOT_NULL(sbs_scene_graph_get_scene(server->scene_graph, "scene-a"));
    SBS_ASSERT_STR_EQ(server->scene_graph->active_scene_id, "scene-a");
    cJSON_Delete(loaded);
}

SBS_TEST_FIXTURE(config_restore, reset_restores_default_seed, setup_restore, teardown_restore)
{
    sbs_source_create_params_t src = { "src-extra", "Extra", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &src, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_config_manager_reset(mgr, server), SBS_OK);
    SBS_ASSERT_NOT_NULL(sbs_scene_graph_get_source(server->scene_graph, "default-src"));
    SBS_ASSERT_NOT_NULL(sbs_scene_graph_get_scene(server->scene_graph, "scene-main"));
    SBS_ASSERT_EQ(sbs_scene_graph_get_source(server->scene_graph, "src-extra"), NULL);
}

SBS_TEST_FIXTURE(config_restore, system_override_applies_canvas, setup_restore, teardown_restore)
{
    char *override = g_build_filename(tmpdir, "sbs.conf", NULL);
    uint16_t api_port = 10086;
    SBS_ASSERT_EQ(g_file_set_contents(override,
        "[canvas]\nwidth=1280\nheight=720\nfps_num=25\nbackground_color=#101010\n[server]\napi_port=12000\n[audio]\ndevice=hw:0,2\n",
        -1, NULL), TRUE);
    g_setenv("SBS_SYSTEM_CONFIG", override, TRUE);
    sbs_config_manager_free(mgr);
    mgr = sbs_config_manager_new(tmpdir);
    server->config = mgr;
    sbs_source_create_params_t src = { "default-src", "Default", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    sbs_scene_graph_create_source(graph, &src, NULL);
    SBS_ASSERT_EQ(sbs_config_manager_apply_system_overrides(mgr, graph, &api_port), SBS_OK);
    SBS_ASSERT_EQ(graph->canvas.width, 1280);
    SBS_ASSERT_EQ(graph->canvas.height, 720);
    SBS_ASSERT_EQ(graph->canvas.fps_num, 25);
    SBS_ASSERT_STR_EQ(graph->canvas.background_color, "#101010");
    SBS_ASSERT_EQ(api_port, 12000);
    g_unsetenv("SBS_SYSTEM_CONFIG");
    g_free(override);
}

SBS_TEST_FIXTURE(config_restore, older_schema_bundle_migrates, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *canvas = cJSON_CreateObject();
    cJSON_AddNumberToObject(bundle, "schema_version", 0);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddNumberToObject(canvas, "width", 1920);
    cJSON_AddNumberToObject(canvas, "height", 1080);
    cJSON_AddNumberToObject(canvas, "fps_num", 30);
    cJSON_AddNumberToObject(canvas, "fps_den", 1);
    cJSON_AddStringToObject(canvas, "color_mode", "sdr");
    cJSON_AddStringToObject(canvas, "background_color", "#000000");
    cJSON_AddItemToObject(state, "canvas", canvas);
    cJSON_AddItemToObject(state, "sources", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "output_groups", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "transitions", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "state", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_OK);
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, vfmcap_restore_preserves_output_format, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *canvas = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *source = cJSON_CreateObject();
    cJSON *config = cJSON_CreateObject();

    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddNumberToObject(canvas, "width", 3840);
    cJSON_AddNumberToObject(canvas, "height", 2160);
    cJSON_AddNumberToObject(canvas, "fps_num", 60);
    cJSON_AddNumberToObject(canvas, "fps_den", 1);
    cJSON_AddStringToObject(canvas, "color_mode", "hdr10");
    cJSON_AddStringToObject(canvas, "background_color", "#000000");
    cJSON_AddStringToObject(source, "type", "vfmcap");
    cJSON_AddBoolToObject(source, "enabled", true);
    cJSON_AddStringToObject(config, "output_format", "nv12");
    cJSON_AddItemToObject(source, "config", config);
    cJSON_AddItemToObject(sources, "cap", source);
    cJSON_AddItemToObject(state, "canvas", canvas);
    cJSON_AddItemToObject(state, "sources", sources);
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "output_groups", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "transitions", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "state", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);

    server->source_sup = (sbs_source_supervisor_t *)0x1;
    test_api_stubs_reset_last_source_start();
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_OK);
    SBS_ASSERT_STR_EQ(test_api_stubs_last_source_output_format(), "nv12");
    SBS_ASSERT_EQ(test_api_stubs_source_start_count(), 1u);
    server->source_sup = NULL;
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, source_restore_start_failure_marks_error, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *canvas = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *source = cJSON_CreateObject();
    sbs_source_state_t *restored = NULL;

    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddNumberToObject(canvas, "width", 1920);
    cJSON_AddNumberToObject(canvas, "height", 1080);
    cJSON_AddNumberToObject(canvas, "fps_num", 60);
    cJSON_AddNumberToObject(canvas, "fps_den", 1);
    cJSON_AddStringToObject(canvas, "color_mode", "sdr");
    cJSON_AddStringToObject(canvas, "background_color", "#000000");
    cJSON_AddStringToObject(source, "type", "videotestsrc");
    cJSON_AddBoolToObject(source, "enabled", true);
    cJSON_AddItemToObject(sources, "src", source);
    cJSON_AddItemToObject(state, "canvas", canvas);
    cJSON_AddItemToObject(state, "sources", sources);
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "output_groups", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "transitions", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "state", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);

    server->source_sup = (sbs_source_supervisor_t *)0x1;
    test_api_stubs_reset_last_source_start();
    test_api_stubs_set_source_start_result(-1);
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_OK);
    restored = sbs_scene_graph_get_source(server->scene_graph, "src");
    SBS_ASSERT_NOT_NULL(restored);
    SBS_ASSERT_EQ(restored->running, false);
    SBS_ASSERT_STR_EQ(restored->runtime_state, "error");
    SBS_ASSERT_EQ(test_api_stubs_source_start_count(), 1u);
    server->source_sup = NULL;
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, output_restore_preserves_encoder_config, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *outputs = cJSON_CreateObject();
    cJSON *output = cJSON_CreateObject();
    cJSON *encoder = cJSON_CreateObject();
    sbs_output_state_t *restored = NULL;

    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddStringToObject(output, "name", "Output");
    cJSON_AddBoolToObject(output, "enabled", true);
    cJSON_AddBoolToObject(output, "autostart", false);
    cJSON_AddStringToObject(encoder, "sink_type", "fakesink");
    cJSON_AddStringToObject(encoder, "bitrate_kbps", "1234");
    cJSON_AddItemToObject(output, "encoder", encoder);
    cJSON_AddItemToObject(outputs, "out", output);
    cJSON_AddItemToObject(state, "sources", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "output_groups", outputs);
    cJSON_AddItemToObject(state, "transitions", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "state", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);

    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_OK);
    restored = sbs_scene_graph_get_output(server->scene_graph, "out");
    SBS_ASSERT_NOT_NULL(restored);
    SBS_ASSERT_STR_EQ(g_hash_table_lookup(restored->encoder, "sink_type"), "fakesink");
    SBS_ASSERT_STR_EQ(g_hash_table_lookup(restored->encoder, "bitrate_kbps"), "1234");
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, output_restore_start_failure_marks_error, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *outputs = cJSON_CreateObject();
    cJSON *output = cJSON_CreateObject();
    cJSON *encoder = cJSON_CreateObject();
    sbs_output_state_t *restored = NULL;

    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddStringToObject(output, "name", "Output");
    cJSON_AddBoolToObject(output, "enabled", true);
    cJSON_AddBoolToObject(output, "autostart", true);
    cJSON_AddStringToObject(encoder, "sink_type", "fakesink");
    cJSON_AddItemToObject(output, "encoder", encoder);
    cJSON_AddItemToObject(outputs, "out", output);
    cJSON_AddItemToObject(state, "sources", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "output_groups", outputs);
    cJSON_AddItemToObject(state, "transitions", cJSON_CreateObject());
    cJSON_AddItemToObject(state, "state", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);

    server->output_sup = (sbs_output_supervisor_t *)0x1;
    test_api_stubs_reset_output_start();
    test_api_stubs_set_output_start_result(-1);
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_OK);
    restored = sbs_scene_graph_get_output(server->scene_graph, "out");
    SBS_ASSERT_NOT_NULL(restored);
    SBS_ASSERT_EQ(restored->running, false);
    SBS_ASSERT_STR_EQ(restored->runtime_state, "error");
    SBS_ASSERT_EQ(test_api_stubs_output_start_count(), 1u);
    server->output_sup = NULL;
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, invalid_bundle_rejected, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_ERR_INVAL);
    cJSON_Delete(bundle);
}

SBS_TEST_FIXTURE(config_restore, unknown_source_type_rejected, setup_restore, teardown_restore)
{
    sbs_source_create_params_t existing = {
        .id = "src-existing",
        .name = "Existing",
        .kind = SBS_SOURCE_KIND_VIDEOTESTSRC,
        .enabled = true,
    };
    cJSON *bundle = cJSON_CreateObject();
    cJSON *state = cJSON_CreateObject();
    cJSON *sources = cJSON_CreateObject();
    cJSON *bad_source = cJSON_CreateObject();

    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &existing, NULL), SBS_OK);

    cJSON_AddNumberToObject(bundle, "schema_version", 1);
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    cJSON_AddStringToObject(bad_source, "type", "mysterysrc");
    cJSON_AddItemToObject(sources, "src-bad", bad_source);
    cJSON_AddItemToObject(state, "sources", sources);
    cJSON_AddItemToObject(state, "scenes", cJSON_CreateObject());
    cJSON_AddItemToObject(bundle, "state", state);

    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_ERR_INVAL);
    SBS_ASSERT_NOT_NULL(sbs_scene_graph_get_source(server->scene_graph, "src-existing"));
    SBS_ASSERT_EQ(sbs_scene_graph_get_source(server->scene_graph, "src-bad"), NULL);
    cJSON_Delete(bundle);
}

SBS_TEST_MAIN()

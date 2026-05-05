#include "sbs_test.h"
#include "sbs/api_server.h"
#include "sbs/config_manager.h"

static sbs_scene_graph_t *graph;
static sbs_api_server_t *server;
static sbs_config_manager_t *mgr;
static char *tmpdir;

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

SBS_TEST_FIXTURE(config_restore, invalid_bundle_rejected, setup_restore, teardown_restore)
{
    cJSON *bundle = cJSON_CreateObject();
    cJSON_AddStringToObject(bundle, "kind", "sbs-config");
    SBS_ASSERT_EQ(sbs_config_manager_apply_bundle(mgr, server, bundle), SBS_ERR_INVAL);
    cJSON_Delete(bundle);
}

SBS_TEST_MAIN()

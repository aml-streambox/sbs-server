#include "sbs_test.h"
#include "sbs/scene_graph.h"
#include "cjson/cJSON.h"

static sbs_scene_graph_t *graph;

static void setup_graph(void)
{
    graph = sbs_scene_graph_new_default();
}

static void teardown_graph(void)
{
    sbs_scene_graph_free(graph);
    graph = NULL;
}

SBS_TEST_FIXTURE(scene_graph, create_scene_sets_active_when_first, setup_graph, teardown_graph)
{
    sbs_scene_create_params_t scene = { "scene-main", "Main" };
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene, NULL), SBS_OK);
    SBS_ASSERT_STR_EQ(graph->active_scene_id, "scene-main");
}

SBS_TEST_FIXTURE(scene_graph, active_scene_cannot_be_deleted, setup_graph, teardown_graph)
{
    sbs_scene_create_params_t scene = { "scene-main", "Main" };
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_remove_scene(graph, "scene-main"), SBS_ERR_INVAL);
}

SBS_TEST_FIXTURE(scene_graph, add_and_reorder_items, setup_graph, teardown_graph)
{
    sbs_source_create_params_t src_a = { "src-a", "A", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    sbs_source_create_params_t src_b = { "src-b", "B", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    sbs_scene_create_params_t scene = { "scene-main", "Main" };
    sbs_scene_item_create_params_t item_a = {
        .id = "item-a", .source_id = "src-a", .visible = true, .locked = false, .z_order = 20,
        .transform = { .width = 1920, .height = 1080, .bounds_type = "stretch", .alignment = "center", .opacity = 1.0 }
    };
    sbs_scene_item_create_params_t item_b = {
        .id = "item-b", .source_id = "src-b", .visible = true, .locked = false, .z_order = 10,
        .transform = { .width = 640, .height = 360, .bounds_type = "fit", .alignment = "center", .opacity = 1.0 }
    };
    const char *order[] = { "item-a", "item-b" };
    sbs_scene_state_t *scene_state;

    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &src_a, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &src_b, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene, &scene_state), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_add_item(graph, "scene-main", &item_a, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_add_item(graph, "scene-main", &item_b, NULL), SBS_OK);
    SBS_ASSERT_STR_EQ(((sbs_scene_item_state_t *)g_ptr_array_index(scene_state->items, 0))->id, "item-b");

    SBS_ASSERT_EQ(sbs_scene_graph_reorder_items(graph, "scene-main", order, 2), SBS_OK);
    SBS_ASSERT_STR_EQ(((sbs_scene_item_state_t *)g_ptr_array_index(scene_state->items, 0))->id, "item-a");
}

SBS_TEST_FIXTURE(scene_graph, transition_runtime_updates_progress, setup_graph, teardown_graph)
{
    sbs_scene_create_params_t scene_a = { "scene-a", "A" };
    sbs_scene_create_params_t scene_b = { "scene-b", "B" };

    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene_a, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene_b, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_set_active_scene(graph, "scene-b", "trans-fade"), SBS_OK);
    SBS_ASSERT(graph->transition_runtime.active);

    sbs_comp_scene_state_t comp_state;
    SBS_ASSERT_EQ(sbs_scene_graph_build_compositor_state(graph, &comp_state), SBS_OK);
    SBS_ASSERT(comp_state.transition_active);
    SBS_ASSERT_EQ((int)comp_state.transition_duration_ms, 2000);
    SBS_ASSERT(comp_state.transition_start_time_us > 0);

    sbs_scene_graph_update_transition_runtime(graph,
        graph->transition_runtime.start_time_us + 2100000);
    SBS_ASSERT(!graph->transition_runtime.active);
    SBS_ASSERT_GE(graph->transition_runtime.progress, 1.0);
}

SBS_TEST_FIXTURE(scene_graph, serialize_full_state_contains_scene_and_source, setup_graph, teardown_graph)
{
    sbs_source_create_params_t source = { "src-a", "A", SBS_SOURCE_KIND_VIDEOTESTSRC, true, false };
    sbs_scene_create_params_t scene = { "scene-main", "Main" };
    cJSON *json;
    char *text;

    SBS_ASSERT_EQ(sbs_scene_graph_create_source(graph, &source, NULL), SBS_OK);
    SBS_ASSERT_EQ(sbs_scene_graph_create_scene(graph, &scene, NULL), SBS_OK);

    json = sbs_scene_graph_serialize_full_state(graph);
    SBS_ASSERT_NOT_NULL(json);
    text = cJSON_PrintUnformatted(json);
    SBS_ASSERT_NOT_NULL(text);
    SBS_ASSERT(strstr(text, "scene-main") != NULL);
    SBS_ASSERT(strstr(text, "src-a") != NULL);
    free(text);
    cJSON_Delete(json);
}

SBS_TEST_MAIN()

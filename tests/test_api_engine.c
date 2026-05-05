#include "sbs_test.h"
#include "sbs/api_server.h"

static sbs_scene_graph_t *graph;
static sbs_api_server_t *server;
static sbs_api_client_t *client;

static void setup_api(void)
{
    graph = sbs_scene_graph_new_default();
    server = sbs_api_server_new(graph, NULL, NULL, NULL, NULL);
    server->config = sbs_config_manager_new("/tmp/sbs-test-config-engine");
    client = sbs_api_client_new(1);
    sbs_api_server_add_client(server, client);
}

static void teardown_api(void)
{
    sbs_api_server_free(server);
    sbs_scene_graph_free(graph);
    server = NULL;
    graph = NULL;
    client = NULL;
}

SBS_TEST_FIXTURE(api_engine, create_source_scene_and_item_round_trip, setup_api, teardown_api)
{
    char *response = NULL;

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"source.create\",\"params\":{\"id\":\"src-a\",\"name\":\"Source A\",\"type\":\"videotestsrc\"}}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "src-a") != NULL);
    free(response);

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"scene.create\",\"params\":{\"id\":\"scene-a\",\"name\":\"Scene A\"}}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "scene-a") != NULL);
    free(response);

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"scene.item.add\",\"params\":{\"scene_id\":\"scene-a\",\"id\":\"item-a\",\"source_id\":\"src-a\",\"z_order\":0,\"visible\":true,\"transform\":{\"position_x\":0,\"position_y\":0,\"width\":640,\"height\":360,\"bounds_type\":\"stretch\",\"alignment\":\"center\",\"opacity\":1.0}}}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "item-a") != NULL);
    free(response);

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"system.getState\"}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "scene-a") != NULL);
    SBS_ASSERT(strstr(response, "src-a") != NULL);
    SBS_ASSERT(strstr(response, "item-a") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(api_engine, audio_source_update_round_trip, setup_api, teardown_api)
{
    char *response = NULL;

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"source.create\",\"params\":{\"id\":\"src-audio\",\"name\":\"Audio Source\",\"type\":\"videotestsrc\"}}",
        &response), SBS_OK);
    free(response);

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"audio.setSource\",\"params\":{\"source_id\":\"src-audio\",\"volume\":0.5,\"mute\":false,\"monitor\":true}}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "src-audio") != NULL);
    free(response);
}

SBS_TEST_FIXTURE(api_engine, preview_profile_catalog_and_ensure_round_trip, setup_api, teardown_api)
{
    char *response = NULL;
    cJSON *json;
    cJSON *result;
    char request[1024];
    const char *profile_id = "program-hevc-srt";

    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client,
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"preview.listProfiles\"}",
        &response), SBS_OK);
    SBS_ASSERT(strstr(response, "program-hevc-srt") != NULL);
    json = cJSON_Parse(response);
    SBS_ASSERT_NOT_NULL(json);
    result = cJSON_GetObjectItemCaseSensitive(json, "result");
    SBS_ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(result, "available_profiles")));
    SBS_ASSERT(cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(result, "requestable_profiles")));
    free(response);

    snprintf(request, sizeof(request),
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"preview.ensureProfile\",\"params\":{\"profile_id\":\"%s\"}}",
        profile_id);
    SBS_ASSERT_EQ(sbs_api_server_dispatch_json(server, client, request, &response), SBS_OK);
    SBS_ASSERT(strstr(response, "active") != NULL);
    free(response);
    cJSON_Delete(json);
}

SBS_TEST_MAIN()

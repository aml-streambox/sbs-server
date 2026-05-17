#define SBS_LOG_COMP "api-scene"

#include "sbs/api_server.h"

#include <math.h>

static cJSON *api_error(int code, const char *message)
{
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return err;
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? cJSON_GetStringValue(item) : NULL;
}

static int json_int(cJSON *obj, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static double json_double(cJSON *obj, const char *key, double fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

static bool json_bool(cJSON *obj, const char *key, bool fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;
    return fallback;
}

static double snap_quarter_rotation(double degrees)
{
    int quarter = (int)lround(degrees / 90.0);
    quarter = ((quarter % 4) + 4) % 4;
    return (double)(quarter * 90);
}

static void parse_transform(cJSON *json, sbs_scene_item_transform_t *transform)
{
    memset(transform, 0, sizeof(*transform));
    transform->position_x = json_int(json, "position_x", 0);
    transform->position_y = json_int(json, "position_y", 0);
    transform->width = json_int(json, "width", 640);
    transform->height = json_int(json, "height", 360);
    transform->crop_top = json_int(json, "crop_top", 0);
    transform->crop_bottom = json_int(json, "crop_bottom", 0);
    transform->crop_left = json_int(json, "crop_left", 0);
    transform->crop_right = json_int(json, "crop_right", 0);
    transform->rotation_deg = snap_quarter_rotation(json_double(json, "rotation_deg", 0.0));
    transform->flip_horizontal = json_bool(json, "flip_horizontal", false);
    transform->flip_vertical = json_bool(json, "flip_vertical", false);
    transform->bounds_type = (char *)(json_str(json, "bounds_type") ? json_str(json, "bounds_type") : "stretch");
    transform->alignment = (char *)(json_str(json, "alignment") ? json_str(json, "alignment") : "center");
    transform->opacity = json_double(json, "opacity", 1.0);
}

static void publish_scene_event(sbs_api_server_t *server, const char *topic, cJSON *payload)
{
    sbs_api_server_publish(server, topic, payload);
}

static cJSON *serialize_item_payload(const sbs_scene_item_state_t *item)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *filters = cJSON_CreateArray();
    guint i;
    if (!item) {
        cJSON_AddItemToObject(obj, "filters", filters);
        return obj;
    }
    cJSON_AddStringToObject(obj, "id", item->id);
    cJSON_AddStringToObject(obj, "source_id", item->source_id);
    cJSON_AddBoolToObject(obj, "visible", item->visible);
    cJSON_AddBoolToObject(obj, "locked", item->locked);
    cJSON_AddNumberToObject(obj, "z_order", item->z_order);
    for (i = 0; i < item->filters->len; i++) {
        sbs_filter_state_t *filter = g_ptr_array_index(item->filters, i);
        cJSON *filter_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(filter_obj, "id", filter->id);
        cJSON_AddStringToObject(filter_obj, "type", filter->type);
        cJSON_AddBoolToObject(filter_obj, "enabled", filter->enabled);
        cJSON_AddItemToObject(filter_obj, "params", cJSON_CreateObject());
        cJSON_AddItemToArray(filters, filter_obj);
    }
    cJSON_AddItemToObject(obj, "filters", filters);
    return obj;
}

static GHashTable *parse_params_object(cJSON *obj)
{
    GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    cJSON *child;
    if (!cJSON_IsObject(obj)) {
        return map;
    }
    child = obj->child;
    while (child) {
        if (child->string) {
            if (cJSON_IsString(child)) {
                g_hash_table_insert(map, g_strdup(child->string), g_strdup(cJSON_GetStringValue(child)));
            } else if (cJSON_IsNumber(child)) {
                char buf[64];
                g_snprintf(buf, sizeof(buf), "%.4f", child->valuedouble);
                g_hash_table_insert(map, g_strdup(child->string), g_strdup(buf));
            }
        }
        child = child->next;
    }
    return map;
}

static const char *resolve_filter_source_id(sbs_scene_graph_t *graph, cJSON *params)
{
    const char *source_id = json_str(params, "source_id");
    const char *scene_id;
    const char *item_id;
    sbs_scene_state_t *scene;

    if (source_id) {
        return source_id;
    }

    scene_id = json_str(params, "scene_id");
    item_id = json_str(params, "item_id");
    scene = sbs_scene_graph_get_scene(graph, scene_id);
    if (!scene || !item_id) {
        return NULL;
    }

    for (guint i = 0; i < scene->items->len; i++) {
        sbs_scene_item_state_t *item = g_ptr_array_index(scene->items, i);
        if (g_strcmp0(item->id, item_id) == 0) {
            return item->source_id;
        }
    }

    return NULL;
}

int sbs_api_handle_scene_list(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    GHashTableIter iter;
    gpointer key, value;
    (void)client; (void)params; (void)error;
    *result = cJSON_CreateObject();
    cJSON *scenes = cJSON_CreateObject();
    g_hash_table_iter_init(&iter, server->scene_graph->scenes);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        cJSON_AddItemToObject(scenes, key, sbs_scene_graph_serialize_scene(value));
    }
    cJSON_AddItemToObject(*result, "scenes", scenes);
    return SBS_OK;
}

int sbs_api_handle_scene_get(sbs_api_server_t *server, sbs_api_client_t *client,
                             cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_state_t *scene = sbs_scene_graph_get_scene(server->scene_graph, json_str(params, "id"));
    (void)client;
    if (!scene) {
        *error = api_error(-32001, "Scene not found");
        return SBS_ERR_NOT_FOUND;
    }
    *result = sbs_scene_graph_serialize_scene(scene);
    return SBS_OK;
}

int sbs_api_handle_scene_create(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_create_params_t create = { json_str(params, "id"), json_str(params, "name") };
    sbs_scene_state_t *scene = NULL;
    int rc;
    (void)client;
    rc = sbs_scene_graph_create_scene(server->scene_graph, &create, &scene);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to create scene");
        return rc;
    }
    publish_scene_event(server, "scene.created", sbs_scene_graph_serialize_scene(scene));
    sbs_api_server_refresh_scene(server);
    *result = sbs_scene_graph_serialize_scene(scene);
    return SBS_OK;
}

int sbs_api_handle_scene_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    sbs_scene_state_t *scene;
    (void)client;
    rc = sbs_scene_graph_update_scene_name(server->scene_graph, json_str(params, "id"), json_str(params, "name"));
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Scene not found");
        return rc;
    }
    scene = sbs_scene_graph_get_scene(server->scene_graph, json_str(params, "id"));
    publish_scene_event(server, "scene.updated", sbs_scene_graph_serialize_scene(scene));
    *result = sbs_scene_graph_serialize_scene(scene);
    return SBS_OK;
}

int sbs_api_handle_scene_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_scene_graph_remove_scene(server->scene_graph, json_str(params, "id"));
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to remove scene");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}

int sbs_api_handle_scene_set_active(sbs_api_server_t *server, sbs_api_client_t *client,
                                    cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_scene_graph_set_active_scene(server->scene_graph,
                                          json_str(params, "scene_id"),
                                          json_str(params, "transition_id"));
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to activate scene");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "active_scene_id", server->scene_graph->active_scene_id);
    cJSON_AddBoolToObject(*result, "transition_active", server->scene_graph->transition_runtime.active);
    publish_scene_event(server, "scene.changed", cJSON_CreateObject());
    return SBS_OK;
}

int sbs_api_handle_scene_set_preview(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_scene_graph_set_preview_scene(server->scene_graph, json_str(params, "scene_id"));
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to set preview scene");
        return rc;
    }
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "preview_scene_id", server->scene_graph->preview_scene_id);
    return SBS_OK;
}

int sbs_api_handle_scene_transition_to_preview(sbs_api_server_t *server, sbs_api_client_t *client,
                                               cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    (void)client;
    rc = sbs_scene_graph_transition_to_preview(server->scene_graph, json_str(params, "transition_id"));
    if (rc != SBS_OK) {
        *error = api_error(-32003, "Unable to transition to preview");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    *result = cJSON_CreateObject();
    cJSON_AddStringToObject(*result, "active_scene_id", server->scene_graph->active_scene_id);
    return SBS_OK;
}

int sbs_api_handle_scene_transition_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                           cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    sbs_transition_state_t *transition = NULL;
    int duration_param;
    uint32_t duration_ms;
    (void)client;

    duration_param = json_int(params, "duration_ms", -1);
    if (duration_param < 0) {
        *error = api_error(-32602, "Invalid transition duration");
        return SBS_ERR_INVAL;
    }
    duration_ms = (uint32_t)duration_param;
    rc = sbs_scene_graph_update_transition(server->scene_graph,
                                           json_str(params, "transition_id"),
                                           duration_ms,
                                           &transition);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to update transition");
        return rc;
    }
    *result = sbs_scene_graph_serialize_transition(transition);
    publish_scene_event(server, "scene.transition.updated",
                        sbs_scene_graph_serialize_transition(transition));
    return SBS_OK;
}

int sbs_api_handle_scene_item_add(sbs_api_server_t *server, sbs_api_client_t *client,
                                  cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_item_create_params_t create;
    sbs_scene_item_state_t *item = NULL;
    int rc;
    cJSON *transform = cJSON_GetObjectItemCaseSensitive(params, "transform");
    (void)client;

    memset(&create, 0, sizeof(create));
    create.id = json_str(params, "id");
    create.source_id = json_str(params, "source_id");
    create.visible = json_bool(params, "visible", true);
    create.locked = json_bool(params, "locked", false);
    create.z_order = json_int(params, "z_order", 0);
    parse_transform(transform, &create.transform);

    rc = sbs_scene_graph_add_item(server->scene_graph, json_str(params, "scene_id"), &create, &item);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to add item");
        return rc;
    }

    sbs_source_state_t *source = sbs_scene_graph_get_source(server->scene_graph, create.source_id);
    if (source) {
        if (!source->running) {
            cJSON *start_params = cJSON_CreateObject();
            cJSON_AddStringToObject(start_params, "id", source->id);
            cJSON *start_result = NULL;
            cJSON *start_error = NULL;
            sbs_api_handle_source_start(server, NULL, start_params, &start_result, &start_error);
            cJSON_Delete(start_params);
            if (start_result) cJSON_Delete(start_result);
            if (start_error) cJSON_Delete(start_error);
        } else if (source->muted) {
            source->muted = false;
            sbs_source_supervisor_unmute_source(server->source_sup, source->id);
            sbs_api_server_publish(server, "source.status", sbs_scene_graph_serialize_source(source));
        }
    }

    sbs_api_server_refresh_scene(server);
    *result = serialize_item_payload(item);
    publish_scene_event(server, "scene.item.added", serialize_item_payload(item));
    return SBS_OK;
}

int sbs_api_handle_scene_item_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    sbs_scene_item_update_params_t update;
    sbs_scene_item_state_t *item = NULL;
    cJSON *transform = cJSON_GetObjectItemCaseSensitive(params, "transform");
    int rc;
    (void)client;

    memset(&update, 0, sizeof(update));
    update.set_visible = cJSON_GetObjectItemCaseSensitive(params, "visible") != NULL;
    update.visible = json_bool(params, "visible", true);
    update.set_locked = cJSON_GetObjectItemCaseSensitive(params, "locked") != NULL;
    update.locked = json_bool(params, "locked", false);
    update.set_z_order = cJSON_GetObjectItemCaseSensitive(params, "z_order") != NULL;
    update.z_order = json_int(params, "z_order", 0);
    update.set_transform = transform != NULL;
    if (transform) {
        parse_transform(transform, &update.transform);
    }

    rc = sbs_scene_graph_update_item(server->scene_graph,
                                     json_str(params, "scene_id"),
                                     json_str(params, "item_id"),
                                     &update, &item);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to update item");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    *result = serialize_item_payload(item);
    publish_scene_event(server, "scene.item.updated", serialize_item_payload(item));
    return SBS_OK;
}

int sbs_api_handle_scene_item_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                     cJSON *params, cJSON **result, cJSON **error)
{
    const char *scene_id = json_str(params, "scene_id");
    const char *item_id = json_str(params, "item_id");
    sbs_scene_state_t *scene;
    char *source_id = NULL;
    int rc;
    (void)client;

    scene = sbs_scene_graph_get_scene(server->scene_graph, scene_id);
    if (!scene) {
        *error = api_error(-32001, "Scene not found");
        return SBS_ERR_NOT_FOUND;
    }

    for (guint i = 0; i < scene->items->len; i++) {
        sbs_scene_item_state_t *it = g_ptr_array_index(scene->items, i);
        if (g_strcmp0(it->id, item_id) == 0) {
            source_id = g_strdup(it->source_id);
            break;
        }
    }

    rc = sbs_scene_graph_remove_item(server->scene_graph, scene_id, item_id);
    if (rc != SBS_OK) {
        g_free(source_id);
        *error = api_error(-32001, "Unable to remove item");
        return rc;
    }

    if (source_id) {
        sbs_source_state_t *source = sbs_scene_graph_get_source(server->scene_graph, source_id);
        if (source && source->running && !source->keep_alive) {
            int refs = sbs_scene_graph_count_source_refs(server->scene_graph, source_id);
            if (refs == 0 && !source->muted) {
                source->muted = true;
                sbs_source_supervisor_mute_source(server->source_sup, source_id);
                sbs_api_server_publish(server, "source.status",
                                       sbs_scene_graph_serialize_source(source));
            }
        }
        g_free(source_id);
    }

    sbs_api_server_refresh_scene(server);
    *result = cJSON_CreateObject();
    cJSON_AddBoolToObject(*result, "ok", 1);
    return SBS_OK;
}

int sbs_api_handle_scene_item_reorder(sbs_api_server_t *server, sbs_api_client_t *client,
                                      cJSON *params, cJSON **result, cJSON **error)
{
    cJSON *order = cJSON_GetObjectItemCaseSensitive(params, "item_order");
    const char **ids;
    int count;
    int i;
    int rc;
    sbs_scene_state_t *scene;
    (void)client;

    if (!cJSON_IsArray(order)) {
        *error = api_error(-32602, "item_order must be an array");
        return SBS_ERR_INVAL;
    }
    count = cJSON_GetArraySize(order);
    ids = g_new0(const char *, count);
    for (i = 0; i < count; i++) {
        ids[i] = cJSON_GetStringValue(cJSON_GetArrayItem(order, i));
    }
    rc = sbs_scene_graph_reorder_items(server->scene_graph, json_str(params, "scene_id"), ids, (size_t)count);
    g_free(ids);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to reorder items");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    scene = sbs_scene_graph_get_scene(server->scene_graph, json_str(params, "scene_id"));
    *result = sbs_scene_graph_serialize_scene(scene);
    return SBS_OK;
}

int sbs_api_handle_filter_add(sbs_api_server_t *server, sbs_api_client_t *client,
                              cJSON *params, cJSON **result, cJSON **error)
{
    sbs_filter_create_params_t create = {0};
    const char *scene_id;
    const char *source_id;
    sbs_source_state_t *source;
    sbs_scene_state_t *scene;
    int rc;
    (void)client;
    scene_id = json_str(params, "scene_id");
    source_id = resolve_filter_source_id(server->scene_graph, params);
    create.id = json_str(params, "id");
    create.type = json_str(params, "type");
    create.enabled = json_bool(params, "enabled", true);
    create.params = parse_params_object(cJSON_GetObjectItemCaseSensitive(params, "params"));
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        rc = sbs_scene_graph_add_scene_filter(server->scene_graph,
                                              scene_id,
                                              &create, NULL);
    } else {
        rc = sbs_scene_graph_add_filter(server->scene_graph,
                                        source_id,
                                        &create, NULL);
    }
    g_hash_table_destroy(create.params);
    if (rc != SBS_OK) {
        *error = api_error(-32005, "Unable to add filter");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        scene = sbs_scene_graph_get_scene(server->scene_graph, scene_id);
        *result = sbs_scene_graph_serialize_scene(scene);
        publish_scene_event(server, "filter.added", sbs_scene_graph_serialize_scene(scene));
    } else {
        source = sbs_scene_graph_get_source(server->scene_graph, source_id);
        *result = sbs_scene_graph_serialize_source(source);
        publish_scene_event(server, "filter.added", sbs_scene_graph_serialize_source(source));
    }
    return SBS_OK;
}

int sbs_api_handle_filter_update(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    sbs_filter_update_params_t update = {0};
    const char *scene_id;
    const char *source_id;
    sbs_source_state_t *source;
    sbs_scene_state_t *scene;
    int rc;
    (void)client;
    scene_id = json_str(params, "scene_id");
    source_id = resolve_filter_source_id(server->scene_graph, params);
    update.set_enabled = cJSON_GetObjectItemCaseSensitive(params, "enabled") != NULL;
    update.enabled = json_bool(params, "enabled", true);
    update.params = parse_params_object(cJSON_GetObjectItemCaseSensitive(params, "params"));
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        rc = sbs_scene_graph_update_scene_filter(server->scene_graph,
                                                 scene_id,
                                                 json_str(params, "filter_id"),
                                                 &update, NULL);
    } else {
        rc = sbs_scene_graph_update_filter(server->scene_graph,
                                           source_id,
                                           json_str(params, "filter_id"),
                                           &update, NULL);
    }
    g_hash_table_destroy(update.params);
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to update filter");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        scene = sbs_scene_graph_get_scene(server->scene_graph, scene_id);
        *result = sbs_scene_graph_serialize_scene(scene);
        publish_scene_event(server, "filter.updated", sbs_scene_graph_serialize_scene(scene));
    } else {
        source = sbs_scene_graph_get_source(server->scene_graph, source_id);
        *result = sbs_scene_graph_serialize_source(source);
        publish_scene_event(server, "filter.updated", sbs_scene_graph_serialize_source(source));
    }
    return SBS_OK;
}

int sbs_api_handle_filter_remove(sbs_api_server_t *server, sbs_api_client_t *client,
                                 cJSON *params, cJSON **result, cJSON **error)
{
    int rc;
    const char *scene_id;
    const char *source_id;
    sbs_source_state_t *source;
    sbs_scene_state_t *scene;
    (void)client;
    scene_id = json_str(params, "scene_id");
    source_id = resolve_filter_source_id(server->scene_graph, params);
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        rc = sbs_scene_graph_remove_scene_filter(server->scene_graph,
                                                 scene_id,
                                                 json_str(params, "filter_id"));
    } else {
        rc = sbs_scene_graph_remove_filter(server->scene_graph,
                                           source_id,
                                           json_str(params, "filter_id"));
    }
    if (rc != SBS_OK) {
        *error = api_error(-32001, "Unable to remove filter");
        return rc;
    }
    sbs_api_server_refresh_scene(server);
    if (scene_id && !source_id && !json_str(params, "item_id")) {
        scene = sbs_scene_graph_get_scene(server->scene_graph, scene_id);
        *result = sbs_scene_graph_serialize_scene(scene);
        publish_scene_event(server, "filter.removed", sbs_scene_graph_serialize_scene(scene));
    } else {
        source = sbs_scene_graph_get_source(server->scene_graph, source_id);
        *result = sbs_scene_graph_serialize_source(source);
        publish_scene_event(server, "filter.removed", sbs_scene_graph_serialize_source(source));
    }
    return SBS_OK;
}

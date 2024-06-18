/*
 * Copyright (c) The ThingSet Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <thingset.h>

#include "thingset_internal.h"

#include <zephyr/logging/log.h>
#include <zephyr/toolchain/common.h>

#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(thingset, CONFIG_THINGSET_LOG_LEVEL);

/* pointers to iterable section data */
#ifdef STRUCT_SECTION_START_EXTERN
STRUCT_SECTION_START_EXTERN(thingset_data_object);
STRUCT_SECTION_END_EXTERN(thingset_data_object);
#else
/* Zephyr versions below v3.4 are missing some macros... */
extern struct thingset_data_object _thingset_data_object_list_start[];
extern struct thingset_data_object _thingset_data_object_list_end[];
#define TYPE_SECTION_START(secname) _CONCAT(_##secname, _list_start)
#endif /* STRUCT_SECTION_START_EXTERN */

/* dummy objects to avoid using NULL pointer for root object or _Paths/_Metadata overlays */
static struct thingset_data_object root_object = THINGSET_GROUP(0, 0, "", NULL);
static struct thingset_data_object paths_object =
    THINGSET_GROUP(0, THINGSET_ID_PATHS, "_Paths", NULL);
#ifdef CONFIG_THINGSET_METADATA_ENDPOINT
static struct thingset_data_object metadata_object =
    THINGSET_GROUP(0, THINGSET_ID_METADATA, "_Metadata", NULL);
#endif

static char *type_name_lookup[THINGSET_TYPE_FN_I32 + 1] = {
    "bool",   "u8",    "i8",     "u16",     "i16",      "u32",    "i32",
    "u64",    "i64",   "f32",    "decimal", "string",   "buffer", "array",
    "record", "group", "subset", "()->()",  "()->(i32)"
};

static void check_id_duplicates(const struct thingset_data_object *objects, size_t num)
{
    for (unsigned int i = 0; i < num; i++) {
        for (unsigned int j = i + 1; j < num; j++) {
            if (objects[i].id == objects[j].id) {
                LOG_ERR("Duplicate data object ID 0x%X.\n", objects[i].id);
            }
        }
    }
}

static void thingset_init_common(struct thingset_global_context *ts)
{
#ifdef CONFIG_THINGSET_OBJECT_LOOKUP_MAP
    for (unsigned int b = 0; b < CONFIG_THINGSET_OBJECT_LOOKUP_BUCKETS; b++) {
        sys_slist_init(&ts->data_objects_lookup[b]);
    }

    for (unsigned int i = 0; i < ts->num_objects; i++) {
        struct thingset_data_object *object = &ts->data_objects[i];
        sys_slist_append(
            &ts->data_objects_lookup[object->id % CONFIG_THINGSET_OBJECT_LOOKUP_BUCKETS],
            &object->node);
    }
#endif
    ts->auth_flags = THINGSET_USR_MASK;

    k_sem_init(&ts->sem, 1, 1);
    k_mutex_init(&ts->reader_wait_writer);
}

void thingset_init(struct thingset_global_context *ts, struct thingset_data_object *objects,
                   size_t num_objects)
{
    check_id_duplicates(objects, num_objects);

    ts->data_objects = objects;
    ts->num_objects = num_objects;
    thingset_init_common(ts);
}

void thingset_init_global(struct thingset_global_context *ts)
{
    /* duplicates are checked at compile-time */

    ts->data_objects = TYPE_SECTION_START(thingset_data_object);
    STRUCT_SECTION_COUNT(thingset_data_object, &ts->num_objects);
    thingset_init_common(ts);
}

int thingset_process_message(struct thingset_global_context *ts, const uint8_t *msg, size_t msg_len,
                             uint8_t *rsp, size_t rsp_size)
{
    int ret;

    if (msg == NULL || msg_len < 1) {
        return -THINGSET_ERR_BAD_REQUEST;
    }

    if (rsp == NULL || rsp_size < 4) {
        /* response buffer with at least 4 bytes required to fit minimum response */
        return -THINGSET_ERR_INTERNAL_SERVER_ERR;
    }

    struct thingset_context req_ctx = {
        .context = ts,
        .msg = msg,
        .msg_len = msg_len,
        .msg_pos = 0,
        .rsp = rsp,
        .rsp_size = rsp_size,
        .rsp_pos = 0,
    };

    if (IS_ENABLED(CONFIG_THINGSET_TEXT_MODE) && req_ctx.msg[0] >= 0x20) {
        ret = thingset_txt_process(&req_ctx);
    }
    else {
        ret = thingset_bin_process(&req_ctx);
    }

    return ret;
}

int thingset_export_subsets_progressively(struct thingset_global_context *ts, uint8_t *buf,
                                          size_t buf_size, uint16_t subsets,
                                          enum thingset_data_format format,
                                          struct thingset_context *req_ctx, unsigned int *index,
                                          size_t *len)
{
    if (*index == 0) {
        struct thingset_context r = {
            .context = ts,
            .rsp = buf,
            .rsp_size = buf_size,
            .rsp_pos = 0,
        };
        *req_ctx = r;

        switch (format) {
            case THINGSET_BIN_IDS_VALUES:
                req_ctx->endpoint.use_ids = true;
                thingset_bin_setup(req_ctx, 0);
                break;
            default:
                return -THINGSET_ERR_NOT_IMPLEMENTED;
        }
    }
    thingset_acquire_read_lock(ts, K_FOREVER);
    int ret = thingset_bin_export_subsets_progressively(req_ctx, subsets, index, len);
    thingset_release_read_lock(ts);

    return ret;
}

int thingset_export_subsets(struct thingset_global_context *ts, uint8_t *buf, size_t buf_size,
                            uint16_t subsets, enum thingset_data_format format)
{
    int ret;

    struct thingset_context req_ctx = {
        .context = ts,
        .rsp = buf,
        .rsp_size = buf_size,
        .rsp_pos = 0,
    };

    switch (format) {
        case THINGSET_TXT_NAMES_VALUES:
            thingset_txt_setup(&req_ctx);
            break;
        case THINGSET_BIN_IDS_VALUES:
            req_ctx.endpoint.use_ids = true;
            thingset_bin_setup(&req_ctx, 0);
            break;
        default:
            return -THINGSET_ERR_NOT_IMPLEMENTED;
    }

    thingset_acquire_read_lock(ts, K_FOREVER);
    ret = req_ctx.api->serialize_subsets(&req_ctx, subsets);

    req_ctx.api->serialize_finish(&req_ctx);

    if (ret == 0) {
        ret = req_ctx.rsp_pos;
    }

    thingset_release_read_lock(ts);

    return ret;
}

int thingset_export_item(struct thingset_global_context *ts, uint8_t *buf, size_t buf_size,
                         const struct thingset_data_object *obj, enum thingset_data_format format)
{
    int ret;

    struct thingset_context req_ctx = {
        .context = ts,
        .rsp = buf,
        .rsp_size = buf_size,
        .rsp_pos = 0,
    };

    switch (format) {
        case THINGSET_TXT_VALUES_ONLY:
            thingset_txt_setup(&req_ctx);
            break;
        case THINGSET_BIN_VALUES_ONLY:
            req_ctx.endpoint.use_ids = true;
            thingset_bin_setup(&req_ctx, 0);
            break;
        default:
            ret = -THINGSET_ERR_NOT_IMPLEMENTED;
            goto out;
    }

    thingset_acquire_read_lock(ts, K_FOREVER);
    ret = req_ctx.api->serialize_value(&req_ctx, obj);

    req_ctx.api->serialize_finish(&req_ctx);

    if (ret == 0) {
        ret = req_ctx.rsp_pos;
    }

out:
    thingset_release_read_lock(ts);

    return ret;
}

struct thingset_data_object *thingset_iterate_subsets(struct thingset_global_context *ts,
                                                      uint16_t subset,
                                                      struct thingset_data_object *start_obj)
{
    if (start_obj == NULL) {
        start_obj = ts->data_objects;
    }

    struct thingset_data_object *end_obj = ts->data_objects + ts->num_objects;
    for (struct thingset_data_object *obj = start_obj; obj < end_obj; obj++) {
        if (obj->subsets & subset) {
            return obj;
        }
    }

    return NULL;
}

int thingset_import_data_progressively(struct thingset_global_context *ts, const uint8_t *data,
                                       size_t len, enum thingset_data_format format,
                                       uint8_t auth_flags, struct thingset_context *req_ctx,
                                       uint32_t *last_id, size_t *consumed)
{
    int err = 0;

    if (*last_id == 0) {
        struct thingset_context r = {
            .context = ts,
            .msg = data,
            .msg_len = len,
            .msg_pos = 0,
            .rsp = NULL,
            .rsp_size = 0,
            .rsp_pos = 0,
        };
        *req_ctx = r;

        switch (format) {
            case THINGSET_BIN_IDS_VALUES:
                req_ctx->endpoint.use_ids = true;
                thingset_bin_setup(req_ctx, 0);
                req_ctx->msg_payload = data;
                req_ctx->api->deserialize_payload_reset(req_ctx);
                break;
            default:
                err = -THINGSET_ERR_NOT_IMPLEMENTED;
                break;
        }

        if (err) {
            return err;
        }

        thingset_acquire_write_lock(ts, K_FOREVER);
    }

    err = thingset_bin_import_data_progressively(req_ctx, auth_flags, len, last_id, consumed);
    if (err < 0) {
        thingset_release_write_lock(ts);
    }
    return err;
}

int thingset_import_data_progressively_end(struct thingset_global_context *ts)
{
    thingset_release_write_lock(ts);
    return 0;
}

int thingset_import_data(struct thingset_global_context *ts, const uint8_t *data, size_t len,
                         uint8_t auth_flags, enum thingset_data_format format)
{
    int err;

    struct thingset_context req_ctx = {
        .context = ts,
        .msg = data,
        .msg_len = len,
        .msg_pos = 0,
        .rsp = NULL,
        .rsp_size = 0,
        .rsp_pos = 0,
    };

    switch (format) {
        case THINGSET_BIN_IDS_VALUES:
            req_ctx.endpoint.use_ids = true;
            thingset_bin_setup(&req_ctx, 0);
            req_ctx.msg_payload = data;
            req_ctx.api->deserialize_payload_reset(&req_ctx);
            thingset_acquire_write_lock(ts, K_FOREVER);
            err = thingset_bin_import_data(&req_ctx, auth_flags, format);
            break;
        default:
            err = -THINGSET_ERR_NOT_IMPLEMENTED;
            break;
    }

    thingset_release_write_lock(ts);

    return err;
}

static int deserialize_value_callback(struct thingset_context *ts,
                                      const struct thingset_data_object *item_offset)
{
    /* already in a locked context */
    return ts->api->deserialize_value(ts, item_offset, false);
}

int thingset_import_record(struct thingset_global_context *ts, const uint8_t *data, size_t len,
                           struct thingset_endpoint *endpoint, enum thingset_data_format format)
{
    int err;

    struct thingset_context req_ctx = {
        .context = ts,
        .msg = data,
        .msg_len = len,
        .msg_pos = 0,
        .rsp = NULL,
        .rsp_size = 0,
        .rsp_pos = 0,
    };

    req_ctx.endpoint = *endpoint;

    switch (format) {
        case THINGSET_TXT_NAMES_VALUES:
            thingset_txt_setup(&req_ctx);
            req_ctx.msg_payload = data;
            req_ctx.api->deserialize_payload_reset(&req_ctx);
            break;
        case THINGSET_BIN_IDS_VALUES:
            req_ctx.endpoint.use_ids = true;
            thingset_bin_setup(&req_ctx, 0);
            req_ctx.msg_payload = data;
            req_ctx.api->deserialize_payload_reset(&req_ctx);
            break;
        default:
            err = -THINGSET_ERR_NOT_IMPLEMENTED;
            goto out;
    }

    thingset_acquire_read_lock(ts, K_FOREVER);
    err = req_ctx.api->deserialize_map_start(&req_ctx);
    if (err != 0) {
        goto out;
    }

    const struct thingset_data_object *item;
    while ((err = req_ctx.api->deserialize_child(&req_ctx, &item))
           != -THINGSET_ERR_DESERIALIZATION_FINISHED)
    {
        if (err == -THINGSET_ERR_NOT_FOUND) {
            /* silently ignore non-existing record items and skip value */
            req_ctx.api->deserialize_skip(&req_ctx);
            continue;
        }
        else if (err != 0) {
            goto out;
        }

        struct thingset_records *records = req_ctx.endpoint.object->data.records;
        uint8_t *record_ptr =
            (uint8_t *)records->records + req_ctx.endpoint.index * records->record_size;
        err = thingset_common_prepare_record_element(&req_ctx, item, record_ptr,
                                                     deserialize_value_callback);

        if (err != 0) {
            goto out;
        }
    }

    err = err == -THINGSET_ERR_DESERIALIZATION_FINISHED ? 0
                                                        : req_ctx.api->deserialize_finish(&req_ctx);

out:
    thingset_release_read_lock(ts);

    return err;
}

int thingset_report_path(struct thingset_global_context *ts, char *buf, size_t buf_size,
                         const char *path, enum thingset_data_format format)
{
    int err;

    struct thingset_context req_ctx = {
        .context = ts,
        .rsp = buf,
        .rsp_size = buf_size,
        .rsp_pos = 0,
    };

    err = thingset_endpoint_by_path(ts, &req_ctx.endpoint, path, strlen(path));
    if (err != 0) {
        goto out;
    }
    else if (req_ctx.endpoint.object == NULL) {
        err = -THINGSET_ERR_BAD_REQUEST;
        goto out;
    }

    switch (format) {
#ifdef CONFIG_THINGSET_TEXT_MODE
        case THINGSET_TXT_NAMES_VALUES:
            thingset_txt_setup(&req_ctx);
            break;
#endif
        case THINGSET_BIN_IDS_VALUES:
            req_ctx.endpoint.use_ids = true;
            thingset_bin_setup(&req_ctx, 1);
            break;
        case THINGSET_BIN_NAMES_VALUES:
            req_ctx.endpoint.use_ids = false;
            thingset_bin_setup(&req_ctx, 1);
            break;
        default:
            err = -THINGSET_ERR_NOT_IMPLEMENTED;
            goto out;
    }

    thingset_acquire_read_lock(ts, K_FOREVER);
    err = req_ctx.api->serialize_report_header(&req_ctx, path);
    if (err != 0) {
        goto out;
    }

    switch (req_ctx.endpoint.object->type) {
        case THINGSET_TYPE_GROUP:
            err = thingset_common_serialize_group(&req_ctx, req_ctx.endpoint.object);
            break;
        case THINGSET_TYPE_SUBSET:
            err = req_ctx.api->serialize_subsets(&req_ctx, req_ctx.endpoint.object->data.subset);
            break;
        case THINGSET_TYPE_FN_VOID:
        case THINGSET_TYPE_FN_I32:
            /* bad request, as we can't read exec object's values */
            err = -THINGSET_ERR_BAD_REQUEST;
            break;
        case THINGSET_TYPE_RECORDS:
            if (req_ctx.endpoint.index != THINGSET_ENDPOINT_INDEX_NONE) {
                err = thingset_common_serialize_record(&req_ctx, req_ctx.endpoint.object,
                                                       req_ctx.endpoint.index);
                break;
            }
            /* fallthrough */
        default:
            err = req_ctx.api->serialize_value(&req_ctx, req_ctx.endpoint.object);
            break;
    }

    req_ctx.api->serialize_finish(&req_ctx);

    if (err == 0) {
        err = req_ctx.rsp_pos;
    }

out:
    thingset_release_read_lock(ts);

    return err;
}

void thingset_set_authentication(struct thingset_global_context *ts, uint8_t flags)
{
    ts->auth_flags = flags;
}

void thingset_set_update_callback(struct thingset_global_context *ts, const uint16_t subsets,
                                  void (*update_cb)(void))
{
    ts->update_subsets = subsets;
    ts->update_cb = update_cb;
}

struct thingset_data_object *thingset_get_child_by_name(struct thingset_global_context *ts,
                                                        uint16_t parent_id, const char *name,
                                                        size_t len)
{
    for (unsigned int i = 0; i < ts->num_objects; i++) {
        if (ts->data_objects[i].parent_id == parent_id
            && strncmp(ts->data_objects[i].name, name, len) == 0
            // without length check foo and fooBar would be recognized as equal
            && strlen(ts->data_objects[i].name) == len)
        {
            return &(ts->data_objects[i]);
        }
    }

#ifdef CONFIG_THINGSET_METADATA_ENDPOINT
    if (len == strlen(metadata_object.name) && strncmp(name, metadata_object.name, len) == 0) {
        return &metadata_object;
    }
#endif

    return NULL;
}

struct thingset_data_object *thingset_get_object_by_id(struct thingset_global_context *ts,
                                                       uint16_t id)
{
#ifdef CONFIG_THINGSET_OBJECT_LOOKUP_MAP
    sys_slist_t *list = &ts->data_objects_lookup[id % CONFIG_THINGSET_OBJECT_LOOKUP_BUCKETS];
    sys_snode_t *pnode;
    struct thingset_data_object *object;
    SYS_SLIST_FOR_EACH_NODE(list, pnode)
    {
        object = CONTAINER_OF(pnode, struct thingset_data_object, node);
        if (object->id == id) {
            return object;
        }
    }
#else
    for (unsigned int i = 0; i < ts->num_objects; i++) {
        if (ts->data_objects[i].id == id) {
            return &(ts->data_objects[i]);
        }
    }
#endif /* CONFIG_THINGSET_OBJECT_LOOKUP_MAP */
    return NULL;
}

struct thingset_data_object *thingset_get_object_by_path(struct thingset_global_context *ts,
                                                         const char *path, size_t path_len,
                                                         int *index)
{
    *index = THINGSET_ENDPOINT_INDEX_NONE;

    struct thingset_data_object *object = NULL;
    const char *start = path;
    const char *end;
    uint16_t parent = 0;

    /* maximum depth of 10 assumed */
    for (int i = 0; i < 10; i++) {
        end = strchr(start, '/');
        if (end == NULL || end >= path + path_len) {
            /* reached at the end of the path */
            if (object != NULL && object->type == THINGSET_TYPE_RECORDS && *start >= '0'
                && *start <= '9')
            {
                /* numeric ID to select index in an array of records */
                /*
                 * Note: strtoul and atoi only work with null-terminated strings, so we have to use
                 * an own implementation to be able to parse CBOR-encoded strings.
                 */
                end = path + path_len; /* ensure we never go past the end of the declared length */
                *index = 0;
                do {
                    if (*start >= '0' && *start <= '9') {
                        *index = (*index) * 10 + *start - '0';
                        start++;
                    }
                    else {
                        return NULL;
                    }
                } while (start < end);
            }
            else if (*start == '-') {
                /* non-existent element behind the last array element */
                *index = THINGSET_ENDPOINT_INDEX_NEW;
            }
            else {
                object = thingset_get_child_by_name(ts, parent, start, path + path_len - start);
            }
            break;
        }
        else if (end == path + path_len - 1) {
            /* path ends with slash */
            object = thingset_get_child_by_name(ts, parent, start, end - start);
            break;
        }
        else {
            /* go further down the path */
            object = thingset_get_child_by_name(ts, parent, start, end - start);
            if (object) {
                parent = object->id;
                start = end + 1;
            }
            else {
                break;
            }
        }
    }

    return object;
}

int thingset_endpoint_by_path(struct thingset_global_context *ts,
                              struct thingset_endpoint *endpoint, const char *path, size_t path_len)
{
    endpoint->index = THINGSET_ENDPOINT_INDEX_NONE;
    endpoint->use_ids = false;

    if (path_len == 0) {
        endpoint->object = &root_object;
        return 0;
    }

    if (path[0] == '/') {
        return -THINGSET_ERR_NOT_A_GATEWAY;
    }

    struct thingset_data_object *object =
        thingset_get_object_by_path(ts, path, path_len, &endpoint->index);

    endpoint->object = object;

    if (object == NULL) {
        return -THINGSET_ERR_NOT_FOUND;
    }

    return 0;
}

int thingset_endpoint_by_id(struct thingset_global_context *ts, struct thingset_endpoint *endpoint,
                            uint16_t id)
{
    struct thingset_data_object *object;
    endpoint->index = THINGSET_ENDPOINT_INDEX_NONE;
    endpoint->use_ids = true;

    if (id == 0) {
        endpoint->object = &root_object;
        return 0;
    }
    else if (id == THINGSET_ID_PATHS) {
        endpoint->object = &paths_object;
        return 0;
    }
#ifdef CONFIG_THINGSET_METADATA_ENDPOINT
    else if (id == THINGSET_ID_METADATA) {
        endpoint->object = &metadata_object;
        return 0;
    }
#endif

    object = thingset_get_object_by_id(ts, id);
    if (object != NULL) {
        /* check that the found endpoint is not part of a record (cannot be queried like this) */
        struct thingset_data_object *parent = thingset_get_object_by_id(ts, object->parent_id);
        if (parent == NULL || parent->type != THINGSET_TYPE_RECORDS
            || object->type == THINGSET_TYPE_RECORDS)
        {
            endpoint->object = object;
            return 0;
        }
    }

    return -THINGSET_ERR_NOT_FOUND;
}

int thingset_get_path(struct thingset_global_context *ts, char *buf, size_t size,
                      const struct thingset_data_object *obj)
{
    int pos = 0;
    if (obj->parent_id != 0) {
        struct thingset_data_object *parent_obj = thingset_get_object_by_id(ts, obj->parent_id);
        if (parent_obj == NULL) {
            return -THINGSET_ERR_NOT_FOUND;
        }

        /*
         * Recursive implementation acceptable because the depth is automatically limited by actual
         * data structure nesting depth.
         */
        pos = thingset_get_path(ts, buf, size, parent_obj);
        if (pos < 0) {
            /* propagate errors back */
            return pos;
        }
        buf[pos++] = '/';
    }

    pos += snprintf(buf + pos, size - pos, "%s", obj->name);

    if (pos < size) {
        return pos;
    }
    else {
        return -THINGSET_ERR_RESPONSE_TOO_LARGE;
    }
}

static inline char *type_to_type_name(const enum thingset_type type)
{
    return type_name_lookup[type];
}

static int get_function_arg_types(struct thingset_global_context *ts, uint16_t parent_id, char *buf,
                                  size_t size)
{
    int len = 0;
    for (unsigned int i = 0; i < ts->num_objects; i++) {
        if (ts->data_objects[i].parent_id == parent_id) {
            if (len > 0) {
                if (size < 2) {
                    return -THINGSET_ERR_RESPONSE_TOO_LARGE;
                }
                len += sprintf(buf, ",");
                size -= 1;
                buf += 1;
            }
            char *elementType = type_to_type_name(ts->data_objects[i].type);
            if (len > size) {
                return -THINGSET_ERR_RESPONSE_TOO_LARGE;
            }
            len += sprintf(buf, "%s", elementType);
            buf += len;
            size -= len;
        }
    }
    return len;
}

int thingset_get_type_name(struct thingset_global_context *ts,
                           const struct thingset_data_object *obj, char *buf, size_t size)
{
    switch (obj->type) {
        case THINGSET_TYPE_ARRAY: {
            char *elementType = type_to_type_name(obj->data.array->element_type);
            if (sizeof(elementType) > size) {
                return -THINGSET_ERR_RESPONSE_TOO_LARGE;
            }
            return sprintf(buf, "%s[]", elementType);
        }
        case THINGSET_TYPE_FN_VOID:
        case THINGSET_TYPE_FN_I32:
            sprintf(buf, "(");
            int len = 1 + get_function_arg_types(ts, obj->id, buf + 1, size - 1);
            if (len < 0) {
                return -THINGSET_ERR_RESPONSE_TOO_LARGE;
            }
            if (size - len < 8) { /* enough space to finish? */
                return -THINGSET_ERR_RESPONSE_TOO_LARGE;
            }
            buf += len;
            size -= len;
            switch (obj->type) {
                case THINGSET_TYPE_FN_VOID:
                    len += sprintf(buf, ")->()");
                    break;
                case THINGSET_TYPE_FN_I32:
                    len += sprintf(buf, ")->(i32)");
                    break;
                default:
                    break;
            }
            return len;
        default: {
            char *type = type_to_type_name(obj->type);
            return sprintf(buf, "%s", type);
        }
    }
}

int thingset_acquire_read_lock(struct thingset_global_context *ts, k_timeout_t timeout)
{
    int result = k_mutex_lock(&ts->reader_wait_writer, timeout);
    if (result) {
        return result;
    }
    k_mutex_unlock(&ts->reader_wait_writer);

    atomic_t previous_count = atomic_inc(&ts->reader_count);
    if (previous_count == 0) {
        result = k_sem_take(&ts->sem, timeout);
        if (result) {
            atomic_dec(&ts->reader_count);
            return result;
        }
    }

    return 0;
}

int thingset_release_read_lock(struct thingset_global_context *ts)
{
    atomic_t previous_count = atomic_dec(&ts->reader_count);
    if (previous_count == 0) {
        k_sem_give(&ts->sem);
    }

    return 0;
}

int thingset_acquire_write_lock(struct thingset_global_context *ts, k_timeout_t timeout)
{
    int result = k_mutex_lock(&ts->reader_wait_writer, timeout);
    if (result) {
        return result;
    }
    result = k_sem_take(&ts->sem, timeout);
    k_mutex_unlock(&ts->reader_wait_writer);
    if (result) {
        return result;
    }
    return 0;
}

int thingset_release_write_lock(struct thingset_global_context *ts)
{
    k_sem_give(&ts->sem);
    return 0;
}
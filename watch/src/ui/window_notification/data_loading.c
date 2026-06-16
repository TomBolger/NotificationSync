#include "data_loading.h"

#include <stdlib.h>

#include "action_list.h"
#include "idle_handler.h"
#include "window_notification.h"
#include "commons/bytes.h"
#include "commons/math.h"
#include "commons/connection/bucket_sync.h"
#include "ui/window_status.h"

static BucketList* buckets;
static NotificationListItem list_items[MAX_NOTIFICATION_ITEMS];
static const uint32_t STORAGE_BUCKET_FLAGS_ID_MIN = 3000;
static AppTimer* metadata_refresh_timer;
static char* staged_body_text;
static size_t staged_body_size;
static uint8_t staged_body_bucket_id;
static bool staged_body_active;
static Action staged_actions[MAX_NOTIFICATION_ACTIONS];
static uint8_t staged_num_actions;

static size_t bounded_cstring_length(const uint8_t* data, const size_t start, const size_t data_size)
{
    size_t length = 0;
    while (start + length < data_size && data[start + length] != '\0')
    {
        length++;
    }

    return length;
}

static void copy_action_text(char* destination, const uint8_t* source, const size_t source_length)
{
    const size_t copy_length = MIN(source_length, MAX_NOTIFICATION_ACTION_TEXT - 1);
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

static void reset_staged_body(const uint8_t bucket_id)
{
    if (staged_body_text == NULL)
    {
        staged_body_text = malloc(MAX_BODY_TEXT_SIZE + 1);
    }
    if (staged_body_text == NULL)
    {
        staged_body_active = false;
        return;
    }

    staged_body_bucket_id = bucket_id;
    staged_body_size = 0;
    staged_body_text[0] = '\0';
    staged_body_active = true;
    staged_num_actions = 0;
    memset(staged_actions, 0, sizeof(staged_actions));
}

static void append_staged_body(const uint8_t bucket_id, const uint8_t* body, const size_t body_size)
{
    if (!staged_body_active || staged_body_bucket_id != bucket_id)
    {
        return;
    }

    if (staged_body_size >= MAX_BODY_TEXT_SIZE)
    {
        return;
    }

    const size_t bytes_to_copy = MIN(body_size, MAX_BODY_TEXT_SIZE - staged_body_size);
    memcpy(&staged_body_text[staged_body_size], body, bytes_to_copy);
    staged_body_size += bytes_to_copy;
    staged_body_text[staged_body_size] = '\0';
}

static void commit_staged_body(const uint8_t bucket_id)
{
    if (!staged_body_active || staged_body_bucket_id != bucket_id)
    {
        return;
    }

    if (bucket_id == window_notification_data.currently_selected_bucket)
    {
        window_notification_ui_replace_current_details(bucket_id, staged_body_text, staged_body_size,
                                                       staged_actions, staged_num_actions);
    }
    else
    {
        window_notification_ui_cache_details_for_bucket(bucket_id, staged_body_text, staged_body_size,
                                                       staged_actions, staged_num_actions);
    }
    staged_body_active = false;
    free(staged_body_text);
    staged_body_text = NULL;
    staged_body_size = 0;
    staged_num_actions = 0;
    window_notification_ui_on_details_cached(bucket_id);
}

static bool is_real_notification_id(const uint8_t id)
{
    return id != 1;
}

static void set_loading_item(NotificationListItem* item)
{
    strcpy(item->app_name, "Loading");
    strcpy(item->title, "Syncing notification");
    strcpy(item->body, "");
}

static void parse_bucket(const uint8_t id, const enum DotState state, NotificationListItem* item)
{
    uint8_t bucket_data[256];
    memset(item, 0, sizeof(NotificationListItem));
    item->bucket_id = id;
    item->state = state;

    const uint8_t bucket_size = bucket_sync_get_bucket_size(id);
    if (bucket_size < 6 || !bucket_sync_load_bucket(id, bucket_data))
    {
        set_loading_item(item);
        return;
    }

    item->receive_time = read_uint32_from_byte_array(bucket_data, 0);
    size_t position = 4;
    item->icon_id = bucket_data[position++];
    item->color_id = bucket_data[position++];

    const size_t app_name_length = bounded_cstring_length(bucket_data, position, bucket_size);
    if (position + app_name_length >= bucket_size)
    {
        set_loading_item(item);
        return;
    }
    strncpy(item->app_name, (char*)&bucket_data[position], MIN(app_name_length, sizeof(item->app_name) - 1));
    position += app_name_length + 1;

    const size_t title_length = bounded_cstring_length(bucket_data, position, bucket_size);
    if (position + title_length >= bucket_size)
    {
        set_loading_item(item);
        return;
    }
    strncpy(item->title, (char*)&bucket_data[position], MIN(title_length, sizeof(item->title) - 1));
    position += title_length + 1;

    const size_t body_bytes = bucket_size - position;
    strncpy(item->body, (char*)&bucket_data[position], MIN(body_bytes, sizeof(item->body) - 1));
}

bool is_notification_unread(const uint8_t bucket_flags, const uint8_t id)
{
    uint8_t on_watch_flags[] = {0};
    persist_read_data(STORAGE_BUCKET_FLAGS_ID_MIN + id, on_watch_flags, 1);
    return (bucket_flags & 0x01) != 0 && on_watch_flags[0] != 1;
}

static enum DotState state_for_bucket(const uint8_t id, const uint8_t flags)
{
    if (is_notification_unread(flags, id))
    {
        return UNREAD;
    }
    if ((flags & 0x02) != 0)
    {
        return PAUSED;
    }
    return NORMAL;
}

void notification_window_ingest_bucket_metadata()
{
    uint8_t count = 0;
    for (int i = 0; buckets != NULL && i < buckets->count && count < MAX_NOTIFICATION_ITEMS; i++)
    {
        const uint8_t id = buckets->data[i].id;
        if (!is_real_notification_id(id))
        {
            continue;
        }

        const enum DotState state = state_for_bucket(id, buckets->data[i].flags);
        parse_bucket(id, state, &list_items[count]);
        window_notification_data.dot_states[count] = state;
        count++;
    }

    if (count == 0)
    {
        if (bucket_sync_is_currently_syncing)
        {
            set_loading_item(&list_items[0]);
            window_notification_ui_set_items(list_items, 1, true);
            return;
        }

        window_notification_ui_set_items(list_items, 0, false);
        return;
    }

    window_notification_ui_set_items(list_items, count, false);
}

void window_notification_data_select_bucket_on_index(const uint8_t target_index)
{
    window_notification_data.currently_selected_bucket_index = target_index;
    window_notification_ui_on_bucket_selected();
}

static void on_buckets_changed()
{
    buckets = bucket_sync_get_bucket_list();
    notification_window_ingest_bucket_metadata();
    idle_handler_notify_notifications_updated();
}

static void on_scheduled_buckets_changed(void* context)
{
    (void)context;
    metadata_refresh_timer = NULL;
    on_buckets_changed();
}

static void schedule_buckets_changed()
{
    if (metadata_refresh_timer != NULL)
    {
        return;
    }

    metadata_refresh_timer = app_timer_register(1, on_scheduled_buckets_changed, NULL);
}

static void on_bucket_updated(const BucketMetadata bucket_metadata, void* context)
{
    if (bucket_metadata.id == 1)
    {
        idle_handler_register_timers();
        return;
    }

    window_notification_ui_uncache_body_for_bucket(bucket_metadata.id);
    window_notification_ui_note_bucket_updated(bucket_metadata.id);
    persist_delete(bucket_metadata.id + STORAGE_BUCKET_FLAGS_ID_MIN);
    schedule_buckets_changed();
}

void window_notification_data_receive_more_text(const uint8_t bucket_id, const uint8_t* data, const size_t data_size)
{
    if (data_size < 1)
    {
        return;
    }

    const bool is_current_bucket = bucket_id == window_notification_data.currently_selected_bucket;
    size_t position = 1;
    const uint8_t num_actions = data[0];
    Action packet_actions[MAX_NOTIFICATION_ACTIONS] = {0};
    const uint8_t packet_num_actions = MIN(num_actions, MAX_NOTIFICATION_ACTIONS);
    for (int i = 0; i < num_actions; i++)
    {
        if (position >= data_size)
        {
            return;
        }
        const uint8_t action_id = data[position++];
        const size_t action_title_position = position;
        const size_t action_title_length = bounded_cstring_length(data, action_title_position, data_size);
        if (action_title_position + action_title_length >= data_size)
        {
            return;
        }
        if (i < MAX_NOTIFICATION_ACTIONS)
        {
            copy_action_text(
                packet_actions[i].text,
                &data[action_title_position],
                action_title_length
            );
            packet_actions[i].id = action_id;
            packet_actions[i].voice = false;
        }
        position += action_title_length + 1;
    }

    if (position + 2 > data_size)
    {
        return;
    }
    const size_t icon_bytes_length = read_uint16_from_byte_array(data, position);
    if (position + 2 + icon_bytes_length > data_size)
    {
        return;
    }
    position += 2 + icon_bytes_length;

    const size_t max_text_size = MIN(MAX_BODY_TEXT_SIZE, data_size - position);
    if (is_current_bucket)
    {
        window_notification_ui_replace_current_details(bucket_id, (char*)&data[position],
                                                       max_text_size, packet_actions,
                                                       packet_num_actions);
        window_notification_ui_redraw();
    }
    else
    {
        window_notification_ui_cache_details_for_bucket(bucket_id, (char*)&data[position],
                                                       max_text_size, packet_actions,
                                                       packet_num_actions);
    }
    window_notification_ui_on_details_cached(bucket_id);
}

bool window_notification_data_receive_more_text_v2(const uint8_t* data, const size_t data_size)
{
    if (data_size < 3)
    {
        return true;
    }

    const uint8_t bucket_id = data[0];
    const uint8_t total_chunks = MAX(data[1], 1);
    const bool is_current_bucket = bucket_id == window_notification_data.currently_selected_bucket;
    size_t position = 2;
    const uint8_t num_actions = data[position++];
    reset_staged_body(bucket_id);
    staged_num_actions = MIN(num_actions, MAX_NOTIFICATION_ACTIONS);
    for (int i = 0; i < num_actions; i++)
    {
        if (position >= data_size)
        {
            return true;
        }
        const uint8_t action_id = data[position++];
        const size_t action_title_position = position;
        const size_t action_title_length = bounded_cstring_length(data, action_title_position, data_size);
        if (action_title_position + action_title_length >= data_size)
        {
            return true;
        }
        if (i < MAX_NOTIFICATION_ACTIONS)
        {
            copy_action_text(
                staged_actions[i].text,
                &data[action_title_position],
                action_title_length
            );
            staged_actions[i].id = action_id;
            staged_actions[i].voice = false;
        }
        position += action_title_length + 1;
    }

    if (position + 2 > data_size)
    {
        return true;
    }
    const size_t icon_bytes_length = read_uint16_from_byte_array(data, position);
    if (position + 2 + icon_bytes_length > data_size)
    {
        return true;
    }
    position += 2 + icon_bytes_length;

    append_staged_body(bucket_id, &data[position], data_size - position);

    const bool complete = total_chunks <= 1;
    if (complete)
    {
        commit_staged_body(bucket_id);
        if (is_current_bucket)
        {
            window_notification_ui_redraw();
        }
    }
    return complete;
}

bool window_notification_data_receive_more_text_v2_continuation(const uint8_t* data, const size_t data_size)
{
    if (data_size < 3)
    {
        return true;
    }

    const uint8_t bucket_id = data[0];
    const uint8_t chunk_index = data[1];
    const uint8_t total_chunks = MAX(data[2], 1);
    const bool complete = chunk_index + 1 >= total_chunks;
    const bool is_current_bucket = bucket_id == window_notification_data.currently_selected_bucket;

    append_staged_body(bucket_id, &data[3], data_size - 3);

    if (complete)
    {
        commit_staged_body(bucket_id);
        if (is_current_bucket)
        {
            window_notification_ui_redraw();
        }
    }
    return complete;
}

void window_notification_data_receive_show_submenu(const uint8_t* data, const size_t data_size)
{
    if (data_size < 3)
    {
        return;
    }

    const uint8_t target_bucket = data[0];
    if (window_notification_data.currently_selected_bucket != target_bucket)
    {
        return;
    }

    const uint8_t menu_id = data[1];
    const uint8_t num_actions = data[2];
    size_t position = 3;
    window_notification_data.num_submenu_actions = MIN(num_actions, MAX_NOTIFICATION_ACTIONS);
    for (int i = 0; i < num_actions; i++)
    {
        const size_t action_title_position = position;
        const size_t action_title_length = bounded_cstring_length(data, action_title_position, data_size);
        if (action_title_position + action_title_length >= data_size)
        {
            return;
        }
        position += action_title_length + 1;
        if (position >= data_size)
        {
            return;
        }
        const bool voice = data[position++] == 1;
        if (i < MAX_NOTIFICATION_ACTIONS)
        {
            copy_action_text(
                window_notification_data.submenu_actions[i].text,
                &data[action_title_position],
                action_title_length
            );
            window_notification_data.submenu_actions[i].id = i;
            window_notification_data.submenu_actions[i].voice = voice;
        }
    }

    if (window_notification_data.menu_displayed)
    {
        window_notification_data.open_menu_on_success = menu_id;
    }
    else
    {
        window_notification_data.currently_displayed_menu_id = menu_id;
        window_notification_action_list_show();
    }
}

static void on_bucket_deleted(const uint8_t bucket_id)
{
    persist_delete(bucket_id + STORAGE_BUCKET_FLAGS_ID_MIN);
    window_notification_ui_on_bucket_deleted(bucket_id);
}

void window_notification_data_app_started()
{
    idle_handler_reset_user_interaction();
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
}

void window_notification_data_init()
{
    on_buckets_changed();
    bucket_sync_set_bucket_list_change_callback(schedule_buckets_changed);
    bucket_sync_set_bucket_data_change_callback(on_bucket_updated, NULL);
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
}

void window_notification_data_deinit()
{
    if (staged_body_text != NULL)
    {
        free(staged_body_text);
        staged_body_text = NULL;
        staged_body_active = false;
    }

    if (metadata_refresh_timer != NULL)
    {
        app_timer_cancel(metadata_refresh_timer);
        metadata_refresh_timer = NULL;
    }

    bucket_sync_clear_bucket_list_change_callback(schedule_buckets_changed);
    bucket_sync_clear_bucket_data_change_callback(on_bucket_updated, NULL);
    bucket_sync_register_bucket_deleted_callback(NULL);
}

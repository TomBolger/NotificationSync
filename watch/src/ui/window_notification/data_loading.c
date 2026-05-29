#include "data_loading.h"

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

static bool is_real_notification_id(const uint8_t id)
{
    return id != 1;
}

static void parse_bucket(const uint8_t id, const enum DotState state, NotificationListItem* item)
{
    uint8_t bucket_data[256];
    memset(item, 0, sizeof(NotificationListItem));
    item->bucket_id = id;
    item->state = state;

    if (!bucket_sync_load_bucket(id, bucket_data))
    {
        strcpy(item->app_name, "Loading");
        strcpy(item->title, "Syncing notification");
        strcpy(item->body, "");
        return;
    }

    item->receive_time = read_uint32_from_byte_array(bucket_data, 0);
    uint8_t position = 4;
    item->icon_id = bucket_data[position++];
    item->color_id = bucket_data[position++];

    strncpy(item->app_name, (char*)&bucket_data[position], sizeof(item->app_name) - 1);
    position += strlen(item->app_name) + 1;

    strncpy(item->title, (char*)&bucket_data[position], sizeof(item->title) - 1);
    position += strlen(item->title) + 1;

    const uint8_t body_bytes = bucket_sync_get_bucket_size(id) - position;
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

static void on_bucket_updated(const BucketMetadata bucket_metadata, void* context)
{
    if (bucket_metadata.id == 1)
    {
        idle_handler_register_timers();
        return;
    }

    persist_delete(bucket_metadata.id + STORAGE_BUCKET_FLAGS_ID_MIN);
    on_buckets_changed();
}

void window_notification_data_receive_more_text(const uint8_t bucket_id, const uint8_t* data, const size_t data_size)
{
    if (window_notification_data.active == false)
    {
        return;
    }

    const bool is_current_bucket = bucket_id == window_notification_data.currently_selected_bucket;
    size_t position = 1;
    const uint8_t num_actions = data[0];
    if (is_current_bucket)
    {
        window_notification_data.num_actions = num_actions;
    }
    for (int i = 0; i < num_actions; i++)
    {
        const uint8_t action_id = data[position++];
        const char* action_title = (char*)&data[position];
        if (is_current_bucket)
        {
            strcpy(window_notification_data.actions[i].text, action_title);
        }
        position += strlen(action_title) + 1;
        if (is_current_bucket)
        {
            window_notification_data.actions[i].id = action_id;
            window_notification_data.actions[i].voice = false;
        }
    }

    const size_t icon_bytes_length = read_uint16_from_byte_array(data, position);
    position += 2 + icon_bytes_length;

    const size_t max_text_size = MIN(MAX_BODY_TEXT_SIZE, data_size - position);
    window_notification_ui_cache_body_for_bucket(bucket_id, (char*)&data[position], max_text_size);
    if (!is_current_bucket)
    {
        return;
    }

    strncpy(window_notification_data.body_text, (char*)&data[position], max_text_size);
    window_notification_data.body_text[max_text_size] = '\0';
    window_notification_ui_cache_current_body();
    window_notification_ui_redraw();
}

void window_notification_data_receive_show_submenu(const uint8_t* data, const size_t data_size)
{
    const uint8_t target_bucket = data[0];
    if (window_notification_data.currently_selected_bucket != target_bucket)
    {
        return;
    }

    const uint8_t menu_id = data[1];
    const uint8_t num_actions = data[2];
    size_t position = 3;
    window_notification_data.num_submenu_actions = num_actions;
    for (int i = 0; i < num_actions; i++)
    {
        const char* action_title = strcpy(window_notification_data.submenu_actions[i].text, (char*)&data[position]);
        position += strlen(action_title) + 1;
        window_notification_data.submenu_actions[i].id = i;
        window_notification_data.submenu_actions[i].voice = data[position++] == 1;
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
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
}

void window_notification_data_init()
{
    on_buckets_changed();
    bucket_sync_set_bucket_list_change_callback(on_buckets_changed);
    bucket_sync_set_bucket_data_change_callback(on_bucket_updated, NULL);
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
}

void window_notification_data_deinit()
{
    bucket_sync_set_bucket_list_change_callback(NULL);
    bucket_sync_clear_bucket_data_change_callback(on_bucket_updated, NULL);
    bucket_sync_register_bucket_deleted_callback(NULL);
}

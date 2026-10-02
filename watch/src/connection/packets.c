#include "packets.h"
#include "commons/connection/bluetooth.h"
#include "commons/connection/bucket_sync.h"
#include <pebble.h>

#include "notification_details_fetcher.h"
#include "../ui/window_status.h"
#include "commons/bytes.h"
#include "ui/window_image.h"
#include "ui/window_notification/action_list.h"
#include "ui/window_notification/notification_image.h"
#include "ui/window_notification/window_notification.h"

static void receive_phone_welcome(const DictionaryIterator* iterator);
static void receive_sync_restart(const DictionaryIterator* iterator);
static void receive_sync_next_packet(const DictionaryIterator* iterator);
static void receive_notification_details_text_packet(const DictionaryIterator* iterator);
static void receive_notification_details_text_packet_v2(const DictionaryIterator* iterator);
static void receive_notification_details_continuation_packet(const DictionaryIterator* iterator);
static void receive_submenu_packet(const DictionaryIterator* iterator);
static void receive_watch_packet(const DictionaryIterator* received);
static void receive_vibrate_packet(const DictionaryIterator* iterator);
static void receive_image_packet(const DictionaryIterator* iterator);
static void receive_reset_watch_mirror_packet(void);

static int close_retries_left = 3;
static uint8_t active_buckets_holder[MAX_BUCKETS];

void packets_init()
{
    bluetooth_register_receive_watch_packet(receive_watch_packet);
}

void send_watch_welcome()
{
    const BucketList* active_buckets = bucket_sync_get_bucket_list();
    for (int i = 0; i < active_buckets->count; i++)
    {
        active_buckets_holder[i] = active_buckets->data[i].id;
    }

    DictionaryIterator* iterator;
    if (app_message_outbox_begin(&iterator) != APP_MSG_OK)
    {
        // Outbox busy (e.g. a reconnect raced another message). The phone re-requests the welcome itself.
        return;
    }
    dict_write_uint8(iterator, 0, 0);
    dict_write_uint16(iterator, 1, PROTOCOL_VERSION);
    dict_write_uint16(iterator, 2, bucket_sync_current_version);
    dict_write_uint16(iterator, 3, appmessage_max_size);
    // Bit 0: colour screen. Bit 1: shows photos inline in notifications (packet 16).
    dict_write_uint8(iterator, 4, PBL_IF_COLOR_ELSE(1, 0) | (NOTIFICATION_IMAGE_SUPPORTED ? 2 : 0));
    dict_write_uint16(iterator, 5, PBL_DISPLAY_WIDTH);
    dict_write_uint16(iterator, 6, PBL_DISPLAY_HEIGHT);
    dict_write_data(iterator, 7, active_buckets_holder, active_buckets->count);
    bluetooth_app_message_outbox_send();
}

bool send_notification_opened(const uint8_t id, const bool prefetch)
{
    DictionaryIterator* iterator;
    const AppMessageResult res = app_message_outbox_begin(&iterator);

    if (res != APP_MSG_OK)
    {
        return false;
    }

    dict_write_uint8(iterator, 0, 4);
    dict_write_uint8(iterator, 1, id);
    if (prefetch)
    {
        // Phone should not treat a speculative fetch as the user having read the notification.
        dict_write_uint8(iterator, 2, 1);
    }
    bluetooth_app_message_outbox_send();
    return true;
}

bool send_mark_read(const uint8_t id)
{
    DictionaryIterator* iterator;
    if (app_message_outbox_begin(&iterator) != APP_MSG_OK)
    {
        return false;
    }

    // Packet 4 with key 3: user opened this notification; details are already on the watch.
    dict_write_uint8(iterator, 0, 4);
    dict_write_uint8(iterator, 1, id);
    dict_write_uint8(iterator, 3, 1);
    bluetooth_app_message_outbox_send();
    return true;
}

bool send_action_trigger(const uint8_t notification_id, const uint8_t action_id, const uint8_t menu_id, const char* text)
{
    DictionaryIterator* iterator;
    const AppMessageResult res = app_message_outbox_begin(&iterator);

    if (res != APP_MSG_OK)
    {
        return false;
    }

    dict_write_uint8(iterator, 0, 6);
    dict_write_uint8(iterator, 1, notification_id);
    dict_write_uint8(iterator, 2, action_id);
    dict_write_uint8(iterator, 3, menu_id);
    if (text != NULL)
    {
        dict_write_cstring(iterator, 4, text);
    }
    bluetooth_app_message_outbox_send();
    return true;
}

static void on_close_me_finished(const bool success)
{
    if (!success)
    {
        if (--close_retries_left == 0)
        {
            window_stack_pop_all(true);
        }
        else
        {
            // Retry sending after a while
            app_timer_register(250, send_close_me, NULL);
        }
    }
}

static void send_close_me_animated(const bool animated)
{
    // Even if close clashes with other packets, we don't really care,
    // we don't want to show errors to the user
    ignore_bluetooth_busy_errors = true;
    close_retries_left = 3;

    bluetooth_register_sending_finish(on_close_me_finished);

    DictionaryIterator* iterator;
    const AppMessageResult res = app_message_outbox_begin(&iterator);
    if (res == APP_MSG_OK)
    {
        dict_write_uint8(iterator, 0, 8);
        bluetooth_app_message_outbox_send();
    }

    window_stack_pop_all(animated);
}

void send_close_me()
{
    send_close_me_animated(true);
}

void send_close_me_without_animation()
{
    send_close_me_animated(false);
}

bool send_setting(const uint8_t id, const uint8_t value)
{
    DictionaryIterator* iterator;
    const AppMessageResult res = app_message_outbox_begin(&iterator);

    if (res != APP_MSG_OK)
    {
        return false;
    }

    dict_write_uint8(iterator, 0, 10);
    dict_write_uint8(iterator, 1, id);
    dict_write_uint8(iterator, 2, value);
    bluetooth_app_message_outbox_send();
    return true;
}

bool send_image_request(const uint8_t bucket_id, const uint16_t width, const uint16_t height)
{
    DictionaryIterator* iterator;
    if (app_message_outbox_begin(&iterator) != APP_MSG_OK)
    {
        return false;
    }
    dict_write_uint8(iterator, 0, 16);
    dict_write_uint8(iterator, 1, bucket_id);
    dict_write_uint16(iterator, 2, width);
    dict_write_uint16(iterator, 3, height);
    bluetooth_app_message_outbox_send();
    return true;
}

static void receive_notification_image_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = dict_find(iterator, 1);
    if (data != NULL && data->type == TUPLE_BYTE_ARRAY)
    {
        notification_image_receive(data->value->data, data->length);
    }
}

static void receive_watch_packet(const DictionaryIterator* received)
{
    const Tuple* packet_id_tuple = dict_find(received, 0);
    if (packet_id_tuple == NULL)
    {
        return;
    }
    const uint8_t packet_id = packet_id_tuple->value->uint8;

    switch (packet_id)
    {
    case 1:
        receive_phone_welcome(received);
        break;
    case 2:
        receive_sync_restart(received);
        break;
    case 3:
        receive_sync_next_packet(received);
        break;
    case 5:
        receive_notification_details_text_packet(received);
        break;
    case 13:
        receive_notification_details_text_packet_v2(received);
        break;
    case 14:
        receive_notification_details_continuation_packet(received);
        break;
    case 15:
        receive_reset_watch_mirror_packet();
        break;
    case 7:
        receive_vibrate_packet(received);
        break;
    case 9:
        receive_submenu_packet(received);
        break;
    case 11:
        receive_image_packet(received);
        break;
    case 12:
        send_watch_welcome();
        break;
    case 16:
        receive_notification_image_packet(received);
        break;
    default:
        break;
    }
}

static const Tuple* data_tuple(const DictionaryIterator* iterator, const uint32_t key)
{
    const Tuple* tuple = dict_find(iterator, key);
    if (tuple == NULL || tuple->type != TUPLE_BYTE_ARRAY)
    {
        return NULL;
    }
    return tuple;
}

static void receive_phone_welcome(const DictionaryIterator* iterator)
{
    const bool phone_launch = launch_reason() == APP_LAUNCH_PHONE;
    if (phone_launch && dict_find(iterator, 3) != NULL)
    {
        bucket_sync_set_auto_close_after_sync();
    }

    if (dict_find(iterator, 5) != NULL)
    {
        notification_details_fetcher_reset();
        bucket_sync_forget_buckets_from(2);
    }

    const Tuple* version = dict_find(iterator, 1);
    const uint16_t phone_protocol_version = version != NULL ? version->value->uint16 : 0;
    if (phone_protocol_version != PROTOCOL_VERSION)
    {
        if (phone_protocol_version > PROTOCOL_VERSION)
        {
            window_status_show_error("Version mismatch\n\nPlease update watch app");
        }
        else
        {
            window_status_show_error("Version mismatch\n\nPlease update phone app");
        }
        return;
    }

    window_notification_ui_on_phone_answered();

    const Tuple* sync = data_tuple(iterator, 2);
    if (sync != NULL)
    {
        bucket_sync_on_start_received(sync->value->data, sync->length);
    }

    if (phone_launch)
    {
        const Tuple* launch_bucket_entry = dict_find(iterator, 4);
        const uint8_t launch_bucket_id = launch_bucket_entry != NULL ? launch_bucket_entry->value->uint8 : 0;
        window_notification_ui_open_phone_launch_detail(launch_bucket_id);
    }

}

static void receive_sync_restart(const DictionaryIterator* iterator)
{
    const Tuple* sync = data_tuple(iterator, 1);
    if (sync != NULL)
    {
        bucket_sync_on_start_received(sync->value->data, sync->length);
    }
}

static void receive_sync_next_packet(const DictionaryIterator* iterator)
{
    const Tuple* sync = data_tuple(iterator, 1);
    if (sync != NULL)
    {
        bucket_sync_on_next_packet_received(sync->value->data, sync->length);
    }
}

static void receive_notification_details_text_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data != NULL)
    {
        notification_details_fetcher_on_text_received(data->value->data, data->length);
    }
}

static void receive_notification_details_text_packet_v2(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data != NULL)
    {
        notification_details_fetcher_on_text_received_v2(data->value->data, data->length);
    }
}

static void receive_notification_details_continuation_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data != NULL)
    {
        notification_details_fetcher_on_text_continuation_received(data->value->data, data->length);
    }
}

static void receive_vibrate_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data == NULL)
    {
        return;
    }

    static uint32_t segments[100];
    uint16_t num_segments = data->length / 2;
    if (num_segments > ARRAY_LENGTH(segments))
    {
        num_segments = ARRAY_LENGTH(segments);
    }

    for (int i = 0; i < num_segments; i++)
    {
        segments[i] = read_uint16_from_byte_array(data->value->data, i * 2);
    }

    window_notification_ui_play_or_defer_vibration(segments, num_segments);
}

static void receive_submenu_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data != NULL)
    {
        window_notification_action_list_receive_submenu(data->value->data, data->length);
    }
}

static void receive_image_packet(const DictionaryIterator* iterator)
{
    const Tuple* data = data_tuple(iterator, 1);
    if (data != NULL)
    {
        window_image_show(data->value->data, data->length);
    }
}

static void receive_reset_watch_mirror_packet()
{
    notification_details_fetcher_reset();
    bucket_sync_forget_buckets_from(2);
}

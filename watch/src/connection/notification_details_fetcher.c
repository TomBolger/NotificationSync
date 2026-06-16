#include "notification_details_fetcher.h"

#include "packets.h"
#include "commons/connection/bluetooth.h"
#include "commons/connection/bucket_sync.h"
#include "ui/window_notification/data_loading.h"

static void (*change_callback)() = NULL;
static bool is_fetching = false;

static int16_t current_notification_to_fetch = -1;
static int16_t next_notification_to_fetch = -1;

static void on_sending_finished(const bool success);
static void finish_fetching_if_complete(bool complete);
static void fetch_notification_details(uint8_t bucket_id, bool user_requested);

void notification_details_fetcher_fetch(const uint8_t bucket_id)
{
    fetch_notification_details(bucket_id, true);
}

void notification_details_fetcher_prefetch(const uint8_t bucket_id)
{
    fetch_notification_details(bucket_id, false);
}

static void fetch_notification_details(const uint8_t bucket_id, const bool user_requested)
{
    if (close_after_sync)
    {
        // Disable notification details fetching on momentary sync open
        return;
    }

    if (is_fetching)
    {
        if (current_notification_to_fetch != bucket_id)
        {
            if (user_requested)
            {
                next_notification_to_fetch = bucket_id;
            }
            else if (next_notification_to_fetch < 0)
            {
                next_notification_to_fetch = bucket_id;
            }
        }
        return;
    }

    current_notification_to_fetch = bucket_id;
    const bool success = send_notification_opened(bucket_id);

    if (!success)
    {
        current_notification_to_fetch = -1;
        if (user_requested)
        {
            next_notification_to_fetch = bucket_id;
        }
        bluetooth_register_sending_finish(on_sending_finished);
        return;
    }


    if (!is_phone_connected)
    {
        // If the phone got disconnected, it will not react to the fetch request.
        // Let this message through anyway to restore the connection and then re-send the fetch request
        // after sync init is sent
        next_notification_to_fetch = bucket_id;
    }

    is_fetching = true;
    if (change_callback != NULL)
    {
        change_callback();
    }
}

void notification_details_fetcher_on_text_received(const uint8_t* data, const size_t data_size)
{
    const uint8_t bucket_id = data[0];

    window_notification_data_receive_more_text(bucket_id, &data[1], data_size - 1);
    finish_fetching_if_complete(true);
}

void notification_details_fetcher_on_text_received_v2(const uint8_t* data, const size_t data_size)
{
    finish_fetching_if_complete(window_notification_data_receive_more_text_v2(data, data_size));
}

void notification_details_fetcher_on_text_continuation_received(const uint8_t* data, const size_t data_size)
{
    finish_fetching_if_complete(window_notification_data_receive_more_text_v2_continuation(data, data_size));
}

static void finish_fetching_if_complete(const bool complete)
{
    if (!complete)
    {
        return;
    }

    is_fetching = false;
    current_notification_to_fetch = -1;
    if (change_callback != NULL)
    {
        change_callback();
    }

    if (next_notification_to_fetch >= 0 && is_phone_connected)
    {
        const uint8_t local_next_notification_to_fetch = next_notification_to_fetch;
        next_notification_to_fetch = -1;
        fetch_notification_details(local_next_notification_to_fetch, true);
    }
}

static void on_sending_finished(const bool success)
{
    if (success && next_notification_to_fetch >= 0)
    {
        if (is_phone_connected)
        {
            const uint8_t local_next_notification_to_fetch = next_notification_to_fetch;
            next_notification_to_fetch = -1;
            fetch_notification_details(local_next_notification_to_fetch, true);
        }
        else
        {
            // After phone reconnects, we will send down sync init packet
            // After this packet is sent, we will receive sending finished, so we can
            bluetooth_register_sending_finish(on_sending_finished);
        }
    }
}

void notification_details_fetcher_init()
{
}

bool notification_details_fetcher_is_fetching()
{
    return is_fetching;
}

void notification_details_fetcher_register_fetching_status_callback(void (*callback)())
{
    change_callback = callback;
}

void notification_details_fetcher_unregister_fetching_status_callback(void (*callback)())
{
    if (change_callback == callback)
    {
        change_callback = NULL;
    }
}

#include "notification_details_fetcher.h"

#include <pebble.h>
#include <stdlib.h>

#include "packets.h"
#include "commons/bytes.h"
#include "commons/connection/bluetooth.h"
#include "commons/connection/bucket_sync.h"
#include "ui/window_notification/notification_store.h"

#define QUEUE_SIZE 6
#define REQUEST_TIMEOUT_MS 12000
#define SEND_RETRY_MS 250
#define MAX_ATTEMPTS 3

typedef struct
{
    uint8_t bucket_id;
    bool urgent;
    uint8_t attempts;
} FetchRequest;

static void (*change_callback)() = NULL;

static FetchRequest in_flight;
static bool has_in_flight = false;
static FetchRequest queue[QUEUE_SIZE];
static uint8_t queue_length = 0;
static AppTimer* timer = NULL;

// Notifications the user looked at whose read state still has to reach the phone.
#define MARK_READ_SLOTS 6
static uint8_t mark_read_pending[MARK_READ_SLOTS];
static uint8_t mark_read_count = 0;

// Multi-packet (v2) details are assembled here until the last chunk arrives.
static uint8_t staging_bucket = 0;
static char* staging_body = NULL;
static size_t staging_size = 0;
static Action staging_actions[MAX_NOTIFICATION_ACTIONS];
static uint8_t staging_num_actions = 0;
static uint8_t staging_total_chunks = 0;
static uint8_t staging_next_chunk = 0;
static void reset_staging(void);
static bool mark_read_in_flight;
static uint8_t mark_read_in_flight_bucket;

static void pump(void);

static void notify_status(void)
{
    if (change_callback != NULL)
    {
        change_callback();
    }
}

static void cancel_timer(void)
{
    if (timer != NULL)
    {
        app_timer_cancel(timer);
        timer = NULL;
    }
}

static void on_timer(void* context)
{
    (void)context;
    timer = NULL;

    if (has_in_flight)
    {
        // The phone did not answer. Put the request back (it will be retried) unless we gave up on it.
        has_in_flight = false;
        reset_staging();
        if (in_flight.attempts < MAX_ATTEMPTS && queue_length < QUEUE_SIZE)
        {
            memmove(&queue[1], &queue[0], sizeof(FetchRequest) * queue_length);
            queue[0] = in_flight;
            queue_length++;
        }
        else
        {
            // The phone has no more text for it (e.g. the notification changed under us): stop waiting for it.
            notification_store_on_details_unavailable(in_flight.bucket_id);
        }
        notify_status();
    }

    pump();
}

static int8_t find_queued(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < queue_length; i++)
    {
        if (queue[i].bucket_id == bucket_id)
        {
            return i;
        }
    }
    return -1;
}

static void remove_queued(const uint8_t index)
{
    memmove(&queue[index], &queue[index + 1], sizeof(FetchRequest) * (queue_length - index - 1));
    queue_length--;
}

static void on_mark_read_sent(const bool success)
{
    mark_read_in_flight = false;
    if (success)
    {
        for (uint8_t i = 0; i < mark_read_count; i++)
        {
            if (mark_read_pending[i] == mark_read_in_flight_bucket)
            {
                memmove(&mark_read_pending[i], &mark_read_pending[i + 1], mark_read_count - i - 1);
                mark_read_count--;
                break;
            }
        }
    }
    if (timer == NULL) timer = app_timer_register(SEND_RETRY_MS, on_timer, NULL);
}

static void pump(void)
{
    if (has_in_flight || mark_read_in_flight || close_after_sync)
    {
        return;
    }

    if (queue_length == 0)
    {
        if (mark_read_count == 0)
        {
            return;
        }
        if (is_phone_connected && !is_currently_sending_data)
        {
            mark_read_in_flight_bucket = mark_read_pending[0];
            mark_read_in_flight = true;
            if (!send_mark_read(mark_read_in_flight_bucket, on_mark_read_sent)) mark_read_in_flight = false;
        }
        if (mark_read_count > 0 && timer == NULL)
        {
            timer = app_timer_register(SEND_RETRY_MS, on_timer, NULL);
        }
        return;
    }

    if (!is_phone_connected || is_currently_sending_data)
    {
        // Try again shortly. Reconnects also re-send the welcome, which re-triggers fetching.
        if (timer == NULL)
        {
            timer = app_timer_register(SEND_RETRY_MS, on_timer, NULL);
        }
        return;
    }

    FetchRequest request = queue[0];
    if (!send_notification_opened(request.bucket_id, !request.urgent))
    {
        if (timer == NULL)
        {
            timer = app_timer_register(SEND_RETRY_MS, on_timer, NULL);
        }
        return;
    }

    remove_queued(0);
    request.attempts++;
    in_flight = request;
    has_in_flight = true;
    cancel_timer();
    timer = app_timer_register(REQUEST_TIMEOUT_MS, on_timer, NULL);
    notify_status();
}

static void enqueue(const uint8_t bucket_id, const bool urgent)
{
    if (bucket_id == 0)
    {
        return;
    }

    if (has_in_flight && in_flight.bucket_id == bucket_id)
    {
        in_flight.urgent = in_flight.urgent || urgent;
        return;
    }

    const int8_t existing = find_queued(bucket_id);
    if (existing >= 0)
    {
        if (!urgent || existing == 0)
        {
            return;
        }
        remove_queued(existing);
    }

    if (urgent && has_in_flight && !in_flight.urgent)
    {
        // Something the user is waiting for beats a speculative prefetch. Park the prefetch; if its answer
        // still arrives it is simply stored as unsolicited details.
        has_in_flight = false;
        cancel_timer();
        if (queue_length < QUEUE_SIZE)
        {
            in_flight.attempts = 0;
            queue[queue_length++] = in_flight;
        }
    }

    if (urgent)
    {
        if (queue_length == QUEUE_SIZE)
        {
            queue_length--;
        }
        memmove(&queue[1], &queue[0], sizeof(FetchRequest) * queue_length);
        queue[0] = (FetchRequest){.bucket_id = bucket_id, .urgent = true, .attempts = 0};
        queue_length++;
    }
    else if (queue_length < QUEUE_SIZE)
    {
        queue[queue_length++] = (FetchRequest){.bucket_id = bucket_id, .urgent = false, .attempts = 0};
    }

    pump();
}

void notification_details_fetcher_fetch(const uint8_t bucket_id)
{
    enqueue(bucket_id, true);
}

void notification_details_fetcher_prefetch(const uint8_t bucket_id)
{
    enqueue(bucket_id, false);
}

void notification_details_fetcher_cancel(const uint8_t bucket_id)
{
    const int8_t index = find_queued(bucket_id);
    if (index >= 0)
    {
        remove_queued(index);
    }
    if (has_in_flight && in_flight.bucket_id == bucket_id)
    {
        has_in_flight = false;
        cancel_timer();
        notify_status();
        pump();
    }
}

void notification_details_fetcher_mark_read(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < mark_read_count; i++)
    {
        if (mark_read_pending[i] == bucket_id)
        {
            return;
        }
    }
    if (mark_read_count == MARK_READ_SLOTS)
    {
        memmove(&mark_read_pending[0], &mark_read_pending[1], MARK_READ_SLOTS - 1);
        mark_read_count--;
    }
    mark_read_pending[mark_read_count++] = bucket_id;
    pump();
}

void notification_details_fetcher_reset(void)
{
    queue_length = 0;
    has_in_flight = false;
    reset_staging();
    cancel_timer();
    notify_status();
}

static void on_details_complete(const uint8_t bucket_id, const char* body, const size_t body_size,
                                const Action* actions, const uint8_t num_actions)
{
    notification_store_on_details_received(bucket_id, body, body_size, actions, num_actions);

    if (has_in_flight && in_flight.bucket_id == bucket_id)
    {
        has_in_flight = false;
        cancel_timer();
        notify_status();
    }
    else
    {
        // Unsolicited (preloaded by the phone): no need to ask for it again.
        const int8_t index = find_queued(bucket_id);
        if (index >= 0)
        {
            remove_queued(index);
        }
    }

    pump();
}

// Parses "number of actions, [id, text\0]..., icon length (uint16), icon bytes" starting at position.
// Returns position of the body text or 0 if the payload is malformed.
static size_t parse_actions_and_icon(const uint8_t* data, const size_t data_size, size_t position,
                                     Action* actions, uint8_t* num_actions_out)
{
    if (position >= data_size)
    {
        return 0;
    }

    const uint8_t num_actions = data[position++];
    uint8_t stored = 0;
    for (uint8_t i = 0; i < num_actions; i++)
    {
        if (position >= data_size)
        {
            return 0;
        }
        const uint8_t action_id = data[position++];
        size_t length = 0;
        while (position + length < data_size && data[position + length] != '\0')
        {
            length++;
        }
        if (position + length >= data_size)
        {
            return 0;
        }
        if (stored < MAX_NOTIFICATION_ACTIONS)
        {
            const size_t copy = length < MAX_NOTIFICATION_ACTION_TEXT - 1 ? length : MAX_NOTIFICATION_ACTION_TEXT - 1;
            memcpy(actions[stored].text, &data[position], copy);
            actions[stored].text[copy] = '\0';
            actions[stored].id = action_id;
            actions[stored].voice = false;
            stored++;
        }
        position += length + 1;
    }

    if (position + 2 > data_size)
    {
        return 0;
    }
    const size_t icon_size = read_uint16_from_byte_array(data, position);
    position += 2;
    if (position + icon_size > data_size)
    {
        return 0;
    }
    position += icon_size;

    *num_actions_out = stored;
    return position;
}

static void reset_staging(void)
{
    free(staging_body);
    staging_body = NULL;
    staging_size = 0;
    staging_bucket = 0;
    staging_num_actions = 0;
    staging_total_chunks = 0;
    staging_next_chunk = 0;
}

static void append_staging(const uint8_t* data, const size_t size)
{
    if (staging_body == NULL)
    {
        return;
    }
    const size_t room = MAX_BODY_TEXT_SIZE - staging_size;
    const size_t to_copy = size < room ? size : room;
    memcpy(&staging_body[staging_size], data, to_copy);
    staging_size += to_copy;
    staging_body[staging_size] = '\0';
}

void notification_details_fetcher_on_text_received(const uint8_t* data, const size_t data_size)
{
    // Legacy single-packet details: bucket id, actions, icon, body.
    if (data_size < 1)
    {
        return;
    }

    static Action actions[MAX_NOTIFICATION_ACTIONS];
    uint8_t num_actions = 0;
    const uint8_t bucket_id = data[0];
    const size_t body_position = parse_actions_and_icon(data, data_size, 1, actions, &num_actions);
    if (body_position == 0)
    {
        return;
    }

    on_details_complete(bucket_id, (const char*)&data[body_position], data_size - body_position, actions,
                        num_actions);
}

void notification_details_fetcher_on_text_received_v2(const uint8_t* data, const size_t data_size)
{
    // bucket id, total chunks, actions, icon, first body chunk.
    if (data_size < 3)
    {
        return;
    }

    const uint8_t bucket_id = data[0];
    const uint8_t total_chunks = data[1] > 0 ? data[1] : 1;

    reset_staging();
    const size_t body_position = parse_actions_and_icon(data, data_size, 2, staging_actions, &staging_num_actions);
    if (body_position == 0)
    {
        return;
    }

    if (total_chunks <= 1)
    {
        on_details_complete(bucket_id, (const char*)&data[body_position], data_size - body_position,
                            staging_actions, staging_num_actions);
        staging_num_actions = 0;
        return;
    }

    staging_body = malloc(MAX_BODY_TEXT_SIZE + 1);
    if (staging_body == NULL)
    {
        // Allocation failure is not a complete message; retain the partial preview and retry later.
        notification_store_on_details_unavailable(bucket_id);
        return;
    }

    staging_bucket = bucket_id;
    staging_total_chunks = total_chunks;
    staging_next_chunk = 1;
    if (has_in_flight && in_flight.bucket_id == bucket_id)
    {
        cancel_timer();
        timer = app_timer_register(REQUEST_TIMEOUT_MS, on_timer, NULL);
    }
    staging_body[0] = '\0';
    append_staging(&data[body_position], data_size - body_position);
}

void notification_details_fetcher_on_text_continuation_received(const uint8_t* data, const size_t data_size)
{
    // bucket id, chunk index, total chunks, body chunk.
    if (data_size < 3 || staging_body == NULL || data[0] != staging_bucket)
    {
        return;
    }

    const uint8_t chunk_index = data[1];
    if (data[2] != staging_total_chunks || chunk_index > staging_next_chunk)
    {
        // Missing or mismatched chunk: never publish a truncated body as complete.
        reset_staging();
        return;
    }
    if (chunk_index < staging_next_chunk) return; // ACK lost: ignore a retransmitted chunk.
    append_staging(&data[3], data_size - 3);
    staging_next_chunk++;
    if (has_in_flight && in_flight.bucket_id == staging_bucket)
    {
        cancel_timer();
        timer = app_timer_register(REQUEST_TIMEOUT_MS, on_timer, NULL);
    }
    if (staging_next_chunk == staging_total_chunks)
    {
        const uint8_t bucket_id = staging_bucket;
        char* body = staging_body;
        const size_t size = staging_size;
        staging_body = NULL;
        on_details_complete(bucket_id, body, size, staging_actions, staging_num_actions);
        free(body);
        reset_staging();
    }
}

void notification_details_fetcher_init()
{
    queue_length = 0;
    has_in_flight = false;
}

bool notification_details_fetcher_is_fetching()
{
    return has_in_flight;
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

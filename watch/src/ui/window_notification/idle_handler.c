#include "idle_handler.h"

#include "data_loading.h"
#include "commons/bytes.h"
#include "commons/connection/bucket_sync.h"
#include "connection/packets.h"

const uint32_t PERIODIC_VIBRATION_PERIOD_MS = 10000;
static const uint16_t DEFAULT_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS = 10;
static const uint8_t CONFIG_FLAGS_INDEX = 0;
static const uint8_t CONFIG_AUTO_CLOSE_SECONDS_INDEX = 1;
static const uint8_t CONFIG_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS_INDEX = 3;
static const uint8_t CONFIG_DEFER_NEW_NOTIFICATIONS_FLAG = 0x04;
static const uint32_t PERIODIC_VIBRATION_SEGMENTS[] = {50};
const VibePattern PERIODIC_VIBRATION_PATTERN = {
    .durations = PERIODIC_VIBRATION_SEGMENTS,
    .num_segments = ARRAY_LENGTH(PERIODIC_VIBRATION_SEGMENTS),
};

bool idle_handler_has_user_interacted_since_app_start = false;
bool idle_handler_has_user_interacted_since_last_vibration = false;
static bool any_notification_vibrated = false;

static AppTimer* auto_close_timer = NULL;
static AppTimer* periodic_vibration_timer = NULL;
static time_t last_user_interaction_time = 0;

static void cancel_auto_close_timer(void)
{
    if (auto_close_timer != NULL)
    {
        app_timer_cancel(auto_close_timer);
        auto_close_timer = NULL;
    }
}

static void cancel_periodic_vibration_timer(void)
{
    if (periodic_vibration_timer != NULL)
    {
        app_timer_cancel(periodic_vibration_timer);
        periodic_vibration_timer = NULL;
    }
}

static void cancel_timers(void)
{
    cancel_auto_close_timer();
    cancel_periodic_vibration_timer();
}

static bool any_notification_wants_periodic_vibration(void)
{
    const BucketList* bucket_list = bucket_sync_get_bucket_list();
    for (int i = 0; i < bucket_list->count; i++)
    {
        const BucketMetadata bucket_metadata = bucket_list->data[i];
        const bool notification_has_periodic_vibration = (bucket_metadata.flags) & 0x04;

        // Special state: After notification shown, it is marked as unread immediately, but the UI is still showing it as
        // unread (since we are not sure if user has seen it yet). In this case, we also have to trigger periodic vibration
        const bool temporary_unread = bucket_metadata.id == window_notification_data.currently_selected_bucket &&
            window_notification_data.dot_states[window_notification_data.currently_selected_bucket_index] == UNREAD;

        if (
            notification_has_periodic_vibration &&
            (temporary_unread || is_notification_unread(bucket_metadata.flags, bucket_metadata.id))
        )
        {
            return true;
        }
    }

    return false;
}

static void maybe_start_periodic_vibration_timer();
static void maybe_start_auto_close_timer();
static bool load_config(uint8_t* config, size_t config_size);

static void handle_periodic_vibration()
{
    vibes_enqueue_custom_pattern(PERIODIC_VIBRATION_PATTERN);
    maybe_start_periodic_vibration_timer();
}

void idle_handler_register_timers()
{
    cancel_timers();

    maybe_start_auto_close_timer();

    if (!idle_handler_has_user_interacted_since_last_vibration)
    {
        maybe_start_periodic_vibration_timer();
    }
}

static void maybe_start_auto_close_timer()
{
    if (launch_reason() != APP_LAUNCH_PHONE)
    {
        return;
    }

    uint8_t config[5];
    if (!load_config(config, sizeof(config)))
    {
        return;
    }

    const uint32_t duration =
        read_uint16_from_byte_array(config, CONFIG_AUTO_CLOSE_SECONDS_INDEX) * 1000;
    if (duration != 0)
    {
        auto_close_timer = app_timer_register(duration, send_close_me, NULL);
    }
}

static void maybe_start_periodic_vibration_timer()
{
    if (any_notification_vibrated &&
        !idle_handler_has_user_interacted_since_last_vibration &&
        any_notification_wants_periodic_vibration()
    )
    {
        periodic_vibration_timer = app_timer_register(PERIODIC_VIBRATION_PERIOD_MS, handle_periodic_vibration, NULL);
    }
    else
    {
        periodic_vibration_timer = NULL;
    }
}


void idle_handler_notify_user_interacted()
{
    idle_handler_has_user_interacted_since_app_start = true;
    idle_handler_has_user_interacted_since_last_vibration = true;
    last_user_interaction_time = time(NULL);

    cancel_timers();
    maybe_start_auto_close_timer();
}

void idle_handler_notify_received_new_vibration()
{
    idle_handler_has_user_interacted_since_last_vibration = false;
    any_notification_vibrated = true;

    idle_handler_register_timers();
}

void idle_handler_notify_notifications_updated()
{
    if (!any_notification_wants_periodic_vibration())
    {
        cancel_periodic_vibration_timer();
    }
}

bool idle_handler_should_keep_current_notification()
{
    return idle_handler_ms_until_current_notification_release() > 0;
}

uint32_t idle_handler_ms_until_current_notification_release()
{
    uint8_t config[5];
    if (!load_config(config, sizeof(config)))
    {
        return 0;
    }

    if ((config[CONFIG_FLAGS_INDEX] & CONFIG_DEFER_NEW_NOTIFICATIONS_FLAG) == 0)
    {
        return 0;
    }

    const uint16_t timeout_seconds = read_uint16_from_byte_array(
        config,
        CONFIG_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS_INDEX
    );
    if (timeout_seconds == 0 || last_user_interaction_time == 0)
    {
        return 0;
    }

    const time_t elapsed_seconds = time(NULL) - last_user_interaction_time;
    if (elapsed_seconds < 0 || elapsed_seconds >= timeout_seconds)
    {
        return 0;
    }

    return (uint32_t)(timeout_seconds - elapsed_seconds) * 1000;
}

static bool load_config(uint8_t* config, const size_t config_size)
{
    memset(config, 0, config_size);
    config[CONFIG_FLAGS_INDEX] = CONFIG_DEFER_NEW_NOTIFICATIONS_FLAG;
    config[CONFIG_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS_INDEX] =
        DEFAULT_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS >> 8;
    config[CONFIG_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS_INDEX + 1] =
        DEFAULT_NEW_NOTIFICATION_INTERACTION_TIMEOUT_SECONDS & 0xff;

    return bucket_sync_load_bucket(1, config);
}

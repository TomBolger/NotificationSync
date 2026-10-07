#pragma once
#include <pebble.h>
#include <stdint.h>

#include "notification_store.h"

/**
 * State shared with the action menu (action_list.c). Everything else about the UI is private to
 * window_notification.c (list + launch flow) and detail_window.c (the notification cards).
 */
typedef struct
{
    bool active;
    bool detail_open;

    uint8_t currently_selected_bucket;

    uint8_t num_actions;
    Action actions[MAX_NOTIFICATION_ACTIONS];
    uint8_t num_submenu_actions;
    Action submenu_actions[MAX_NOTIFICATION_ACTIONS];
    bool menu_displayed;
    uint8_t currently_displayed_menu_id;
    uint8_t open_menu_on_success;
} NotificationWindowData;

extern NotificationWindowData window_notification_data;

void window_notification_show();

/** The phone app answered our hello (so "no notifications" really means none). */
void window_notification_ui_on_phone_answered(void);

/** Phone told us which notification it launched the app for (0 = unknown). */
void window_notification_ui_open_phone_launch_detail(uint8_t bucket_id);

/** Phone asked us to vibrate for a new notification. */
void window_notification_ui_play_or_defer_vibration(const uint32_t* durations, uint32_t num_segments);

/** The user dismissed this notification from the watch and the request reached the phone. */
void window_notification_ui_on_dismiss_sent(uint8_t bucket_id);

/** True when the phone opened the app for a notification (back exits, like a PebbleOS popup). */
bool window_notification_ui_is_popup_session(void);

/** Something that can interrupt (the action menu) closed; deferred work may run now. */
void window_notification_ui_on_menu_closed(void);

GColor window_notification_ui_get_primary_color();

/** Bucket of the alert the user has not acknowledged yet (for periodic reminder vibration), or 0. */
uint8_t window_notification_ui_unacknowledged_alert(void);

void window_notification_ui_on_listener_unavailable(void);

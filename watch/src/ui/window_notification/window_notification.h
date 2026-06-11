#pragma once
#include <pebble.h>
#include <stdint.h>

#include "ui/layers/dots.h"

#define MAX_BODY_TEXT_SIZE PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 1200, 1800, 4000, 1800, 4000, 4000, 4000)
#define MAX_NOTIFICATION_ITEMS 30
#define MAX_NOTIFICATION_TITLE 64
#define MAX_NOTIFICATION_SNIPPET 160
#define MAX_NOTIFICATION_ACTIONS 20
#define MAX_NOTIFICATION_ACTION_TEXT 21

typedef struct
{
    uint8_t id;
    char text[MAX_NOTIFICATION_ACTION_TEXT];
    bool voice;
} Action;

typedef struct
{
    uint8_t bucket_id;
    time_t receive_time;
    uint8_t icon_id;
    uint8_t color_id;
    enum DotState state;
    char app_name[MAX_NOTIFICATION_TITLE];
    char title[MAX_NOTIFICATION_TITLE];
    char body[MAX_NOTIFICATION_SNIPPET];
} NotificationListItem;

typedef struct
{
    bool active;
    bool detail_open;
    bool using_dummy_data;

    uint8_t currently_selected_bucket;
    int16_t currently_selected_bucket_index;
    uint8_t bucket_count;
    enum DotState dot_states[MAX_NOTIFICATION_ITEMS];

    uint8_t icon_id;
    uint8_t color_id;
    char title_text[MAX_NOTIFICATION_TITLE];
    char subtitle_text[MAX_NOTIFICATION_TITLE];
    char body_text[MAX_BODY_TEXT_SIZE + 40];
    time_t receive_time;

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
void window_notification_ui_set_items(const NotificationListItem* items, uint8_t count, bool using_dummy_data);
void window_notification_ui_redraw();
void window_notification_ui_cache_current_body();
void window_notification_ui_cache_body_for_bucket(uint8_t bucket_id, const char* body, size_t body_size);
void window_notification_ui_cache_details_for_bucket(uint8_t bucket_id, const char* body,
                                                     size_t body_size, const Action* actions,
                                                     uint8_t num_actions);
void window_notification_ui_replace_current_body(uint8_t bucket_id, const char* body, size_t body_size);
void window_notification_ui_replace_current_details(uint8_t bucket_id, const char* body,
                                                    size_t body_size, const Action* actions,
                                                    uint8_t num_actions);
void window_notification_ui_on_details_cached(uint8_t bucket_id);
void window_notification_ui_note_actions_updated();
void window_notification_ui_uncache_body_for_bucket(uint8_t bucket_id);
void window_notification_ui_note_bucket_updated(uint8_t bucket_id);
void window_notification_ui_on_bucket_selected();
void window_notification_ui_on_bucket_list_updated();
void window_notification_ui_on_bucket_deleted(uint8_t bucket_id);
void window_notification_ui_open_phone_launch_detail(uint8_t bucket_id);
void window_notification_ui_open_selected_detail();
bool window_notification_ui_should_exit_detail_on_back(void);
void window_notification_ui_close_detail();
void window_notification_ui_scroll_detail_up(ClickRecognizerRef recognizer, void* context);
void window_notification_ui_scroll_detail_down(ClickRecognizerRef recognizer, void* context);
GColor window_notification_ui_get_primary_color();

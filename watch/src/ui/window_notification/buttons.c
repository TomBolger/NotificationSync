#include "buttons.h"

#include "action_list.h"
#include "idle_handler.h"
#include "window_notification.h"
#include "connection/packets.h"

// PebbleOS source: src/fw/services/timeline/swap_layer.c
#define SCROLL_REPEAT_MS 200

static void open_action_list_callback(void* context)
{
    (void)context;
    window_notification_action_list_show();
}

static void button_select_single(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    idle_handler_notify_user_interacted();

    if (!window_notification_data.detail_open)
    {
        window_notification_ui_open_selected_detail();
        return;
    }

    if (window_notification_data.num_actions == 0)
    {
        vibes_double_pulse();
        return;
    }

    if (window_notification_data.menu_displayed)
    {
        window_notification_action_select();
    }
    else
    {
        window_notification_data.currently_displayed_menu_id = 0;
        app_timer_register(1, open_action_list_callback, NULL);
    }
}

static void button_back_single(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    idle_handler_notify_user_interacted();

    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_hide();
    }
    else if (window_notification_data.detail_open)
    {
        if (window_notification_ui_should_exit_detail_on_back())
        {
            send_close_me_without_animation();
        }
        else
        {
            window_notification_ui_close_detail();
        }
    }
    else
    {
        send_close_me();
    }
}

static void button_up_repeating(ClickRecognizerRef recognizer, void* context)
{
    if (!click_recognizer_is_repeating(recognizer))
    {
        return;
    }

    idle_handler_notify_user_interacted();
    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_move_up();
        return;
    }
    if (!window_notification_data.detail_open)
    {
        window_notification_ui_select_relative(-1);
        return;
    }

    window_notification_ui_scroll_detail_up(recognizer, context);
}

static void button_down_repeating(ClickRecognizerRef recognizer, void* context)
{
    if (!click_recognizer_is_repeating(recognizer))
    {
        return;
    }

    idle_handler_notify_user_interacted();
    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_move_down();
        return;
    }
    if (!window_notification_data.detail_open)
    {
        window_notification_ui_select_relative(1);
        return;
    }

    window_notification_ui_scroll_detail_down(recognizer, context);
}

static void button_up_raw(ClickRecognizerRef recognizer, void* context)
{
    idle_handler_notify_user_interacted();
    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_move_up();
        return;
    }
    if (!window_notification_data.detail_open)
    {
        window_notification_ui_select_relative(-1);
        return;
    }

    window_notification_ui_scroll_detail_up(recognizer, context);
}

static void button_down_raw(ClickRecognizerRef recognizer, void* context)
{
    idle_handler_notify_user_interacted();
    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_move_down();
        return;
    }
    if (!window_notification_data.detail_open)
    {
        window_notification_ui_select_relative(1);
        return;
    }

    window_notification_ui_scroll_detail_down(recognizer, context);
}

void window_notification_buttons_config(void* context)
{
    window_raw_click_subscribe(BUTTON_ID_UP, button_up_raw, NULL, context);
    window_single_repeating_click_subscribe(BUTTON_ID_UP, SCROLL_REPEAT_MS, button_up_repeating);
    window_raw_click_subscribe(BUTTON_ID_DOWN, button_down_raw, NULL, context);
    window_single_repeating_click_subscribe(BUTTON_ID_DOWN, SCROLL_REPEAT_MS, button_down_repeating);
    window_single_click_subscribe(BUTTON_ID_SELECT, button_select_single);
    window_single_click_subscribe(BUTTON_ID_BACK, button_back_single);
}

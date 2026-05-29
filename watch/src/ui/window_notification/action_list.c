#include "action_list.h"

#include <pebble.h>

#include "window_notification.h"
#include "commons/connection/bluetooth.h"
#include "connection/packets.h"

typedef struct
{
    uint8_t action_id;
    uint8_t menu_id;
    bool voice;
} MenuActionData;

static ActionMenu* action_menu;
static MenuActionData action_data[20];
static MenuActionData active_voice_action;
static uint8_t active_voice_notification_id;
static bool active_voice_action_valid;

static void send_notification_voice(void);
static void confirm_action(uint8_t notification_id, uint8_t action_id, uint8_t menu_id, const char* text);

static uint16_t get_num_actions(void)
{
    if (window_notification_data.currently_displayed_menu_id == 0)
    {
        return window_notification_data.num_actions;
    }

    return window_notification_data.num_submenu_actions;
}

static Action* get_actions(void)
{
    if (window_notification_data.currently_displayed_menu_id == 0)
    {
        return window_notification_data.actions;
    }

    return window_notification_data.submenu_actions;
}

static void close_current_menu(const bool animated)
{
    if (action_menu == NULL)
    {
        window_notification_data.menu_displayed = false;
        return;
    }

    action_menu_close(action_menu, animated);
}

static void on_action_menu_closed(ActionMenu* menu, const ActionMenuItem* performed_action, void* context)
{
    ActionMenuLevel* root_level = action_menu_get_root_level(menu);
    if (root_level != NULL)
    {
        action_menu_hierarchy_destroy(root_level, NULL, NULL);
    }

    if (action_menu == menu)
    {
        action_menu = NULL;
        window_notification_data.menu_displayed = false;
    }
}

static void on_sending_finished(const bool success)
{
    if (success)
    {
        if (window_notification_data.open_menu_on_success != 0)
        {
            close_current_menu(false);
            window_notification_data.currently_displayed_menu_id = window_notification_data.open_menu_on_success;
            window_notification_data.open_menu_on_success = 0;
            window_notification_action_list_show();
        }
        else
        {
            close_current_menu(true);
        }
    }
    else
    {
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        vibes_double_pulse();
    }
}

static void action_selected(ActionMenu* menu, const ActionMenuItem* item, void* context)
{
    MenuActionData* data = action_menu_item_get_action_data(item);
    if (data == NULL)
    {
        vibes_double_pulse();
        return;
    }

    if (data->voice)
    {
        active_voice_action = *data;
        active_voice_notification_id = window_notification_data.currently_selected_bucket;
        active_voice_action_valid = true;
        action_menu_freeze(menu);
        send_notification_voice();
        return;
    }

    confirm_action(window_notification_data.currently_selected_bucket, data->action_id, data->menu_id, NULL);
}

void window_notification_action_list_init(const Window* window)
{
    action_menu = NULL;
    active_voice_notification_id = 0;
    active_voice_action_valid = false;
}

void window_notification_action_list_deinit()
{
    if (action_menu != NULL)
    {
        action_menu_close(action_menu, false);
        action_menu = NULL;
    }

    active_voice_action_valid = false;
    active_voice_notification_id = 0;
    window_notification_data.menu_displayed = false;
}

void window_notification_action_list_show()
{
    if (get_num_actions() == 0)
    {
        vibes_double_pulse();
        return;
    }

    if (action_menu != NULL)
    {
        action_menu_close(action_menu, false);
        action_menu = NULL;
    }

    const uint16_t num_actions = get_num_actions();
    ActionMenuLevel* root_level = action_menu_level_create(num_actions);
    Action* actions = get_actions();

    for (uint16_t i = 0; i < num_actions; i++)
    {
        action_data[i].action_id = actions[i].id;
        action_data[i].menu_id = window_notification_data.currently_displayed_menu_id;
        action_data[i].voice = actions[i].voice;
        action_menu_level_add_action(root_level, actions[i].text, action_selected, &action_data[i]);
    }

    ActionMenuConfig config = {
        .root_level = root_level,
        .context = NULL,
        .colors = {
            .background = window_notification_ui_get_primary_color(),
            .foreground = GColorWhite,
        },
        .will_close = NULL,
        .did_close = on_action_menu_closed,
        .align = ActionMenuAlignCenter,
    };

    window_notification_data.menu_displayed = true;
    action_menu = action_menu_open(&config);
}

void window_notification_action_list_hide()
{
    close_current_menu(true);
}

void window_notification_action_list_move_up()
{
}

void window_notification_action_list_move_down()
{
}

void window_notification_action_select()
{
}

static void confirm_action(const uint8_t notification_id, const uint8_t action_id, const uint8_t menu_id, const char* text)
{
    if (!send_action_trigger(notification_id, action_id, menu_id, text))
    {
        vibes_double_pulse();
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        return;
    }

    bluetooth_register_sending_finish(on_sending_finished);
    if (action_menu != NULL)
    {
        action_menu_freeze(action_menu);
    }
}

static void voice_callback(DictationSession* session, DictationSessionStatus status, char* transcription, void* context)
{
    if (status == DictationSessionStatusSuccess && active_voice_action_valid)
    {
        confirm_action(
            active_voice_notification_id,
            active_voice_action.action_id,
            active_voice_action.menu_id,
            transcription
        );
    }
    else if (action_menu != NULL)
    {
        action_menu_unfreeze(action_menu);
    }

    active_voice_action_valid = false;
    active_voice_notification_id = 0;
    dictation_session_destroy(session);
}

static void send_notification_voice()
{
    DictationSession* session = dictation_session_create(300, voice_callback, NULL);

    if (session == NULL)
    {
        active_voice_action_valid = false;
        active_voice_notification_id = 0;
        vibes_double_pulse();
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        return;
    }

    const DictationSessionStatus start_status = dictation_session_start(session);

    if (start_status != DictationSessionStatusSuccess)
    {
        active_voice_action_valid = false;
        active_voice_notification_id = 0;
        vibes_double_pulse();
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        dictation_session_destroy(session);
    }
}

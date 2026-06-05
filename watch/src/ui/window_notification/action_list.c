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
static Layer* inline_action_layer;
static MenuActionData action_data[MAX_NOTIFICATION_ACTIONS];
static MenuActionData active_voice_action;
static uint8_t active_voice_notification_id;
static bool active_voice_action_valid;
static bool action_send_in_progress;
static uint16_t selected_action_index;

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
    (void)animated;

    if (inline_action_layer != NULL)
    {
        layer_destroy(inline_action_layer);
        inline_action_layer = NULL;
        window_notification_data.menu_displayed = false;
        return;
    }

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
    action_send_in_progress = false;
    if (success)
    {
        if (window_notification_data.open_menu_on_success != 0)
        {
            window_notification_data.currently_displayed_menu_id = window_notification_data.open_menu_on_success;
            window_notification_data.open_menu_on_success = 0;
            if (inline_action_layer != NULL)
            {
                selected_action_index = 0;
                layer_mark_dirty(inline_action_layer);
                return;
            }

            close_current_menu(false);
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
    if (action_send_in_progress)
    {
        return;
    }

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

static bool should_use_menu_layer_actions(void)
{
    return PBL_PLATFORM_TYPE_CURRENT == PlatformTypeBasalt;
}

static void select_action_data(const MenuActionData data)
{
    if (data.voice)
    {
        const bool was_inline_action = inline_action_layer != NULL;
        if (was_inline_action)
        {
            close_current_menu(false);
        }
        active_voice_action = data;
        active_voice_notification_id = window_notification_data.currently_selected_bucket;
        active_voice_action_valid = true;
        action_send_in_progress = true;
        send_notification_voice();
        return;
    }

    confirm_action(window_notification_data.currently_selected_bucket, data.action_id, data.menu_id, NULL);
}

static void inline_action_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    const uint16_t num_actions = get_num_actions();
    Action* actions = get_actions();
    const int16_t header_height = 26;
    const int16_t row_height = 28;
    int16_t visible_rows = (bounds.size.h - header_height) / row_height;
    if (visible_rows < 1)
    {
        visible_rows = 1;
    }
    uint16_t first_row = 0;

    if (selected_action_index >= visible_rows)
    {
        first_row = selected_action_index - visible_rows + 1;
    }

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx,
                       "Actions",
                       fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                       GRect(0, 2, bounds.size.w, header_height),
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter,
                       NULL);

    for (uint16_t visible_index = 0; visible_index < visible_rows; visible_index++)
    {
        const uint16_t action_index = first_row + visible_index;
        if (action_index >= num_actions)
        {
            break;
        }

        const GRect row = GRect(0, header_height + (visible_index * row_height), bounds.size.w, row_height);
        if (action_index == selected_action_index)
        {
            graphics_context_set_fill_color(ctx, GColorBlack);
            graphics_fill_rect(ctx, row, 0, GCornerNone);
            graphics_context_set_text_color(ctx, GColorWhite);
        }
        else
        {
            graphics_context_set_text_color(ctx, GColorBlack);
        }

        graphics_draw_text(ctx,
                           actions[action_index].text,
                           fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
                           GRect(row.origin.x + 6, row.origin.y - 2, row.size.w - 12, row.size.h),
                           GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft,
                           NULL);
    }
}

static bool show_inline_actions(void)
{
    Window* window = window_stack_get_top_window();
    if (window == NULL)
    {
        return false;
    }

    Layer* window_layer = window_get_root_layer(window);
    if (window_layer == NULL)
    {
        return false;
    }

    inline_action_layer = layer_create(layer_get_bounds(window_layer));
    if (inline_action_layer == NULL)
    {
        return false;
    }

    layer_set_update_proc(inline_action_layer, inline_action_layer_update);
    layer_add_child(window_layer, inline_action_layer);
    selected_action_index = 0;
    window_notification_data.menu_displayed = true;
    action_send_in_progress = false;
    return true;
}

void window_notification_action_list_init(const Window* window)
{
    action_menu = NULL;
    inline_action_layer = NULL;
    active_voice_notification_id = 0;
    active_voice_action_valid = false;
    action_send_in_progress = false;
    selected_action_index = 0;
}

void window_notification_action_list_deinit()
{
    close_current_menu(false);

    if (action_menu != NULL)
    {
        action_menu_close(action_menu, false);
        action_menu = NULL;
    }

    active_voice_action_valid = false;
    active_voice_notification_id = 0;
    action_send_in_progress = false;
    selected_action_index = 0;
    window_notification_data.menu_displayed = false;
}

void window_notification_action_list_show()
{
    if (get_num_actions() == 0)
    {
        vibes_double_pulse();
        return;
    }

    if (should_use_menu_layer_actions())
    {
        if (inline_action_layer != NULL)
        {
            layer_mark_dirty(inline_action_layer);
            return;
        }
        if (!show_inline_actions())
        {
            window_notification_data.menu_displayed = false;
            vibes_double_pulse();
        }
        return;
    }

    if (action_menu != NULL)
    {
        action_menu_close(action_menu, false);
        action_menu = NULL;
    }

    const uint16_t num_actions = get_num_actions();
    ActionMenuLevel* root_level = action_menu_level_create(num_actions);
    if (root_level == NULL)
    {
        window_notification_data.menu_displayed = false;
        vibes_double_pulse();
        return;
    }
    Action* actions = get_actions();

    for (uint16_t i = 0; i < num_actions; i++)
    {
        action_data[i].action_id = actions[i].id;
        action_data[i].menu_id = window_notification_data.currently_displayed_menu_id;
        action_data[i].voice = actions[i].voice;
        if (!action_menu_level_add_action(root_level, actions[i].text, action_selected, &action_data[i]))
        {
            action_menu_hierarchy_destroy(root_level, NULL, NULL);
            window_notification_data.menu_displayed = false;
            vibes_double_pulse();
            return;
        }
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
    if (action_menu == NULL)
    {
        action_menu_hierarchy_destroy(root_level, NULL, NULL);
        window_notification_data.menu_displayed = false;
        vibes_double_pulse();
    }
}

void window_notification_action_list_hide()
{
    close_current_menu(true);
}

void window_notification_action_list_move_up()
{
    if (inline_action_layer == NULL || action_send_in_progress)
    {
        return;
    }

    if (selected_action_index > 0)
    {
        selected_action_index--;
        layer_mark_dirty(inline_action_layer);
    }
    else
    {
        vibes_short_pulse();
    }
}

void window_notification_action_list_move_down()
{
    if (inline_action_layer == NULL || action_send_in_progress)
    {
        return;
    }

    if (selected_action_index + 1 < get_num_actions())
    {
        selected_action_index++;
        layer_mark_dirty(inline_action_layer);
    }
    else
    {
        vibes_short_pulse();
    }
}

void window_notification_action_select()
{
    if (inline_action_layer == NULL || action_send_in_progress || selected_action_index >= get_num_actions())
    {
        return;
    }

    Action* actions = get_actions();
    select_action_data((MenuActionData){
        .action_id = actions[selected_action_index].id,
        .menu_id = window_notification_data.currently_displayed_menu_id,
        .voice = actions[selected_action_index].voice,
    });
}

static void confirm_action(const uint8_t notification_id, const uint8_t action_id, const uint8_t menu_id, const char* text)
{
    action_send_in_progress = true;
    if (!send_action_trigger(notification_id, action_id, menu_id, text))
    {
        action_send_in_progress = false;
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
        action_send_in_progress = false;
        action_menu_unfreeze(action_menu);
    }
    else
    {
        action_send_in_progress = false;
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
        action_send_in_progress = false;
        vibes_double_pulse();
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        else if (!window_notification_data.menu_displayed)
        {
            window_notification_action_list_show();
        }
        return;
    }

    const DictationSessionStatus start_status = dictation_session_start(session);

    if (start_status != DictationSessionStatusSuccess)
    {
        active_voice_action_valid = false;
        active_voice_notification_id = 0;
        action_send_in_progress = false;
        vibes_double_pulse();
        if (action_menu != NULL)
        {
            action_menu_unfreeze(action_menu);
        }
        else if (!window_notification_data.menu_displayed)
        {
            window_notification_action_list_show();
        }
        dictation_session_destroy(session);
    }
}

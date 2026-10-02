#include "window_notification.h"

#include <pebble.h>

#include "action_list.h"
#include "card.h"
#include "detail_window.h"
#include "idle_handler.h"
#include "notification_store.h"
#include "connection/packets.h"
#include "commons/connection/bucket_sync.h"

#define DEFAULT_LIST_HIGHLIGHT GColorFolly
#define LIST_CELL_HEIGHT 46
#define MAX_DEFERRED_VIBE_SEGMENTS 16
// If the phone never answers a launch, stop showing the splash and show what we have.
#define PHONE_LAUNCH_TIMEOUT_MS 6000
// How long to retry an alert that is waiting for the user to stop interacting.
#define ALERT_RETRY_MS 500

NotificationWindowData window_notification_data = {
    .active = false,
    .detail_open = false,
};

static Window* list_window;
static MenuLayer* menu_layer;
static Layer* empty_layer;
static Layer* splash_layer;

static uint8_t list_selected_bucket;

// Launch by the phone for a specific notification behaves like a PebbleOS notification popup: the list stays
// hidden and back exits the app.
static bool popup_session;
static bool waiting_for_launch_target;
static uint8_t launch_target_bucket;
static AppTimer* launch_timeout_timer;

// Whether the phone app has answered our hello. Without it the list can only ever be empty or stale.
#define PHONE_ANSWER_TIMEOUT_MS 4000
typedef enum
{
    PhoneStateWaiting,
    PhoneStateAnswered,
    PhoneStateUnreachable,
} PhoneState;
static PhoneState phone_state = PhoneStateWaiting;
static AppTimer* phone_timeout_timer;

static uint32_t deferred_vibration[MAX_DEFERRED_VIBE_SEGMENTS];
static uint32_t deferred_vibration_segments;

// Notifications whose content changed in the most recent sync, newest first as the list orders them.
static uint8_t last_changed[STORE_MAX_ITEMS + 1];
static uint8_t last_changed_count;

static uint8_t pending_alert_bucket;
static AppTimer* pending_alert_timer;
static uint8_t unacknowledged_alert_bucket;
static uint8_t exit_when_gone_bucket;

static char app_glance_subtitle[151];

static void open_detail(uint8_t bucket_id, bool animated);

// ------------------------------------------------------------------------------------------------ helpers

static bool launched_by_phone(void)
{
    return launch_reason() == APP_LAUNCH_PHONE;
}

static int16_t selected_index(void)
{
    const int16_t index = notification_store_index_of(list_selected_bucket);
    return index >= 0 ? index : 0;
}

GColor window_notification_ui_get_primary_color()
{
    const NotificationItem* item = notification_store_item_by_bucket(window_notification_data.currently_selected_bucket);
    return card_color_for_id(item != NULL ? item->color_id : 0);
}

uint8_t window_notification_ui_unacknowledged_alert(void)
{
    return unacknowledged_alert_bucket;
}

static void play_vibration(const uint32_t* durations, const uint32_t num_segments)
{
    if (durations == NULL || num_segments == 0)
    {
        return;
    }

    const VibePattern pattern = {
        .durations = (uint32_t*)durations,
        .num_segments = num_segments,
    };
    vibes_cancel();
    vibes_enqueue_custom_pattern(pattern);
    idle_handler_notify_received_new_vibration();
}

static void flush_deferred_vibration(void)
{
    if (deferred_vibration_segments == 0)
    {
        return;
    }
    const uint32_t segments = deferred_vibration_segments;
    deferred_vibration_segments = 0;
    play_vibration(deferred_vibration, segments);
}

static void update_layer_visibility(void)
{
    if (list_window == NULL)
    {
        return;
    }

    const bool show_splash = waiting_for_launch_target || (popup_session && !detail_window_is_open());
    layer_set_hidden(splash_layer, !show_splash);
    layer_set_hidden(menu_layer_get_layer(menu_layer), show_splash);
    layer_set_hidden(empty_layer, show_splash || notification_store_count() != 0);
    layer_mark_dirty(empty_layer);
}

// ------------------------------------------------------------------------------------------------ app glance

static void app_glance_reload_callback(AppGlanceReloadSession* session, size_t limit, void* context)
{
    (void)context;
    if (limit == 0 || app_glance_subtitle[0] == '\0')
    {
        return;
    }

    const AppGlanceSlice slice = {
        .layout = {
            .icon = APP_GLANCE_SLICE_DEFAULT_ICON,
            .subtitle_template_string = app_glance_subtitle,
        },
        .expiration_time = APP_GLANCE_SLICE_NO_EXPIRATION,
    };
    app_glance_add_slice(session, slice);
}

static void update_app_glance(void)
{
    char subtitle[sizeof(app_glance_subtitle)];
    subtitle[0] = '\0';

    const NotificationItem* top = notification_store_item(0);
    if (top != NULL && top->loaded)
    {
        // PebbleOS source: launcher/default/app_glance_notifications.c
        const char* text = top->title[0] != '\0' ? top->title : (top->summary[0] != '\0' ? top->summary : top->app_name);
        strncpy(subtitle, text, sizeof(subtitle) - 1);
        subtitle[sizeof(subtitle) - 1] = '\0';
        for (char* c = subtitle; *c != '\0'; c++)
        {
            if (*c == '\n' || *c == '\r')
            {
                *c = ' ';
            }
        }
    }

    if (strcmp(subtitle, app_glance_subtitle) == 0)
    {
        return;
    }
    strcpy(app_glance_subtitle, subtitle);
    app_glance_reload(app_glance_reload_callback, NULL);
}

// ------------------------------------------------------------------------------------------------ list window

static uint16_t get_num_rows(MenuLayer* layer, uint16_t section, void* context)
{
    (void)layer;
    (void)section;
    (void)context;
    return notification_store_count();
}

static int16_t get_cell_height(MenuLayer* layer, MenuIndex* index, void* context)
{
    (void)layer;
    (void)index;
    (void)context;
    return LIST_CELL_HEIGHT;
}

// PebbleOS source: src/fw/apps/system/notifications.c (prv_draw_notification_cell_rect)
static void draw_row(GContext* ctx, const Layer* cell_layer, MenuIndex* index, void* context)
{
    (void)context;
    const NotificationItem* item = notification_store_item(index->row);
    if (item == NULL)
    {
        return;
    }

    const GRect bounds = layer_get_bounds(cell_layer);
    const int16_t inset = 5;

    GDrawCommandImage* icon = card_icon_for_id(item->icon_id);
    GSize icon_size = GSize(0, 0);
    if (icon != NULL)
    {
        icon_size = gdraw_command_image_get_bounds_size(icon);
        GRect box = bounds;
        box.origin.x += inset;
        GRect icon_rect = {.size = icon_size};
        grect_align(&icon_rect, &box, GAlignLeft, false);
        gdraw_command_image_draw(ctx, icon, icon_rect.origin);
    }

    // PebbleOS: notifications with the generic icon are titled with the app name.
    const char* title = item->title;
    if (item->icon_id == 0 && item->app_name[0] != '\0')
    {
        title = item->app_name;
    }
    if (!item->loaded)
    {
        title = "Loading";
    }
    const char* subtitle = card_body_text(item, item->summary);
    if (title[0] == '\0')
    {
        title = subtitle[0] != '\0' ? subtitle : "[Empty]";
        subtitle = NULL;
    }

    const int16_t text_left = inset + (icon_size.w > 25 ? icon_size.w : 25);
    const GRect text_box = grect_inset(bounds, GEdgeInsets(0, 5, 0, text_left));
    const GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    const GFont subtitle_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
    const int16_t title_height = 24;
    const int16_t subtitle_height = subtitle != NULL ? 14 : 0;
    const int16_t top = text_box.origin.y + (text_box.size.h - (title_height + subtitle_height + 10)) / 2;

    graphics_draw_text(ctx, title, title_font, GRect(text_box.origin.x, top, text_box.size.w, title_height + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    if (subtitle != NULL)
    {
        graphics_draw_text(ctx, subtitle, subtitle_font,
                           GRect(text_box.origin.x, top + title_height, text_box.size.w, subtitle_height + 4),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
}

static void select_click(MenuLayer* layer, MenuIndex* index, void* context)
{
    (void)layer;
    (void)context;
    idle_handler_notify_user_interacted();
    const NotificationItem* item = notification_store_item(index->row);
    if (item != NULL)
    {
        list_selected_bucket = item->bucket_id;
        open_detail(item->bucket_id, true);
    }
}

static void selection_changed(MenuLayer* layer, MenuIndex new_index, MenuIndex old_index, void* context)
{
    (void)layer;
    (void)old_index;
    (void)context;
    idle_handler_notify_user_interacted();
    const NotificationItem* item = notification_store_item(new_index.row);
    if (item != NULL)
    {
        list_selected_bucket = item->bucket_id;
        notification_store_want_details(item->bucket_id, false);
    }
}

static void sync_list_selection(void)
{
    if (menu_layer == NULL)
    {
        return;
    }
    menu_layer_reload_data(menu_layer);
    if (notification_store_count() > 0)
    {
        const int16_t index = selected_index();
        list_selected_bucket = notification_store_item(index)->bucket_id;
        menu_layer_set_selected_index(menu_layer, MenuIndex(0, index), MenuRowAlignCenter, false);
    }
}

static void draw_unreachable_phone_icon(GContext* ctx, const GPoint center)
{
    // A phone outline with a slash through it, drawn in the 2px stroke style of the PebbleOS system icons.
    graphics_context_set_stroke_color(ctx, GColorBlack);
    graphics_context_set_stroke_width(ctx, 3);
    const GRect phone = GRect(center.x - 13, center.y - 21, 26, 42);
    graphics_draw_round_rect(ctx, phone, 4);
    graphics_draw_line(ctx, GPoint(phone.origin.x + 9, phone.origin.y + phone.size.h - 7),
                       GPoint(phone.origin.x + phone.size.w - 10, phone.origin.y + phone.size.h - 7));
    graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorBlack));
    graphics_draw_line(ctx, GPoint(center.x - 21, center.y - 21), GPoint(center.x + 21, center.y + 21));
    graphics_context_set_stroke_width(ctx, 1);
}

static void draw_centered_message(GContext* ctx, const GRect bounds, const char* title, const char* body,
                                  const bool with_icon)
{
    const int16_t margin = 10;
    const GRect text_box = GRect(margin, 0, bounds.size.w - margin * 2, bounds.size.h);
    const GFont title_font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    const GFont body_font = fonts_get_system_font(FONT_KEY_GOTHIC_18);
    const int16_t icon_height = with_icon ? 50 : 0;
    const int16_t title_height = graphics_text_layout_get_content_size(title, title_font, text_box,
                                                                       GTextOverflowModeWordWrap,
                                                                       GTextAlignmentCenter).h;
    const int16_t body_height = body != NULL ?
        graphics_text_layout_get_content_size(body, body_font, text_box, GTextOverflowModeWordWrap,
                                              GTextAlignmentCenter).h : 0;
    int16_t y = (bounds.size.h - (icon_height + title_height + body_height + 4)) / 2;

    if (with_icon)
    {
        draw_unreachable_phone_icon(ctx, GPoint(bounds.size.w / 2, y + 22));
        y += icon_height;
    }

    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, title, title_font, GRect(text_box.origin.x, y, text_box.size.w, title_height + 4),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    y += title_height + 4;
    if (body != NULL)
    {
        graphics_draw_text(ctx, body, body_font, GRect(text_box.origin.x, y, text_box.size.w, body_height + 4),
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
}

static void empty_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    switch (phone_state)
    {
    case PhoneStateWaiting:
        draw_centered_message(ctx, bounds, "Loading...", NULL, false);
        break;
    case PhoneStateAnswered:
        draw_centered_message(ctx, bounds, "No Notifications", NULL, false);
        break;
    case PhoneStateUnreachable:
        if (!connection_service_peek_pebble_app_connection())
        {
            draw_centered_message(ctx, bounds, "Phone Disconnected",
                                  "Connect your phone to see notifications.", true);
        }
        else
        {
            draw_centered_message(ctx, bounds, "Phone App Not Responding",
                                  "Open Notification Sync on your phone and finish its setup.", true);
        }
        break;
    }
}

static void set_phone_state(const PhoneState state)
{
    phone_state = state;
    if (empty_layer != NULL)
    {
        layer_mark_dirty(empty_layer);
    }
}

static void on_phone_timeout(void* context)
{
    (void)context;
    phone_timeout_timer = NULL;
    if (phone_state == PhoneStateWaiting)
    {
        set_phone_state(PhoneStateUnreachable);
    }
}

void window_notification_ui_on_phone_answered(void)
{
    if (phone_timeout_timer != NULL)
    {
        app_timer_cancel(phone_timeout_timer);
        phone_timeout_timer = NULL;
    }
    set_phone_state(PhoneStateAnswered);
}

static void splash_layer_update(Layer* layer, GContext* ctx)
{
    // Plain white while the phone hands us the notification, so nothing flashes before the card appears.
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

// ------------------------------------------------------------------------------------------------ detail

static void on_detail_back(void)
{
    if (popup_session)
    {
        send_close_me_without_animation();
        return;
    }

    const uint8_t bucket = detail_window_current_bucket();
    if (bucket != 0)
    {
        list_selected_bucket = bucket;
    }
    detail_window_close(true);
    sync_list_selection();
}

static void on_detail_emptied(void)
{
    if (popup_session)
    {
        send_close_me_without_animation();
        return;
    }
    detail_window_close(true);
    sync_list_selection();
    update_layer_visibility();
}

static void on_detail_card_shown(const uint8_t bucket_id)
{
    notification_store_mark_seen(bucket_id);
    list_selected_bucket = bucket_id;
    if (bucket_id == launch_target_bucket && deferred_vibration_segments > 0)
    {
        flush_deferred_vibration();
    }
}

static void on_detail_interacted(void)
{
    idle_handler_notify_user_interacted();
    unacknowledged_alert_bucket = 0;
}

static const DetailCallbacks detail_callbacks = {
    .back_pressed = on_detail_back,
    .emptied = on_detail_emptied,
    .card_shown = on_detail_card_shown,
    .interacted = on_detail_interacted,
};

static void open_detail(const uint8_t bucket_id, const bool animated)
{
    if (detail_window_is_open())
    {
        detail_window_show_bucket(bucket_id);
    }
    else
    {
        detail_window_open(bucket_id, animated);
    }
    update_layer_visibility();
}

// ------------------------------------------------------------------------------------------------ phone launch

static void cancel_launch_timeout(void)
{
    if (launch_timeout_timer != NULL)
    {
        app_timer_cancel(launch_timeout_timer);
        launch_timeout_timer = NULL;
    }
}

static uint8_t pick_launch_target(const bool may_wait)
{
    if (launch_target_bucket != 0)
    {
        const NotificationItem* item = notification_store_item_by_bucket(launch_target_bucket);
        if (item != NULL && item->loaded)
        {
            return launch_target_bucket;
        }
        if (may_wait && bucket_sync_is_currently_syncing)
        {
            // It may still be on its way.
            return 0;
        }
    }

    // Fall back to the newest unread notification, then simply the newest one.
    for (uint8_t i = 0; i < notification_store_count(); i++)
    {
        const NotificationItem* item = notification_store_item(i);
        if (item->unread && item->loaded)
        {
            return item->bucket_id;
        }
    }
    const NotificationItem* top = notification_store_item(0);
    return top != NULL && top->loaded ? top->bucket_id : 0;
}

static void finish_phone_launch(const bool may_wait)
{
    if (!waiting_for_launch_target || close_after_sync)
    {
        // Background sync launches only refresh data and close again; nothing to show.
        return;
    }

    const uint8_t target = pick_launch_target(may_wait);
    if (target == 0)
    {
        if ((!may_wait || !bucket_sync_is_currently_syncing) && notification_store_count() == 0)
        {
            // Nothing to show (it was dismissed on the phone before we got here).
            waiting_for_launch_target = false;
            cancel_launch_timeout();
            send_close_me_without_animation();
        }
        return;
    }

    waiting_for_launch_target = false;
    cancel_launch_timeout();
    launch_target_bucket = target;
    unacknowledged_alert_bucket = target;
    open_detail(target, false);
    if (detail_window_current_bucket() == target)
    {
        flush_deferred_vibration();
    }
}

static void on_launch_timeout(void* context)
{
    (void)context;
    launch_timeout_timer = NULL;
    if (!waiting_for_launch_target)
    {
        return;
    }

    if (notification_store_count() == 0)
    {
        // Phone never answered and we have nothing stored: fall back to the plain list.
        waiting_for_launch_target = false;
        popup_session = false;
        update_layer_visibility();
        flush_deferred_vibration();
        return;
    }

    finish_phone_launch(false);
}

static void try_finish_phone_launch(void)
{
    finish_phone_launch(true);
}

void window_notification_ui_open_phone_launch_detail(const uint8_t bucket_id)
{
    if (!popup_session)
    {
        return;
    }
    launch_target_bucket = bucket_id;
    try_finish_phone_launch();
}

// ------------------------------------------------------------------------------------------------ alerts

static void cancel_pending_alert(void)
{
    pending_alert_bucket = 0;
    if (pending_alert_timer != NULL)
    {
        app_timer_cancel(pending_alert_timer);
        pending_alert_timer = NULL;
    }
}

static bool should_hold_alert(void)
{
    return window_notification_data.menu_displayed || idle_handler_should_keep_current_notification();
}

static void schedule_alert_retry(void);

static void deliver_alert(void)
{
    const uint8_t bucket = pending_alert_bucket;
    if (bucket == 0)
    {
        return;
    }

    const NotificationItem* item = notification_store_item_by_bucket(bucket);
    if (item == NULL || !item->loaded)
    {
        cancel_pending_alert();
        return;
    }

    if (should_hold_alert())
    {
        schedule_alert_retry();
        return;
    }

    cancel_pending_alert();
    open_detail(bucket, true);
}

static void on_alert_timer(void* context)
{
    (void)context;
    pending_alert_timer = NULL;
    deliver_alert();
}

static void schedule_alert_retry(void)
{
    if (pending_alert_timer != NULL)
    {
        return;
    }
    uint32_t wait = idle_handler_ms_until_current_notification_release();
    if (wait < ALERT_RETRY_MS)
    {
        wait = ALERT_RETRY_MS;
    }
    pending_alert_timer = app_timer_register(wait, on_alert_timer, NULL);
}

static uint8_t newest_changed_unread(void)
{
    uint8_t best = 0;
    int16_t best_index = INT16_MAX;
    for (uint8_t i = 0; i < last_changed_count; i++)
    {
        const int16_t index = notification_store_index_of(last_changed[i]);
        const NotificationItem* item = notification_store_item(index >= 0 ? index : 255);
        if (item != NULL && item->unread && item->loaded && index < best_index)
        {
            best = item->bucket_id;
            best_index = index;
        }
    }
    return best;
}

void window_notification_ui_play_or_defer_vibration(const uint32_t* durations, const uint32_t num_segments)
{
    if (waiting_for_launch_target)
    {
        // Vibrate when the notification is actually on screen, like a PebbleOS popup.
        const uint32_t copied = num_segments < MAX_DEFERRED_VIBE_SEGMENTS ? num_segments : MAX_DEFERRED_VIBE_SEGMENTS;
        memcpy(deferred_vibration, durations, copied * sizeof(uint32_t));
        deferred_vibration_segments = copied;
        return;
    }

    play_vibration(durations, num_segments);

    // A vibration means "this is a new notification the user should see" (muted/silent ones never
    // vibrate). Bring the newest such notification to the screen.
    uint8_t target = newest_changed_unread();
    if (target == 0)
    {
        const NotificationItem* top = notification_store_item(0);
        target = top != NULL && top->unread ? top->bucket_id : 0;
    }
    if (target == 0)
    {
        return;
    }

    unacknowledged_alert_bucket = target;
    cancel_pending_alert();
    pending_alert_bucket = target;
    deliver_alert();
}

void window_notification_ui_on_menu_closed(void)
{
    if (pending_alert_bucket != 0 && pending_alert_timer == NULL)
    {
        schedule_alert_retry();
    }
}

void window_notification_ui_on_dismiss_sent(const uint8_t bucket_id)
{
    // Like a PebbleOS popup: dismissing the notification the app was opened for ends the session.
    if (popup_session && bucket_id == launch_target_bucket)
    {
        exit_when_gone_bucket = bucket_id;
    }
    detail_window_play_dismissed(bucket_id);
}

bool window_notification_ui_is_popup_session(void)
{
    return popup_session;
}

// ------------------------------------------------------------------------------------------------ store events

static void on_list_changed(const uint8_t* changed, const uint8_t changed_count)
{
    if (exit_when_gone_bucket != 0 && notification_store_index_of(exit_when_gone_bucket) < 0)
    {
        exit_when_gone_bucket = 0;
        send_close_me_without_animation();
        return;
    }

    last_changed_count = changed_count < sizeof(last_changed) ? changed_count : sizeof(last_changed);
    memcpy(last_changed, changed, last_changed_count);

    if (pending_alert_bucket != 0 && notification_store_index_of(pending_alert_bucket) < 0)
    {
        cancel_pending_alert();
    }
    if (unacknowledged_alert_bucket != 0 && notification_store_index_of(unacknowledged_alert_bucket) < 0)
    {
        unacknowledged_alert_bucket = 0;
    }

    detail_window_on_list_changed(changed, changed_count);
    sync_list_selection();
    update_app_glance();
    try_finish_phone_launch();
    update_layer_visibility();
    idle_handler_notify_notifications_updated();
}

static void on_details_changed(const uint8_t bucket_id)
{
    detail_window_on_details_changed(bucket_id);
    if (menu_layer != NULL && !detail_window_is_open())
    {
        layer_mark_dirty(menu_layer_get_layer(menu_layer));
    }
}

static void on_settings_changed(void)
{
    idle_handler_register_timers();
}

static const StoreListener store_listener = {
    .list_changed = on_list_changed,
    .details_changed = on_details_changed,
    .settings_changed = on_settings_changed,
};

// ------------------------------------------------------------------------------------------------ window

static void window_load(Window* window)
{
    Layer* root = window_get_root_layer(window);
    const GRect bounds = layer_get_bounds(root);

    menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(menu_layer, NULL, (MenuLayerCallbacks){
        .get_num_rows = get_num_rows,
        .get_cell_height = get_cell_height,
        .draw_row = draw_row,
        .select_click = select_click,
        .selection_changed = selection_changed,
    });
    menu_layer_set_normal_colors(menu_layer, GColorWhite, GColorBlack);
    menu_layer_set_highlight_colors(menu_layer, PBL_IF_COLOR_ELSE(DEFAULT_LIST_HIGHLIGHT, GColorBlack), GColorWhite);
    menu_layer_set_click_config_onto_window(menu_layer, window);
    layer_add_child(root, menu_layer_get_layer(menu_layer));

    empty_layer = layer_create(bounds);
    layer_set_update_proc(empty_layer, empty_layer_update);
    layer_add_child(root, empty_layer);

    splash_layer = layer_create(bounds);
    layer_set_update_proc(splash_layer, splash_layer_update);
    layer_add_child(root, splash_layer);

    window_notification_data.active = true;
    window_notification_action_list_init(window);
    sync_list_selection();
    update_layer_visibility();
}

static void window_unload(Window* window)
{
    cancel_pending_alert();
    cancel_launch_timeout();
    if (phone_timeout_timer != NULL)
    {
        app_timer_cancel(phone_timeout_timer);
        phone_timeout_timer = NULL;
    }
    detail_window_close(false);
    window_notification_action_list_deinit();

    menu_layer_destroy(menu_layer);
    layer_destroy(empty_layer);
    layer_destroy(splash_layer);
    menu_layer = NULL;
    empty_layer = NULL;
    splash_layer = NULL;

    window_notification_data.active = false;
    notification_store_set_listener(NULL);
    card_unload_icons();
    window_destroy(window);
    list_window = NULL;
}

void window_notification_show()
{
    list_window = window_create();
    if (list_window == NULL)
    {
        vibes_double_pulse();
        return;
    }

    popup_session = launched_by_phone();
    waiting_for_launch_target = popup_session;
    launch_target_bucket = 0;
    deferred_vibration_segments = 0;

    detail_window_set_callbacks(&detail_callbacks);
    notification_store_set_listener(&store_listener);

    window_set_background_color(list_window, GColorWhite);
    window_set_window_handlers(list_window, (WindowHandlers){
        .load = window_load,
        .unload = window_unload,
    });
    window_stack_push(list_window, !popup_session);

    if (popup_session)
    {
        launch_timeout_timer = app_timer_register(PHONE_LAUNCH_TIMEOUT_MS, on_launch_timeout, NULL);
    }
    if (phone_state == PhoneStateWaiting)
    {
        if (connection_service_peek_pebble_app_connection())
        {
            phone_timeout_timer = app_timer_register(PHONE_ANSWER_TIMEOUT_MS, on_phone_timeout, NULL);
        }
        else
        {
            phone_state = PhoneStateUnreachable;
        }
    }
    idle_handler_register_timers();
}

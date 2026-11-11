#include "window_notification.h"
#include "../theme.h"

#include <pebble.h>

#include "action_list.h"
#include "card.h"
#include "detail_window.h"
#include "idle_handler.h"
#include "notification_image.h"
#include "notification_store.h"
#include "touch_gesture.h"
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
#define PHONE_ANSWER_TIMEOUT_MS 10000
// Nothing at all on screen for this long while the phone answers: a blank screen that fills is calmer than a
// "Loading..." flashing up for a fraction of a second.
#define QUIET_START_MS 1500
typedef enum
{
    PhoneStateWaiting,
    PhoneStateAnswered,
    PhoneStateUnreachable,
    PhoneStateListenerUnavailable,
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

// Row last selected; used when the selection is on a header (which has no notification of its own).
static int16_t list_selected_row;
// The user moved the selection (or opened something) since the list appeared.
static bool list_selection_moved;
// Selection set by the app (not by the user): don't treat it as the user's choice.
static bool setting_selection;

/** Row to select: the selected notification's row (or the closed header/stack holding it). */
static int16_t selected_index(void)
{
    const int16_t row = notification_store_row_of_bucket(list_selected_bucket);
    if (row >= 0)
    {
        return row;
    }
    const int16_t rows = notification_store_row_count();
    if (rows == 0)
    {
        return 0;
    }
    if (!list_selection_moved)
    {
        // Nothing chosen yet: start on the first notification rather than on a section header.
        for (int16_t i = 0; i < rows; i++)
        {
            const ListRow* row = notification_store_row(i);
            if (row->kind != ROW_HEADER && !(row->kind == ROW_STACK && row->expanded))
            {
                return i;
            }
        }
    }
    return list_selected_row < rows ? list_selected_row : rows - 1;
}

/** The notification a row stands for (a collapsed stack stands for its newest), NULL for headers. */
static const NotificationItem* row_item(const ListRow* row)
{
    if (row == NULL || row->kind == ROW_HEADER || (row->kind == ROW_STACK && row->expanded))
    {
        return NULL;
    }
    return notification_store_item(row->item);
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
    if (list_window == NULL || menu_layer == NULL || empty_layer == NULL || splash_layer == NULL)
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

#define LIST_HEADER_HEIGHT PBL_IF_ROUND_ELSE(26, 22)

static int16_t full_row_height(const ListRow* row)
{
    if (row != NULL && (row->kind == ROW_HEADER || (row->kind == ROW_STACK && row->expanded)))
    {
        return LIST_HEADER_HEIGHT;
    }
    return LIST_CELL_HEIGHT;
}

// ---------------------------------------------------------------------------------- open / close animation
// Opening or closing a section grows or shrinks its rows in place, so the rows below slide instead of jumping,
// with a little overshoot like PebbleOS's own animations.

#define FOLD_OPEN_MS 280
#define FOLD_CLOSE_MS 220
#define FOLD_FULL 1000

static Animation* fold_animation;
static int16_t fold_first = -1;
static int16_t fold_count;
static int16_t fold_header;
static bool fold_opening;
// How open the folding rows are, in thousandths; overshoots a little past FOLD_FULL.
static int32_t fold_amount = FOLD_FULL;

static bool row_is_folding(const int16_t index)
{
    return fold_first >= 0 && index >= fold_first && index < fold_first + fold_count;
}

static int16_t row_height_at(const int16_t index)
{
    const int16_t full = full_row_height(notification_store_row(index));
    if (row_is_folding(index))
    {
        const int32_t height = full * fold_amount / FOLD_FULL;
        return height > 0 ? height : 0;
    }
    return full;
}

/** Back easing: past the target by about a tenth, then back (PebbleOS calls its version "moook"). */
static int32_t ease_out_back(const int32_t t)
{
    const int64_t u = t - FOLD_FULL;
    return FOLD_FULL + (int32_t)(2702 * u * u * u / 1000000000LL + 1702 * u * u / 1000000LL);
}

static int32_t ease_in_back(const int32_t t)
{
    const int64_t v = t;
    return (int32_t)(2702 * v * v * v / 1000000000LL - 1702 * v * v / 1000000LL);
}

static void fold_reload(void)
{
    if (menu_layer != NULL)
    {
        menu_layer_reload_data(menu_layer);
    }
}

static void fold_finish(const bool apply)
{
    if (fold_first < 0)
    {
        return;
    }
    const bool closing = !fold_opening;
    const int16_t header = fold_header;
    fold_first = -1;
    fold_count = 0;
    fold_amount = FOLD_FULL;
    Animation* running = fold_animation;
    fold_animation = NULL;
    if (running != NULL)
    {
        animation_unschedule(running);
    }
    if (closing && apply)
    {
        notification_store_toggle_row(header);
    }
    fold_reload();
}

/** After a section opens, scroll just enough to show what it revealed, keeping its header on screen. */
static void reveal_opened_rows(const int16_t header, const int16_t count)
{
#if !PBL_ROUND
    ScrollLayer* scroll = menu_layer_get_scroll_layer(menu_layer);
    const int16_t offset = -scroll_layer_get_content_offset(scroll).y;
    int16_t header_top = 0;
    for (int16_t i = 0; i < header; i++)
    {
        header_top += row_height_at(i);
    }
    int16_t bottom = header_top + row_height_at(header);
    for (int16_t i = 1; i <= count; i++)
    {
        bottom += row_height_at(header + i);
    }
    const int16_t view = layer_get_bounds(menu_layer_get_layer(menu_layer)).size.h;
    if (bottom <= offset + view)
    {
        return;
    }
    int16_t target = bottom - view;
    if (target > header_top)
    {
        target = header_top;
    }
    if (target > offset)
    {
        scroll_layer_set_content_offset(scroll, GPoint(0, -target), true);
    }
#else
    (void)header;
    (void)count;
#endif
}

static void fold_update(Animation* animation, const AnimationProgress progress)
{
    (void)animation;
    const int32_t t = (int32_t)progress * FOLD_FULL / ANIMATION_NORMALIZED_MAX;
    fold_amount = fold_opening ? ease_out_back(t) : FOLD_FULL - ease_in_back(t);
    fold_reload();
}

static void fold_stopped(Animation* animation, const bool finished, void* context)
{
    (void)context;
    if (animation != fold_animation)
    {
        return;
    }
    fold_animation = NULL;
    if (finished)
    {
        const bool opened = fold_opening;
        const int16_t header = fold_header;
        const int16_t count = fold_count;
        fold_finish(true);
        if (opened)
        {
            reveal_opened_rows(header, count);
        }
    }
}

static void fold_start(const int16_t header, const int16_t count, const bool opening)
{
    fold_header = header;
    fold_first = header + 1;
    fold_count = count;
    fold_opening = opening;
    fold_amount = opening ? 0 : FOLD_FULL;

    static const AnimationImplementation implementation = {.update = fold_update};
    fold_animation = animation_create();
    if (fold_animation == NULL)
    {
        fold_finish(true);
        return;
    }
    animation_set_implementation(fold_animation, &implementation);
    animation_set_duration(fold_animation, opening ? FOLD_OPEN_MS : FOLD_CLOSE_MS);
    animation_set_curve(fold_animation, AnimationCurveLinear);
    animation_set_handlers(fold_animation, (AnimationHandlers){.stopped = fold_stopped}, NULL);
    fold_reload();
    if (!animation_schedule(fold_animation))
    {
        fold_finish(true);
    }
}

static uint16_t get_num_rows(MenuLayer* layer, uint16_t section, void* context)
{
    (void)layer;
    (void)section;
    (void)context;
    return notification_store_row_count();
}

static int16_t get_cell_height(MenuLayer* layer, MenuIndex* index, void* context)
{
    (void)layer;
    (void)context;
    return row_height_at(index->row);
}

/** A small triangle: pointing down when open, right when closed. */
static void draw_caret(GContext* ctx, const GPoint center, const bool open)
{
    GPoint points[3];
    if (open)
    {
        points[0] = GPoint(center.x - 4, center.y - 2);
        points[1] = GPoint(center.x + 4, center.y - 2);
        points[2] = GPoint(center.x, center.y + 3);
    }
    else
    {
        points[0] = GPoint(center.x - 2, center.y - 4);
        points[1] = GPoint(center.x - 2, center.y + 4);
        points[2] = GPoint(center.x + 3, center.y);
    }
    // Filled line by line: no allocation while the list is scrolling.
    if (open)
    {
        for (int16_t i = 0; i <= 4; i++)
        {
            graphics_fill_rect(ctx, GRect(points[0].x + i, points[0].y + i, 9 - 2 * i, 1), 0, GCornerNone);
        }
    }
    else
    {
        for (int16_t i = 0; i <= 4; i++)
        {
            graphics_fill_rect(ctx, GRect(points[0].x + i, points[0].y + i, 1, 9 - 2 * i), 0, GCornerNone);
        }
    }
}

/** Section, category or opened stack: a small caption with a caret, and the count when closed. */
static void draw_header_row(GContext* ctx, const Layer* cell_layer, const GRect bounds, const ListRow* row)
{
    const bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
    const GColor color = highlighted ? PBL_IF_COLOR_ELSE(GColorWhite, theme_bg()) : theme_secondary();
    graphics_context_set_text_color(ctx, color);
    graphics_context_set_fill_color(ctx, color);

    char text[48];
    const char* name = row->kind == ROW_STACK
        ? notification_store_item(row->item)->app_name
        : notification_store_group_name(row->group);
    if (row->expanded && row->kind == ROW_HEADER)
    {
        snprintf(text, sizeof(text), "%s", name);
    }
    else
    {
        snprintf(text, sizeof(text), "%s  %d", name, row->count);
    }

    const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    // Categories sit a little further in than their Silent section; opened stacks further still.
    const int16_t indent = row->kind == ROW_STACK ? 16 : row->group >= GROUP_SUGGESTED ? 10 : 0;
#if PBL_ROUND
    const GSize size = graphics_text_layout_get_content_size(text, font, GRect(0, 0, bounds.size.w - 60, 20),
                                                             GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
    const int16_t left = (bounds.size.w - size.w - 14) / 2 + indent / 2;
#else
    const int16_t left = 8 + indent;
#endif
    const int16_t text_top = (bounds.size.h - 18) / 2;
    draw_caret(ctx, GPoint(left + 4, bounds.size.h / 2), row->expanded);
    graphics_draw_text(ctx, text, font, GRect(left + 14, text_top, bounds.size.w - left - 20, 18),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_item_row(GContext* ctx, const Layer* cell_layer, GRect bounds, const NotificationItem* item,
                          const ListRow* row, int16_t row_index);

#define STACK_LINE_EDGE 5
#define STACK_LINE_WIDTH 2
#define STACK_LINE_GAP 4
// Space between the line and where the selection highlight starts.
#define STACK_LINE_HIGHLIGHT_GAP 2

#if PBL_ROUND
static int32_t isqrt(const int32_t value)
{
    if (value <= 0)
    {
        return 0;
    }
    int32_t root = 0;
    int32_t bit = 1 << 14;
    int32_t rest = value;
    while (bit > rest)
    {
        bit >>= 2;
    }
    while (bit != 0)
    {
        if (rest >= root + bit)
        {
            rest -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}
#endif

/**
 * A line down the left of an opened stack ties its notifications together without indenting them more. It
 * stops a little short of the rows above and below the stack; on round watches it follows the edge of the
 * screen as an arc.
 */
static void draw_stack_line(GContext* ctx, const Layer* cell_layer, const GRect bounds, const int16_t row_index,
                            const int16_t content_left)
{
    const ListRow* above = notification_store_row(row_index - 1);
    const ListRow* below = notification_store_row(row_index + 1);
    const int16_t top = bounds.origin.y + (above != NULL && above->in_stack ? 0 : STACK_LINE_GAP);
    const int16_t bottom = bounds.origin.y + bounds.size.h - (below != NULL && below->in_stack ? 0 : STACK_LINE_GAP);
#if PBL_ROUND
    // Same grey whether or not the row is selected (black and white watches have no grey: there the selected row
    // is black, so the line has to flip to stay visible).
#if PBL_COLOR
    graphics_context_set_fill_color(ctx, theme_secondary());
#else
    graphics_context_set_fill_color(ctx, menu_cell_layer_is_highlighted(cell_layer) ? theme_bg() : theme_fg());
#endif
    // A ring concentric with the screen, so it stays the same distance from the edge all the way round. Each pixel
    // row fills the span between the ring's outer and inner circle, which keeps the stroke even where it turns
    // sideways. It ends where it would run into the row's icon and text.
    const int32_t radius = PBL_DISPLAY_WIDTH / 2;
    const int32_t outer = radius - STACK_LINE_EDGE;
    const int32_t inner = outer - STACK_LINE_WIDTH;
    const int16_t screen_y = layer_convert_point_to_screen(cell_layer, GPointZero).y;
    const int16_t limit = content_left - STACK_LINE_GAP;
    for (int16_t y = top; y < bottom; y++)
    {
        const int32_t dy = screen_y + y - PBL_DISPLAY_HEIGHT / 2;
        if (dy * dy >= outer * outer)
        {
            continue;
        }
        const int16_t from = radius - isqrt(outer * outer - dy * dy);
        int16_t to = dy * dy < inner * inner ? radius - isqrt(inner * inner - dy * dy) : radius;
        if (to <= from)
        {
            to = from + 1;
        }
        if (to > limit)
        {
            continue;
        }
        graphics_fill_rect(ctx, GRect(from, y, to - from, 1), 0, GCornerNone);
    }
#else
    // The selection highlight stops between the line and the icon: the strip holding the line stays unselected.
    if (menu_cell_layer_is_highlighted(cell_layer))
    {
        graphics_context_set_fill_color(ctx, theme_bg());
        graphics_fill_rect(ctx, GRect(0, bounds.origin.y, STACK_LINE_EDGE + STACK_LINE_WIDTH + STACK_LINE_HIGHLIGHT_GAP,
                                      bounds.size.h), 0, GCornerNone);
    }
    (void)content_left;
    graphics_context_set_fill_color(ctx, theme_secondary());
    graphics_fill_rect(ctx, GRect(STACK_LINE_EDGE, top, STACK_LINE_WIDTH, bottom - top), 0, GCornerNone);
#endif
}

static void draw_row(GContext* ctx, const Layer* cell_layer, MenuIndex* index, void* context)
{
    (void)context;
    const ListRow* row = notification_store_row(index->row);
    if (row == NULL)
    {
        return;
    }
    // A row that is opening or closing is drawn at full size, sliding out from under the row above it.
    GRect bounds = layer_get_bounds(cell_layer);
    const int16_t full = full_row_height(row);
    if (bounds.size.h < full)
    {
        bounds.origin.y = bounds.size.h - full;
        bounds.size.h = full;
    }
    const NotificationItem* item = row_item(row);
    if (item == NULL)
    {
        draw_header_row(ctx, cell_layer, bounds, row);
        return;
    }
    draw_item_row(ctx, cell_layer, bounds, item, row, index->row);
}

// PebbleOS source: src/fw/apps/system/notifications.c (prv_draw_notification_cell_rect)
static void draw_item_row(GContext* ctx, const Layer* cell_layer, const GRect bounds, const NotificationItem* item,
                          const ListRow* row, const int16_t row_index)
{
#if PBL_ROUND
    // Keep the icon and text clear of the curved edge: rows above and below the centre start further in.
    int16_t inset = 12;
    {
        const GPoint on_screen = layer_convert_point_to_screen(cell_layer, bounds.origin);
        const int32_t radius = PBL_DISPLAY_WIDTH / 2;
        int32_t dy = on_screen.y + bounds.size.h / 2 - PBL_DISPLAY_HEIGHT / 2;
        dy += dy > 0 ? bounds.size.h / 2 : -bounds.size.h / 2; // the row's edge nearest the rim
        const int32_t span_sq = radius * radius - dy * dy;
        int32_t half = 0;
        while (span_sq > 0 && (half + 1) * (half + 1) <= span_sq)
        {
            half++;
        }
        const int16_t edge = radius - half + 4;
        if (edge > inset)
        {
            inset = edge > 60 ? 60 : edge;
        }
    }
    if (row->in_stack)
    {
        inset += 7;
    }
#else
    const int16_t inset = row->in_stack ? 12 : 5;
#endif

    if (row->in_stack)
    {
        draw_stack_line(ctx, cell_layer, bounds, row_index, inset);
    }

    GDrawCommandImage* icon = card_icon_for_id(item->icon_id);
    GSize icon_size = GSize(0, 0);
    if (icon != NULL)
    {
        icon_size = gdraw_command_image_get_bounds_size(icon);
        GRect box = bounds;
        box.origin.x += inset;
        GRect icon_rect = {.size = icon_size};
        grect_align(&icon_rect, &box, GAlignLeft, false);
        theme_draw_icon(ctx, icon, icon_rect.origin, menu_cell_layer_is_highlighted(cell_layer));
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

    // A little breathing room between the icon and the text.
    const int16_t icon_gap = 5;
    const int16_t text_left = inset + (icon_size.w > 25 ? icon_size.w : 25) + icon_gap;
    // A closed stack keeps room on the right for its count.
    const int16_t right_room = PBL_IF_ROUND_ELSE(inset, 5) + (row->kind == ROW_STACK ? 22 : 0);
    const GRect text_box = grect_inset(bounds, GEdgeInsets(0, right_room, 0, text_left));
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

    if (row->kind == ROW_STACK)
    {
        // A closed stack: how many notifications it holds, with a caret to open it.
        char count[6];
        snprintf(count, sizeof(count), "%d", row->count);
        const GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
        const int16_t right = bounds.size.w - PBL_IF_ROUND_ELSE(inset, 5);
        const int16_t middle = bounds.size.h / 2;
        graphics_draw_text(ctx, count, font, GRect(right - 26, middle - 10, 16, 18), GTextOverflowModeFill,
                           GTextAlignmentRight, NULL);
        const bool highlighted = menu_cell_layer_is_highlighted(cell_layer);
        graphics_context_set_fill_color(ctx, highlighted ? PBL_IF_COLOR_ELSE(GColorWhite, theme_bg()) : theme_fg());
        draw_caret(ctx, GPoint(right - 4, middle), false);
    }
}

static void toggle_row(const uint8_t row)
{
    if (fold_first >= 0)
    {
        // Pressed again mid-animation: just finish it (closing changes which row is where).
        const bool was_closing = !fold_opening;
        fold_finish(true);
        if (was_closing)
        {
            return;
        }
    }
    list_selection_moved = true;
    list_selected_row = row;
    list_selected_bucket = 0;
    const ListRow* current = notification_store_row(row);
    if (current == NULL)
    {
        return;
    }
    if (current->expanded)
    {
        // Close: shrink the rows first, the store drops them when the animation ends.
        const uint8_t count = notification_store_rows_folded_by(row);
        if (count > 0)
        {
            fold_start(row, count, false);
            return;
        }
        notification_store_toggle_row(row);
    }
    else
    {
        const uint8_t before = notification_store_row_count();
        notification_store_toggle_row(row);
        const uint8_t after = notification_store_row_count();
        if (after > before)
        {
            fold_start(row, after - before, true);
            return;
        }
    }
    menu_layer_reload_data(menu_layer);
}

static void activate_row(const uint8_t index)
{
    const ListRow* row = notification_store_row(index);
    if (row == NULL)
    {
        return;
    }
    if (row->kind != ROW_ITEM)
    {
        // Headers and stacks open or close, like on the phone.
        toggle_row(index);
        return;
    }
    const NotificationItem* item = notification_store_item(row->item);
    if (item != NULL)
    {
        list_selected_row = index;
        list_selected_bucket = item->bucket_id;
        open_detail(item->bucket_id, true);
    }
}

static void select_click(MenuLayer* layer, MenuIndex* index, void* context)
{
    (void)layer;
    (void)context;
    idle_handler_notify_user_interacted();
    activate_row(index->row);
}

// Fetching a message's full text while the list is still moving makes scrolling stutter, so wait until the
// selection rests for a moment.
#define PREFETCH_DELAY_MS 350
static AppTimer* prefetch_timer;

static void prefetch_selected(void* context)
{
    (void)context;
    prefetch_timer = NULL;
    if (list_selected_bucket != 0)
    {
        notification_store_want_details(list_selected_bucket, false);
    }
}

static void selection_changed(MenuLayer* layer, MenuIndex new_index, MenuIndex old_index, void* context)
{
    (void)layer;
    (void)old_index;
    (void)context;
    if (setting_selection)
    {
        return;
    }
    idle_handler_notify_user_interacted();
    list_selection_moved = true;
    list_selected_row = new_index.row;
    const NotificationItem* item = row_item(notification_store_row(new_index.row));
    list_selected_bucket = item != NULL ? item->bucket_id : 0;
    if (fold_first >= 0 && new_index.row != fold_header)
    {
        fold_finish(true);
    }
    if (item != NULL)
    {
        if (prefetch_timer != NULL)
        {
            app_timer_reschedule(prefetch_timer, PREFETCH_DELAY_MS);
        }
        else
        {
            prefetch_timer = app_timer_register(PREFETCH_DELAY_MS, prefetch_selected, NULL);
        }
    }
}

// ------------------------------------------------------------------------------------------------ list touch

#define LIST_FLING_DECELERATION 2600
#define LIST_FLING_MIN_MS 180
#define LIST_FLING_MAX_MS 900

static TouchGesture list_touch;
static int16_t list_drag_base;
static bool list_dragging;
static Animation* list_fling;
static int16_t list_fling_from;
static int16_t list_fling_to;

static int16_t list_content_height(void)
{
    return scroll_layer_get_content_size(menu_layer_get_scroll_layer(menu_layer)).h;
}

/** Round menus pad the content so the first and last rows can sit in the middle of the screen. */
static int16_t rows_height(void)
{
    int16_t total = 0;
    for (uint8_t i = 0; i < notification_store_row_count(); i++)
    {
        total += row_height_at(i);
    }
    return total;
}

static int16_t list_top_padding(void)
{
    const int16_t padding = (list_content_height() - rows_height()) / 2;
    return padding > 0 ? padding : 0;
}

/** The row at this distance from the top of the first row, -1 above it, the last row below the end. */
static int16_t row_at(const int16_t position)
{
    if (position < 0)
    {
        return -1;
    }
    int16_t top = 0;
    const uint8_t count = notification_store_row_count();
    for (uint8_t i = 0; i < count; i++)
    {
        top += row_height_at(i);
        if (position < top)
        {
            return i;
        }
    }
    return count > 0 ? count - 1 : -1;
}

static int16_t list_max_offset(void)
{
    const int16_t max = list_content_height() - PBL_DISPLAY_HEIGHT;
    return max > 0 ? max : 0;
}

static int16_t list_offset(void)
{
    return -scroll_layer_get_content_offset(menu_layer_get_scroll_layer(menu_layer)).y;
}

static void set_list_offset(const int16_t value)
{
    scroll_layer_set_content_offset(menu_layer_get_scroll_layer(menu_layer), GPoint(0, -value), false);
}

/** After the list comes to rest, highlight the row in the middle of the screen (without scrolling). */
static void select_row_at_center(void)
{
    if (menu_layer == NULL || notification_store_row_count() == 0)
    {
        return;
    }
    int16_t row = row_at(list_offset() + PBL_DISPLAY_HEIGHT / 2 - list_top_padding());
    if (row < 0)
    {
        row = 0;
    }
    menu_layer_set_selected_index(menu_layer, MenuIndex(0, row), MenuRowAlignNone, false);
}

static void list_fling_update(Animation* animation, const AnimationProgress progress)
{
    (void)animation;
    if (menu_layer == NULL)
    {
        return;
    }
    const int32_t range = (int32_t)list_fling_to - list_fling_from;
    set_list_offset(list_fling_from + (int16_t)((range * (int32_t)progress) / ANIMATION_NORMALIZED_MAX));
}

static void list_fling_stopped(Animation* animation, const bool finished, void* context)
{
    (void)finished;
    (void)context;
    if (animation == list_fling)
    {
        list_fling = NULL;
        select_row_at_center();
    }
}

static void stop_list_fling(void)
{
    Animation* running = list_fling;
    list_fling = NULL;
    if (running != NULL)
    {
        animation_unschedule(running);
    }
}

static void list_touch_down(void* context)
{
    (void)context;
    idle_handler_notify_user_interacted();
    stop_list_fling();
    list_drag_base = list_offset();
    list_dragging = false;
}

static void list_drag_moved(const int16_t dy, void* context)
{
    (void)context;
    list_dragging = true;
    int16_t value = list_drag_base - dy;
    const int16_t max = list_max_offset();
    if (value < 0)
    {
        value /= 3;
    }
    else if (value > max)
    {
        value = max + (value - max) / 3;
    }
    set_list_offset(value);
}

static void list_drag_ended(const int16_t dy, const int32_t velocity, void* context)
{
    (void)dy;
    (void)context;
    if (!list_dragging)
    {
        return;
    }
    list_dragging = false;

    const int32_t speed = velocity < 0 ? -velocity : velocity;
    int32_t travel = (speed * speed) / (2 * LIST_FLING_DECELERATION);
    if (velocity > 0)
    {
        travel = -travel;
    }
    const int16_t from = list_offset();
    int32_t target = from + travel;
    const int16_t max = list_max_offset();
    target = target < 0 ? 0 : (target > max ? max : target);

    uint32_t duration = (uint32_t)((speed * 1000) / LIST_FLING_DECELERATION);
    duration = duration < LIST_FLING_MIN_MS ? LIST_FLING_MIN_MS :
        (duration > LIST_FLING_MAX_MS ? LIST_FLING_MAX_MS : duration);

    if (target == from)
    {
        select_row_at_center();
        return;
    }

    static const AnimationImplementation implementation = {
        .update = list_fling_update,
    };
    list_fling_from = from;
    list_fling_to = (int16_t)target;
    list_fling = animation_create();
    if (list_fling == NULL)
    {
        set_list_offset((int16_t)target);
        select_row_at_center();
        return;
    }
    animation_set_implementation(list_fling, &implementation);
    animation_set_duration(list_fling, duration);
    animation_set_curve(list_fling, AnimationCurveEaseOut);
    animation_set_handlers(list_fling, (AnimationHandlers){.stopped = list_fling_stopped}, NULL);
    if (!animation_schedule(list_fling))
    {
        list_fling = NULL;
        set_list_offset((int16_t)target);
        select_row_at_center();
    }
}

static void list_tap(const GPoint point, void* context)
{
    (void)context;
    const int16_t position = list_offset() + point.y - list_top_padding();
    if (position < 0 || position >= rows_height())
    {
        return;
    }
    const int16_t row = row_at(position);
    if (row < 0)
    {
        return;
    }
    menu_layer_set_selected_index(menu_layer, MenuIndex(0, row), MenuRowAlignNone, false);
    activate_row(row);
}

static void list_swipe_back(void* context)
{
    (void)context;
    send_close_me();
}

static const TouchGestureHandlers list_touch_handlers = {
    .touch_down = list_touch_down,
    .tap = list_tap,
    .drag_moved = list_drag_moved,
    .drag_ended = list_drag_ended,
    .swipe_back = list_swipe_back,
    .edge_swipe_up = NULL,
};

static void on_touch(const TouchEvent* event, void* context)
{
    (void)context;
    if (detail_window_handle_touch(event))
    {
        return;
    }
    if (list_window == NULL || menu_layer == NULL || window_stack_get_top_window() != list_window ||
        notification_store_count() == 0 || waiting_for_launch_target)
    {
        return;
    }
    touch_gesture_feed(&list_touch, event, &list_touch_handlers, NULL);
}

static void sync_list_selection(void)
{
    if (menu_layer == NULL)
    {
        return;
    }
    // The rows changed underneath a running open/close: stop it where the list now is.
    fold_finish(false);
    menu_layer_reload_data(menu_layer);
    if (notification_store_row_count() > 0)
    {
        const int16_t index = selected_index();
        const NotificationItem* item = row_item(notification_store_row(index));
        if (item != NULL)
        {
            list_selected_bucket = item->bucket_id;
        }
        list_selected_row = index;
        // Only move when the selection actually moved, and then without re-centring: an update arriving while
        // you scroll must not make the list jump.
        if (menu_layer_get_selected_index(menu_layer).row != index)
        {
            setting_selection = true;
            menu_layer_set_selected_index(menu_layer, MenuIndex(0, index), MenuRowAlignNone, false);
            setting_selection = false;
        }
    }
}

static void draw_unreachable_phone_icon(GContext* ctx, const GPoint center)
{
    // A phone outline with a slash through it, drawn in the 2px stroke style of the PebbleOS system icons.
    graphics_context_set_stroke_color(ctx, theme_fg());
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

    graphics_context_set_text_color(ctx, theme_fg());
    graphics_draw_text(ctx, title, title_font, GRect(text_box.origin.x, y, text_box.size.w, title_height + 4),
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    y += title_height + 4;
    if (body != NULL)
    {
        graphics_draw_text(ctx, body, body_font, GRect(text_box.origin.x, y, text_box.size.w, body_height + 4),
                           GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
    }
}

static bool quiet_start;

static void empty_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, theme_bg());
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    switch (phone_state)
    {
    case PhoneStateWaiting:
        if (!quiet_start)
        {
            draw_centered_message(ctx, bounds, "Loading...", NULL, false);
        }
        break;
    case PhoneStateAnswered:
        if (notification_store_is_live())
        {
            draw_centered_message(ctx, bounds, "No Notifications", NULL, false);
        }
        else if (!quiet_start)
        {
            // Answered, first notifications still on the way.
            draw_centered_message(ctx, bounds, "Loading...", NULL, false);
        }
        break;
    case PhoneStateListenerUnavailable:
        draw_centered_message(ctx, bounds, "Waiting for Notifications",
                              "Check notification access in the phone app.", false);
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
    if (menu_layer != NULL)
    {
        layer_mark_dirty(menu_layer_get_layer(menu_layer));
    }
}

static AppTimer* quiet_start_timer = NULL;

static void on_quiet_start_over(void* context)
{
    (void)context;
    quiet_start_timer = NULL;
    quiet_start = false;
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

void window_notification_ui_on_listener_unavailable(void)
{
    set_phone_state(PhoneStateListenerUnavailable);
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
    // Plain background while the phone hands us the notification, so nothing flashes before the card appears.
    graphics_context_set_fill_color(ctx, theme_bg());
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
    notification_image_request(bucket_id);
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

    if (may_wait && !notification_store_is_live())
    {
        // Notifications only show once this session's sync from the phone is in.
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
    notification_image_on_list_changed();
    if (detail_window_is_open())
    {
        notification_image_request(detail_window_current_bucket());
    }
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

// Settings bucket layout (phone: WatchSyncerImpl.syncPreferences): flags, auto close (2), interaction timeout (2),
// text size.
#define SETTINGS_TEXT_SIZE_INDEX 5
#define SETTINGS_SENDER_WEIGHT_INDEX 6
#define SETTINGS_MESSAGE_WEIGHT_INDEX 7
#define SETTINGS_THEME_INDEX 8
#define SETTINGS_SECTIONS_INDEX 9
#define SETTINGS_STACKS_INDEX 10

static void apply_list_colors(void)
{
    if (menu_layer == NULL)
    {
        return;
    }
    menu_layer_set_normal_colors(menu_layer, theme_bg(), theme_fg());
    // Black and white watches highlight with the text colour: black in light mode, white in dark mode.
    menu_layer_set_highlight_colors(menu_layer, PBL_IF_COLOR_ELSE(DEFAULT_LIST_HIGHLIGHT, theme_fg()),
                                    PBL_IF_COLOR_ELSE(GColorWhite, theme_bg()));
    if (list_window != NULL)
    {
        window_set_background_color(list_window, theme_bg());
    }
    layer_mark_dirty(menu_layer_get_layer(menu_layer));
    if (empty_layer != NULL)
    {
        layer_mark_dirty(empty_layer);
    }
}

static void load_text_size(void)
{
    uint8_t config[SETTINGS_STACKS_INDEX + 1];
    memset(config, 0, sizeof(config));
    config[SETTINGS_TEXT_SIZE_INDEX] = CARD_TEXT_SIZE_DEFAULT;
    config[SETTINGS_SENDER_WEIGHT_INDEX] = 1;
    config[SETTINGS_MESSAGE_WEIGHT_INDEX] = 1;
    config[SETTINGS_THEME_INDEX] = 0;
    config[SETTINGS_SECTIONS_INDEX] = 0;
    config[SETTINGS_STACKS_INDEX] = 0;
    const uint8_t size = bucket_sync_get_bucket_size(1);
    if (size > SETTINGS_TEXT_SIZE_INDEX)
    {
        bucket_sync_load_bucket_limited(1, config, size < sizeof(config) ? size : sizeof(config));
    }
    card_set_text_style(config[SETTINGS_TEXT_SIZE_INDEX], config[SETTINGS_SENDER_WEIGHT_INDEX] != 0,
                        config[SETTINGS_MESSAGE_WEIGHT_INDEX] != 0);
    theme_set_dark(config[SETTINGS_THEME_INDEX] == 1);
    notification_store_set_grouping(config[SETTINGS_SECTIONS_INDEX] != 0, config[SETTINGS_STACKS_INDEX] != 0);
    apply_list_colors();
    sync_list_selection();
    update_layer_visibility();
    detail_window_on_style_changed();
}

static void on_settings_changed(void)
{
    idle_handler_register_timers();
    load_text_size();
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
    apply_list_colors();
    menu_layer_set_click_config_onto_window(menu_layer, window);
    layer_add_child(root, menu_layer_get_layer(menu_layer));

    empty_layer = layer_create(bounds);
    layer_set_update_proc(empty_layer, empty_layer_update);
    layer_add_child(root, empty_layer);

    splash_layer = layer_create(bounds);
    layer_set_update_proc(splash_layer, splash_layer_update);
    layer_add_child(root, splash_layer);

    window_set_touch_bridge_disabled(window, true);
    touch_gesture_reset(&list_touch);
    if (touch_service_is_enabled())
    {
        touch_service_subscribe(on_touch, NULL);
    }

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
    if (quiet_start_timer != NULL)
    {
        app_timer_cancel(quiet_start_timer);
        quiet_start_timer = NULL;
    }
    quiet_start = false;
    touch_service_unsubscribe();
    fold_finish(false);
    if (prefetch_timer != NULL)
    {
        app_timer_cancel(prefetch_timer);
        prefetch_timer = NULL;
    }
    stop_list_fling();
    touch_gesture_reset(&list_touch);
    detail_window_close(false);
    notification_image_clear();
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
    notification_image_set_listener(detail_window_on_image_changed);
    load_text_size();

    window_set_background_color(list_window, theme_bg());
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
    if (phone_state == PhoneStateWaiting)
    {
        quiet_start = true;
        quiet_start_timer = app_timer_register(QUIET_START_MS, on_quiet_start_over, NULL);
    }
    idle_handler_register_timers();
}

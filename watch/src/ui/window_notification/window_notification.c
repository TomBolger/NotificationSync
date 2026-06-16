#include "window_notification.h"

#include <pebble.h>

#include "action_list.h"
#include "buttons.h"
#include "data_loading.h"
#include "idle_handler.h"
#include "connection/packets.h"
#include "connection/notification_details_fetcher.h"
#include "commons/math.h"

#include <stdlib.h>

#define ATTRIBUTE_ICON_TINY_SIZE_PX 25
#define DEFAULT_NOTIFICATION_COLOR GColorFolly

#define LAYOUT_BANNER_HEIGHT_RECT 36
#define LAYOUT_TOP_BANNER_HEIGHT LAYOUT_BANNER_HEIGHT_RECT
#define LAYOUT_ARROW_HEIGHT 19
#define NOTIFICATION_TINY_RESOURCE_WIDTH 30
#define NOTIFICATION_TINY_RESOURCE_HEIGHT 25
#define NOTIFICATION_TINY_RESOURCE_VERTICAL_OFFSET -1
#define CARD_ICON_UPPER_PADDING (((LAYOUT_TOP_BANNER_HEIGHT - NOTIFICATION_TINY_RESOURCE_HEIGHT) / 2) + NOTIFICATION_TINY_RESOURCE_VERTICAL_OFFSET)
#define CARD_MARGIN 10
#define CARD_BOTTOM_PADDING 18

// PebbleOS source: src/fw/services/timeline/swap_layer.c
#define INITIAL_SCROLL_PX LAYOUT_BANNER_HEIGHT_RECT
#define SCROLL_PX 48
#define REPEATING_SCROLL_PX 72
#define SWAP_MS 150
#define DISMISS_FALLBACK_MS 500
#define PEEK_PX LAYOUT_BANNER_HEIGHT_RECT
#define FUDGE_PX PEEK_PX
#define MESSAGE_SWAP_DELAY 3
#define DETAIL_BODY_CACHE_SLOTS PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 2, 2, 2, 2, 3, 3, 3)
#define MAX_DEFERRED_VIBE_SEGMENTS 16

typedef enum
{
    TextStyleFont_Header,
    TextStyleFont_MenuCellTitle,
    TextStyleFont_MenuCellSubtitle,
    TextStyleFont_Caption,
    TextStyleFont_Title,
    TextStyleFont_Body,
    TextStyleFont_Footer,
} TextStyleFont;

typedef enum
{
    DetailScrollDirectionUp,
    DetailScrollDirectionDown,
} DetailScrollDirection;

typedef struct
{
    uint8_t bucket_id;
    time_t receive_time;
    char* body;
    uint8_t num_actions;
    Action actions[MAX_NOTIFICATION_ACTIONS];
    bool has_actions;
} DetailBodyCacheEntry;

NotificationWindowData window_notification_data = {
    .active = false,
    .detail_open = false,
    .using_dummy_data = false,
    .num_actions = 0,
    .menu_displayed = false,
    .currently_selected_bucket = 0,
    .currently_selected_bucket_index = 0,
    .bucket_count = 0,
    .open_menu_on_success = 0,
    .icon_id = 0,
    .color_id = 0,
};

static Window* notification_window;
static Window* detail_window;
static MenuLayer* menu_layer;
static Layer* empty_state_layer;
static Layer* phone_launch_pending_layer;
static Animation* phone_launch_pending_animation;
static ScrollLayer* detail_scroll_layer;
static Layer* detail_content_layer;
static Layer* detail_swap_layer;
static Layer* detail_action_button_layer;
static Layer* detail_arrow_layer;
static NotificationListItem notification_items[MAX_NOTIFICATION_ITEMS];
static uint8_t notification_item_count;
static int16_t detail_content_height;
static int16_t detail_current_content_height;
static uint8_t detail_swap_delay_remaining = MESSAGE_SWAP_DELAY;
static Animation* detail_swap_animation;
static Animation* detail_dismiss_animation;
static GDrawCommandSequence* detail_dismiss_sequence;
static int8_t detail_transition_delta;
static int16_t detail_transition_target_index = -1;
static int16_t detail_transition_current_height;
static int16_t detail_transition_target_height;
static int16_t detail_transition_offset;
static int16_t detail_transition_start_offset;
static int16_t detail_transition_end_offset;
static bool detail_dismiss_active;
static bool detail_dismiss_close_app;
static uint32_t detail_dismiss_elapsed;
static char cell_body_buffer[MAX_BODY_TEXT_SIZE];
static char detail_body_buffer[MAX_BODY_TEXT_SIZE + 40];
static char detail_item_body_buffer[MAX_BODY_TEXT_SIZE + 40];
static char app_glance_subtitle[151];
static DetailBodyCacheEntry detail_body_cache[DETAIL_BODY_CACHE_SLOTS];
static uint8_t next_detail_body_cache_slot;
static bool phone_launch_detail_pending;
static int16_t phone_launch_detail_bucket_id = -1;
static int16_t pending_auto_open_bucket_id = -1;
static int16_t updated_notification_bucket_id = -1;
static int16_t pending_manual_swap_bucket_id = -1;
static bool detail_opened_from_phone_launch;
static bool detail_arrow_hidden = true;
static AppTimer* deferred_new_top_timer;
static bool has_seen_real_notification_items;
static bool phone_launch_pending_exit_animating;
static uint32_t deferred_phone_launch_vibration[MAX_DEFERRED_VIBE_SEGMENTS];
static uint32_t deferred_phone_launch_vibration_segments;
static GRect phone_launch_pending_start_frame;
static GRect phone_launch_pending_end_frame;

static bool load_selected_detail_data(void);
static void reload_detail_after_selected_bucket_changed(void);
static void reload_detail_after_selected_bucket_updated(void);
static void maybe_open_phone_launch_detail(void);
static bool notification_item_has_loaded_content(const NotificationListItem* item);
static bool notification_item_is_auto_open_candidate(const NotificationListItem* item);
static int16_t find_notification_index(uint8_t bucket_id, bool require_loaded_content);
static int16_t find_auto_open_notification_index(uint8_t bucket_id);
static int16_t detail_scroll_max_offset(ScrollLayer* scroll_layer);
static void update_detail_arrow_visibility(void);
static int16_t detail_scroll_current_offset(ScrollLayer* scroll_layer);
static void detail_scroll_to_offset(ScrollLayer* scroll_layer, int16_t offset, bool animated);
static void restore_detail_click_config(void);
static void sync_menu_selection(MenuRowAlign align, bool animated);
static void prefetch_detail_for_item(const NotificationListItem* item);
static void prefetch_detail_around_index(int16_t selected_index);
static void select_pending_notification_and_open_detail(void);
static bool begin_detail_swap_animation(int8_t delta);
static void flush_deferred_phone_launch_vibration(void);
static void phone_launch_pending_layer_update(Layer* layer, GContext* ctx);
static void start_phone_launch_pending_exit_animation(void);

static bool launched_from_phone_notification(void)
{
    return launch_reason() == APP_LAUNCH_PHONE;
}

static bool should_hide_list_window_contents(void)
{
    return launched_from_phone_notification() &&
        (detail_window == NULL || phone_launch_pending_exit_animating);
}

static void update_list_layer_visibility(void)
{
    const bool hide_list = should_hide_list_window_contents();
    if (notification_window != NULL)
    {
        window_set_background_color(notification_window, GColorWhite);
    }
    if (menu_layer != NULL)
    {
        layer_set_hidden(menu_layer_get_layer(menu_layer), hide_list);
    }
    if (empty_state_layer != NULL)
    {
        layer_set_hidden(empty_state_layer, hide_list || notification_item_count != 0);
        layer_mark_dirty(empty_state_layer);
    }
    if (phone_launch_pending_layer != NULL)
    {
        layer_set_hidden(phone_launch_pending_layer, !hide_list);
        if (hide_list)
        {
            layer_mark_dirty(phone_launch_pending_layer);
        }
    }
}

static void play_vibration_pattern(const uint32_t* durations, const uint32_t num_segments)
{
    if (durations == NULL || num_segments == 0)
    {
        return;
    }

    const VibePattern vibe_pattern = {
        .durations = (uint32_t*)durations,
        .num_segments = num_segments,
    };
    vibes_cancel();
    vibes_enqueue_custom_pattern(vibe_pattern);
    idle_handler_notify_received_new_vibration();
}

static void flush_deferred_phone_launch_vibration(void)
{
    if (deferred_phone_launch_vibration_segments == 0 || detail_window == NULL)
    {
        return;
    }

    const uint32_t num_segments = deferred_phone_launch_vibration_segments;
    deferred_phone_launch_vibration_segments = 0;
    play_vibration_pattern(deferred_phone_launch_vibration, num_segments);
}

static void close_failed_detail_window(void* context)
{
    (void)context;
    if (detail_window != NULL)
    {
        window_stack_remove(detail_window, false);
    }
}

static const uint32_t icon_resource_ids[] = {
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_GENERIC,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_GMAIL,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_WHATSAPP,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_MESSENGER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_FACEBOOK,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_TWITTER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_TELEGRAM,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_HANGOUTS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_INBOX,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_SMS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_EMAIL,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_PHONE,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_INSTAGRAM,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_SLACK,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_LINKEDIN,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_AMAZON,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_MAPS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_PHOTOS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_CALENDAR,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_GOOGLE_MESSAGES,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_OUTLOOK,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_SKYPE,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_SNAPCHAT,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_LINE,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_WECHAT,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_KIK,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_VIBER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_KAKAOTALK,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_BLACKBERRY_MESSENGER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_YAHOO_MAIL,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_WEATHER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_MUSIC,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_LOCATION,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_REMINDER,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_WARNING,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_DISCORD,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_TEAMS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_GOOGLE_CHAT,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_SIGNAL,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_REDDIT,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_YOUTUBE,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_ZOOM,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_TWITCH,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_GOOGLE_TASKS,
    RESOURCE_ID_PEBBLEOS_NOTIFICATION_TESLA,
};
static GDrawCommandImage* notification_icons[ARRAY_LENGTH(icon_resource_ids)];

static GFont system_theme_get_font_for_default_size(const TextStyleFont font)
{
    switch (font)
    {
    case TextStyleFont_Header:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    case TextStyleFont_MenuCellTitle:
    case TextStyleFont_Title:
    case TextStyleFont_Body:
        return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    case TextStyleFont_MenuCellSubtitle:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    case TextStyleFont_Caption:
        return fonts_get_system_font(FONT_KEY_GOTHIC_14);
    case TextStyleFont_Footer:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    default:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    }
}

static int16_t pebbleos_fonts_get_font_height(GFont font)
{
    return graphics_text_layout_get_content_size(
        "Ag",
        font,
        GRect(0, 0, 100, 60),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentLeft
    ).h;
}

static int16_t pebbleos_menu_cell_basic_horizontal_inset(void)
{
    return 5;
}

static GColor color_for_id(const uint8_t color_id)
{
    switch (color_id)
    {
    case 1:
        return PBL_IF_COLOR_ELSE(GColorRed, GColorBlack);
    case 2:
        return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
    case 3:
        return PBL_IF_COLOR_ELSE(GColorBlueMoon, GColorBlack);
    case 4:
        return PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    case 5:
        return PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorBlack);
    case 6:
        return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    case 7:
        return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
    case 8:
        return PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    case 9:
        return PBL_IF_COLOR_ELSE(GColorFolly, GColorBlack);
    case 10:
        return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    case 11:
        return PBL_IF_COLOR_ELSE(GColorVividViolet, GColorBlack);
    case 12:
        return PBL_IF_COLOR_ELSE(GColorIndigo, GColorBlack);
    case 13:
        return PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack);
    case 14:
        return PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack);
    case 15:
        return PBL_IF_COLOR_ELSE(GColorYellow, GColorBlack);
    case 16:
        return PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
    default:
        return PBL_IF_COLOR_ELSE(DEFAULT_NOTIFICATION_COLOR, GColorBlack);
    }
}

GColor window_notification_ui_get_primary_color()
{
    return color_for_id(window_notification_data.color_id);
}

static GDrawCommandImage* create_normalized_tiny_icon_with_resource(uint32_t resource_id);

static GDrawCommandImage* icon_for_id(uint8_t icon_id)
{
    if (icon_id >= ARRAY_LENGTH(notification_icons))
    {
        icon_id = 0;
    }

    if (notification_icons[icon_id] == NULL)
    {
        notification_icons[icon_id] =
            create_normalized_tiny_icon_with_resource(icon_resource_ids[icon_id]);
    }
    if (notification_icons[icon_id] == NULL && icon_id != 0)
    {
        return icon_for_id(0);
    }

    return notification_icons[icon_id];
}

static bool normalize_tiny_icon_stroke_width(GDrawCommand* command, uint32_t index, void* context)
{
    (void)index;
    (void)context;

    if (gdraw_command_get_stroke_width(command) > 0)
    {
        gdraw_command_set_stroke_width(command, 2);
    }

    return true;
}

static void normalize_tiny_icon(GDrawCommandImage* icon)
{
    if (icon == NULL)
    {
        return;
    }

    GDrawCommandList* command_list = gdraw_command_image_get_command_list(icon);
    gdraw_command_list_iterate(command_list, normalize_tiny_icon_stroke_width, NULL);
}

static GDrawCommandImage* create_normalized_tiny_icon_with_resource(const uint32_t resource_id)
{
    GDrawCommandImage* icon = gdraw_command_image_create_with_resource(resource_id);
    if (icon == NULL)
    {
        return NULL;
    }

    GDrawCommandImage* writable_icon = gdraw_command_image_clone(icon);
    if (writable_icon == NULL)
    {
        return icon;
    }

    gdraw_command_image_destroy(icon);
    normalize_tiny_icon(writable_icon);
    return writable_icon;
}

static bool is_empty_string(const char* text)
{
    return text == NULL || text[0] == '\0';
}

static void clear_detail_body_cache_entry(DetailBodyCacheEntry* entry)
{
    if (entry == NULL)
    {
        return;
    }

    if (entry->body != NULL)
    {
        free(entry->body);
    }
    entry->bucket_id = 0;
    entry->receive_time = 0;
    entry->body = NULL;
    entry->num_actions = 0;
    entry->has_actions = false;
}

static const DetailBodyCacheEntry* cached_detail_for_item(const NotificationListItem* item)
{
    if (item == NULL)
    {
        return NULL;
    }

    const DetailBodyCacheEntry* bucket_only_match = NULL;
    for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
    {
        if (detail_body_cache[i].body != NULL &&
            detail_body_cache[i].bucket_id == item->bucket_id)
        {
            if (detail_body_cache[i].receive_time == item->receive_time)
            {
                return &detail_body_cache[i];
            }
            if (detail_body_cache[i].receive_time == 0)
            {
                bucket_only_match = &detail_body_cache[i];
            }
        }
    }

    return bucket_only_match;
}

static const char* cached_body_for_item(const NotificationListItem* item)
{
    const DetailBodyCacheEntry* entry = cached_detail_for_item(item);
    return entry != NULL ? entry->body : NULL;
}

static const char* body_for_detail_item(const NotificationListItem* item)
{
    const char* cached_body = cached_body_for_item(item);
    return cached_body != NULL ? cached_body : item != NULL ? item->body : "";
}

static bool should_fetch_full_detail_before_open(const NotificationListItem* item)
{
    const DetailBodyCacheEntry* cached_detail = cached_detail_for_item(item);
    return item != NULL &&
        notification_item_has_loaded_content(item) &&
        (cached_detail == NULL || !cached_detail->has_actions);
}

static bool item_has_renderable_detail(const NotificationListItem* item)
{
    return item != NULL &&
        notification_item_has_loaded_content(item);
}

static void prefetch_detail_for_item(const NotificationListItem* item)
{
    if (!should_fetch_full_detail_before_open(item))
    {
        return;
    }

    notification_details_fetcher_prefetch(item->bucket_id);
}

static void prefetch_detail_around_index(const int16_t selected_index)
{
    if (selected_index < 0 || selected_index >= notification_item_count)
    {
        return;
    }

    prefetch_detail_for_item(&notification_items[selected_index]);

    for (int16_t distance = 1; distance <= 2; distance++)
    {
        const int16_t next_index = selected_index + distance;
        if (next_index < notification_item_count)
        {
            prefetch_detail_for_item(&notification_items[next_index]);
        }

        const int16_t previous_index = selected_index - distance;
        if (previous_index >= 0)
        {
            prefetch_detail_for_item(&notification_items[previous_index]);
        }
    }
}

static bool notification_item_exists(const uint8_t bucket_id, const time_t receive_time)
{
    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_items[i].bucket_id == bucket_id &&
            (receive_time == 0 || notification_items[i].receive_time == receive_time))
        {
            return true;
        }
    }

    return false;
}

static void prune_detail_body_cache(void)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
    {
        if (detail_body_cache[i].body != NULL &&
            !notification_item_exists(detail_body_cache[i].bucket_id,
                                      detail_body_cache[i].receive_time))
        {
            clear_detail_body_cache_entry(&detail_body_cache[i]);
        }
    }
}

static void cache_detail_body_sized(uint8_t bucket_id, time_t receive_time,
                                    const char* body, size_t body_size);
static void cache_detail_payload_sized(uint8_t bucket_id, time_t receive_time,
                                       const char* body, size_t body_size,
                                       const Action* actions, uint8_t num_actions);

static void cache_detail_body(const uint8_t bucket_id, const time_t receive_time,
                              const char* body)
{
    if (body == NULL)
    {
        return;
    }

    const size_t body_size = MIN(strlen(body), MAX_BODY_TEXT_SIZE);
    cache_detail_body_sized(bucket_id, receive_time, body, body_size);
}

static void cache_detail_body_sized(const uint8_t bucket_id, const time_t receive_time,
                                    const char* body, const size_t body_size)
{
    cache_detail_payload_sized(bucket_id, receive_time, body, body_size, NULL, 0);
}

static void cache_detail_payload_sized(const uint8_t bucket_id, const time_t receive_time,
                                       const char* body, const size_t body_size,
                                       const Action* actions, const uint8_t num_actions)
{
    if (bucket_id == 0 || body == NULL)
    {
        return;
    }

    uint8_t cache_slot = ARRAY_LENGTH(detail_body_cache);
    for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
    {
        if (detail_body_cache[i].body != NULL &&
            detail_body_cache[i].bucket_id == bucket_id &&
            (detail_body_cache[i].receive_time == receive_time ||
                detail_body_cache[i].receive_time == 0 ||
                receive_time == 0))
        {
            cache_slot = i;
            break;
        }
    }

    if (cache_slot == ARRAY_LENGTH(detail_body_cache))
    {
        for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
        {
            if (detail_body_cache[i].body == NULL)
            {
                cache_slot = i;
                break;
            }
        }
    }

    if (cache_slot == ARRAY_LENGTH(detail_body_cache))
    {
        cache_slot = next_detail_body_cache_slot;
        next_detail_body_cache_slot =
            (next_detail_body_cache_slot + 1) % ARRAY_LENGTH(detail_body_cache);
    }

    const size_t clipped_body_size = MIN(body_size, MAX_BODY_TEXT_SIZE);
    char* cached_body = malloc(clipped_body_size + 1);
    if (cached_body == NULL)
    {
        return;
    }

    memcpy(cached_body, body, clipped_body_size);
    cached_body[clipped_body_size] = '\0';
    clear_detail_body_cache_entry(&detail_body_cache[cache_slot]);
    detail_body_cache[cache_slot].bucket_id = bucket_id;
    detail_body_cache[cache_slot].receive_time = receive_time;
    detail_body_cache[cache_slot].body = cached_body;
    detail_body_cache[cache_slot].num_actions = MIN(num_actions, MAX_NOTIFICATION_ACTIONS);
    detail_body_cache[cache_slot].has_actions = actions != NULL;
    if (actions != NULL && detail_body_cache[cache_slot].num_actions > 0)
    {
        memcpy(detail_body_cache[cache_slot].actions, actions,
               sizeof(Action) * detail_body_cache[cache_slot].num_actions);
    }
}

static void clear_detail_body_cache(void)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
    {
        clear_detail_body_cache_entry(&detail_body_cache[i]);
    }
    next_detail_body_cache_slot = 0;
}

static void strip_sender_prefix_in_place(char* text, const char* sender)
{
    if (is_empty_string(text) || is_empty_string(sender))
    {
        return;
    }

    const size_t sender_length = strlen(sender);
    if (strncmp(text, sender, sender_length) != 0)
    {
        return;
    }

    if (text[sender_length] == ':' && text[sender_length + 1] == ' ')
    {
        memmove(text, text + sender_length + 2, strlen(text + sender_length + 2) + 1);
    }
    else if (text[sender_length] == '\n')
    {
        memmove(text, text + sender_length + 1, strlen(text + sender_length + 1) + 1);
    }
}

static const char* copy_body_without_sender_prefix(char* buffer, const size_t buffer_size,
                                                   const char* body, const char* sender)
{
    if (buffer_size == 0)
    {
        return "";
    }

    strncpy(buffer, body != NULL ? body : "", buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    strip_sender_prefix_in_place(buffer, sender);
    return buffer;
}

// PebbleOS source: src/fw/apps/system/notifications.c
// Adapted only where third-party SDK APIs differ from firmware-private APIs.
static void prv_menu_cell_basic_draw_custom(GContext* ctx, const Layer* cell_layer, GFont title_font,
                                            const char* title, GFont subtitle_font,
                                            const char* subtitle)
{
    const GRect bounds = layer_get_bounds(cell_layer);
    const int16_t title_height = pebbleos_fonts_get_font_height(title_font);
    const int16_t subtitle_height = subtitle != NULL ? pebbleos_fonts_get_font_height(subtitle_font) : 0;
    const int16_t full_height = title_height + subtitle_height + 10;
    const int horizontal_margin = pebbleos_menu_cell_basic_horizontal_inset();
    const int vertical_margin = (bounds.size.h - full_height) / 2;

    GRect box = bounds;
    box.origin.x += horizontal_margin;
    box.size.w -= horizontal_margin;
    box.origin.y += vertical_margin;
    box.size.h = title_height + 4;

    if (title != NULL)
    {
        graphics_draw_text(ctx, title, title_font, box, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft, NULL);
    }

    if (subtitle != NULL)
    {
        box.origin.y += title_height;
        box.size.h = subtitle_height + 4;
        graphics_draw_text(ctx, subtitle, subtitle_font, box, GTextOverflowModeTrailingEllipsis,
                           GTextAlignmentLeft, NULL);
    }
}

// PebbleOS source: src/fw/apps/system/notifications.c::prv_draw_notification_cell_rect
static void prv_draw_notification_cell_rect(GContext* ctx, const Layer* cell_layer,
                                            const char* title, const char* subtitle,
                                            GDrawCommandImage* icon)
{
    const GRect cell_layer_bounds = layer_get_bounds(cell_layer);
    GSize icon_size = GSize(0, 0);
    const int16_t icon_left_margin = pebbleos_menu_cell_basic_horizontal_inset();
    if (icon)
    {
        icon_size = gdraw_command_image_get_bounds_size(icon);

        GRect box = cell_layer_bounds;
        box.origin.x += icon_left_margin;

        GRect icon_rect = (GRect){ .size = icon_size };
        grect_align(&icon_rect, &box, GAlignLeft, false);

        gdraw_command_image_draw(ctx, icon, icon_rect.origin);
    }

    const int text_left_margin = icon_left_margin + MAX(icon_size.w, ATTRIBUTE_ICON_TINY_SIZE_PX);
    Layer* mutable_cell_layer = (Layer*)cell_layer;
    layer_set_bounds(mutable_cell_layer,
                     grect_inset(cell_layer_bounds, GEdgeInsets(0, 5, 0, text_left_margin)));

    const GFont title_font = system_theme_get_font_for_default_size(TextStyleFont_MenuCellTitle);
    const GFont subtitle_font = system_theme_get_font_for_default_size(TextStyleFont_Caption);
    prv_menu_cell_basic_draw_custom(ctx, cell_layer, title_font, title, subtitle_font, subtitle);

    layer_set_bounds(mutable_cell_layer, cell_layer_bounds);
}

static void resolve_pebbleos_cell_text(const NotificationListItem* item,
                                       const char** title,
                                       const char** subtitle)
{
    const char* resolved_title = item->title;
    const char* resolved_subtitle = NULL;
    const char* app_name = item->app_name;
    const char* body = item->body;

    // PebbleOS source: if the tiny resource is the default icon, show the app name.
    if (!is_empty_string(app_name) && item->icon_id == 0)
    {
        resolved_title = app_name;
    }

    if (!is_empty_string(resolved_title) && !is_empty_string(resolved_subtitle))
    {
    }
    else if (is_empty_string(resolved_title) && is_empty_string(resolved_subtitle))
    {
        if (is_empty_string(body))
        {
            resolved_title = "[Empty]";
        }
        else
        {
            resolved_title = body;
            resolved_subtitle = strchr(body, '\n');
        }
    }
    else if (is_empty_string(resolved_title))
    {
        resolved_title = resolved_subtitle;
        resolved_subtitle = body;
    }
    else if (is_empty_string(resolved_subtitle))
    {
        resolved_subtitle = copy_body_without_sender_prefix(cell_body_buffer,
                                                            sizeof(cell_body_buffer),
                                                            body,
                                                            resolved_title);
    }

    *title = resolved_title;
    *subtitle = resolved_subtitle;
}

static uint16_t get_num_rows_callback(MenuLayer* layer, uint16_t section_index, void* context)
{
    return notification_item_count;
}

static int16_t get_cell_height_callback(MenuLayer* layer, MenuIndex* cell_index, void* context)
{
    return 46;
}

static void draw_row_callback(GContext* ctx, const Layer* cell_layer, MenuIndex* cell_index,
                              void* context)
{
    if (cell_index->row >= notification_item_count)
    {
        return;
    }

    const NotificationListItem* item = &notification_items[cell_index->row];
    const char* title;
    const char* subtitle;
    resolve_pebbleos_cell_text(item, &title, &subtitle);
    char title_buffer[MAX_NOTIFICATION_TITLE];
    char subtitle_buffer[MAX_NOTIFICATION_SNIPPET];
    strncpy(title_buffer, title != NULL ? title : "", sizeof(title_buffer) - 1);
    title_buffer[sizeof(title_buffer) - 1] = '\0';
    if (subtitle != NULL)
    {
        strncpy(subtitle_buffer, subtitle, sizeof(subtitle_buffer) - 1);
        subtitle_buffer[sizeof(subtitle_buffer) - 1] = '\0';
        subtitle = subtitle_buffer;
    }
    title = title_buffer;

    prv_draw_notification_cell_rect(ctx, cell_layer, title, subtitle, icon_for_id(item->icon_id));
}

static void empty_state_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    static const char* text = "No Notifications";
    GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    const int16_t text_height = graphics_text_layout_get_content_size(
        text,
        font,
        bounds,
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter
    ).h;
    const GRect text_frame = GRect(bounds.origin.x,
                                  bounds.origin.y + (bounds.size.h - text_height) / 2,
                                  bounds.size.w,
                                  text_height);

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx,
                       text,
                       font,
                       text_frame,
                       GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter,
                       NULL);
}

static int16_t scale_logo_point(const int16_t value)
{
    return (value * 3) / 2;
}

static GPoint logo_point(const GPoint origin, const int16_t x, const int16_t y)
{
    return GPoint(origin.x + scale_logo_point(x), origin.y + scale_logo_point(y));
}

static void draw_phone_launch_logo_line(GContext* ctx, const GPoint a, const GPoint b)
{
    for (int8_t dx = -1; dx <= 1; dx++)
    {
        for (int8_t dy = -1; dy <= 1; dy++)
        {
            graphics_draw_line(ctx,
                               GPoint(a.x + dx, a.y + dy),
                               GPoint(b.x + dx, b.y + dy));
        }
    }
}

static void phone_launch_pending_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    graphics_context_set_stroke_color(ctx, GColorBlack);
    const int16_t logo_size = scale_logo_point(50);
    const GPoint origin = GPoint(bounds.origin.x + (bounds.size.w - logo_size) / 2,
                                 bounds.origin.y + (bounds.size.h - logo_size) / 2 - 2);
    const GPoint outline[] = {
        logo_point(origin, 10, 15),
        logo_point(origin, 10, 27),
        logo_point(origin, 4, 33),
        logo_point(origin, 4, 41),
        logo_point(origin, 44, 41),
        logo_point(origin, 44, 33),
        logo_point(origin, 38, 27),
        logo_point(origin, 38, 15),
        logo_point(origin, 30, 7),
        logo_point(origin, 18, 7),
        logo_point(origin, 10, 15),
    };

    for (uint8_t i = 1; i < ARRAY_LENGTH(outline); i++)
    {
        draw_phone_launch_logo_line(ctx, outline[i - 1], outline[i]);
    }
    draw_phone_launch_logo_line(ctx, logo_point(origin, 4, 33), logo_point(origin, 20, 33));
    draw_phone_launch_logo_line(ctx, logo_point(origin, 24, 6), logo_point(origin, 24, 2));
    draw_phone_launch_logo_line(ctx, logo_point(origin, 34, 41), logo_point(origin, 36, 47));
}

static void phone_launch_pending_animation_update(Animation* animation, const AnimationProgress progress)
{
    (void)animation;

    if (phone_launch_pending_layer == NULL)
    {
        return;
    }

    GRect frame = phone_launch_pending_start_frame;
    const int32_t range =
        (int32_t)phone_launch_pending_end_frame.origin.y -
        (int32_t)phone_launch_pending_start_frame.origin.y;
    frame.origin.y = phone_launch_pending_start_frame.origin.y +
        (int16_t)((range * (int32_t)progress) / ANIMATION_NORMALIZED_MAX);
    layer_set_frame(phone_launch_pending_layer, frame);
}

static void phone_launch_pending_animation_stopped(Animation* animation, const bool finished, void* context)
{
    (void)animation;
    (void)finished;
    (void)context;

    if (phone_launch_pending_animation != NULL)
    {
        animation_destroy(phone_launch_pending_animation);
        phone_launch_pending_animation = NULL;
    }

    phone_launch_pending_exit_animating = false;
    if (phone_launch_pending_layer != NULL && notification_window != NULL)
    {
        Layer* window_layer = window_get_root_layer(notification_window);
        layer_set_frame(phone_launch_pending_layer, layer_get_bounds(window_layer));
    }
    update_list_layer_visibility();
}

static void start_phone_launch_pending_exit_animation(void)
{
    if (phone_launch_pending_layer == NULL || notification_window == NULL)
    {
        phone_launch_pending_exit_animating = false;
        return;
    }

    if (phone_launch_pending_animation != NULL)
    {
        Animation* animation = phone_launch_pending_animation;
        phone_launch_pending_animation = NULL;
        animation_unschedule(animation);
        animation_destroy(animation);
    }

    Layer* window_layer = window_get_root_layer(notification_window);
    phone_launch_pending_start_frame = layer_get_bounds(window_layer);
    phone_launch_pending_end_frame = phone_launch_pending_start_frame;
    phone_launch_pending_end_frame.origin.y = -phone_launch_pending_start_frame.size.h;

    layer_set_frame(phone_launch_pending_layer, phone_launch_pending_start_frame);
    layer_set_hidden(phone_launch_pending_layer, false);
    layer_mark_dirty(phone_launch_pending_layer);

    phone_launch_pending_animation = animation_create();
    if (phone_launch_pending_animation == NULL)
    {
        phone_launch_pending_exit_animating = false;
        update_list_layer_visibility();
        return;
    }

    phone_launch_pending_exit_animating = true;
    static const AnimationImplementation implementation =
    {
        .update = phone_launch_pending_animation_update,
    };
    animation_set_implementation(phone_launch_pending_animation, &implementation);
    animation_set_duration(phone_launch_pending_animation, 160);
    animation_set_curve(phone_launch_pending_animation, AnimationCurveEaseIn);
    animation_set_handlers(phone_launch_pending_animation,
                           (AnimationHandlers)
                           {
                               .stopped = phone_launch_pending_animation_stopped,
                           },
                           NULL);
    animation_schedule(phone_launch_pending_animation);
}

static void select_callback(MenuLayer* layer, MenuIndex* cell_index, void* context)
{
    (void)layer;
    (void)context;

    if (window_notification_data.detail_open)
    {
        return;
    }

    window_notification_data.currently_selected_bucket_index = cell_index->row;
    if (cell_index->row < notification_item_count)
    {
        window_notification_data.currently_selected_bucket = notification_items[cell_index->row].bucket_id;
    }
    detail_opened_from_phone_launch = false;
    window_notification_ui_open_selected_detail();
}

static void selection_changed_callback(MenuLayer* layer, MenuIndex new_index, MenuIndex old_index,
                                       void* context)
{
    (void)layer;
    (void)old_index;
    (void)context;

    if (new_index.row >= notification_item_count)
    {
        return;
    }

    window_notification_data.currently_selected_bucket_index = new_index.row;
    window_notification_data.currently_selected_bucket = notification_items[new_index.row].bucket_id;
    pending_manual_swap_bucket_id = -1;
    prefetch_detail_around_index(new_index.row);
}

static void reload_menu_layer(void)
{
    if (menu_layer == NULL)
    {
        return;
    }

    int16_t selected_index = window_notification_data.currently_selected_bucket_index;
    menu_layer_reload_data(menu_layer);
    update_list_layer_visibility();
    if (notification_item_count == 0)
    {
        return;
    }

    if (selected_index < 0 || selected_index >= notification_item_count)
    {
        selected_index = 0;
    }

    window_notification_data.currently_selected_bucket_index = selected_index;
    window_notification_data.currently_selected_bucket = notification_items[selected_index].bucket_id;
    menu_layer_set_selected_index(
        menu_layer,
        MenuIndex(0, selected_index),
        MenuRowAlignNone,
        false
    );
    prefetch_detail_around_index(selected_index);
}

static void sync_menu_selection(const MenuRowAlign align, const bool animated)
{
    if (menu_layer == NULL || notification_item_count == 0)
    {
        return;
    }

    int16_t selected_index = window_notification_data.currently_selected_bucket_index;
    if (selected_index < 0 || selected_index >= notification_item_count)
    {
        selected_index = 0;
    }

    window_notification_data.currently_selected_bucket_index = selected_index;
    window_notification_data.currently_selected_bucket = notification_items[selected_index].bucket_id;
    menu_layer_set_selected_index(
        menu_layer,
        MenuIndex(0, selected_index),
        align,
        animated
    );
    prefetch_detail_around_index(selected_index);
}

static void cancel_deferred_new_top_timer(void)
{
    if (deferred_new_top_timer != NULL)
    {
        app_timer_cancel(deferred_new_top_timer);
        deferred_new_top_timer = NULL;
    }
}

static bool top_notification_is_selected(void)
{
    return notification_item_count > 0 &&
        window_notification_data.currently_selected_bucket_index == 0 &&
        window_notification_data.currently_selected_bucket == notification_items[0].bucket_id;
}

static bool top_notification_is_ready_to_open(void)
{
    return notification_item_count > 0 &&
        notification_item_has_loaded_content(&notification_items[0]);
}

static void select_top_notification_and_open_detail(void)
{
    if (notification_item_count == 0 || !top_notification_is_ready_to_open())
    {
        return;
    }

    if (window_notification_data.detail_open &&
        !item_has_renderable_detail(&notification_items[0]))
    {
        pending_auto_open_bucket_id = notification_items[0].bucket_id;
        notification_details_fetcher_fetch(notification_items[0].bucket_id);
        return;
    }

    const uint8_t previously_selected_bucket = window_notification_data.currently_selected_bucket;
    if (!top_notification_is_selected())
    {
        window_notification_data.currently_selected_bucket_index = 0;
        window_notification_data.currently_selected_bucket = notification_items[0].bucket_id;
        sync_menu_selection(MenuRowAlignCenter, false);
    }

    if (window_notification_data.detail_open)
    {
        if (previously_selected_bucket != window_notification_data.currently_selected_bucket)
        {
            reload_detail_after_selected_bucket_changed();
        }
        else
        {
            reload_detail_after_selected_bucket_updated();
        }
    }
    else
    {
        detail_opened_from_phone_launch = false;
        window_notification_ui_open_selected_detail();
    }
    pending_auto_open_bucket_id = -1;
}

static void select_pending_notification_and_open_detail(void)
{
    if (pending_auto_open_bucket_id <= 0)
    {
        return;
    }

    const int16_t target_index =
        find_auto_open_notification_index((uint8_t)pending_auto_open_bucket_id);
    if (target_index < 0)
    {
        return;
    }

    if (window_notification_data.detail_open &&
        !item_has_renderable_detail(&notification_items[target_index]))
    {
        notification_details_fetcher_fetch(notification_items[target_index].bucket_id);
        return;
    }

    const uint8_t previously_selected_bucket = window_notification_data.currently_selected_bucket;
    if (window_notification_data.currently_selected_bucket_index != target_index)
    {
        window_notification_data.currently_selected_bucket_index = target_index;
        window_notification_data.currently_selected_bucket = notification_items[target_index].bucket_id;
        sync_menu_selection(MenuRowAlignCenter, false);
    }

    if (window_notification_data.detail_open)
    {
        if (previously_selected_bucket != window_notification_data.currently_selected_bucket)
        {
            reload_detail_after_selected_bucket_changed();
        }
        else
        {
            reload_detail_after_selected_bucket_updated();
        }
    }
    else
    {
        detail_opened_from_phone_launch = false;
        window_notification_ui_open_selected_detail();
    }
    pending_auto_open_bucket_id = -1;
}

static void handle_deferred_new_top_timer(void* context);

static void schedule_deferred_new_top_selection(void)
{
    cancel_deferred_new_top_timer();

    const uint32_t wait_ms = idle_handler_ms_until_current_notification_release();
    if (wait_ms > 0)
    {
        deferred_new_top_timer = app_timer_register(wait_ms + 100, handle_deferred_new_top_timer, NULL);
    }
}

static void handle_deferred_new_top_timer(void* context)
{
    (void)context;
    deferred_new_top_timer = NULL;

    if (idle_handler_should_keep_current_notification())
    {
        schedule_deferred_new_top_selection();
        return;
    }

    if (phone_launch_detail_pending)
    {
        maybe_open_phone_launch_detail();
        if (!phone_launch_detail_pending)
        {
            return;
        }
    }

    if (pending_auto_open_bucket_id > 0)
    {
        select_pending_notification_and_open_detail();
    }
    else
    {
        select_top_notification_and_open_detail();
    }
}

static bool notification_item_has_loaded_content(const NotificationListItem* item)
{
    return item != NULL &&
        (strcmp(item->app_name, "Loading") != 0 ||
            strcmp(item->title, "Syncing notification") != 0);
}

static bool notification_item_is_auto_open_candidate(const NotificationListItem* item)
{
    return item != NULL &&
        item->state == UNREAD &&
        notification_item_has_loaded_content(item);
}

static int16_t find_notification_index(const uint8_t bucket_id, const bool require_loaded_content)
{
    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_items[i].bucket_id == bucket_id &&
            (!require_loaded_content || notification_item_has_loaded_content(&notification_items[i])))
        {
            return i;
        }
    }

    return -1;
}

static int16_t find_auto_open_notification_index(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_items[i].bucket_id == bucket_id &&
            notification_item_is_auto_open_candidate(&notification_items[i]))
        {
            return i;
        }
    }

    return -1;
}

static int16_t find_phone_launch_detail_index(void)
{
    if (phone_launch_detail_bucket_id > 0)
    {
        for (uint8_t i = 0; i < notification_item_count; i++)
        {
            if (notification_items[i].bucket_id == phone_launch_detail_bucket_id)
            {
                return notification_item_has_loaded_content(&notification_items[i]) ? i : -1;
            }
        }
    }

    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_items[i].state == UNREAD &&
            notification_item_has_loaded_content(&notification_items[i]))
        {
            return i;
        }
    }

    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_item_has_loaded_content(&notification_items[i]))
        {
            return i;
        }
    }

    return -1;
}

static void maybe_open_phone_launch_detail(void)
{
    if (!phone_launch_detail_pending ||
        window_notification_data.using_dummy_data ||
        notification_item_count == 0)
    {
        return;
    }

    const int16_t target_index = find_phone_launch_detail_index();
    if (target_index < 0)
    {
        return;
    }

    if (detail_window != NULL)
    {
        if (idle_handler_should_keep_current_notification())
        {
            if (!item_has_renderable_detail(&notification_items[target_index]))
            {
                notification_details_fetcher_fetch(notification_items[target_index].bucket_id);
            }
            schedule_deferred_new_top_selection();
            return;
        }

        if (!item_has_renderable_detail(&notification_items[target_index]))
        {
            notification_details_fetcher_fetch(notification_items[target_index].bucket_id);
            return;
        }

        if (window_notification_data.currently_selected_bucket_index == target_index)
        {
            phone_launch_detail_pending = false;
            phone_launch_detail_bucket_id = -1;
            update_list_layer_visibility();
            detail_opened_from_phone_launch = true;
            reload_detail_after_selected_bucket_updated();
            restore_detail_click_config();
            flush_deferred_phone_launch_vibration();
            return;
        }

        window_notification_data.currently_selected_bucket_index = target_index;
        window_notification_data.currently_selected_bucket = notification_items[target_index].bucket_id;
        sync_menu_selection(MenuRowAlignCenter, false);
        detail_opened_from_phone_launch = true;
        reload_detail_after_selected_bucket_changed();
        if (window_notification_data.currently_selected_bucket_index == target_index &&
            item_has_renderable_detail(&notification_items[target_index]))
        {
            phone_launch_detail_pending = false;
            phone_launch_detail_bucket_id = -1;
            update_list_layer_visibility();
            flush_deferred_phone_launch_vibration();
        }
        return;
    }

    if (idle_handler_should_keep_current_notification())
    {
        schedule_deferred_new_top_selection();
        return;
    }

    window_notification_data.currently_selected_bucket_index = target_index;
    window_notification_data.currently_selected_bucket = notification_items[target_index].bucket_id;
    sync_menu_selection(MenuRowAlignCenter, false);
    detail_opened_from_phone_launch = true;
    window_notification_ui_open_selected_detail();
    if (detail_window != NULL)
    {
        phone_launch_detail_pending = false;
        phone_launch_detail_bucket_id = -1;
        update_list_layer_visibility();
        flush_deferred_phone_launch_vibration();
    }
}

void window_notification_ui_open_phone_launch_detail(const uint8_t bucket_id)
{
    phone_launch_detail_pending = true;
    phone_launch_detail_bucket_id = bucket_id > 0 ? bucket_id : -1;
    update_list_layer_visibility();
    maybe_open_phone_launch_detail();
}

void window_notification_ui_play_or_defer_vibration(const uint32_t* durations, const uint32_t num_segments)
{
    if (launched_from_phone_notification() && phone_launch_detail_pending && detail_window == NULL)
    {
        const uint32_t copied_segments = MIN(num_segments, MAX_DEFERRED_VIBE_SEGMENTS);
        memcpy(deferred_phone_launch_vibration,
               durations,
               copied_segments * sizeof(deferred_phone_launch_vibration[0]));
        deferred_phone_launch_vibration_segments = copied_segments;
        return;
    }

    play_vibration_pattern(durations, num_segments);
}

static int16_t measure_text_height(const char* text, GFont font, const int16_t width)
{
    if (is_empty_string(text))
    {
        return 0;
    }

    return graphics_text_layout_get_content_size(
        text,
        font,
        GRect(0, 0, width, 3000),
        GTextOverflowModeWordWrap,
        GTextAlignmentLeft
    ).h;
}

static void format_since_time(char* buffer, size_t buffer_size, time_t timestamp);
static int16_t detail_scroll_max_offset(ScrollLayer* scroll_layer);
static int16_t detail_scroll_current_offset(ScrollLayer* scroll_layer);
static bool begin_detail_dismiss_animation(bool close_app);

static const char* detail_header_text(void)
{
    if (!is_empty_string(window_notification_data.subtitle_text))
    {
        return window_notification_data.subtitle_text;
    }
    if (!is_empty_string(window_notification_data.title_text))
    {
        return window_notification_data.title_text;
    }
    return "Notification";
}

static const char* detail_body_text(void)
{
    return copy_body_without_sender_prefix(detail_body_buffer,
                                           sizeof(detail_body_buffer),
                                           window_notification_data.body_text,
                                           detail_header_text());
}

static const char* item_header_text(const NotificationListItem* item)
{
    if (item == NULL)
    {
        return "Notification";
    }
    if (!is_empty_string(item->title))
    {
        return item->title;
    }
    if (!is_empty_string(item->app_name))
    {
        return item->app_name;
    }
    return "Notification";
}

static bool detail_has_relative_item(const int8_t relative_change)
{
    const int16_t target_index =
        window_notification_data.currently_selected_bucket_index + relative_change;
    return target_index >= 0 && target_index < notification_item_count;
}

static const NotificationListItem* detail_relative_item(const int8_t relative_change)
{
    if (!detail_has_relative_item(relative_change))
    {
        return NULL;
    }

    return &notification_items[
        window_notification_data.currently_selected_bucket_index + relative_change
    ];
}

static void format_since_time(char* buffer, const size_t buffer_size, const time_t timestamp)
{
    if (buffer_size == 0)
    {
        return;
    }

    if (timestamp == 0)
    {
        buffer[0] = '\0';
        return;
    }

    const time_t now = time(NULL);
    const int32_t seconds = now > timestamp ? now - timestamp : 0;
    if (seconds < 60)
    {
        snprintf(buffer, buffer_size, "Just now");
    }
    else if (seconds < 3600)
    {
        const int32_t minutes = seconds / 60;
        snprintf(buffer, buffer_size, "%ld minute%s ago", (long)minutes,
                 minutes == 1 ? "" : "s");
    }
    else if (seconds < 86400)
    {
        const int32_t hours = seconds / 3600;
        snprintf(buffer, buffer_size, "%ld hour%s ago", (long)hours,
                 hours == 1 ? "" : "s");
    }
    else
    {
        const int32_t days = seconds / 86400;
        snprintf(buffer, buffer_size, "%ld day%s ago", (long)days,
                 days == 1 ? "" : "s");
    }
}

static void update_detail_metrics(const int16_t width)
{
    const int16_t text_width = width - (CARD_MARGIN * 2);
    char footer_text[32];
    format_since_time(footer_text, sizeof(footer_text), window_notification_data.receive_time);

    int16_t height = STATUS_BAR_LAYER_HEIGHT + LAYOUT_TOP_BANNER_HEIGHT + 3;
    height += measure_text_height(detail_header_text(),
                                  system_theme_get_font_for_default_size(TextStyleFont_Header),
                                  text_width) + 3;
    height += measure_text_height(detail_body_text(),
                                  system_theme_get_font_for_default_size(TextStyleFont_Body),
                                  text_width) + 3;
    height += measure_text_height(footer_text,
                                  system_theme_get_font_for_default_size(TextStyleFont_Footer),
                                  text_width);
    height += CARD_BOTTOM_PADDING + LAYOUT_ARROW_HEIGHT;
    detail_current_content_height = MAX(height, PBL_DISPLAY_HEIGHT);
    detail_content_height = detail_current_content_height +
        (detail_has_relative_item(1) ? PEEK_PX : 0);
}

// PebbleOS source: src/fw/applib/ui/action_button.c
// Adapted only where SDK layers are opaque and preferred-content private helpers are unavailable.
static int16_t measure_detail_item_height(const NotificationListItem* item, const int16_t width)
{
    const int16_t text_width = width - (CARD_MARGIN * 2);
    const char* header_text = item_header_text(item);
    const char* body_text = copy_body_without_sender_prefix(detail_item_body_buffer,
                                                            sizeof(detail_item_body_buffer),
                                                            body_for_detail_item(item),
                                                            header_text);

    char footer_text[32];
    format_since_time(footer_text, sizeof(footer_text), item != NULL ? item->receive_time : 0);

    int16_t height = STATUS_BAR_LAYER_HEIGHT + LAYOUT_TOP_BANNER_HEIGHT + 3;
    height += measure_text_height(header_text,
                                  system_theme_get_font_for_default_size(TextStyleFont_Header),
                                  text_width) + 3;
    height += measure_text_height(body_text,
                                  system_theme_get_font_for_default_size(TextStyleFont_Body),
                                  text_width) + 3;
    height += measure_text_height(footer_text,
                                  system_theme_get_font_for_default_size(TextStyleFont_Footer),
                                  text_width);
    height += CARD_BOTTOM_PADDING + LAYOUT_ARROW_HEIGHT;
    return MAX(height, PBL_DISPLAY_HEIGHT);
}

static void action_button_draw(GContext* ctx, Layer* layer, GColor fill_color)
{
    const GRect bounds = layer_get_bounds(layer);
    const int radius = PBL_IF_ROUND_ELSE(12, 13);
    GRect rect = { .size = { radius * 2, radius * 2 } };
    grect_align(&rect, &bounds, GAlignRight, false);
    rect.origin.x += radius;
    rect.origin.x += PBL_IF_ROUND_ELSE(1, 8);

    graphics_context_set_fill_color(ctx, fill_color);
    graphics_fill_circle(ctx, grect_center_point(&rect), radius);
}

static void action_button_update_proc(Layer* action_button_layer, GContext* ctx)
{
    action_button_draw(ctx, action_button_layer, GColorBlack);
}

static void update_detail_action_button_visibility(void)
{
    if (detail_action_button_layer == NULL)
    {
        return;
    }

    layer_set_hidden(detail_action_button_layer, window_notification_data.num_actions == 0);
    layer_mark_dirty(detail_action_button_layer);
}

static void set_detail_fixed_layers_hidden(const bool hidden)
{
    if (detail_arrow_layer != NULL)
    {
        if (hidden)
        {
            detail_arrow_hidden = true;
            layer_set_hidden(detail_arrow_layer, true);
        }
        else
        {
            detail_arrow_hidden = true;
            layer_set_hidden(detail_arrow_layer, true);
            update_detail_arrow_visibility();
        }
    }

    if (detail_action_button_layer != NULL)
    {
        if (hidden)
        {
            layer_set_hidden(detail_action_button_layer, true);
        }
        else
        {
            update_detail_action_button_visibility();
        }
    }
}

static int16_t detail_scroll_offset(void)
{
    if (detail_scroll_layer == NULL)
    {
        return 0;
    }

    return MAX(-scroll_layer_get_content_offset(detail_scroll_layer).y, 0);
}

static void update_detail_arrow_visibility(void)
{
    if (detail_arrow_layer == NULL)
    {
        return;
    }

    // PebbleOS source: src/fw/services/timeline/swap_layer.c::prv_update_arrow
    if (detail_transition_delta != 0 || detail_dismiss_active)
    {
        if (!detail_arrow_hidden)
        {
            detail_arrow_hidden = true;
            layer_set_hidden(detail_arrow_layer, true);
            layer_mark_dirty(detail_arrow_layer);
        }
        return;
    }

    const bool viewing_entire_notif = detail_current_content_height == PBL_DISPLAY_HEIGHT;
    const bool at_top = detail_scroll_offset() == 0;
    const bool has_next = detail_has_relative_item(1);
    const bool hidden = !(at_top && (!viewing_entire_notif || has_next));
    if (detail_arrow_hidden != hidden)
    {
        detail_arrow_hidden = hidden;
        layer_set_hidden(detail_arrow_layer, hidden);
        layer_mark_dirty(detail_arrow_layer);
    }
}

static void arrow_layer_update_proc(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    const int16_t center_x = bounds.size.w / 2;
    for (int16_t y = 0; y <= 5; y++)
    {
        graphics_draw_line(ctx, GPoint(center_x - 5 + y, 7 + y),
                           GPoint(center_x + 5 - y, 7 + y));
    }
}

static void draw_detail_icon(GContext* ctx, uint8_t icon_id, GRect frame)
{
    GDrawCommandImage* icon = icon_for_id(icon_id);
    if (icon != NULL)
    {
        GRect icon_rect = { .size = gdraw_command_image_get_bounds_size(icon) };
        grect_align(&icon_rect, &frame, GAlignCenter, false);
        gdraw_command_image_draw(ctx, icon, icon_rect.origin);
    }
}

static void draw_detail_layout(GContext* ctx,
                               const GRect bounds,
                               const int16_t origin_y,
                               const int16_t layout_height,
                               const uint8_t icon_id,
                               const uint8_t color_id,
                               const time_t receive_time,
                               const char* header_text,
                               const char* body_text,
                               const int16_t item_index)
{
    const GColor primary = color_for_id(color_id);
    const int16_t top_banner_height = STATUS_BAR_LAYER_HEIGHT + LAYOUT_TOP_BANNER_HEIGHT;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, origin_y, bounds.size.w, layout_height), 0, GCornerNone);

    graphics_context_set_fill_color(ctx, primary);
    graphics_fill_rect(ctx, GRect(0, origin_y, bounds.size.w, top_banner_height), 0, GCornerNone);

    char time_text[16];
    clock_copy_time_string(time_text, sizeof(time_text));
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(
        ctx,
        time_text,
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(0, origin_y, bounds.size.w, STATUS_BAR_LAYER_HEIGHT),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter,
        NULL
    );

    if (notification_item_count > 1 && item_index >= 0)
    {
        char counter_text[12];
        snprintf(counter_text, sizeof(counter_text), "%u/%u",
                 (unsigned)MIN(item_index + 1, notification_item_count),
                 (unsigned)notification_item_count);
        graphics_draw_text(
            ctx,
            counter_text,
            fonts_get_system_font(FONT_KEY_GOTHIC_14),
            GRect(bounds.size.w - 54, origin_y, 50, STATUS_BAR_LAYER_HEIGHT),
            GTextOverflowModeTrailingEllipsis,
            GTextAlignmentRight,
            NULL
        );
    }

    draw_detail_icon(
        ctx,
        icon_id,
        GRect((bounds.size.w - NOTIFICATION_TINY_RESOURCE_WIDTH) / 2,
              origin_y + STATUS_BAR_LAYER_HEIGHT + CARD_ICON_UPPER_PADDING,
              NOTIFICATION_TINY_RESOURCE_WIDTH,
              NOTIFICATION_TINY_RESOURCE_HEIGHT)
    );

    graphics_context_set_text_color(ctx, GColorBlack);
    const int16_t text_width = bounds.size.w - (CARD_MARGIN * 2);
    int16_t y = origin_y + top_banner_height + 3;

    const GFont header_font = system_theme_get_font_for_default_size(TextStyleFont_Header);
    const int16_t header_height = measure_text_height(header_text, header_font, text_width);
    graphics_draw_text(ctx, header_text, header_font,
                       GRect(CARD_MARGIN, y, text_width, header_height + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    y += header_height + 3;

    const GFont body_font = system_theme_get_font_for_default_size(TextStyleFont_Body);
    const int16_t body_height = measure_text_height(body_text, body_font, text_width);
    graphics_draw_text(ctx, body_text, body_font,
                       GRect(CARD_MARGIN, y, text_width, body_height),
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    y += body_height + 3;

    char footer_text[32];
    format_since_time(footer_text, sizeof(footer_text), receive_time);
    const GFont footer_font = system_theme_get_font_for_default_size(TextStyleFont_Footer);
    const int16_t footer_height = measure_text_height(footer_text, footer_font, text_width);
    graphics_draw_text(ctx, footer_text, footer_font,
                       GRect(CARD_MARGIN, y, text_width, footer_height + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void draw_detail_next_peek(GContext* ctx, const GRect bounds)
{
    const NotificationListItem* next_item = detail_relative_item(1);
    if (next_item == NULL || detail_current_content_height >= bounds.size.h)
    {
        return;
    }

    const int16_t y = detail_current_content_height;
    graphics_context_set_fill_color(ctx, color_for_id(next_item->color_id));
    graphics_fill_rect(ctx, GRect(0, y, bounds.size.w, PEEK_PX), 0, GCornerNone);
    draw_detail_icon(
        ctx,
        next_item->icon_id,
        GRect((bounds.size.w - NOTIFICATION_TINY_RESOURCE_WIDTH) / 2,
              y + CARD_ICON_UPPER_PADDING,
              NOTIFICATION_TINY_RESOURCE_WIDTH,
              NOTIFICATION_TINY_RESOURCE_HEIGHT)
    );
}

static void draw_notification_item_layout(GContext* ctx,
                                          const GRect bounds,
                                          const NotificationListItem* item,
                                          const int16_t origin_y,
                                          const int16_t layout_height,
                                          const int16_t item_index)
{
    const char* header_text = item_header_text(item);
    const char* body_text = copy_body_without_sender_prefix(detail_item_body_buffer,
                                                            sizeof(detail_item_body_buffer),
                                                            body_for_detail_item(item),
                                                            header_text);
    draw_detail_layout(ctx,
                       bounds,
                       origin_y,
                       layout_height,
                       item != NULL ? item->icon_id : 0,
                       item != NULL ? item->color_id : 0,
                       item != NULL ? item->receive_time : 0,
                       header_text,
                       body_text,
                       item_index);
}

static void draw_current_detail_layout(GContext* ctx,
                                       const GRect bounds,
                                       const int16_t origin_y,
                                       const int16_t layout_height,
                                       const int16_t item_index)
{
    draw_detail_layout(ctx,
                       bounds,
                       origin_y,
                       layout_height,
                       window_notification_data.icon_id,
                       window_notification_data.color_id,
                       window_notification_data.receive_time,
                       detail_header_text(),
                       detail_body_text(),
                       item_index);
}

static void draw_detail(GContext* ctx, const GRect bounds)
{
    draw_current_detail_layout(ctx,
                               bounds,
                               0,
                               detail_current_content_height,
                               window_notification_data.currently_selected_bucket_index);
    draw_detail_next_peek(ctx, bounds);
}

static void detail_content_layer_update(Layer* layer, GContext* ctx)
{
    draw_detail(ctx, layer_get_bounds(layer));
}

static void detail_swap_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);
    if (detail_dismiss_active)
    {
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);

        if (detail_dismiss_sequence == NULL)
        {
            return;
        }

        GDrawCommandFrame* frame = gdraw_command_sequence_get_frame_by_elapsed(
            detail_dismiss_sequence,
            detail_dismiss_elapsed);
        if (frame == NULL)
        {
            return;
        }

        const GSize frame_size = gdraw_command_sequence_get_bounds_size(detail_dismiss_sequence);
        const GPoint offset = GPoint(bounds.origin.x + (bounds.size.w - frame_size.w) / 2,
                                    bounds.origin.y + (bounds.size.h - frame_size.h) / 2);
        gdraw_command_frame_draw(ctx, detail_dismiss_sequence, frame, offset);
        return;
    }

    if (detail_transition_delta == 0 ||
        detail_transition_target_index < 0 ||
        detail_transition_target_index >= notification_item_count)
    {
        return;
    }

    if (detail_transition_delta > 0)
    {
        draw_current_detail_layout(ctx,
                                   bounds,
                                   detail_transition_offset,
                                   detail_transition_current_height,
                                   window_notification_data.currently_selected_bucket_index);
        draw_notification_item_layout(ctx,
                                      bounds,
                                      &notification_items[detail_transition_target_index],
                                      detail_transition_offset + detail_transition_current_height,
                                      detail_transition_target_height,
                                      detail_transition_target_index);
    }
    else
    {
        draw_notification_item_layout(ctx,
                                      bounds,
                                      &notification_items[detail_transition_target_index],
                                      detail_transition_offset,
                                      detail_transition_target_height,
                                      detail_transition_target_index);
        draw_current_detail_layout(ctx,
                                   bounds,
                                   detail_transition_offset + detail_transition_target_height,
                                   detail_transition_current_height,
                                   window_notification_data.currently_selected_bucket_index);
    }
}

static void detail_content_offset_changed(ScrollLayer* scroll_layer, void* context)
{
    (void)scroll_layer;
    (void)context;
    update_detail_arrow_visibility();
}

static void reload_detail_content_size(void)
{
    if (detail_scroll_layer == NULL || detail_content_layer == NULL)
    {
        return;
    }

    const int16_t width = PBL_DISPLAY_WIDTH;
    update_detail_metrics(width);
    scroll_layer_set_content_size(detail_scroll_layer, GSize(width, detail_content_height));
    layer_set_frame(detail_content_layer, GRect(0, 0, width, detail_content_height));
    layer_mark_dirty(detail_content_layer);
    update_detail_arrow_visibility();
    update_detail_action_button_visibility();
}

void window_notification_ui_cache_current_body()
{
    cache_detail_body(window_notification_data.currently_selected_bucket,
                      window_notification_data.receive_time,
                      window_notification_data.body_text);
}

void window_notification_ui_cache_body_for_bucket(const uint8_t bucket_id, const char* body,
                                                  const size_t body_size)
{
    window_notification_ui_cache_details_for_bucket(bucket_id, body, body_size, NULL, 0);
}

void window_notification_ui_cache_details_for_bucket(const uint8_t bucket_id, const char* body,
                                                     const size_t body_size,
                                                     const Action* actions,
                                                     const uint8_t num_actions)
{
    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        if (notification_items[i].bucket_id == bucket_id)
        {
            cache_detail_payload_sized(bucket_id,
                                       notification_items[i].receive_time,
                                       body,
                                       body_size,
                                       actions,
                                       num_actions);
            return;
        }
    }

    cache_detail_payload_sized(bucket_id, 0, body, body_size, actions, num_actions);
}

void window_notification_ui_on_details_cached(const uint8_t bucket_id)
{
    if (pending_manual_swap_bucket_id == bucket_id && window_notification_data.detail_open)
    {
        const int16_t target_index = find_notification_index(bucket_id, false);
        const int16_t current_index = window_notification_data.currently_selected_bucket_index;
        if (target_index >= 0 &&
            item_has_renderable_detail(&notification_items[target_index]))
        {
            const int8_t delta = target_index - current_index;
            if (delta == 1 || delta == -1)
            {
                pending_manual_swap_bucket_id = -1;
                begin_detail_swap_animation(delta);
                return;
            }
        }
    }

    if (phone_launch_detail_pending &&
        (phone_launch_detail_bucket_id <= 0 || phone_launch_detail_bucket_id == bucket_id))
    {
        maybe_open_phone_launch_detail();
        return;
    }

    if (pending_auto_open_bucket_id == bucket_id)
    {
        if (window_notification_data.detail_open && idle_handler_should_keep_current_notification())
        {
            schedule_deferred_new_top_selection();
        }
        else
        {
            select_pending_notification_and_open_detail();
        }
        return;
    }

}

void window_notification_ui_replace_current_body(const uint8_t bucket_id, const char* body,
                                                 const size_t body_size)
{
    window_notification_ui_replace_current_details(bucket_id, body, body_size, NULL, 0);
}

void window_notification_ui_replace_current_details(const uint8_t bucket_id, const char* body,
                                                    const size_t body_size,
                                                    const Action* actions,
                                                    const uint8_t num_actions)
{
    if (bucket_id != window_notification_data.currently_selected_bucket)
    {
        return;
    }

    const size_t bytes_to_copy = MIN(body_size, MAX_BODY_TEXT_SIZE);
    const bool body_changed =
        strlen(window_notification_data.body_text) != bytes_to_copy ||
        memcmp(window_notification_data.body_text, body, bytes_to_copy) != 0;
    memcpy(window_notification_data.body_text, body, bytes_to_copy);
    window_notification_data.body_text[bytes_to_copy] = '\0';
    if (actions != NULL)
    {
        window_notification_data.num_actions = MIN(num_actions, MAX_NOTIFICATION_ACTIONS);
        if (window_notification_data.num_actions > 0)
        {
            memcpy(window_notification_data.actions, actions,
                   sizeof(Action) * window_notification_data.num_actions);
        }
        update_detail_action_button_visibility();
    }
    window_notification_ui_cache_details_for_bucket(
        bucket_id,
        window_notification_data.body_text,
        strlen(window_notification_data.body_text),
        actions,
        num_actions
    );
    if (body_changed)
    {
        window_notification_ui_redraw();
    }
}

void window_notification_ui_note_actions_updated()
{
    update_detail_action_button_visibility();
}

void window_notification_ui_uncache_body_for_bucket(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(detail_body_cache); i++)
    {
        if (detail_body_cache[i].bucket_id == bucket_id)
        {
            clear_detail_body_cache_entry(&detail_body_cache[i]);
        }
    }
}

void window_notification_ui_note_bucket_updated(const uint8_t bucket_id)
{
    if (bucket_id > 1)
    {
        updated_notification_bucket_id = bucket_id;
    }
}

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
    app_glance_subtitle[0] = '\0';

    if (notification_item_count > 0)
    {
        const NotificationListItem* item = &notification_items[0];
        // PebbleOS source: launcher/default/app_glance_notifications.c
        const char* text = !is_empty_string(item->title) ? item->title :
            (!is_empty_string(item->body) ? item->body : item->app_name);
        if (!is_empty_string(text))
        {
            strncpy(app_glance_subtitle, text, sizeof(app_glance_subtitle) - 1);
            app_glance_subtitle[sizeof(app_glance_subtitle) - 1] = '\0';
            for (char* c = app_glance_subtitle; *c != '\0'; c++)
            {
                if (*c == '\n' || *c == '\r')
                {
                    *c = ' ';
                }
            }
        }
    }

    app_glance_reload(app_glance_reload_callback, NULL);
}

static void detail_scroll_to_edge(const bool bottom)
{
    if (detail_scroll_layer == NULL)
    {
        return;
    }

    GPoint offset = GPointZero;
    if (bottom && detail_content_height > PBL_DISPLAY_HEIGHT)
    {
        offset.y = PBL_DISPLAY_HEIGHT - detail_content_height;
    }
    scroll_layer_set_content_offset(detail_scroll_layer, offset, false);
    update_detail_arrow_visibility();
}

static void restore_detail_click_config(void)
{
    if (detail_scroll_layer != NULL && detail_window != NULL)
    {
        scroll_layer_set_click_config_onto_window(detail_scroll_layer, detail_window);
    }
}

static void reload_detail_after_selected_bucket_changed(void)
{
    if (!window_notification_data.detail_open ||
        detail_transition_delta != 0 ||
        detail_dismiss_active)
    {
        return;
    }

    window_notification_action_list_hide();
    const NotificationListItem* item =
        &notification_items[window_notification_data.currently_selected_bucket_index];
    if (!item_has_renderable_detail(item))
    {
        pending_auto_open_bucket_id = item->bucket_id;
        notification_details_fetcher_fetch(item->bucket_id);
        return;
    }

    if (!load_selected_detail_data())
    {
        return;
    }

    reload_detail_content_size();
    restore_detail_click_config();
    detail_scroll_to_edge(false);
}

static void reload_detail_after_selected_bucket_updated(void)
{
    if (!window_notification_data.detail_open ||
        detail_transition_delta != 0 ||
        detail_dismiss_active)
    {
        return;
    }

    const bool was_at_bottom = detail_scroll_layer != NULL &&
        detail_scroll_current_offset(detail_scroll_layer) >=
        detail_scroll_max_offset(detail_scroll_layer);
    const int16_t previous_offset = detail_scroll_current_offset(detail_scroll_layer);
    const NotificationListItem* item =
        &notification_items[window_notification_data.currently_selected_bucket_index];
    if (!item_has_renderable_detail(item))
    {
        pending_auto_open_bucket_id = item->bucket_id;
        notification_details_fetcher_fetch(item->bucket_id);
        return;
    }

    if (!load_selected_detail_data())
    {
        return;
    }

    reload_detail_content_size();
    restore_detail_click_config();
    if (was_at_bottom)
    {
        detail_scroll_to_edge(true);
    }
    else
    {
        detail_scroll_to_offset(detail_scroll_layer, previous_offset, false);
    }
}

void window_notification_ui_set_items(const NotificationListItem* items, const uint8_t count,
                                      const bool using_dummy_data)
{
    const uint8_t previously_selected_bucket = window_notification_data.currently_selected_bucket;
    const int16_t previously_selected_index = window_notification_data.currently_selected_bucket_index;
    const bool previously_using_dummy_data = window_notification_data.using_dummy_data;
    const uint8_t previously_top_bucket =
        notification_item_count > 0 ? notification_items[0].bucket_id : 0;
    const time_t previously_top_receive_time =
        notification_item_count > 0 ? notification_items[0].receive_time : 0;

    notification_item_count = MIN(count, MAX_NOTIFICATION_ITEMS);
    for (uint8_t i = 0; i < notification_item_count; i++)
    {
        notification_items[i] = items[i];
    }

    const bool has_real_top = notification_item_count > 0 && !using_dummy_data;
    const bool had_previous_real_top = previously_top_bucket != 0 && !previously_using_dummy_data;
    const bool initial_real_list_load =
        has_real_top &&
        !has_seen_real_notification_items &&
        !window_notification_data.detail_open &&
        !launched_from_phone_notification();
    const bool top_bucket_changed =
        has_real_top &&
        had_previous_real_top &&
        notification_items[0].bucket_id != previously_top_bucket &&
        notification_item_is_auto_open_candidate(&notification_items[0]);
    const bool top_bucket_updated =
        has_real_top &&
        had_previous_real_top &&
        notification_items[0].bucket_id == previously_top_bucket &&
        notification_items[0].receive_time != previously_top_receive_time &&
        notification_item_is_auto_open_candidate(&notification_items[0]);
    const bool allow_generic_auto_open = !phone_launch_detail_pending;
    if (updated_notification_bucket_id > 0)
    {
        if (allow_generic_auto_open && !initial_real_list_load)
        {
            pending_auto_open_bucket_id = updated_notification_bucket_id;
        }
        updated_notification_bucket_id = -1;
    }
    else if (allow_generic_auto_open && (top_bucket_changed || top_bucket_updated))
    {
        pending_auto_open_bucket_id = notification_items[0].bucket_id;
    }
    else if (notification_item_count == 0 ||
             (pending_auto_open_bucket_id > 0 &&
                 find_notification_index((uint8_t)pending_auto_open_bucket_id, false) < 0))
    {
        pending_auto_open_bucket_id = -1;
    }
    const int16_t pending_auto_open_index =
        pending_auto_open_bucket_id > 0 ?
            find_auto_open_notification_index((uint8_t)pending_auto_open_bucket_id) : -1;
    const bool pending_auto_open_is_ready = pending_auto_open_index >= 0;
    const bool pending_auto_open_detail_is_renderable =
        pending_auto_open_is_ready &&
        item_has_renderable_detail(&notification_items[pending_auto_open_index]);
    const bool should_defer_auto_open =
        allow_generic_auto_open &&
        pending_auto_open_is_ready &&
        idle_handler_should_keep_current_notification();
    const bool should_select_auto_open =
        allow_generic_auto_open &&
        pending_auto_open_is_ready &&
        pending_auto_open_detail_is_renderable &&
        !should_defer_auto_open;
    const bool should_fetch_auto_open_detail =
        allow_generic_auto_open &&
        pending_auto_open_is_ready &&
        !pending_auto_open_detail_is_renderable;

    window_notification_data.bucket_count = notification_item_count;
    window_notification_data.using_dummy_data = using_dummy_data;
    if (has_real_top)
    {
        has_seen_real_notification_items = true;
    }
    int16_t selected_index = -1;
    if (previously_selected_bucket != 0)
    {
        for (uint8_t i = 0; i < notification_item_count; i++)
        {
            if (notification_items[i].bucket_id == previously_selected_bucket)
            {
                selected_index = i;
                break;
            }
        }
    }

    if (notification_item_count > 0)
    {
        if (should_select_auto_open)
        {
            selected_index = pending_auto_open_index;
        }
        else if (selected_index < 0)
        {
            selected_index = MIN(MAX(previously_selected_index, 0), notification_item_count - 1);
        }
        window_notification_data.currently_selected_bucket_index = selected_index;
        window_notification_data.currently_selected_bucket =
            notification_items[window_notification_data.currently_selected_bucket_index].bucket_id;
    }
    else
    {
        window_notification_data.currently_selected_bucket_index = 0;
        window_notification_data.currently_selected_bucket = 0;
    }

    prune_detail_body_cache();
    update_app_glance();
    reload_menu_layer();
    if (should_fetch_auto_open_detail)
    {
        notification_details_fetcher_fetch(notification_items[pending_auto_open_index].bucket_id);
    }
    if (window_notification_data.detail_open &&
        previously_selected_bucket != window_notification_data.currently_selected_bucket)
    {
        reload_detail_after_selected_bucket_changed();
    }
    else if (window_notification_data.detail_open &&
             previously_selected_bucket == window_notification_data.currently_selected_bucket)
    {
        reload_detail_after_selected_bucket_updated();
    }
    else if (should_select_auto_open)
    {
        detail_opened_from_phone_launch = false;
        window_notification_ui_open_selected_detail();
    }
    if (should_select_auto_open)
    {
        pending_auto_open_bucket_id = -1;
    }
    if (should_defer_auto_open)
    {
        schedule_deferred_new_top_selection();
    }
    else if (pending_auto_open_bucket_id <= 0 || top_notification_is_selected())
    {
        cancel_deferred_new_top_timer();
    }
    if (notification_item_count == 0 && window_notification_data.detail_open && !detail_dismiss_active)
    {
        begin_detail_dismiss_animation(detail_opened_from_phone_launch);
    }
    maybe_open_phone_launch_detail();
}

void window_notification_ui_redraw()
{
    if (window_notification_data.detail_open)
    {
        if (detail_transition_delta != 0 || detail_dismiss_active)
        {
            return;
        }
        const bool was_at_bottom = detail_scroll_layer != NULL &&
            detail_scroll_current_offset(detail_scroll_layer) >=
            detail_scroll_max_offset(detail_scroll_layer);
        reload_detail_content_size();
        restore_detail_click_config();
        if (was_at_bottom)
        {
            detail_scroll_to_edge(true);
        }
    }
    else
    {
        reload_menu_layer();
    }
}

static bool load_selected_detail_data(void)
{
    if (notification_item_count == 0 ||
        window_notification_data.currently_selected_bucket_index < 0 ||
        window_notification_data.currently_selected_bucket_index >= notification_item_count)
    {
        return false;
    }

    const NotificationListItem* item =
        &notification_items[window_notification_data.currently_selected_bucket_index];
    const DetailBodyCacheEntry* cached_detail = cached_detail_for_item(item);
    const bool needs_detail_fetch = cached_detail == NULL || !cached_detail->has_actions;
    const char* body_text = body_for_detail_item(item);

    window_notification_data.currently_selected_bucket = item->bucket_id;
    window_notification_data.icon_id = item->icon_id;
    window_notification_data.color_id = item->color_id;
    window_notification_data.receive_time = item->receive_time;
    strncpy(window_notification_data.title_text, item->app_name,
            sizeof(window_notification_data.title_text) - 1);
    strncpy(window_notification_data.subtitle_text, item->title,
            sizeof(window_notification_data.subtitle_text) - 1);
    strncpy(window_notification_data.body_text, body_text,
            sizeof(window_notification_data.body_text) - 1);
    window_notification_data.title_text[sizeof(window_notification_data.title_text) - 1] = '\0';
    window_notification_data.subtitle_text[sizeof(window_notification_data.subtitle_text) - 1] = '\0';
    window_notification_data.body_text[sizeof(window_notification_data.body_text) - 1] = '\0';
    if (cached_detail != NULL && cached_detail->has_actions)
    {
        window_notification_data.num_actions = MIN(cached_detail->num_actions, MAX_NOTIFICATION_ACTIONS);
        if (window_notification_data.num_actions > 0)
        {
            memcpy(window_notification_data.actions, cached_detail->actions,
                   sizeof(Action) * window_notification_data.num_actions);
        }
    }
    else
    {
        window_notification_data.num_actions = 0;
    }

    if (needs_detail_fetch)
    {
        notification_details_fetcher_fetch(item->bucket_id);
    }
    else
    {
        const int16_t next_index = window_notification_data.currently_selected_bucket_index + 1;
        if (next_index < notification_item_count &&
            should_fetch_full_detail_before_open(&notification_items[next_index]))
        {
            notification_details_fetcher_prefetch(notification_items[next_index].bucket_id);
        }
    }

    return true;
}

static bool switch_detail_by_delta(const int8_t delta)
{
    const int16_t target_index =
        window_notification_data.currently_selected_bucket_index + delta;
    if (target_index < 0 || target_index >= notification_item_count)
    {
        return false;
    }

    window_notification_action_list_hide();
    if (!item_has_renderable_detail(&notification_items[target_index]))
    {
        pending_manual_swap_bucket_id = notification_items[target_index].bucket_id;
        notification_details_fetcher_fetch(notification_items[target_index].bucket_id);
        return false;
    }

    window_notification_data.currently_selected_bucket_index = target_index;
    if (!load_selected_detail_data())
    {
        return false;
    }

    pending_manual_swap_bucket_id = -1;
    reload_menu_layer();
    sync_menu_selection(MenuRowAlignCenter, false);
    reload_detail_content_size();
    detail_scroll_to_edge(delta < 0);
    detail_swap_delay_remaining = MESSAGE_SWAP_DELAY;
    return true;
}

static void show_detail_swap_layer(const bool show)
{
    if (detail_scroll_layer != NULL)
    {
        layer_set_hidden(scroll_layer_get_layer(detail_scroll_layer), show);
    }
    if (detail_swap_layer != NULL)
    {
        layer_set_hidden(detail_swap_layer, !show);
        layer_mark_dirty(detail_swap_layer);
    }
    update_detail_arrow_visibility();
}

static void detail_dismiss_animation_update(Animation* animation, const AnimationProgress progress)
{
    (void)animation;

    uint32_t duration = DISMISS_FALLBACK_MS;
    if (detail_dismiss_sequence != NULL)
    {
        duration = gdraw_command_sequence_get_total_duration(detail_dismiss_sequence);
        if (duration == 0)
        {
            duration = DISMISS_FALLBACK_MS;
        }
    }

    detail_dismiss_elapsed =
        (uint32_t)(((uint64_t)duration * (uint64_t)progress) / ANIMATION_NORMALIZED_MAX);
    if (detail_swap_layer != NULL)
    {
        layer_mark_dirty(detail_swap_layer);
    }
}

static void detail_dismiss_animation_stopped(Animation* animation, const bool finished, void* context)
{
    (void)animation;
    (void)context;

    const bool close_app = detail_dismiss_close_app;
    if (detail_dismiss_animation != NULL)
    {
        animation_destroy(detail_dismiss_animation);
        detail_dismiss_animation = NULL;
    }

    detail_dismiss_active = false;
    detail_dismiss_close_app = false;
    detail_dismiss_elapsed = 0;
    if (detail_dismiss_sequence != NULL)
    {
        gdraw_command_sequence_destroy(detail_dismiss_sequence);
        detail_dismiss_sequence = NULL;
    }
    show_detail_swap_layer(false);
    set_detail_fixed_layers_hidden(false);

    if (!finished)
    {
        reload_detail_content_size();
        return;
    }

    if (close_app)
    {
        send_close_me_without_animation();
    }
    else
    {
        window_notification_ui_close_detail();
    }
}

static bool begin_detail_dismiss_animation(const bool close_app)
{
    if (!window_notification_data.detail_open)
    {
        return false;
    }
    if (detail_dismiss_active || detail_dismiss_animation != NULL ||
        detail_transition_delta != 0 || detail_swap_animation != NULL)
    {
        return true;
    }

    window_notification_action_list_hide();
    window_notification_data.num_actions = 0;
    window_notification_data.num_submenu_actions = 0;
    window_notification_data.open_menu_on_success = 0;
    update_detail_action_button_visibility();

    if (detail_swap_layer == NULL || detail_scroll_layer == NULL)
    {
        if (close_app)
        {
            send_close_me_without_animation();
        }
        else
        {
            window_notification_ui_close_detail();
        }
        return true;
    }

    detail_dismiss_active = true;
    detail_dismiss_close_app = close_app;
    detail_dismiss_elapsed = 0;
    detail_dismiss_sequence =
        gdraw_command_sequence_create_with_resource(RESOURCE_ID_PEBBLEOS_RESULT_DISMISSED_LARGE);
    if (detail_dismiss_sequence == NULL)
    {
        detail_dismiss_active = false;
        detail_dismiss_close_app = false;
        if (close_app)
        {
            send_close_me_without_animation();
        }
        else
        {
            window_notification_ui_close_detail();
        }
        return true;
    }

    show_detail_swap_layer(true);
    set_detail_fixed_layers_hidden(true);

    static const AnimationImplementation implementation =
    {
        .update = detail_dismiss_animation_update,
    };

    detail_dismiss_animation = animation_create();
    if (detail_dismiss_animation == NULL)
    {
        detail_dismiss_active = false;
        detail_dismiss_close_app = false;
        detail_dismiss_elapsed = 0;
        gdraw_command_sequence_destroy(detail_dismiss_sequence);
        detail_dismiss_sequence = NULL;
        show_detail_swap_layer(false);
        set_detail_fixed_layers_hidden(false);
        if (close_app)
        {
            send_close_me_without_animation();
        }
        else
        {
            window_notification_ui_close_detail();
        }
        return true;
    }

    uint32_t duration = gdraw_command_sequence_get_total_duration(detail_dismiss_sequence);
    if (duration == 0)
    {
        duration = DISMISS_FALLBACK_MS;
    }

    animation_set_implementation(detail_dismiss_animation, &implementation);
    animation_set_duration(detail_dismiss_animation, duration);
    animation_set_curve(detail_dismiss_animation, AnimationCurveLinear);
    animation_set_handlers(detail_dismiss_animation,
                           (AnimationHandlers)
                           {
                               .stopped = detail_dismiss_animation_stopped,
                           },
                           NULL);
    animation_schedule(detail_dismiss_animation);
    return true;
}

static void detail_swap_animation_update(Animation* animation, const AnimationProgress progress)
{
    (void)animation;

    const int32_t range =
        (int32_t)detail_transition_end_offset - (int32_t)detail_transition_start_offset;
    detail_transition_offset = detail_transition_start_offset +
        (int16_t)((range * (int32_t)progress) / ANIMATION_NORMALIZED_MAX);

    if (detail_swap_layer != NULL)
    {
        layer_mark_dirty(detail_swap_layer);
    }
}

static void detail_swap_animation_stopped(Animation* animation, const bool finished, void* context)
{
    (void)animation;
    (void)context;

    const int8_t delta = detail_transition_delta;
    if (detail_swap_animation != NULL)
    {
        animation_destroy(detail_swap_animation);
        detail_swap_animation = NULL;
    }

    detail_transition_delta = 0;
    detail_transition_target_index = -1;
    detail_transition_current_height = 0;
    detail_transition_target_height = 0;
    detail_transition_offset = 0;
    detail_transition_start_offset = 0;
    detail_transition_end_offset = 0;

    if (finished && delta != 0)
    {
        switch_detail_by_delta(delta);
    }

    show_detail_swap_layer(false);
    set_detail_fixed_layers_hidden(false);
}

static bool begin_detail_swap_animation(const int8_t delta)
{
    const int16_t target_index =
        window_notification_data.currently_selected_bucket_index + delta;
    if (detail_scroll_layer == NULL || detail_content_layer == NULL ||
        target_index < 0 || target_index >= notification_item_count ||
        detail_swap_animation != NULL || detail_transition_delta != 0 ||
        detail_dismiss_active || detail_dismiss_animation != NULL)
    {
        return false;
    }

    if (!item_has_renderable_detail(&notification_items[target_index]))
    {
        pending_manual_swap_bucket_id = notification_items[target_index].bucket_id;
        notification_details_fetcher_fetch(notification_items[target_index].bucket_id);
        return false;
    }

    if (detail_swap_layer == NULL)
    {
        return switch_detail_by_delta(delta);
    }

    const GRect bounds = layer_get_bounds(detail_swap_layer);
    window_notification_action_list_hide();
    detail_transition_delta = delta;
    detail_transition_target_index = target_index;
    detail_transition_current_height = detail_current_content_height;
    detail_transition_target_height =
        measure_detail_item_height(&notification_items[target_index], bounds.size.w);

    if (delta > 0)
    {
        detail_transition_start_offset = -detail_scroll_current_offset(detail_scroll_layer);
        detail_transition_end_offset = -detail_transition_current_height;
    }
    else
    {
        detail_transition_start_offset = -detail_transition_target_height;
        detail_transition_end_offset =
            -detail_transition_target_height + (PBL_DISPLAY_HEIGHT - PEEK_PX);
    }
    detail_transition_offset = detail_transition_start_offset;

    show_detail_swap_layer(true);
    set_detail_fixed_layers_hidden(true);

    static const AnimationImplementation implementation =
    {
        .update = detail_swap_animation_update,
    };

    detail_swap_animation = animation_create();
    if (detail_swap_animation == NULL)
    {
        detail_transition_delta = 0;
        detail_transition_target_index = -1;
        detail_transition_current_height = 0;
        detail_transition_target_height = 0;
        detail_transition_offset = 0;
        detail_transition_start_offset = 0;
        detail_transition_end_offset = 0;
        show_detail_swap_layer(false);
        set_detail_fixed_layers_hidden(false);
        return switch_detail_by_delta(delta);
    }

    animation_set_implementation(detail_swap_animation, &implementation);
    animation_set_duration(detail_swap_animation, SWAP_MS);
    animation_set_curve(detail_swap_animation,
                        delta > 0 ? AnimationCurveEaseOut : AnimationCurveLinear);
    animation_set_handlers(detail_swap_animation,
                           (AnimationHandlers)
                           {
                               .stopped = detail_swap_animation_stopped,
                           },
                           NULL);
    animation_schedule(detail_swap_animation);
    return true;
}

static int16_t detail_scroll_max_offset(ScrollLayer* scroll_layer)
{
    if (scroll_layer == NULL)
    {
        return 0;
    }

    const GRect frame = layer_get_frame(scroll_layer_get_layer(scroll_layer));
    const GSize content_size = scroll_layer_get_content_size(scroll_layer);
    return MAX(content_size.h - frame.size.h, 0);
}

static int16_t detail_scroll_current_offset(ScrollLayer* scroll_layer)
{
    if (scroll_layer == NULL)
    {
        return 0;
    }

    const int16_t max_offset = detail_scroll_max_offset(scroll_layer);
    return MAX(MIN(-scroll_layer_get_content_offset(scroll_layer).y, max_offset), 0);
}

static void detail_scroll_to_offset(ScrollLayer* scroll_layer, const int16_t offset,
                                    const bool animated)
{
    if (scroll_layer == NULL)
    {
        return;
    }

    const int16_t max_offset = detail_scroll_max_offset(scroll_layer);
    const int16_t clipped_offset = MAX(MIN(offset, max_offset), 0);
    scroll_layer_set_content_offset(scroll_layer, GPoint(0, -clipped_offset), animated);
}

static bool detail_handle_swap_attempt(const DetailScrollDirection direction,
                                       const bool is_repeating)
{
    // PebbleOS source: src/fw/services/timeline/swap_layer.c::prv_handle_swap_attempt
    if (!is_repeating || detail_swap_delay_remaining == 0)
    {
        const int8_t delta = direction == DetailScrollDirectionDown ? 1 : -1;
        const bool swapped = begin_detail_swap_animation(delta);
        detail_swap_delay_remaining = MESSAGE_SWAP_DELAY;
        return swapped;
    }

    detail_swap_delay_remaining--;
    return true;
}

static void detail_attempt_scroll(ScrollLayer* scroll_layer,
                                  const DetailScrollDirection direction,
                                  const bool is_repeating)
{
    if (scroll_layer == NULL)
    {
        return;
    }
    if (detail_transition_delta != 0 || detail_swap_animation != NULL ||
        detail_dismiss_active || detail_dismiss_animation != NULL)
    {
        return;
    }

    // PebbleOS source: src/fw/services/timeline/swap_layer.c::prv_attempt_scroll
    const int16_t offset = detail_scroll_current_offset(scroll_layer);
    const int16_t max_dy = detail_scroll_max_offset(scroll_layer);
    int16_t next_offset = offset;

    switch (direction)
    {
    case DetailScrollDirectionUp:
        if (offset == 0)
        {
            detail_handle_swap_attempt(direction, is_repeating);
            return;
        }
        else if ((offset - FUDGE_PX) < SCROLL_PX)
        {
            next_offset = 0;
        }
        else
        {
            next_offset = offset - (is_repeating ? REPEATING_SCROLL_PX : SCROLL_PX);
        }
        break;
    case DetailScrollDirectionDown:
        if (max_dy == offset)
        {
            detail_handle_swap_attempt(direction, is_repeating);
            return;
        }

        if (offset == 0 && is_repeating && detail_swap_delay_remaining > 0)
        {
            detail_swap_delay_remaining--;
            return;
        }

        detail_swap_delay_remaining = MESSAGE_SWAP_DELAY;
        if ((max_dy - offset - FUDGE_PX) < SCROLL_PX)
        {
            next_offset = max_dy;
        }
        else if (offset == 0)
        {
            next_offset = offset + INITIAL_SCROLL_PX;
        }
        else
        {
            next_offset = offset + (is_repeating ? REPEATING_SCROLL_PX : SCROLL_PX);
        }
        break;
    }

    detail_scroll_to_offset(scroll_layer, next_offset, true);
}

void window_notification_ui_scroll_detail_up(ClickRecognizerRef recognizer, void* context)
{
    ScrollLayer* scroll_layer = context != NULL ? (ScrollLayer*)context : detail_scroll_layer;
    if (!window_notification_data.detail_open)
    {
        return;
    }

    const bool is_repeating = recognizer != NULL && click_recognizer_is_repeating(recognizer);
    detail_attempt_scroll(scroll_layer, DetailScrollDirectionUp, is_repeating);
}

void window_notification_ui_scroll_detail_down(ClickRecognizerRef recognizer, void* context)
{
    ScrollLayer* scroll_layer = context != NULL ? (ScrollLayer*)context : detail_scroll_layer;
    if (!window_notification_data.detail_open)
    {
        return;
    }

    const bool is_repeating = recognizer != NULL && click_recognizer_is_repeating(recognizer);
    detail_attempt_scroll(scroll_layer, DetailScrollDirectionDown, is_repeating);
}

static void detail_window_load(Window* window)
{
    Layer* window_layer = window_get_root_layer(window);
    const GRect bounds = layer_get_bounds(window_layer);

    detail_scroll_layer = scroll_layer_create(bounds);
    detail_content_layer = layer_create(bounds);
    if (detail_scroll_layer == NULL || detail_content_layer == NULL)
    {
        if (detail_scroll_layer != NULL)
        {
            scroll_layer_destroy(detail_scroll_layer);
            detail_scroll_layer = NULL;
        }
        if (detail_content_layer != NULL)
        {
            layer_destroy(detail_content_layer);
            detail_content_layer = NULL;
        }
        window_notification_data.detail_open = false;
        vibes_double_pulse();
        app_timer_register(1, close_failed_detail_window, NULL);
        return;
    }

    layer_set_update_proc(detail_content_layer, detail_content_layer_update);
    detail_swap_layer = layer_create(bounds);
    if (detail_swap_layer != NULL)
    {
        layer_set_update_proc(detail_swap_layer, detail_swap_layer_update);
        layer_set_hidden(detail_swap_layer, true);
    }

    layer_add_child(window_layer, scroll_layer_get_layer(detail_scroll_layer));
    scroll_layer_add_child(detail_scroll_layer, detail_content_layer);
    scroll_layer_set_shadow_hidden(detail_scroll_layer, true);
    scroll_layer_set_callbacks(
        detail_scroll_layer,
        (ScrollLayerCallbacks)
    {
        .content_offset_changed_handler = detail_content_offset_changed,
        .click_config_provider = window_notification_buttons_config,
    });
    scroll_layer_set_click_config_onto_window(detail_scroll_layer, window);

    detail_action_button_layer = layer_create(bounds);
    if (detail_action_button_layer != NULL)
    {
        layer_set_update_proc(detail_action_button_layer, action_button_update_proc);
    }

    detail_arrow_layer = layer_create(GRect(0,
                                            bounds.size.h - LAYOUT_ARROW_HEIGHT,
                                            bounds.size.w,
                                            LAYOUT_ARROW_HEIGHT));
    if (detail_arrow_layer != NULL)
    {
        detail_arrow_hidden = true;
        layer_set_hidden(detail_arrow_layer, true);
        layer_set_update_proc(detail_arrow_layer, arrow_layer_update_proc);
    }
    if (detail_swap_layer != NULL)
    {
        layer_add_child(window_layer, detail_swap_layer);
    }
    if (detail_arrow_layer != NULL)
    {
        layer_add_child(window_layer, detail_arrow_layer);
    }

    if (detail_action_button_layer != NULL)
    {
        layer_add_child(window_layer, detail_action_button_layer);
    }
    reload_detail_content_size();
}

static void detail_window_unload(Window* window)
{
    window_notification_action_list_hide();

    if (detail_dismiss_animation != NULL)
    {
        Animation* dismiss_animation = detail_dismiss_animation;
        detail_dismiss_animation = NULL;
        detail_dismiss_active = false;
        if (animation_is_scheduled(dismiss_animation))
        {
            animation_unschedule(dismiss_animation);
        }
        animation_destroy(dismiss_animation);
    }
    detail_dismiss_active = false;
    detail_dismiss_close_app = false;
    detail_dismiss_elapsed = 0;
    if (detail_dismiss_sequence != NULL)
    {
        gdraw_command_sequence_destroy(detail_dismiss_sequence);
        detail_dismiss_sequence = NULL;
    }

    if (detail_swap_animation != NULL)
    {
        Animation* swap_animation = detail_swap_animation;
        detail_swap_animation = NULL;
        detail_transition_delta = 0;
        if (animation_is_scheduled(swap_animation))
        {
            animation_unschedule(swap_animation);
        }
        animation_destroy(swap_animation);
    }
    detail_transition_delta = 0;
    detail_transition_target_index = -1;

    if (detail_action_button_layer != NULL)
    {
        layer_destroy(detail_action_button_layer);
        detail_action_button_layer = NULL;
    }
    if (detail_arrow_layer != NULL)
    {
        layer_destroy(detail_arrow_layer);
        detail_arrow_layer = NULL;
        detail_arrow_hidden = true;
    }
    if (detail_swap_layer != NULL)
    {
        layer_destroy(detail_swap_layer);
        detail_swap_layer = NULL;
    }
    if (detail_scroll_layer != NULL)
    {
        scroll_layer_destroy(detail_scroll_layer);
        detail_scroll_layer = NULL;
    }
    if (detail_content_layer != NULL)
    {
        layer_destroy(detail_content_layer);
        detail_content_layer = NULL;
    }

    detail_window = NULL;
    window_notification_data.detail_open = false;
    window_destroy(window);
}

void window_notification_ui_open_selected_detail()
{
    if (notification_item_count == 0 || detail_window != NULL)
    {
        return;
    }
    if (window_notification_data.currently_selected_bucket_index < 0 ||
        window_notification_data.currently_selected_bucket_index >= notification_item_count)
    {
        return;
    }

    window_notification_data.detail_open = true;
    if (!load_selected_detail_data())
    {
        window_notification_data.detail_open = false;
        return;
    }

    detail_window = window_create();
    if (detail_window == NULL)
    {
        window_notification_data.detail_open = false;
        vibes_double_pulse();
        return;
    }

    window_set_background_color(detail_window, GColorWhite);
    window_set_window_handlers(
        detail_window,
        (WindowHandlers)
    {
        .load = detail_window_load,
        .unload = detail_window_unload,
    });

    if (detail_opened_from_phone_launch)
    {
        start_phone_launch_pending_exit_animation();
    }
    window_stack_push(detail_window, true);
    if (detail_opened_from_phone_launch)
    {
        flush_deferred_phone_launch_vibration();
    }
}

bool window_notification_ui_should_exit_detail_on_back(void)
{
    return detail_opened_from_phone_launch;
}

void window_notification_ui_close_detail()
{
    detail_opened_from_phone_launch = false;
    pending_manual_swap_bucket_id = -1;
    window_notification_action_list_hide();
    sync_menu_selection(MenuRowAlignCenter, false);
    if (detail_window != NULL)
    {
        window_stack_remove(detail_window, true);
    }
}

void window_notification_ui_on_bucket_selected()
{
    reload_menu_layer();
}

void window_notification_ui_on_bucket_list_updated()
{
    reload_menu_layer();
}

void window_notification_ui_on_bucket_deleted(const uint8_t bucket_id)
{
    if (bucket_id != window_notification_data.currently_selected_bucket)
    {
        reload_menu_layer();
        return;
    }

    window_notification_action_list_hide();
    window_notification_data.num_actions = 0;
    window_notification_data.num_submenu_actions = 0;
    window_notification_data.open_menu_on_success = 0;
    update_detail_action_button_visibility();

    if (window_notification_data.detail_open)
    {
        begin_detail_dismiss_animation(detail_opened_from_phone_launch);
    }
    else
    {
        reload_menu_layer();
    }
}

static void unload_icons(void)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(notification_icons); i++)
    {
        if (notification_icons[i] != NULL)
        {
            gdraw_command_image_destroy(notification_icons[i]);
            notification_icons[i] = NULL;
        }
    }
}

static void window_load(Window* window)
{
    has_seen_real_notification_items = false;
    Layer* window_layer = window_get_root_layer(window);
    const GRect bounds = layer_get_bounds(window_layer);

    menu_layer = menu_layer_create(bounds);
    menu_layer_set_callbacks(
        menu_layer,
        NULL,
        (MenuLayerCallbacks)
    {
        .get_num_rows = get_num_rows_callback,
        .draw_row = draw_row_callback,
        .get_cell_height = get_cell_height_callback,
        .select_click = select_callback,
        .selection_changed = selection_changed_callback,
    });
    menu_layer_set_normal_colors(menu_layer, GColorWhite, GColorBlack);
    menu_layer_set_highlight_colors(
        menu_layer,
        PBL_IF_COLOR_ELSE(DEFAULT_NOTIFICATION_COLOR, GColorBlack),
        GColorWhite
    );
    layer_add_child(window_layer, menu_layer_get_layer(menu_layer));
    menu_layer_set_click_config_onto_window(menu_layer, window);

    empty_state_layer = layer_create(bounds);
    layer_set_update_proc(empty_state_layer, empty_state_layer_update);
    layer_add_child(window_layer, empty_state_layer);

    phone_launch_pending_layer = layer_create(bounds);
    if (phone_launch_pending_layer != NULL)
    {
        layer_set_update_proc(phone_launch_pending_layer, phone_launch_pending_layer_update);
        layer_add_child(window_layer, phone_launch_pending_layer);
    }
    update_list_layer_visibility();

    window_notification_data.active = true;
    has_seen_real_notification_items = notification_item_count > 0;
    window_notification_action_list_init(window);
    idle_handler_register_timers();
}

static void window_unload(Window* window)
{
    cancel_deferred_new_top_timer();
    deferred_phone_launch_vibration_segments = 0;
    phone_launch_pending_exit_animating = false;
    if (phone_launch_pending_animation != NULL)
    {
        Animation* animation = phone_launch_pending_animation;
        phone_launch_pending_animation = NULL;
        animation_unschedule(animation);
        animation_destroy(animation);
    }

    if (detail_window != NULL)
    {
        window_stack_remove(detail_window, false);
    }

    window_notification_data.active = false;
    window_notification_data_deinit();
    window_notification_action_list_deinit();
    if (menu_layer != NULL)
    {
        menu_layer_destroy(menu_layer);
        menu_layer = NULL;
    }
    if (empty_state_layer != NULL)
    {
        layer_destroy(empty_state_layer);
        empty_state_layer = NULL;
    }
    if (phone_launch_pending_layer != NULL)
    {
        layer_destroy(phone_launch_pending_layer);
        phone_launch_pending_layer = NULL;
    }
    unload_icons();
    clear_detail_body_cache();
    window_destroy(window);
}

static void window_appear(Window* window)
{
    (void)window;
    window_notification_data_init();
}

static void window_disappear(Window* window)
{
    if (detail_window != NULL)
    {
        return;
    }

    window_notification_data_deinit();
}

void window_notification_show()
{
    notification_window = window_create();
    if (notification_window == NULL)
    {
        vibes_double_pulse();
        return;
    }

    window_set_background_color(notification_window, GColorWhite);
    window_set_window_handlers(
        notification_window,
        (WindowHandlers)
    {
        .load = window_load,
        .unload = window_unload,
        .appear = window_appear,
        .disappear = window_disappear,
    });

    window_stack_push(notification_window, !launched_from_phone_notification());
}

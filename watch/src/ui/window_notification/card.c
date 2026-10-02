#include "card.h"

#include <stdlib.h>

#include "notification_image.h"

#define DEFAULT_NOTIFICATION_COLOR GColorFolly
#define ICON_VERTICAL_OFFSET -1
// A few pixels more than stock between the banner and the sender, so the compact layout breathes a little.
#define HEADER_TOP_GAP 7
#define CARD_ICON_UPPER_PADDING (((CARD_BANNER_HEIGHT - CARD_ICON_HEIGHT) / 2) + ICON_VERTICAL_OFFSET)

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
static GDrawCommandImage* icons[ARRAY_LENGTH(icon_resource_ids)];

GColor card_color_for_id(const uint8_t color_id)
{
    switch (color_id)
    {
    case 1: return PBL_IF_COLOR_ELSE(GColorRed, GColorBlack);
    case 2: return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
    case 3: return PBL_IF_COLOR_ELSE(GColorBlueMoon, GColorBlack);
    case 4: return PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    case 5: return PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorBlack);
    case 6: return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    case 7: return PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack);
    case 8: return PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
    case 9: return PBL_IF_COLOR_ELSE(GColorFolly, GColorBlack);
    case 10: return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    case 11: return PBL_IF_COLOR_ELSE(GColorVividViolet, GColorBlack);
    case 12: return PBL_IF_COLOR_ELSE(GColorIndigo, GColorBlack);
    case 13: return PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack);
    case 14: return PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack);
    case 15: return PBL_IF_COLOR_ELSE(GColorYellow, GColorBlack);
    case 16: return PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack);
    default: return PBL_IF_COLOR_ELSE(DEFAULT_NOTIFICATION_COLOR, GColorBlack);
    }
}

static bool normalize_stroke(GDrawCommand* command, uint32_t index, void* context)
{
    (void)index;
    (void)context;
    if (gdraw_command_get_stroke_width(command) > 0)
    {
        gdraw_command_set_stroke_width(command, 2);
    }
    return true;
}

static GDrawCommandImage* load_icon(const uint32_t resource_id)
{
    GDrawCommandImage* icon = gdraw_command_image_create_with_resource(resource_id);
    if (icon == NULL)
    {
        return NULL;
    }

    GDrawCommandImage* writable = gdraw_command_image_clone(icon);
    if (writable == NULL)
    {
        return icon;
    }
    gdraw_command_image_destroy(icon);
    gdraw_command_list_iterate(gdraw_command_image_get_command_list(writable), normalize_stroke, NULL);
    return writable;
}

GDrawCommandImage* card_icon_for_id(uint8_t icon_id)
{
    if (icon_id >= ARRAY_LENGTH(icons))
    {
        icon_id = 0;
    }
    if (icons[icon_id] == NULL)
    {
        icons[icon_id] = load_icon(icon_resource_ids[icon_id]);
    }
    if (icons[icon_id] == NULL && icon_id != 0)
    {
        return card_icon_for_id(0);
    }
    return icons[icon_id];
}

void card_unload_icons(void)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(icons); i++)
    {
        if (icons[i] != NULL)
        {
            gdraw_command_image_destroy(icons[i]);
            icons[i] = NULL;
        }
    }
}

static bool is_empty(const char* text)
{
    return text == NULL || text[0] == '\0';
}

const char* card_header_text(const NotificationItem* item)
{
    if (item == NULL)
    {
        return "Notification";
    }
    if (!item->loaded)
    {
        return "Loading";
    }
    if (!is_empty(item->title))
    {
        return item->title;
    }
    if (!is_empty(item->app_name))
    {
        return item->app_name;
    }
    return "Notification";
}

const char* card_body_text(const NotificationItem* item, const char* body)
{
    if (body == NULL)
    {
        return "";
    }

    const char* sender = card_header_text(item);
    const size_t sender_length = strlen(sender);
    if (sender_length > 0 && strncmp(body, sender, sender_length) == 0)
    {
        if (body[sender_length] == ':' && body[sender_length + 1] == ' ')
        {
            return body + sender_length + 2;
        }
        if (body[sender_length] == '\n')
        {
            return body + sender_length + 1;
        }
    }
    return body;
}

static uint8_t text_size = CARD_TEXT_SIZE_DEFAULT;

void card_set_text_size(const uint8_t size)
{
    text_size = size <= CARD_TEXT_SIZE_LARGE ? size : CARD_TEXT_SIZE_DEFAULT;
}

// Sender in bold, message text in the regular weight, at every size.
static GFont header_font(void)
{
    switch (text_size)
    {
    case CARD_TEXT_SIZE_SMALL:
        return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
    case CARD_TEXT_SIZE_LARGE:
        return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    default:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD);
    }
}

static GFont body_font(void)
{
    switch (text_size)
    {
    case CARD_TEXT_SIZE_SMALL:
        return fonts_get_system_font(FONT_KEY_GOTHIC_18);
    case CARD_TEXT_SIZE_LARGE:
        return fonts_get_system_font(FONT_KEY_GOTHIC_28);
    default:
        return fonts_get_system_font(FONT_KEY_GOTHIC_24);
    }
}

static GFont footer_font(void)
{
    return fonts_get_system_font(text_size == CARD_TEXT_SIZE_SMALL ? FONT_KEY_GOTHIC_14 : FONT_KEY_GOTHIC_18);
}

static int16_t text_height(const char* text, const GFont font, const int16_t width, const GTextOverflowMode mode)
{
    if (is_empty(text))
    {
        return 0;
    }
    return graphics_text_layout_get_content_size(text, font, GRect(0, 0, width, 4000), mode,
                                                 GTextAlignmentLeft).h;
}

void card_format_since(char* buffer, const size_t size, const time_t timestamp)
{
    if (size == 0)
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
        snprintf(buffer, size, "Just now");
    }
    else if (seconds < 3600)
    {
        const int32_t minutes = seconds / 60;
        snprintf(buffer, size, "%ld minute%s ago", (long)minutes, minutes == 1 ? "" : "s");
    }
    else if (seconds < 86400)
    {
        const int32_t hours = seconds / 3600;
        snprintf(buffer, size, "%ld hour%s ago", (long)hours, hours == 1 ? "" : "s");
    }
    else
    {
        const int32_t days = seconds / 86400;
        snprintf(buffer, size, "%ld day%s ago", (long)days, days == 1 ? "" : "s");
    }
}

void card_measure(const NotificationItem* item, const char* body, const int16_t width, const bool open_ended,
                  CardMetrics* metrics)
{
    metrics->open_ended = open_ended;
    const int16_t text_width = width - (CARD_MARGIN * 2);
    char footer[32];
    card_format_since(footer, sizeof(footer), item != NULL ? item->receive_time : 0);

    metrics->header_height = text_height(card_header_text(item), header_font(), text_width,
                                         GTextOverflowModeTrailingEllipsis);
    metrics->body_height = text_height(card_body_text(item, body), body_font(), text_width,
                                       GTextOverflowModeWordWrap);
    // An open-ended card is still waiting for the rest of its text: instead of the "x minutes ago" footer
    // (which would mark the end of the message) it ends with a "..." line where the text will continue.
    metrics->footer_height = open_ended ?
        text_height("...", body_font(), text_width, GTextOverflowModeTrailingEllipsis) :
        text_height(footer, footer_font(), text_width, GTextOverflowModeTrailingEllipsis);

    int16_t height = STATUS_BAR_LAYER_HEIGHT + CARD_BANNER_HEIGHT + HEADER_TOP_GAP;
    height += metrics->header_height + 3;
    height += metrics->body_height + 3;
    // PebbleOS: the photo goes below the message text, above the time.
    metrics->image_height = notification_image_band_height(item, text_width);
    height += metrics->image_height;
    height += metrics->footer_height;
    height += CARD_BOTTOM_PADDING + CARD_ARROW_HEIGHT;
    metrics->total_height = height > PBL_DISPLAY_HEIGHT ? height : PBL_DISPLAY_HEIGHT;
}

static void draw_icon(GContext* ctx, const uint8_t icon_id, const GRect frame)
{
    GDrawCommandImage* icon = card_icon_for_id(icon_id);
    if (icon != NULL)
    {
        GRect icon_rect = {.size = gdraw_command_image_get_bounds_size(icon)};
        grect_align(&icon_rect, &frame, GAlignCenter, false);
        gdraw_command_image_draw(ctx, icon, icon_rect.origin);
    }
}

void card_draw(GContext* ctx, const NotificationItem* item, const char* body, const CardMetrics* metrics,
               const int16_t width, const int16_t origin_y, const int16_t counter_index,
               const int16_t counter_total)
{
    const int16_t top_height = STATUS_BAR_LAYER_HEIGHT + CARD_BANNER_HEIGHT;
    const int16_t screen_height = PBL_DISPLAY_HEIGHT;
    if (origin_y >= screen_height || origin_y + metrics->total_height <= 0)
    {
        return;
    }

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, origin_y, width, metrics->total_height), 0, GCornerNone);

    graphics_context_set_fill_color(ctx, card_color_for_id(item != NULL ? item->color_id : 0));
    graphics_fill_rect(ctx, GRect(0, origin_y, width, top_height), 0, GCornerNone);

    // Clock and counter belong to the status bar of the card at the top; a peeking card only shows its icon.
    if (origin_y <= 0 && origin_y + top_height > 0)
    {
        char time_text[16];
        clock_copy_time_string(time_text, sizeof(time_text));
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(ctx, time_text, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                           GRect(0, origin_y, width, STATUS_BAR_LAYER_HEIGHT),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);

        if (counter_total > 1 && counter_index >= 0)
        {
            char counter_text[12];
            snprintf(counter_text, sizeof(counter_text), "%d/%d", counter_index + 1, counter_total);
            graphics_draw_text(ctx, counter_text, fonts_get_system_font(FONT_KEY_GOTHIC_14),
                               GRect(width - 54, origin_y, 50, STATUS_BAR_LAYER_HEIGHT),
                               GTextOverflowModeTrailingEllipsis, GTextAlignmentRight, NULL);
        }
    }

    if (origin_y + top_height > 0)
    {
        draw_icon(ctx, item != NULL ? item->icon_id : 0,
                  GRect((width - CARD_ICON_WIDTH) / 2, origin_y + STATUS_BAR_LAYER_HEIGHT + CARD_ICON_UPPER_PADDING,
                        CARD_ICON_WIDTH, CARD_ICON_HEIGHT));
    }

    const int16_t text_width = width - (CARD_MARGIN * 2);
    int16_t y = origin_y + top_height + HEADER_TOP_GAP;
    graphics_context_set_text_color(ctx, GColorBlack);

    if (y < screen_height && y + metrics->header_height > 0)
    {
        graphics_draw_text(ctx, card_header_text(item), header_font(),
                           GRect(CARD_MARGIN, y, text_width, metrics->header_height + 4),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
    y += metrics->header_height + 3;

    if (y < screen_height && y + metrics->body_height > 0)
    {
        graphics_draw_text(ctx, card_body_text(item, body), body_font(),
                           GRect(CARD_MARGIN, y, text_width, metrics->body_height),
                           GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    }
    y += metrics->body_height + 3;

    if (metrics->open_ended)
    {
        if (y < screen_height && y + metrics->footer_height > 0)
        {
            graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
            graphics_draw_text(ctx, "...", body_font(), GRect(CARD_MARGIN, y, text_width, metrics->footer_height + 4),
                               GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
        }
        y += metrics->footer_height;
        if (metrics->image_height > 0 && y < screen_height && y + metrics->image_height > 0)
        {
            notification_image_draw(ctx, item, CARD_MARGIN, y, text_width);
        }
        return;
    }

    if (metrics->image_height > 0)
    {
        if (y < screen_height && y + metrics->image_height > 0)
        {
            notification_image_draw(ctx, item, CARD_MARGIN, y, text_width);
        }
        y += metrics->image_height;
    }

    if (y < screen_height && y + metrics->footer_height > 0)
    {
        char footer[32];
        card_format_since(footer, sizeof(footer), item != NULL ? item->receive_time : 0);
        graphics_draw_text(ctx, footer, footer_font(),
                           GRect(CARD_MARGIN, y, text_width, metrics->footer_height + 4),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    }
}

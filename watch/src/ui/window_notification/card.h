#pragma once
#include <pebble.h>
#include "notification_store.h"

// Geometry shared with PebbleOS' notification layout (src/fw/services/timeline/notification_layout.c).
#define CARD_BANNER_HEIGHT 36
#define CARD_ARROW_HEIGHT 19
#define CARD_MARGIN 10
#define CARD_BOTTOM_PADDING 18
#define CARD_ICON_WIDTH 30
#define CARD_ICON_HEIGHT 25

typedef struct
{
    int16_t header_height;
    int16_t body_height;
    int16_t footer_height;
    /** Band reserved for an attached photo, padding included (0 for none). */
    int16_t image_height;
    int16_t total_height;
    bool open_ended;
} CardMetrics;

GColor card_color_for_id(uint8_t color_id);
GDrawCommandImage* card_icon_for_id(uint8_t icon_id);
void card_unload_icons(void);

/** Header line shown in bold above the body (sender / conversation / app). */
const char* card_header_text(const NotificationItem* item);

/** Body text, skipping a leading "Sender: " prefix that only repeats the header. */
const char* card_body_text(const NotificationItem* item, const char* body);

/**
 * Measure the card. total_height is never less than the screen height.
 * open_ended: the text shown is only the beginning of the message (the rest is still loading).
 */
void card_measure(const NotificationItem* item, const char* body, int16_t width, bool open_ended,
                  CardMetrics* metrics);

/** Draw the card with its top edge at origin_y. counter_total <= 1 hides the "2/5" counter. */
void card_draw(GContext* ctx, const NotificationItem* item, const char* body, const CardMetrics* metrics,
               int16_t width, int16_t origin_y, int16_t counter_index, int16_t counter_total);

#define CARD_TEXT_SIZE_SMALL 0
#define CARD_TEXT_SIZE_DEFAULT 1
#define CARD_TEXT_SIZE_LARGE 2

/** Notification text size, as chosen in the phone app's settings. */
void card_set_text_size(uint8_t size);

void card_format_since(char* buffer, size_t size, time_t timestamp);

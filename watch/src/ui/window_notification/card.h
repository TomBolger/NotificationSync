#pragma once
#include <pebble.h>
#include "notification_store.h"

// Geometry shared with PebbleOS' notification layout (src/fw/services/timeline/notification_layout.c).
// Round (Gabbro): PebbleOS's LAYOUT_TOP_BANNER_HEIGHT_ROUND (60) less the status bar, its 16 px arrow, and a text
// column narrow enough to stay inside the circle over most of the screen.
#define CARD_BANNER_HEIGHT PBL_IF_ROUND_ELSE(44, 36)
#define CARD_ARROW_HEIGHT PBL_IF_ROUND_ELSE(16, 19)
#define CARD_MARGIN PBL_IF_ROUND_ELSE(30, 10)
#define CARD_BOTTOM_PADDING 18
#define CARD_ICON_WIDTH 30
#define CARD_ICON_HEIGHT 25
#define CARD_ICON_UPPER_PADDING (((CARD_BANNER_HEIGHT - CARD_ICON_HEIGHT) / 2) - 1)

#define CARD_ROUND_BANNER_RADIUS 140

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

/**
 * Draw the card with its top edge at origin_y. counter_total <= 1 hides the "2/5" counter.
 * Round: like PebbleOS, the banner follows the card's position, from a half dot peeking at the bottom edge to the
 * full banner at the top.
 */
void card_draw(GContext* ctx, const NotificationItem* item, const char* body, const CardMetrics* metrics,
               int16_t width, int16_t origin_y, int16_t counter_index, int16_t counter_total);

/** Draws an icon scaled around its centre (1000 = normal size). */
void card_draw_icon_scaled(GContext* ctx, uint8_t icon_id, GPoint center, int16_t scale_permille);

#if PBL_ROUND
/** Fills the rows from_y..to_y-1 of a circle (clipped to the screen). */
void card_fill_circle_rows(GContext* ctx, GPoint center, int16_t radius, int16_t from_y, int16_t to_y);
#endif

#define CARD_TEXT_SIZE_SMALL 0
#define CARD_TEXT_SIZE_DEFAULT 1
#define CARD_TEXT_SIZE_LARGE 2

/** Notification text size and weights, as chosen in the phone app's settings. */
void card_set_text_style(uint8_t size, bool bold_sender, bool bold_message);

void card_format_since(char* buffer, size_t size, time_t timestamp);

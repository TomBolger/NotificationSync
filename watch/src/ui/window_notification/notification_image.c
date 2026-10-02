#include "notification_image.h"

#include "card.h"
#include "commons/bytes.h"
#include "connection/packets.h"

// Transfer framing (packet 16, phone -> watch), big endian like the rest of the protocol:
//   u8 bucket, u8 flags, u16 offset of this chunk's pixels in the pixel stream,
//   first chunk only: u16 width, u16 height, u8 palette_count, u8 palette[palette_count] (GColor8),
//   then pixel bytes: 4 bits per pixel, rows of (width + 1) / 2 bytes, even x in the high nibble.
// Mirrors the PebbleOS imaging endpoint's ImagingResponseHeader.
#define FLAG_FIRST 0x01
#define FLAG_LAST 0x02
#define FLAG_NO_IMAGE 0x04

#define REQUEST_RETRY_MS 250
#define REQUEST_MAX_TRIES 20
// A transfer that stops arriving (phone gone, app killed) gives up so the placeholder does not stay forever,
// like PebbleOS clearing the placeholder on a failed transfer.
#define TRANSFER_TIMEOUT_MS 20000

typedef enum
{
    SlotEmpty,
    SlotPending,
    SlotReady,
    SlotNone,
} SlotState;

static uint8_t slot_bucket;
static uint8_t slot_aspect;
static uint8_t slot_tag;
static SlotState slot_state = SlotEmpty;
static GSize slot_size;
static GBitmap* bitmap;
static uint8_t request_tries;
static AppTimer* retry_timer;
static AppTimer* timeout_timer;
static void (*listener)(uint8_t bucket_id);

static uint8_t clamp_aspect(const uint8_t aspect)
{
    if (aspect == 0)
    {
        return 0;
    }
    if (aspect < NOTIFICATION_IMAGE_MIN_ASPECT)
    {
        return NOTIFICATION_IMAGE_MIN_ASPECT;
    }
    return aspect > NOTIFICATION_IMAGE_MAX_ASPECT ? NOTIFICATION_IMAGE_MAX_ASPECT : aspect;
}

GSize notification_image_size(const NotificationItem* item, const int16_t width)
{
#if NOTIFICATION_IMAGE_SUPPORTED
    const uint8_t aspect = item != NULL ? clamp_aspect(item->image_aspect) : 0;
    if (aspect == 0)
    {
        return GSizeZero;
    }
    return GSize(width, (width * aspect) / 16);
#else
    (void)item;
    (void)width;
    return GSizeZero;
#endif
}

int16_t notification_image_band_height(const NotificationItem* item, const int16_t width)
{
    const GSize size = notification_image_size(item, width);
    return size.h > 0 ? size.h + 2 * NOTIFICATION_IMAGE_PADDING : 0;
}

static bool slot_is(const NotificationItem* item)
{
    return item != NULL && slot_state != SlotEmpty && slot_bucket == item->bucket_id &&
        slot_aspect == item->image_aspect && slot_tag == item->image_tag;
}

void notification_image_draw(GContext* ctx, const NotificationItem* item, const int16_t x, const int16_t band_top,
                             const int16_t width)
{
    const GSize image = notification_image_size(item, width);
    if (image.h <= 0)
    {
        return;
    }
    const GPoint origin = GPoint(x + (width - image.w) / 2, band_top + NOTIFICATION_IMAGE_PADDING);
    const bool ours = slot_is(item);

    if (ours && slot_state == SlotReady && bitmap != NULL)
    {
        const GSize bitmap_size = gbitmap_get_bounds(bitmap).size;
        graphics_context_set_compositing_mode(ctx, GCompOpAssign);
        graphics_draw_bitmap_in_rect(ctx, bitmap,
                                     GRect(origin.x + (image.w - bitmap_size.w) / 2,
                                           origin.y + (image.h - bitmap_size.h) / 2,
                                           bitmap_size.w, bitmap_size.h));
    }
    else if (!ours || slot_state == SlotPending)
    {
        // Still on its way (or not asked for yet, e.g. a card sliding in): fill the reserved area so the card
        // doesn't look broken. Once the phone says it has no image the space is simply blank.
        graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
        graphics_fill_rect(ctx, GRect(origin.x, origin.y, image.w, image.h), NOTIFICATION_IMAGE_CORNER_RADIUS,
                           GCornersAll);
    }
}

static void cancel_timer(AppTimer** timer)
{
    if (*timer != NULL)
    {
        app_timer_cancel(*timer);
        *timer = NULL;
    }
}

static void free_bitmap(void)
{
    if (bitmap != NULL)
    {
        // Created with free_on_destroy, so the palette goes with it.
        gbitmap_destroy(bitmap);
        bitmap = NULL;
    }
}

void notification_image_clear(void)
{
    cancel_timer(&retry_timer);
    cancel_timer(&timeout_timer);
    free_bitmap();
    slot_state = SlotEmpty;
    slot_bucket = 0;
}

void notification_image_set_listener(void (*on_changed)(uint8_t bucket_id))
{
    listener = on_changed;
}

static void resolve(const SlotState state)
{
    cancel_timer(&retry_timer);
    cancel_timer(&timeout_timer);
    slot_state = state;
    if (state != SlotReady)
    {
        free_bitmap();
    }
    if (listener != NULL)
    {
        listener(slot_bucket);
    }
}

static void on_transfer_timeout(void* context)
{
    (void)context;
    timeout_timer = NULL;
    if (slot_state == SlotPending)
    {
        resolve(SlotNone);
    }
}

static void restart_timeout(void)
{
    cancel_timer(&timeout_timer);
    timeout_timer = app_timer_register(TRANSFER_TIMEOUT_MS, on_transfer_timeout, NULL);
}

static void try_send_request(void* context)
{
    (void)context;
    retry_timer = NULL;
    if (slot_state != SlotPending)
    {
        return;
    }
    if (send_image_request(slot_bucket, slot_size.w, slot_size.h))
    {
        restart_timeout();
        return;
    }
    // Outbox busy (text or a sync going out): try again shortly.
    if (++request_tries < REQUEST_MAX_TRIES)
    {
        retry_timer = app_timer_register(REQUEST_RETRY_MS, try_send_request, NULL);
    }
    else
    {
        resolve(SlotNone);
    }
}

void notification_image_request(const uint8_t bucket_id)
{
#if NOTIFICATION_IMAGE_SUPPORTED
    const int16_t index = notification_store_index_of(bucket_id);
    const NotificationItem* item = index >= 0 ? notification_store_item(index) : NULL;
    if (item == NULL || item->image_aspect == 0 || slot_is(item))
    {
        return;
    }

    notification_image_clear();
    slot_bucket = bucket_id;
    slot_aspect = item->image_aspect;
    slot_tag = item->image_tag;
    slot_size = notification_image_size(item, PBL_DISPLAY_WIDTH - 2 * CARD_MARGIN);
    slot_state = SlotPending;
    request_tries = 0;
    try_send_request(NULL);
#else
    (void)bucket_id;
#endif
}

void notification_image_on_list_changed(void)
{
    if (slot_state == SlotEmpty)
    {
        return;
    }
    const int16_t index = notification_store_index_of(slot_bucket);
    const NotificationItem* item = index >= 0 ? notification_store_item(index) : NULL;
    if (!slot_is(item))
    {
        notification_image_clear();
    }
}

void notification_image_receive(const uint8_t* data, const size_t length)
{
    if (length < 4 || slot_state != SlotPending || data[0] != slot_bucket)
    {
        return;
    }
    const uint8_t flags = data[1];
    if ((flags & FLAG_NO_IMAGE) != 0)
    {
        resolve(SlotNone);
        return;
    }

    const uint16_t offset = read_uint16_from_byte_array(data, 2);
    size_t position = 4;

    if ((flags & FLAG_FIRST) != 0)
    {
        if (length < position + 5)
        {
            resolve(SlotNone);
            return;
        }
        const uint16_t width = read_uint16_from_byte_array(data, position);
        const uint16_t height = read_uint16_from_byte_array(data, position + 2);
        const uint8_t palette_count = data[position + 4];
        position += 5;
        if (width == 0 || height == 0 || width > slot_size.w || height > slot_size.h || palette_count == 0 ||
            palette_count > 16 || length < position + palette_count)
        {
            resolve(SlotNone);
            return;
        }

        free_bitmap();
        GColor* palette = malloc(sizeof(GColor) * 16);
        if (palette == NULL)
        {
            resolve(SlotNone);
            return;
        }
        memset(palette, 0, sizeof(GColor) * 16);
        for (uint8_t i = 0; i < palette_count; i++)
        {
            palette[i].argb = data[position + i];
        }
        position += palette_count;
        bitmap = gbitmap_create_blank_with_palette(GSize(width, height), GBitmapFormat4BitPalette, palette, true);
        if (bitmap == NULL)
        {
            free(palette);
            resolve(SlotNone);
            return;
        }
    }

    if (bitmap == NULL)
    {
        return;
    }

    // Copy this chunk into the bitmap, row by row (the bitmap's rows may be padded).
    const GSize size = gbitmap_get_bounds(bitmap).size;
    const uint16_t stream_row = (size.w + 1) / 2;
    const uint16_t bitmap_row = gbitmap_get_bytes_per_row(bitmap);
    uint8_t* pixels = gbitmap_get_data(bitmap);
    uint32_t stream_offset = offset;
    const uint32_t stream_total = (uint32_t)stream_row * size.h;
    while (position < length && stream_offset < stream_total)
    {
        const uint16_t row = stream_offset / stream_row;
        const uint16_t column = stream_offset % stream_row;
        size_t run = stream_row - column;
        if (run > length - position)
        {
            run = length - position;
        }
        memcpy(&pixels[(uint32_t)row * bitmap_row + column], &data[position], run);
        position += run;
        stream_offset += run;
    }

    if ((flags & FLAG_LAST) != 0)
    {
        resolve(SlotReady);
    }
    else
    {
        restart_timeout();
    }
}

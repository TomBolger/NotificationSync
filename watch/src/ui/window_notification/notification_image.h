#pragma once
#include <pebble.h>

#include "notification_store.h"

/**
 * Photos sent with notifications, as in PebbleOS (services/notifications/notification_image.c and the image band
 * in services/timeline/notification_layout.c, coredevices/PebbleOS a0f41c0e).
 *
 * The notification's bucket carries the image's aspect (height/width in sixteenths). The card reserves a band of
 * exactly that shape, so its height is final before any pixels arrive, and the pixels (4-bit palettized, sized to
 * the band) are pulled from the phone when the card comes on screen. One slot: only the card on screen has its
 * image loaded.
 *
 * Like PebbleOS, only on Emery and Gabbro: it needs a colour display and the RAM for the decoded bitmap.
 */
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define NOTIFICATION_IMAGE_SUPPORTED 1
#else
#define NOTIFICATION_IMAGE_SUPPORTED 0
#endif

/** Bounds on the aspect a band can reserve; the phone clamps to the same range. */
#define NOTIFICATION_IMAGE_MIN_ASPECT 4  // 4:1 landscape
#define NOTIFICATION_IMAGE_MAX_ASPECT 24 // 2:3 portrait

/** Space above and below the image band. */
#define NOTIFICATION_IMAGE_PADDING 8
#define NOTIFICATION_IMAGE_CORNER_RADIUS 4

/** Size of the image itself for a content box `width` wide, or GSizeZero if the item has no image. */
GSize notification_image_size(const NotificationItem* item, int16_t width);

/** Height the card reserves for the image, padding included (0 for none). */
int16_t notification_image_band_height(const NotificationItem* item, int16_t width);

/** Draw the image (or its placeholder while it is on the way) in the band starting at `band_top`. */
void notification_image_draw(GContext* ctx, const NotificationItem* item, int16_t x, int16_t band_top,
                             int16_t width);

/** The card for this bucket is on screen: fetch its image if it has one and it is not already here. */
void notification_image_request(uint8_t bucket_id);

/** The notification list changed; drop the image if its notification was rewritten with a different image. */
void notification_image_on_list_changed(void);

/** Called whenever the slot resolves (image arrived, or the phone has none), so the card can redraw. */
void notification_image_set_listener(void (*on_changed)(uint8_t bucket_id));

/** Phone -> watch image chunk (packet 16). */
void notification_image_receive(const uint8_t* data, size_t length);

void notification_image_clear(void);

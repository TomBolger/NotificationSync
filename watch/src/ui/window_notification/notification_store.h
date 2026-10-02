#pragma once
#include <pebble.h>
#include "commons/connection/bucket_sync.h"

#define MAX_BODY_TEXT_SIZE PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 1200, 1800, 3470, 1800, 3470, 3470, 3470)
#define MAX_NOTIFICATION_ACTIONS 20
#define MAX_NOTIFICATION_ACTION_TEXT 21

#define STORE_MAX_ITEMS MAX_BUCKETS
#define STORE_APP_NAME_SIZE 32
#define STORE_TITLE_SIZE 48
#define STORE_SUMMARY_SIZE 256

typedef struct
{
    uint8_t id;
    char text[MAX_NOTIFICATION_ACTION_TEXT];
    bool voice;
} Action;

typedef struct
{
    uint8_t bucket_id;
    bool loaded;
    bool unread;
    bool paused;
    bool periodic_vibration;
    /** The summary already holds the entire message text (details only add actions). */
    bool summary_complete;
    time_t receive_time;
    uint8_t icon_id;
    uint8_t color_id;
    /** Height/width of the attached photo in sixteenths, 0 for none (see notification_image.h). */
    uint8_t image_aspect;
    /** Changes when the photo does, so a rewritten notification with a new photo fetches it again. */
    uint8_t image_tag;
    char app_name[STORE_APP_NAME_SIZE];
    char title[STORE_TITLE_SIZE];
    char summary[STORE_SUMMARY_SIZE];
} NotificationItem;

typedef struct
{
    uint8_t bucket_id;
    char* body;
    uint8_t num_actions;
    Action actions[MAX_NOTIFICATION_ACTIONS];
    uint32_t last_used;
    bool received_during_sync;
} NotificationDetails;

typedef struct
{
    /**
     * Called after the set of notifications or their contents changed.
     * changed_buckets lists notifications whose content was (re)written since the previous call.
     */
    void (*list_changed)(const uint8_t* changed_buckets, uint8_t changed_count);
    /** Called when full details (body/actions) for a notification arrived. */
    void (*details_changed)(uint8_t bucket_id);
    /** Called when the settings bucket (id 1) changed. */
    void (*settings_changed)(void);
} StoreListener;

void notification_store_init(void);
void notification_store_set_listener(const StoreListener* listener);
/** The phone answered this session and its first sync was handed over: start showing notifications. */
void notification_store_on_phone_synced(void);
/** The phone never sent the rest of this message: show what we have as the whole message. */
void notification_store_on_details_unavailable(uint8_t bucket_id);
/** True once notifications come from this session's sync (never from what was saved last time). */
bool notification_store_is_live(void);
void notification_store_set_settings_listener(void (*listener)(void));

uint8_t notification_store_count(void);
const NotificationItem* notification_store_item(uint8_t index);
const NotificationItem* notification_store_item_by_bucket(uint8_t bucket_id);
int16_t notification_store_index_of(uint8_t bucket_id);

/** Full details if they arrived, NULL otherwise. Pointer is valid until the next event loop turn. */
const NotificationDetails* notification_store_details(uint8_t bucket_id);
/** Best text available for the notification body (full details, otherwise the synced summary). */
const char* notification_store_body(uint8_t bucket_id);
/** Only the beginning of this notification's text is on the watch so far. */
bool notification_store_is_partial(uint8_t bucket_id);
/** Make sure details for this notification are (or will be) available. */
void notification_store_want_details(uint8_t bucket_id, bool urgent);
/** Keep this notification's details in the cache even under memory pressure. */
void notification_store_pin(uint8_t bucket_id);

void notification_store_mark_seen(uint8_t bucket_id);
/** Hide a notification immediately (the user dismissed it on the watch; the phone will delete it shortly). */
void notification_store_hide(uint8_t bucket_id);
bool notification_store_any_wants_periodic_vibration(void);

/** Called by the details fetcher when a complete details payload has arrived. */
void notification_store_on_details_received(uint8_t bucket_id, const char* body, size_t body_size,
                                            const Action* actions, uint8_t num_actions);

#pragma once
#include <pebble.h>
#include "commons/connection/bucket_sync.h"

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define MAX_BODY_TEXT_SIZE 8192
#else
#define MAX_BODY_TEXT_SIZE PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 1200, 1800, 3470, 1800, 3470, 3470, 3470)
#endif
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
    /** Where the phone shows it: one of the GROUP_* values below. */
    uint8_t group;
    /** Same non-zero value for notifications the phone stacks together (same app group), 0 for none. */
    uint8_t stack_id;
    char app_name[STORE_APP_NAME_SIZE];
    char title[STORE_TITLE_SIZE];
    char summary[STORE_SUMMARY_SIZE];
} NotificationItem;

// Sections and categories of the phone's notification shade, in the order they are listed.
#define GROUP_CONVERSATIONS 0
#define GROUP_NOTIFICATIONS 1
#define GROUP_SILENT 2
#define GROUP_SUGGESTED 3
#define GROUP_PROMOTIONS 4
#define GROUP_NEWS 5
#define GROUP_SOCIAL 6
#define GROUP_COUNT 7

typedef enum
{
    /** A notification (item = its display index). */
    ROW_ITEM,
    /** Small header of a section or category (group), with a caret. */
    ROW_HEADER,
    /** An app's stack: collapsed it is one full row for its newest notification (item); expanded, a small header. */
    ROW_STACK,
} ListRowKind;

typedef struct
{
    uint8_t kind;
    uint8_t group;
    uint8_t item;
    uint8_t count;
    bool expanded;
    /** Item rows inside an expanded stack are drawn slightly indented. */
    bool in_stack;
} ListRow;

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
/** A details attempt failed; keep the preview partial and allow another attempt. */
void notification_store_on_details_unavailable(uint8_t bucket_id);
/** True once notifications come from this session's sync (never from what was saved last time). */
bool notification_store_is_live(void);
void notification_store_set_settings_listener(void (*listener)(void));

/** Group into the phone's sections (with headers) and/or stack an app's notifications. */
void notification_store_set_grouping(bool sections, bool stacks);
/** Rows of the list: headers, stacks and notifications, as currently opened and closed. */
uint8_t notification_store_row_count(void);
const ListRow* notification_store_row(uint8_t row);
/** The row showing this notification (or the closed header or stack that holds it), -1 if none. */
int16_t notification_store_row_of_bucket(uint8_t bucket_id);
/** Open or close the header or stack at this row (remembered for sections and categories). */
void notification_store_toggle_row(uint8_t row);
const char* notification_store_group_name(uint8_t group);

/** All notifications in list order (closed headers and stacks included), for paging through cards. */
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

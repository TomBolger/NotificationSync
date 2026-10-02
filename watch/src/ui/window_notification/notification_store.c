#include "notification_store.h"

#include <stdlib.h>

#include "commons/bytes.h"
#include "connection/notification_details_fetcher.h"

#define DETAILS_CACHE_SLOTS PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 2, 3, 5, 3, 5, 5, 5)
#define SETTINGS_BUCKET_ID 1

static const uint32_t STORAGE_SEEN_FLAG_MIN = 3000;

static NotificationItem items[STORE_MAX_ITEMS];
static uint8_t item_count = 0;

static NotificationDetails details_cache[DETAILS_CACHE_SLOTS];
static uint32_t details_use_counter = 0;
static uint8_t pinned_bucket = 0;

static uint8_t changed_since_last_notify[STORE_MAX_ITEMS + 1];
static uint8_t changed_count = 0;

static const StoreListener* listener = NULL;

// Notifications the user just dismissed on the watch. They are hidden right away instead of waiting for the
// phone round trip; if the phone does not actually remove them they come back after HIDE_SECONDS.
#define HIDDEN_SLOTS 4
#define HIDE_SECONDS 15
static uint8_t hidden_buckets[HIDDEN_SLOTS];
static time_t hidden_until[HIDDEN_SLOTS];

static bool is_hidden(const uint8_t bucket_id)
{
    const time_t now = time(NULL);
    for (uint8_t i = 0; i < HIDDEN_SLOTS; i++)
    {
        if (hidden_buckets[i] == bucket_id)
        {
            if (now < hidden_until[i])
            {
                return true;
            }
            hidden_buckets[i] = 0;
        }
    }
    return false;
}
static void (*settings_listener)(void) = NULL;

#define SUMMARY_COMPLETE_FLAG 0x08

static NotificationDetails* find_details(uint8_t bucket_id);

bool notification_store_is_partial(const uint8_t bucket_id)
{
    const NotificationItem* item = notification_store_item_by_bucket(bucket_id);
    return item != NULL && item->loaded && !item->summary_complete && find_details(bucket_id) == NULL;
}

static bool is_seen_locally(const uint8_t bucket_id)
{
    uint8_t flag = 0;
    persist_read_data(STORAGE_SEEN_FLAG_MIN + bucket_id, &flag, 1);
    return flag == 1;
}

static size_t bounded_strlen(const uint8_t* data, const size_t start, const size_t size)
{
    size_t length = 0;
    while (start + length < size && data[start + length] != '\0')
    {
        length++;
    }
    return length;
}

static void copy_text(char* target, const size_t target_size, const uint8_t* source, const size_t length)
{
    const size_t to_copy = length < target_size - 1 ? length : target_size - 1;
    memcpy(target, source, to_copy);
    target[to_copy] = '\0';
}

static void parse_item(const BucketMetadata metadata, NotificationItem* item)
{
    memset(item, 0, sizeof(NotificationItem));
    item->bucket_id = metadata.id;
    item->paused = (metadata.flags & 0x02) != 0;
    item->periodic_vibration = (metadata.flags & 0x04) != 0;
    item->unread = (metadata.flags & 0x01) != 0 && !is_seen_locally(metadata.id);

    uint8_t data[256];
    const uint8_t size = bucket_sync_get_bucket_size(metadata.id);
    if (size < 8 || !bucket_sync_load_bucket_limited(metadata.id, data, sizeof(data) - 1))
    {
        return;
    }

    item->receive_time = read_uint32_from_byte_array(data, 0);
    item->icon_id = data[4];
    item->color_id = data[5];
    item->image_aspect = data[6];
    item->image_tag = data[7];

    size_t position = 8;
    const size_t app_name_length = bounded_strlen(data, position, size);
    if (position + app_name_length >= size)
    {
        return;
    }
    copy_text(item->app_name, sizeof(item->app_name), &data[position], app_name_length);
    position += app_name_length + 1;

    const size_t title_length = bounded_strlen(data, position, size);
    if (position + title_length > size)
    {
        return;
    }
    copy_text(item->title, sizeof(item->title), &data[position], title_length);
    position += title_length + 1;

    if (position < size)
    {
        copy_text(item->summary, sizeof(item->summary), &data[position], size - position);
    }

    // Phone sets 0x08 when the summary is the whole message text. Otherwise the summary is the beginning of
    // the message, cut short (and ellipsized). Trim it back to the last complete word so that every line
    // shown now stays exactly where it is once the rest of the text arrives.
    item->summary_complete = (metadata.flags & SUMMARY_COMPLETE_FLAG) != 0;
    if (!item->summary_complete)
    {
        size_t length = strlen(item->summary);
        if (length >= 3 && strcmp(&item->summary[length - 3], "...") == 0)
        {
            length -= 3;
        }
        while (length > 0 && item->summary[length - 1] != ' ' && item->summary[length - 1] != '\n')
        {
            length--;
        }
        while (length > 0 && (item->summary[length - 1] == ' ' || item->summary[length - 1] == '\n'))
        {
            length--;
        }
        item->summary[length] = '\0';
    }

    item->loaded = true;
}

static void rebuild_items(void)
{
    const BucketList* buckets = bucket_sync_get_bucket_list();
    item_count = 0;
    for (int i = 0; i < buckets->count && item_count < STORE_MAX_ITEMS; i++)
    {
        if (buckets->data[i].id == SETTINGS_BUCKET_ID || is_hidden(buckets->data[i].id))
        {
            continue;
        }

        parse_item(buckets->data[i], &items[item_count++]);
    }
}

// ------------------------------------------------------------------------------------------------ details cache

static NotificationDetails* find_details(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < DETAILS_CACHE_SLOTS; i++)
    {
        if (details_cache[i].bucket_id == bucket_id && details_cache[i].body != NULL)
        {
            return &details_cache[i];
        }
    }
    return NULL;
}

static void clear_details(NotificationDetails* details)
{
    free(details->body);
    memset(details, 0, sizeof(NotificationDetails));
}

static void drop_details(const uint8_t bucket_id, const bool keep_fresh)
{
    for (uint8_t i = 0; i < DETAILS_CACHE_SLOTS; i++)
    {
        if (details_cache[i].bucket_id == bucket_id &&
            !(keep_fresh && details_cache[i].received_during_sync))
        {
            clear_details(&details_cache[i]);
        }
    }
}

static NotificationDetails* slot_for_details(const uint8_t bucket_id)
{
    NotificationDetails* existing = find_details(bucket_id);
    if (existing != NULL)
    {
        return existing;
    }

    NotificationDetails* oldest = NULL;
    for (uint8_t i = 0; i < DETAILS_CACHE_SLOTS; i++)
    {
        NotificationDetails* slot = &details_cache[i];
        if (slot->body == NULL)
        {
            return slot;
        }
        if (slot->bucket_id == pinned_bucket)
        {
            continue;
        }
        if (oldest == NULL || slot->last_used < oldest->last_used)
        {
            oldest = slot;
        }
    }

    if (oldest != NULL)
    {
        clear_details(oldest);
    }
    return oldest;
}

void notification_store_on_details_received(const uint8_t bucket_id, const char* body, const size_t body_size,
                                            const Action* actions, const uint8_t num_actions)
{
    if (notification_store_index_of(bucket_id) < 0 && !bucket_sync_is_currently_syncing)
    {
        // Details for a notification that is no longer on the watch. Not useful.
        // (While a sync runs the notification may simply not have arrived yet, so keep those.)
        return;
    }

    NotificationDetails* slot = slot_for_details(bucket_id);
    if (slot == NULL)
    {
        return;
    }

    const size_t clipped = body_size < MAX_BODY_TEXT_SIZE ? body_size : MAX_BODY_TEXT_SIZE;
    char* copy = malloc(clipped + 1);
    if (copy == NULL)
    {
        // Out of memory: evict everything except the pinned entry and try once more.
        for (uint8_t i = 0; i < DETAILS_CACHE_SLOTS; i++)
        {
            if (&details_cache[i] != slot && details_cache[i].bucket_id != pinned_bucket)
            {
                clear_details(&details_cache[i]);
            }
        }
        copy = malloc(clipped + 1);
        if (copy == NULL)
        {
            return;
        }
    }
    memcpy(copy, body, clipped);
    copy[clipped] = '\0';

    free(slot->body);
    slot->bucket_id = bucket_id;
    slot->body = copy;
    slot->num_actions = num_actions < MAX_NOTIFICATION_ACTIONS ? num_actions : MAX_NOTIFICATION_ACTIONS;
    if (slot->num_actions > 0 && actions != NULL)
    {
        memcpy(slot->actions, actions, sizeof(Action) * slot->num_actions);
    }
    slot->last_used = ++details_use_counter;
    // The phone builds details from its live state, so details that land while a sync is running are at
    // least as new as the bucket contents in that sync and must survive its "content changed" pass.
    slot->received_during_sync = bucket_sync_is_currently_syncing;

    if (listener != NULL && listener->details_changed != NULL)
    {
        listener->details_changed(bucket_id);
    }
}

const NotificationDetails* notification_store_details(const uint8_t bucket_id)
{
    NotificationDetails* details = find_details(bucket_id);
    if (details != NULL)
    {
        details->last_used = ++details_use_counter;
    }
    return details;
}

const char* notification_store_body(const uint8_t bucket_id)
{
    const NotificationDetails* details = find_details(bucket_id);
    if (details != NULL)
    {
        return details->body;
    }

    const NotificationItem* item = notification_store_item_by_bucket(bucket_id);
    return item != NULL ? item->summary : "";
}

void notification_store_want_details(const uint8_t bucket_id, const bool urgent)
{
    if (find_details(bucket_id) != NULL || notification_store_index_of(bucket_id) < 0)
    {
        return;
    }

    if (urgent)
    {
        notification_details_fetcher_fetch(bucket_id);
    }
    else
    {
        notification_details_fetcher_prefetch(bucket_id);
    }
}

void notification_store_pin(const uint8_t bucket_id)
{
    pinned_bucket = bucket_id;
}

// ------------------------------------------------------------------------------------------------ queries

uint8_t notification_store_count(void)
{
    return item_count;
}

const NotificationItem* notification_store_item(const uint8_t index)
{
    return index < item_count ? &items[index] : NULL;
}

int16_t notification_store_index_of(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < item_count; i++)
    {
        if (items[i].bucket_id == bucket_id)
        {
            return i;
        }
    }
    return -1;
}

const NotificationItem* notification_store_item_by_bucket(const uint8_t bucket_id)
{
    const int16_t index = notification_store_index_of(bucket_id);
    return index >= 0 ? &items[index] : NULL;
}

void notification_store_mark_seen(const uint8_t bucket_id)
{
    const int16_t index = notification_store_index_of(bucket_id);
    if (index < 0 || !items[index].unread)
    {
        return;
    }

    const uint8_t flag = 1;
    persist_write_data(STORAGE_SEEN_FLAG_MIN + bucket_id, &flag, 1);
    items[index].unread = false;
    notification_details_fetcher_mark_read(bucket_id);
}

bool notification_store_any_wants_periodic_vibration(void)
{
    for (uint8_t i = 0; i < item_count; i++)
    {
        if (items[i].periodic_vibration && items[i].unread)
        {
            return true;
        }
    }
    return false;
}

// ------------------------------------------------------------------------------------------------ sync callbacks

static void remember_changed(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < changed_count; i++)
    {
        if (changed_since_last_notify[i] == bucket_id)
        {
            return;
        }
    }
    if (changed_count < sizeof(changed_since_last_notify))
    {
        changed_since_last_notify[changed_count++] = bucket_id;
    }
}

static void on_bucket_data_changed(const BucketMetadata metadata, void* context)
{
    (void)context;
    if (metadata.id == SETTINGS_BUCKET_ID)
    {
        if (listener != NULL && listener->settings_changed != NULL)
        {
            listener->settings_changed();
        }
        if (settings_listener != NULL)
        {
            settings_listener();
        }
        return;
    }

    // New content: whatever we cached is stale and the notification counts as unseen again.
    persist_delete(STORAGE_SEEN_FLAG_MIN + metadata.id);
    drop_details(metadata.id, true);
    if (find_details(metadata.id) == NULL)
    {
        notification_details_fetcher_cancel(metadata.id);
    }
    remember_changed(metadata.id);
}

static void on_bucket_deleted(const uint8_t bucket_id)
{
    persist_delete(STORAGE_SEEN_FLAG_MIN + bucket_id);
    drop_details(bucket_id, false);
    notification_details_fetcher_cancel(bucket_id);
}

static void on_bucket_list_changed(void)
{
    rebuild_items();

    for (uint8_t i = 0; i < DETAILS_CACHE_SLOTS; i++)
    {
        details_cache[i].received_during_sync = false;
        if (details_cache[i].body != NULL && notification_store_index_of(details_cache[i].bucket_id) < 0)
        {
            clear_details(&details_cache[i]);
        }
    }

    uint8_t changed[STORE_MAX_ITEMS + 1];
    const uint8_t count = changed_count;
    memcpy(changed, changed_since_last_notify, count);
    changed_count = 0;

    if (listener != NULL && listener->list_changed != NULL)
    {
        listener->list_changed(changed, count);
    }
}

void notification_store_init(void)
{
    bucket_sync_set_bucket_list_change_callback(on_bucket_list_changed);
    bucket_sync_set_bucket_data_change_callback(on_bucket_data_changed, NULL);
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
    rebuild_items();
}

void notification_store_hide(const uint8_t bucket_id)
{
    uint8_t slot = 0;
    for (uint8_t i = 0; i < HIDDEN_SLOTS; i++)
    {
        if (hidden_buckets[i] == bucket_id || hidden_buckets[i] == 0)
        {
            slot = i;
            break;
        }
        if (hidden_until[i] < hidden_until[slot])
        {
            slot = i;
        }
    }
    hidden_buckets[slot] = bucket_id;
    hidden_until[slot] = time(NULL) + HIDE_SECONDS;
    on_bucket_list_changed();
}

void notification_store_set_listener(const StoreListener* new_listener)
{
    listener = new_listener;
}

void notification_store_set_settings_listener(void (*new_listener)(void))
{
    settings_listener = new_listener;
}

#include "notification_store.h"

#include <stdlib.h>

#include "commons/bytes.h"
#include "connection/notification_details_fetcher.h"

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define DETAILS_CACHE_SLOTS 3
#else
#define DETAILS_CACHE_SLOTS PBL_PLATFORM_SWITCH(PBL_PLATFORM_TYPE_CURRENT, 2, 3, 5, 3, 5, 5, 5)
#endif
#define SETTINGS_BUCKET_ID 1

static const uint32_t STORAGE_SEEN_FLAG_MIN = 3000;

static NotificationItem items[STORE_MAX_ITEMS];
static uint8_t item_count = 0;

// List order: order[display index] = index into items (which stay in the phone's bucket order).
static uint8_t order[STORE_MAX_ITEMS];

#define MAX_ROWS (STORE_MAX_ITEMS * 2 + GROUP_COUNT)
static ListRow rows[MAX_ROWS];
static uint8_t row_count = 0;

static bool group_sections = false;
static bool group_stacks = false;

// Closed sections and categories (bit per GROUP_*), remembered between launches. Silent starts closed.
#define STORAGE_COLLAPSED_GROUPS 3500
static uint8_t collapsed_groups = 1 << GROUP_SILENT;
// Stacks the user opened this session.
#define MAX_OPEN_STACKS 8
static uint8_t open_stacks[MAX_OPEN_STACKS];

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
static void rebuild_layout(void);

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
    if (size < 10 || !bucket_sync_load_bucket_limited(metadata.id, data, sizeof(data) - 1))
    {
        return;
    }

    item->receive_time = read_uint32_from_byte_array(data, 0);
    item->icon_id = data[4];
    item->color_id = data[5];
    item->image_aspect = data[6];
    item->image_tag = data[7];
    // Bits 0-1: 0 alerting, 1 conversation, 2 silent. Bits 2-4: 1 suggested, 2 promotions, 3 news, 4 social.
    const uint8_t section = data[8] & 0x03;
    const uint8_t category = (data[8] >> 2) & 0x07;
    if (category >= 1 && category <= 4)
    {
        item->group = GROUP_SUGGESTED + category - 1;
    }
    else
    {
        item->group = section == 1 ? GROUP_CONVERSATIONS : section == 2 ? GROUP_SILENT : GROUP_NOTIFICATIONS;
    }
    item->stack_id = data[9];

    size_t position = 10;
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

// A mirror, not a store: what was saved on the watch last time is never shown as such. Once the phone answers,
// its first sync lists the current notifications in order and says which of them still have data on the way;
// the list fills from the top as that data arrives (everything else the watch holds is confirmed current), and
// rows never disappear or reorder while filling. After that sync every update comes straight through.
static bool phone_answered;
static bool live;

// Buckets already on screen during the first fill. They stay, even if a retried sync sends them again.
static uint8_t shown_ids[STORE_MAX_ITEMS];
static uint8_t shown_count;

static bool was_shown(const uint8_t id)
{
    for (int i = 0; i < shown_count; i++)
    {
        if (shown_ids[i] == id)
        {
            return true;
        }
    }
    return false;
}

static void rebuild_items(void)
{
    const BucketList* buckets = bucket_sync_get_bucket_list();
    if (!live && phone_answered && !bucket_sync_is_currently_syncing && !bucket_sync_last_sync_failed)
    {
        live = true;
    }
    item_count = 0;
    if (!live && !phone_answered)
    {
        return;
    }
    for (int i = 0; i < buckets->count && item_count < STORE_MAX_ITEMS; i++)
    {
        const uint8_t id = buckets->data[i].id;
        if (id == SETTINGS_BUCKET_ID || is_hidden(id))
        {
            continue;
        }
        if (!live && bucket_sync_is_bucket_awaited(id) && !was_shown(id))
        {
            // First fill: stop at the first row whose current data hasn't arrived; more fill in below.
            break;
        }

        parse_item(buckets->data[i], &items[item_count]);
        if (!live && !items[item_count].loaded)
        {
            break;
        }
        if (!live && !was_shown(id) && shown_count < STORE_MAX_ITEMS)
        {
            shown_ids[shown_count++] = id;
        }
        item_count++;
    }
    rebuild_layout();
}

// ------------------------------------------------------------------------------------------------ grouping

static uint8_t display_group(const NotificationItem* item)
{
    return group_sections ? item->group : GROUP_NOTIFICATIONS;
}

static uint8_t stack_size(const uint8_t raw, const uint8_t group)
{
    if (!group_stacks || items[raw].stack_id == 0)
    {
        return 1;
    }
    uint8_t count = 0;
    for (uint8_t i = 0; i < item_count; i++)
    {
        if (items[i].stack_id == items[raw].stack_id && display_group(&items[i]) == group)
        {
            count++;
        }
    }
    return count;
}

static bool is_stack_open(const uint8_t stack_id)
{
    for (uint8_t i = 0; i < MAX_OPEN_STACKS; i++)
    {
        if (open_stacks[i] == stack_id)
        {
            return true;
        }
    }
    return false;
}

static void set_stack_open(const uint8_t stack_id, const bool open)
{
    for (uint8_t i = 0; i < MAX_OPEN_STACKS; i++)
    {
        if (open_stacks[i] == stack_id)
        {
            if (!open)
            {
                open_stacks[i] = 0;
            }
            return;
        }
    }
    if (!open)
    {
        return;
    }
    for (uint8_t i = 0; i < MAX_OPEN_STACKS; i++)
    {
        if (open_stacks[i] == 0)
        {
            open_stacks[i] = stack_id;
            return;
        }
    }
    open_stacks[0] = stack_id;
}

static bool group_open(const uint8_t group)
{
    if ((collapsed_groups & (1 << group)) != 0)
    {
        return false;
    }
    // Categories live inside Silent.
    return group < GROUP_SUGGESTED || (collapsed_groups & (1 << GROUP_SILENT)) == 0;
}

static void add_row(const ListRowKind kind, const uint8_t group, const uint8_t item, const uint8_t count,
                    const bool expanded, const bool in_stack)
{
    if (row_count < MAX_ROWS)
    {
        rows[row_count++] = (ListRow){
            .kind = kind, .group = group, .item = item, .count = count, .expanded = expanded, .in_stack = in_stack,
        };
    }
}

static uint8_t count_in_group(const uint8_t group, const bool with_categories)
{
    uint8_t count = 0;
    for (uint8_t i = 0; i < item_count; i++)
    {
        const uint8_t g = display_group(&items[i]);
        if (g == group || (with_categories && group == GROUP_SILENT && g >= GROUP_SUGGESTED))
        {
            count++;
        }
    }
    return count;
}

/**
 * Lays the notifications out like the phone's shade: sections (Conversations, Notifications, Silent with its
 * categories) in that order, each in the phone's own order, with an app's stacked notifications kept together
 * at the place of its newest one. Builds both the card order and the list rows.
 */
static void rebuild_layout(void)
{
    bool placed[STORE_MAX_ITEMS] = {false};
    uint8_t placed_count = 0;
    row_count = 0;

    for (uint8_t group = 0; group < GROUP_COUNT; group++)
    {
        const uint8_t total = count_in_group(group, true);
        if (total == 0)
        {
            continue;
        }
        const bool open = !group_sections || group_open(group);
        if (group_sections)
        {
            const bool parent_open = group < GROUP_SUGGESTED || group_open(GROUP_SILENT);
            if (parent_open)
            {
                add_row(ROW_HEADER, group, 0, group == GROUP_SILENT ? total : count_in_group(group, false),
                        open, false);
            }
        }

        for (uint8_t raw = 0; raw < item_count; raw++)
        {
            if (placed[raw] || display_group(&items[raw]) != group)
            {
                continue;
            }
            const uint8_t members = stack_size(raw, group);
            const uint8_t stack_id = items[raw].stack_id;
            const bool stack_open = members > 1 && is_stack_open(stack_id);
            if (members > 1 && open)
            {
                add_row(ROW_STACK, group, placed_count, members, stack_open, false);
            }
            for (uint8_t member = raw; member < item_count; member++)
            {
                const bool same = member == raw ||
                    (members > 1 && items[member].stack_id == stack_id && display_group(&items[member]) == group);
                if (!same || placed[member])
                {
                    continue;
                }
                placed[member] = true;
                if (open && (members == 1 || stack_open))
                {
                    add_row(ROW_ITEM, group, placed_count, 1, true, members > 1);
                }
                order[placed_count++] = member;
            }
        }
    }
}

void notification_store_set_grouping(const bool sections, const bool stacks)
{
    if (sections == group_sections && stacks == group_stacks)
    {
        return;
    }
    group_sections = sections;
    group_stacks = stacks;
    rebuild_layout();
}

uint8_t notification_store_row_count(void)
{
    return row_count;
}

const ListRow* notification_store_row(const uint8_t row)
{
    return row < row_count ? &rows[row] : NULL;
}

int16_t notification_store_row_of_bucket(const uint8_t bucket_id)
{
    const int16_t index = notification_store_index_of(bucket_id);
    if (index < 0)
    {
        return -1;
    }
    const NotificationItem* item = notification_store_item(index);
    int16_t container = -1;
    for (uint8_t i = 0; i < row_count; i++)
    {
        const ListRow* row = &rows[i];
        if (row->kind == ROW_ITEM && row->item == index)
        {
            return i;
        }
        if (row->kind == ROW_STACK && !row->expanded && index >= row->item && index < row->item + row->count)
        {
            return i;
        }
        if (row->kind == ROW_HEADER &&
            (row->group == display_group(item) || (row->group == GROUP_SILENT && display_group(item) >= GROUP_SUGGESTED)))
        {
            container = i;
        }
    }
    return container;
}

void notification_store_toggle_row(const uint8_t index)
{
    const ListRow* row = notification_store_row(index);
    if (row == NULL)
    {
        return;
    }
    if (row->kind == ROW_HEADER)
    {
        collapsed_groups ^= 1 << row->group;
        persist_write_data(STORAGE_COLLAPSED_GROUPS, &collapsed_groups, sizeof(collapsed_groups));
    }
    else if (row->kind == ROW_STACK)
    {
        set_stack_open(items[order[row->item]].stack_id, !row->expanded);
    }
    else
    {
        return;
    }
    rebuild_layout();
}

const char* notification_store_group_name(const uint8_t group)
{
    switch (group)
    {
    case GROUP_CONVERSATIONS:
        return "Conversations";
    case GROUP_SILENT:
        return "Silent";
    case GROUP_SUGGESTED:
        return "Suggested";
    case GROUP_PROMOTIONS:
        return "Promotions";
    case GROUP_NEWS:
        return "News";
    case GROUP_SOCIAL:
        return "Social";
    default:
        return "Notifications";
    }
}

static void on_bucket_list_changed(void);

static void on_sync_progress(void)
{
    if (!live && phone_answered)
    {
        on_bucket_list_changed();
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

void notification_store_on_details_unavailable(const uint8_t bucket_id)
{
    // A timeout says nothing about the length of the message. Keep the summary partial so opening or
    // scrolling this card can request its body again once the connection recovers.
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
    return index < item_count ? &items[order[index]] : NULL;
}

static int16_t raw_index_of(const uint8_t bucket_id)
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

int16_t notification_store_index_of(const uint8_t bucket_id)
{
    for (uint8_t i = 0; i < item_count; i++)
    {
        if (items[order[i]].bucket_id == bucket_id)
        {
            return i;
        }
    }
    return -1;
}

const NotificationItem* notification_store_item_by_bucket(const uint8_t bucket_id)
{
    const int16_t index = raw_index_of(bucket_id);
    return index >= 0 ? &items[index] : NULL;
}

void notification_store_mark_seen(const uint8_t bucket_id)
{
    const int16_t index = raw_index_of(bucket_id);
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
    if (persist_exists(STORAGE_COLLAPSED_GROUPS))
    {
        persist_read_data(STORAGE_COLLAPSED_GROUPS, &collapsed_groups, sizeof(collapsed_groups));
    }
    bucket_sync_set_bucket_list_change_callback(on_bucket_list_changed);
    bucket_sync_set_bucket_data_change_callback(on_bucket_data_changed, NULL);
    bucket_sync_register_bucket_deleted_callback(on_bucket_deleted);
    bucket_sync_set_progress_callback(on_sync_progress);
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

void notification_store_on_phone_synced(void)
{
    phone_answered = true;
    if (!live)
    {
        on_bucket_list_changed();
    }
}

bool notification_store_is_live(void)
{
    return live;
}

void notification_store_set_listener(const StoreListener* new_listener)
{
    listener = new_listener;
}

void notification_store_set_settings_listener(void (*new_listener)(void))
{
    settings_listener = new_listener;
}

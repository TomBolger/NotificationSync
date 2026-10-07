// Exercise the actual watch fetcher/store on the host, with only Pebble OS calls stubbed.
#include <assert.h>
#include <stdio.h>
#include "../src/connection/notification_details_fetcher.c"
#include "../src/ui/window_notification/notification_store.c"

bool is_currently_sending_data;
bool is_phone_connected = true;
bool bucket_sync_is_currently_syncing;
bool close_after_sync;
static AppTimer fake_timer;
static void (*read_callback)(bool);
static BucketList fake_buckets;
AppTimer* app_timer_register(uint32_t delay, void (*cb)(void*), void* context)
{ (void)delay; (void)context; fake_timer.callback = cb; return &fake_timer; }
void app_timer_cancel(AppTimer* value) { (void)value; }
int persist_read_data(uint32_t key, void* dst, size_t size) { (void)key; memset(dst,0,size); return -1; }
int persist_write_data(uint32_t key, const void* src, size_t size) { (void)key; (void)src; return size; }
int persist_delete(uint32_t key) { (void)key; return 0; }
bool send_notification_opened(uint8_t bucket, bool prefetch) { (void)bucket; (void)prefetch; return true; }
bool send_mark_read(uint8_t bucket, void (*cb)(bool)) { (void)bucket; read_callback=cb; return true; }
void bluetooth_register_sending_finish(void (*cb)(bool)) { (void)cb; }
BucketList* bucket_sync_get_bucket_list(void) { return &fake_buckets; }
uint8_t bucket_sync_get_bucket_size(uint8_t id) { (void)id; return 0; }
bool bucket_sync_load_bucket_limited(uint8_t id,uint8_t*dst,uint8_t max) { (void)id;(void)dst;(void)max;return false; }
void bucket_sync_set_bucket_list_change_callback(void(*cb)(void)) { (void)cb; }
void bucket_sync_set_bucket_data_change_callback(void(*cb)(BucketMetadata,void*),void*context) { (void)cb;(void)context; }
void bucket_sync_register_bucket_deleted_callback(void(*cb)(uint8_t)) { (void)cb; }

static void reset_fixture(void)
{
    notification_details_fetcher_reset();
    reset_staging();
    mark_read_count=0; mark_read_in_flight=false;
    for (int i=0;i<DETAILS_CACHE_SLOTS;i++) clear_details(&details_cache[i]);
    item_count=1;
    items[0]=(NotificationItem){.bucket_id=2,.loaded=true,.summary_complete=false};
    strcpy(items[0].summary,"preview");
}
int main(void)
{
    reset_fixture();
    notification_details_fetcher_fetch(2);
    on_timer(NULL); on_timer(NULL); on_timer(NULL);
    assert(notification_store_is_partial(2));
    assert(!items[0].summary_complete);
    notification_details_fetcher_fetch(2);
    assert(has_in_flight); // timeout did not make the preview permanent

    const uint8_t first[]={2,3,0,0,0,'a','b'};
    const uint8_t middle[]={2,1,3,'c','d'};
    const uint8_t last[]={2,2,3,'e','f'};
    notification_details_fetcher_on_text_received_v2(first,sizeof(first));
    notification_details_fetcher_on_text_continuation_received(middle,sizeof(middle));
    notification_details_fetcher_on_text_continuation_received(middle,sizeof(middle)); // duplicate ACK retry
    assert(notification_store_details(2)==NULL);
    notification_details_fetcher_on_text_continuation_received(last,sizeof(last));
    assert(strcmp(notification_store_body(2),"abcdef")==0);
    assert(!notification_store_is_partial(2));

    reset_fixture();
    notification_details_fetcher_fetch(2);
    notification_details_fetcher_on_text_received_v2(first,sizeof(first));
    notification_details_fetcher_on_text_continuation_received(last,sizeof(last)); // missing middle
    assert(notification_store_details(2)==NULL);
    assert(notification_store_is_partial(2));

    reset_fixture();
    notification_details_fetcher_mark_read(2);
    assert(mark_read_count==1);
    read_callback(false);
    assert(mark_read_count==1); // failed delivery retains read update
    on_timer(NULL);
    read_callback(true);
    assert(mark_read_count==0);
    puts("Watch regressions passed: failed preview, complete/duplicate/missing chunks, read delivery.");
}

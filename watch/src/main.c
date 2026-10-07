#include <pebble.h>
#include "commons/connection/bluetooth.h"
#include "commons/connection/bucket_sync.h"
#include "connection/notification_details_fetcher.h"
#include "connection/packets.h"
#include "ui/window_status.h"
#include "ui/window_notification/notification_store.h"
#include "ui/window_notification/window_notification.h"
#include "ui/window_notification/idle_handler.h"

const uint16_t PROTOCOL_VERSION = 11;

int main(void)
{
    packets_init();
    bluetooth_init();
    idle_handler_reset_user_interaction();
    bucket_sync_init();
    notification_details_fetcher_init();
    notification_store_init();
    bluetooth_register_reconnect_callback(send_watch_welcome);

    send_watch_welcome();

    window_notification_show();

    app_event_loop();
}

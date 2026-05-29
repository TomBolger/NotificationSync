#include <pebble.h>
#include "commons/connection/bluetooth.h"
#include "commons/connection/bucket_sync.h"
#include "connection/notification_details_fetcher.h"
#include "connection/packets.h"
#include "ui/window_status.h"
#include "ui/window_notification/data_loading.h"
#include "ui/window_notification/window_notification.h"
#include "utils/bucket_utils.h"

const uint16_t PROTOCOL_VERSION = 9;

int main(void)
{
    packets_init();
    bluetooth_init();
    window_notification_data_app_started();
    bucket_sync_init();
    notification_details_fetcher_init();

    send_watch_welcome();

    window_notification_show();

    app_event_loop();
}

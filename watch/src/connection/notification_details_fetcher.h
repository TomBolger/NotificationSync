#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Requests full notification details (body + actions) from the phone.
 *
 * Only one request is in flight at a time. Every request has a timeout and is retried, so a lost packet or
 * a phone that silently ignores a request (for example because the notification was dismissed in the
 * meantime) can never wedge the queue.
 */

void notification_details_fetcher_init();

/** Fetch details the user is waiting for. Jumps the queue. */
void notification_details_fetcher_fetch(uint8_t bucket_id);

/** Fetch details we will probably need soon. Does not mark the notification as read on the phone. */
void notification_details_fetcher_prefetch(uint8_t bucket_id);

/** Forget any queued or in-flight request for this bucket (e.g. it was deleted). */
void notification_details_fetcher_cancel(uint8_t bucket_id);

/** Drop everything (e.g. after a full resync). */
void notification_details_fetcher_reset(void);

void notification_details_fetcher_on_text_received(const uint8_t* data, size_t data_size);
void notification_details_fetcher_on_text_received_v2(const uint8_t* data, size_t data_size);
void notification_details_fetcher_on_text_continuation_received(const uint8_t* data, size_t data_size);

bool notification_details_fetcher_is_fetching(void);
void notification_details_fetcher_register_fetching_status_callback(void (*callback)());
void notification_details_fetcher_unregister_fetching_status_callback(void (*callback)());

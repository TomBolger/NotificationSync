#pragma once
#include <pebble.h>

/**
 * Startup timing, sent to the phone once (packet 18) so it shows up in the phone's Save logs: how long each step
 * of opening the app took on the watch itself, which the phone cannot see.
 */
typedef enum
{
    TIMING_HELLO_FIRST_SENT,
    TIMING_HELLO_DELIVERED,
    TIMING_PHONE_ANSWERED,
    TIMING_LIVE,
    TIMING_FIRST_ROW,
    TIMING_FIRST_DETAILS,
    TIMING_COUNT
} TimingMark;

void timing_init(void);
void timing_mark(TimingMark mark);
void timing_hello_attempt(void);
void timing_hello_failed(AppMessageResult reason);
/** Sends the report once the list is live, when the outbox is free. */
void timing_maybe_send(void);

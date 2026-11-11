#include "timing.h"
#include "commons/connection/bluetooth.h"

#define TIMING_UNSET 0xFFFF
#define TIMING_SEND_DELAY_MS 3000

static time_t start_s;
static uint16_t start_ms;
static uint16_t marks[TIMING_COUNT];
static uint8_t hello_attempts;
static uint8_t hello_failures;
static uint8_t last_failure;
static bool sent;
static AppTimer* send_timer;

static uint16_t now_ms(void)
{
    time_t s;
    uint16_t ms;
    time_ms(&s, &ms);
    const int32_t elapsed = (int32_t)(s - start_s) * 1000 + ms - start_ms;
    return elapsed < 0 ? 0 : (elapsed > 0xFFFE ? 0xFFFE : (uint16_t)elapsed);
}

void timing_init(void)
{
    time_ms(&start_s, &start_ms);
    for (int i = 0; i < TIMING_COUNT; i++)
    {
        marks[i] = TIMING_UNSET;
    }
}

void timing_mark(const TimingMark mark)
{
    if (marks[mark] == TIMING_UNSET)
    {
        marks[mark] = now_ms();
    }
    if (mark == TIMING_LIVE)
    {
        timing_maybe_send();
    }
}

void timing_hello_attempt(void)
{
    if (hello_attempts < 255)
    {
        hello_attempts++;
    }
    timing_mark(TIMING_HELLO_FIRST_SENT);
}

void timing_hello_failed(const AppMessageResult reason)
{
    if (hello_failures < 255)
    {
        hello_failures++;
    }
    last_failure = (uint8_t)reason;
}

static void send_now(void* context)
{
    (void)context;
    send_timer = NULL;
    if (sent)
    {
        return;
    }
    DictionaryIterator* iterator;
    if (is_currently_sending_data || app_message_outbox_begin(&iterator) != APP_MSG_OK)
    {
        send_timer = app_timer_register(500, send_now, NULL);
        return;
    }
    sent = true;
    dict_write_uint8(iterator, 0, 18);
    // Milliseconds since the app started, 0xFFFF = never happened.
    dict_write_data(iterator, 1, (const uint8_t*)marks, sizeof(marks));
    dict_write_uint8(iterator, 2, hello_attempts);
    dict_write_uint8(iterator, 3, hello_failures);
    dict_write_uint8(iterator, 4, last_failure);
    bluetooth_app_message_outbox_send();
}

void timing_maybe_send(void)
{
    if (!sent && send_timer == NULL && marks[TIMING_LIVE] != TIMING_UNSET)
    {
        // Give the first details a moment to arrive so they're in the report too.
        send_timer = app_timer_register(TIMING_SEND_DELAY_MS, send_now, NULL);
    }
}

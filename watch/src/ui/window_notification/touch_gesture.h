#pragma once
#include <pebble.h>

/**
 * Turns the raw touch stream into the few gestures the notification UI needs:
 * a tap, a vertical drag (with release velocity for momentum), a "back" swipe to the right and a swipe up
 * that starts on the bottom edge of the screen.
 *
 * Positive dy / velocity means the finger moved down the screen.
 */
typedef struct
{
    void (*touch_down)(void* context);
    void (*tap)(GPoint point, void* context);
    void (*drag_moved)(int16_t dy, void* context);
    void (*drag_ended)(int16_t dy, int32_t velocity, void* context);
    void (*swipe_back)(void* context);
    void (*edge_swipe_up)(void* context);
} TouchGestureHandlers;

typedef struct
{
    uint8_t state;
    GPoint start;
    GPoint last;
    uint32_t start_ms;
    uint32_t sample_ms[4];
    int16_t sample_y[4];
    uint8_t sample_count;
} TouchGesture;

/** Height of the strip at the bottom of the screen where an upward swipe means "next notification". */
#define TOUCH_EDGE_ZONE 26

void touch_gesture_feed(TouchGesture* gesture, const TouchEvent* event, const TouchGestureHandlers* handlers,
                        void* context);
void touch_gesture_reset(TouchGesture* gesture);

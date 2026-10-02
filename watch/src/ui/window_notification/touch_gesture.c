#include "touch_gesture.h"

#include <stdlib.h>

#define SLOP_PX 7
#define TAP_MAX_MS 450
#define BACK_SWIPE_PX 50
#define EDGE_SWIPE_PX 40
// Finger held still before lifting: no fling.
#define FLING_STALE_MS 90
#define VELOCITY_WINDOW_MS 120

enum
{
    StateIdle,
    StatePending,
    StateVertical,
    StateHorizontal,
    StateEdge,
};

static uint32_t now_ms(void)
{
    time_t seconds;
    uint16_t millis;
    time_ms(&seconds, &millis);
    return (uint32_t)seconds * 1000 + millis;
}

static void add_sample(TouchGesture* gesture, const int16_t y, const uint32_t ms)
{
    if (gesture->sample_count == ARRAY_LENGTH(gesture->sample_y))
    {
        memmove(&gesture->sample_y[0], &gesture->sample_y[1], sizeof(int16_t) * 3);
        memmove(&gesture->sample_ms[0], &gesture->sample_ms[1], sizeof(uint32_t) * 3);
        gesture->sample_count--;
    }
    gesture->sample_y[gesture->sample_count] = y;
    gesture->sample_ms[gesture->sample_count] = ms;
    gesture->sample_count++;
}

static int32_t release_velocity(const TouchGesture* gesture, const uint32_t release_ms)
{
    if (gesture->sample_count < 2)
    {
        return 0;
    }

    const uint8_t newest = gesture->sample_count - 1;
    if (release_ms - gesture->sample_ms[newest] > FLING_STALE_MS)
    {
        return 0;
    }

    uint8_t oldest = 0;
    while (oldest < newest && gesture->sample_ms[newest] - gesture->sample_ms[oldest] > VELOCITY_WINDOW_MS)
    {
        oldest++;
    }
    const int32_t dt = (int32_t)(gesture->sample_ms[newest] - gesture->sample_ms[oldest]);
    if (dt <= 0)
    {
        return 0;
    }
    return ((int32_t)(gesture->sample_y[newest] - gesture->sample_y[oldest]) * 1000) / dt;
}

void touch_gesture_reset(TouchGesture* gesture)
{
    memset(gesture, 0, sizeof(TouchGesture));
}

void touch_gesture_feed(TouchGesture* gesture, const TouchEvent* event, const TouchGestureHandlers* handlers,
                        void* context)
{
    const uint32_t ms = now_ms();
    const GPoint point = GPoint(event->x, event->y);

    switch (event->type)
    {
    case TouchEvent_Touchdown:
        touch_gesture_reset(gesture);
        if (event->non_navigational)
        {
            return;
        }
        gesture->state = StatePending;
        gesture->start = point;
        gesture->last = point;
        gesture->start_ms = ms;
        add_sample(gesture, point.y, ms);
        if (handlers->touch_down != NULL)
        {
            handlers->touch_down(context);
        }
        return;

    case TouchEvent_PositionUpdate:
    {
        if (gesture->state == StateIdle)
        {
            return;
        }
        gesture->last = point;
        add_sample(gesture, point.y, ms);
        const int16_t dx = point.x - gesture->start.x;
        const int16_t dy = point.y - gesture->start.y;

        if (gesture->state == StatePending)
        {
            if (abs(dy) > SLOP_PX && abs(dy) >= abs(dx))
            {
                gesture->state = handlers->edge_swipe_up != NULL &&
                    gesture->start.y >= PBL_DISPLAY_HEIGHT - TOUCH_EDGE_ZONE && dy < 0 ?
                    StateEdge : StateVertical;
            }
            else if (abs(dx) > SLOP_PX)
            {
                gesture->state = StateHorizontal;
            }
        }

        if (gesture->state == StateVertical && handlers->drag_moved != NULL)
        {
            handlers->drag_moved(dy, context);
        }
        return;
    }

    case TouchEvent_Liftoff:
    {
        const uint8_t state = gesture->state;
        gesture->state = StateIdle;
        const int16_t dx = gesture->last.x - gesture->start.x;
        const int16_t dy = gesture->last.y - gesture->start.y;

        if (state == StatePending)
        {
            if (ms - gesture->start_ms <= TAP_MAX_MS && handlers->tap != NULL)
            {
                handlers->tap(gesture->start, context);
            }
        }
        else if (state == StateVertical)
        {
            if (handlers->drag_ended != NULL)
            {
                handlers->drag_ended(dy, release_velocity(gesture, ms), context);
            }
        }
        else if (state == StateEdge)
        {
            if (dy <= -EDGE_SWIPE_PX && handlers->edge_swipe_up != NULL)
            {
                handlers->edge_swipe_up(context);
            }
        }
        else if (state == StateHorizontal)
        {
            if (dx >= BACK_SWIPE_PX && abs(dx) > abs(dy) && handlers->swipe_back != NULL)
            {
                handlers->swipe_back(context);
            }
            else if (dx <= -BACK_SWIPE_PX && abs(dx) > abs(dy) && handlers->swipe_select != NULL)
            {
                handlers->swipe_select(context);
            }
        }
        return;
    }
    }
}

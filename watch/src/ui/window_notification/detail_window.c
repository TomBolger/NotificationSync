#include "detail_window.h"

#include "action_list.h"
#include "card.h"
#include "notification_store.h"
#include "touch_gesture.h"
#include "window_notification.h"

// Scrolling behaviour mirrors PebbleOS src/fw/services/timeline/swap_layer.c (rectangular displays).
#define SCROLL_PX 48
#define REPEATING_SCROLL_PX 24
#define INITIAL_SCROLL_PX CARD_BANNER_HEIGHT
#define PEEK_PX CARD_BANNER_HEIGHT
#define FUDGE_PX PEEK_PX
#define SCROLL_MS 200
#define SWAP_MS 200
#define MESSAGE_SWAP_DELAY 3
#define DISMISS_FALLBACK_MS 500
#define ACTIONS_WAIT_MS 5000

typedef enum
{
    AnimNone,
    AnimScroll,
    AnimSwapDown,
    AnimSwapUp,
    AnimDismiss,
} AnimKind;

typedef struct
{
    uint8_t bucket;
    bool valid;
    CardMetrics metrics;
} MetricsEntry;

static const DetailCallbacks* callbacks;

static Window* window;
static Layer* card_layer;
static Layer* action_dot_layer;

static uint8_t current_bucket;
static int16_t last_known_index;
static int16_t offset;
static int8_t swap_delay_remaining = MESSAGE_SWAP_DELAY;

static MetricsEntry metrics_cache[4];
static uint8_t metrics_next_slot;

static Animation* animation;
static AnimKind anim_kind = AnimNone;
static int16_t anim_from;
static int16_t anim_to;
static int16_t anim_value;
static uint8_t anim_other_bucket;
static int16_t anim_start_offset;

static GDrawCommandSequence* dismiss_sequence;
static uint32_t dismiss_elapsed;
static uint8_t dismiss_bucket;

static bool actions_wanted;
static AppTimer* actions_timer;

static void finish_animation(void);
static void settle_on(uint8_t bucket, int16_t new_offset);
static void hold_step(void);
static void hold_cancel(void);

// Press-and-hold: one continuous, even scroll (no stepping), a short bump at the end of each message,
// then straight on into the next one.
#define HOLD_START_MS 120
#define HOLD_SPEED_PX_S 260
// The hop to the next message: as quick as a pressed swap, but it ends moving at the hold speed, so the
// scroll carries straight on instead of slowing to a stop and starting again.
#define HOLD_SWAP_MS 240
#define HOLD_BUMP_MS 600
#define HOLD_RETRY_MS 100
#define HOLD_RELEASE_PX 8
#define HOLD_RELEASE_MS 140

static int8_t hold_direction;
static bool hold_active;
static bool hold_bumped;
static bool hold_in_step;
static AppTimer* hold_timer;
static int16_t hold_press_target;
// Slope (x1000) the hold swap curve ends with; see hold_swap_curve().
static int32_t hold_swap_end_slope;
static AnimationCurveFunction next_custom_curve;

// ------------------------------------------------------------------------------------------------ model helpers

static int16_t current_index(void)
{
    return notification_store_index_of(current_bucket);
}

static const NotificationItem* neighbor(const int8_t delta)
{
    const int16_t index = current_index();
    if (index < 0)
    {
        return NULL;
    }
    const int16_t target = index + delta;
    if (target < 0 || target >= notification_store_count())
    {
        return NULL;
    }
    return notification_store_item(target);
}

static void invalidate_metrics(void)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(metrics_cache); i++)
    {
        metrics_cache[i].valid = false;
    }
}

static void invalidate_metrics_for(const uint8_t bucket)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(metrics_cache); i++)
    {
        if (metrics_cache[i].bucket == bucket)
        {
            metrics_cache[i].valid = false;
        }
    }
}

static const CardMetrics* metrics_for(const uint8_t bucket)
{
    for (uint8_t i = 0; i < ARRAY_LENGTH(metrics_cache); i++)
    {
        if (metrics_cache[i].valid && metrics_cache[i].bucket == bucket)
        {
            return &metrics_cache[i].metrics;
        }
    }

    MetricsEntry* entry = NULL;
    for (uint8_t i = 0; i < ARRAY_LENGTH(metrics_cache); i++)
    {
        // Never evict the card on screen.
        if (!metrics_cache[i].valid)
        {
            entry = &metrics_cache[i];
            break;
        }
    }
    if (entry == NULL)
    {
        for (uint8_t attempt = 0; attempt < ARRAY_LENGTH(metrics_cache); attempt++)
        {
            MetricsEntry* candidate = &metrics_cache[metrics_next_slot];
            metrics_next_slot = (metrics_next_slot + 1) % ARRAY_LENGTH(metrics_cache);
            if (candidate->bucket != current_bucket)
            {
                entry = candidate;
                break;
            }
        }
    }

    entry->bucket = bucket;
    entry->valid = true;
    card_measure(notification_store_item_by_bucket(bucket), notification_store_body(bucket), PBL_DISPLAY_WIDTH,
                 notification_store_is_partial(bucket), &entry->metrics);
    return &entry->metrics;
}

/** The current card only shows the beginning of its text; the rest is on the way. */
static bool current_is_partial(void)
{
    return current_bucket != 0 && notification_store_is_partial(current_bucket);
}

static int16_t card_height(const uint8_t bucket)
{
    return metrics_for(bucket)->total_height;
}

static int16_t max_scroll(void)
{
    if (current_bucket == 0)
    {
        return 0;
    }
    int16_t max = card_height(current_bucket) - PBL_DISPLAY_HEIGHT;
    // A partial card has no end yet, so it never reveals the next card underneath it.
    if (neighbor(1) != NULL && !current_is_partial())
    {
        max += PEEK_PX;
    }
    return max > 0 ? max : 0;
}

static void refresh_decorations(void)
{
    if (card_layer == NULL)
    {
        return;
    }

    const bool idle = anim_kind == AnimNone || anim_kind == AnimScroll;
    const NotificationItem* item = notification_store_item_by_bucket(current_bucket);
    layer_set_hidden(action_dot_layer, !(idle && item != NULL && item->loaded));

    layer_mark_dirty(card_layer);
}

// ------------------------------------------------------------------------------------------------ drawing

/** Whether this card has anything below its first screen (more text, or another notification after it). */
static bool card_wants_arrow(const uint8_t bucket)
{
    const int16_t index = notification_store_index_of(bucket);
    if (index < 0)
    {
        return false;
    }
    return card_height(bucket) > PBL_DISPLAY_HEIGHT ||
        notification_store_is_partial(bucket) ||
        index + 1 < notification_store_count();
}

static void draw_down_arrow(GContext* ctx, const int16_t y)
{
    const int16_t width = PBL_DISPLAY_WIDTH;
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, y, width, CARD_ARROW_HEIGHT), 0, GCornerNone);
    graphics_context_set_stroke_color(ctx, GColorBlack);
    const int16_t center_x = width / 2;
    for (int16_t row = 0; row <= 5; row++)
    {
        graphics_draw_line(ctx, GPoint(center_x - 5 + row, y + 7 + row), GPoint(center_x + 5 - row, y + 7 + row));
    }
}

/**
 * The "more below" arrow belongs to a card's first screen (PebbleOS swap_layer.c shows it only while the card
 * sits at the top). Drawing it with the card instead of as a fixed overlay means it slides in together with an
 * incoming card, and slides off the bottom edge as soon as you start scrolling, instead of popping on and off.
 */
static void draw_card_arrow(GContext* ctx, const uint8_t bucket, const int16_t card_y)
{
    if (!card_wants_arrow(bucket))
    {
        return;
    }

    const int16_t resting_y = PBL_DISPLAY_HEIGHT - CARD_ARROW_HEIGHT;
    int16_t y;
    if (card_y >= 0)
    {
        y = card_y + resting_y;
    }
    else
    {
        const int16_t scrolled = -card_y;
        if (scrolled >= CARD_ARROW_HEIGHT)
        {
            return;
        }
        y = resting_y + scrolled;
    }

    if (y < PBL_DISPLAY_HEIGHT)
    {
        draw_down_arrow(ctx, y);
    }
}

static void draw_card(GContext* ctx, const uint8_t bucket, const int16_t y)
{
    const NotificationItem* item = notification_store_item_by_bucket(bucket);
    if (item == NULL)
    {
        return;
    }
    card_draw(ctx, item, notification_store_body(bucket), metrics_for(bucket), PBL_DISPLAY_WIDTH, y,
              notification_store_index_of(bucket), notification_store_count());
}

static void card_layer_update(Layer* layer, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(layer);

    if (anim_kind == AnimDismiss)
    {
        graphics_context_set_fill_color(ctx, GColorWhite);
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);
        if (dismiss_sequence != NULL)
        {
            GDrawCommandFrame* frame = gdraw_command_sequence_get_frame_by_elapsed(dismiss_sequence, dismiss_elapsed);
            if (frame != NULL)
            {
                const GSize size = gdraw_command_sequence_get_bounds_size(dismiss_sequence);
                gdraw_command_frame_draw(ctx, dismiss_sequence, frame,
                                         GPoint((bounds.size.w - size.w) / 2, (bounds.size.h - size.h) / 2));
            }
        }
        return;
    }

    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);

    if (current_bucket == 0)
    {
        return;
    }

    if (anim_kind == AnimSwapUp)
    {
        // The previous card slides down from above, pushing the current one off the bottom.
        const int16_t other_height = card_height(anim_other_bucket);
        const int16_t other_y = -other_height - anim_start_offset + anim_value;
        const int16_t current_y = -anim_start_offset + anim_value;
        draw_card(ctx, current_bucket, current_y);
        draw_card_arrow(ctx, current_bucket, current_y);
        draw_card(ctx, anim_other_bucket, other_y);
        draw_card_arrow(ctx, anim_other_bucket, other_y);
        return;
    }

    const int16_t current_y = anim_kind == AnimSwapDown ? -anim_value : -offset;
    const int16_t height = card_height(current_bucket);
    draw_card(ctx, current_bucket, current_y);

    const NotificationItem* next = anim_kind == AnimSwapDown ?
        notification_store_item_by_bucket(anim_other_bucket) : (current_is_partial() ? NULL : neighbor(1));
    if (next != NULL && current_y + height < bounds.size.h)
    {
        draw_card(ctx, next->bucket_id, current_y + height);
        if (anim_kind == AnimSwapDown)
        {
            // The incoming card brings its own arrow with it.
            draw_card_arrow(ctx, next->bucket_id, current_y + height);
        }
    }
    draw_card_arrow(ctx, current_bucket, current_y);
}


static void action_dot_update(Layer* layer, GContext* ctx)
{
    // PebbleOS src/fw/applib/ui/action_button.c
    const GRect bounds = layer_get_bounds(layer);
    const int radius = 13;
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(bounds.size.w + radius - 5, bounds.size.h / 2), radius);
}

// ------------------------------------------------------------------------------------------------ animation

static void animation_update(Animation* anim, const AnimationProgress progress)
{
    (void)anim;
    const int32_t range = (int32_t)anim_to - (int32_t)anim_from;
    anim_value = anim_from + (int16_t)((range * (int32_t)progress) / ANIMATION_NORMALIZED_MAX);

    if (anim_kind == AnimScroll)
    {
        offset = anim_value;
    }
    else if (anim_kind == AnimDismiss)
    {
        dismiss_elapsed = (uint32_t)anim_value;
    }

    if (card_layer != NULL)
    {
        layer_mark_dirty(card_layer);
    }
}

static void complete_dismiss(void)
{
    if (dismiss_sequence != NULL)
    {
        gdraw_command_sequence_destroy(dismiss_sequence);
        dismiss_sequence = NULL;
    }
    const uint8_t bucket = dismiss_bucket;
    dismiss_bucket = 0;
    // Drops the notification from the store; detail_window_on_list_changed then moves to a neighbour.
    notification_store_hide(bucket);
}

static void animation_stopped(Animation* anim, const bool finished, void* context)
{
    (void)finished;
    (void)context;
    if (anim != animation)
    {
        return;
    }
    animation = NULL;

    // Whether it ran to completion or was cut short by a button press, land on the end state.
    const AnimKind kind = anim_kind;
    anim_kind = AnimNone;

    switch (kind)
    {
    case AnimScroll:
        offset = anim_to;
        break;
    case AnimSwapDown:
        settle_on(anim_other_bucket, 0);
        break;
    case AnimSwapUp:
        // The card that slid in ends with its top at y = anim_to - height - start offset.
        settle_on(anim_other_bucket, card_height(anim_other_bucket) + anim_start_offset - anim_to);
        break;
    case AnimDismiss:
        complete_dismiss();
        break;
    case AnimNone:
        break;
    }

    if (window != NULL)
    {
        refresh_decorations();
    }

    if (finished && hold_active && kind != AnimDismiss)
    {
        hold_step();
    }
}

static bool start_animation(const AnimKind kind, const int16_t from, const int16_t to, const uint32_t duration,
                            const AnimationCurve curve)
{
    finish_animation();

    anim_kind = kind;
    anim_from = from;
    anim_to = to;
    anim_value = from;

    static const AnimationImplementation implementation = {
        .update = animation_update,
    };

    animation = animation_create();
    if (animation == NULL)
    {
        anim_kind = AnimNone;
        return false;
    }
    animation_set_implementation(animation, &implementation);
    animation_set_duration(animation, duration);
    if (next_custom_curve != NULL)
    {
        animation_set_custom_curve(animation, next_custom_curve);
        next_custom_curve = NULL;
    }
    else
    {
        animation_set_curve(animation, curve);
    }
    animation_set_handlers(animation, (AnimationHandlers){.stopped = animation_stopped}, NULL);
    if (!animation_schedule(animation))
    {
        animation = NULL;
        anim_kind = AnimNone;
        return false;
    }
    refresh_decorations();
    return true;
}

static void finish_animation(void)
{
    if (animation != NULL)
    {
        // The stopped handler applies the final state.
        animation_unschedule(animation);
    }
    if (animation != NULL)
    {
        // Scheduling had already finished; treat as stopped.
        animation_stopped(animation, true, NULL);
    }
}

// ------------------------------------------------------------------------------------------------ card changes

static void cancel_actions_wait(void)
{
    actions_wanted = false;
    if (actions_timer != NULL)
    {
        app_timer_cancel(actions_timer);
        actions_timer = NULL;
    }
}

static void request_neighbors(void)
{
    const NotificationItem* next = neighbor(1);
    if (next != NULL)
    {
        notification_store_want_details(next->bucket_id, false);
    }
    const NotificationItem* previous = neighbor(-1);
    if (previous != NULL)
    {
        notification_store_want_details(previous->bucket_id, false);
    }
}

static void settle_on(const uint8_t bucket, int16_t new_offset)
{
    const bool changed = bucket != current_bucket;
    if (changed)
    {
        window_notification_action_list_hide();
        cancel_actions_wait();
    }

    current_bucket = bucket;
    last_known_index = notification_store_index_of(bucket);
    window_notification_data.currently_selected_bucket = bucket;
    notification_store_pin(bucket);

    const int16_t max = max_scroll();
    offset = new_offset < 0 ? 0 : (new_offset > max ? max : new_offset);
    swap_delay_remaining = MESSAGE_SWAP_DELAY;

    if (bucket != 0)
    {
        notification_store_want_details(bucket, true);
        request_neighbors();
        if (changed && callbacks != NULL && callbacks->card_shown != NULL)
        {
            callbacks->card_shown(bucket);
        }
    }
}

static uint32_t duration_for(const int16_t distance, const uint16_t speed_px_s)
{
    const int32_t d = distance < 0 ? -distance : distance;
    const uint32_t ms = (uint32_t)((d * 1000) / speed_px_s);
    return ms > 0 ? ms : 1;
}

/**
 * Cubic p(t) = a t^3 + b t^2 with p(0) = 0, p'(0) = 0 (starting from the bump at rest), p(1) = 1 and
 * p'(1) = s, the hold speed: quick in the middle and handing over to the scroll without a slowdown.
 */
static AnimationProgress hold_swap_curve(const AnimationProgress progress)
{
    const int64_t max = ANIMATION_NORMALIZED_MAX;
    const int64_t t = progress;
    const int64_t s = hold_swap_end_slope; // x1000
    const int64_t a = s - 2000;            // x1000
    const int64_t b = 3000 - s;            // x1000
    const int64_t t2 = (t * t) / max;
    const int64_t t3 = (t2 * t) / max;
    int64_t value = (a * t3 + b * t2) / 1000;
    if (value < 0)
    {
        value = 0;
    }
    return (AnimationProgress)(value > max ? max : value);
}

static uint32_t hold_swap_duration(const int16_t distance)
{
    // End slope in normalized units: speed * duration / distance.
    int32_t slope = distance > 0 ? (int32_t)HOLD_SPEED_PX_S * HOLD_SWAP_MS / distance : 0;
    hold_swap_end_slope = slope > 2500 ? 2500 : slope;
    next_custom_curve = hold_swap_curve;
    return HOLD_SWAP_MS;
}

/** speed_px_s == 0: the stock snappy swap. Otherwise an even, linear slide at that speed (press-and-hold). */
static bool swap_at(const int8_t direction, const bool to_top, const uint16_t speed_px_s)
{
    const NotificationItem* target = neighbor(direction);
    if (target == NULL)
    {
        return false;
    }

    anim_other_bucket = target->bucket_id;
    anim_start_offset = offset;
    window_notification_action_list_hide();

    if (direction > 0)
    {
        // Slide the current card (from wherever it is scrolled) up and out; the next card ends at the top.
        const int16_t height = card_height(current_bucket);
        return start_animation(AnimSwapDown, offset, height,
                               speed_px_s ? hold_swap_duration(height - offset) : SWAP_MS,
                               AnimationCurveEaseOut) ||
            (settle_on(target->bucket_id, 0), true);
    }

    // Swap up: previous card slides in from above. Normally it lands scrolled to its end with the current
    // card peeking below it (stock behaviour); a double click lands on its top instead.
    const int16_t target_height = card_height(target->bucket_id);
    int16_t distance;
    if (to_top)
    {
        distance = target_height + offset;
    }
    else
    {
        distance = PBL_DISPLAY_HEIGHT - PEEK_PX + offset;
    }
    if (!start_animation(AnimSwapUp, 0, distance, speed_px_s ? hold_swap_duration(distance) : SWAP_MS,
                         AnimationCurveEaseOut))
    {
        settle_on(target->bucket_id, to_top ? 0 : target_height - PBL_DISPLAY_HEIGHT + PEEK_PX);
    }
    return true;
}

static bool swap(const int8_t direction, const bool to_top)
{
    return swap_at(direction, to_top, 0);
}

static void scroll_to(const int16_t target, const bool repeating)
{
    if (target == offset)
    {
        return;
    }
    if (!start_animation(AnimScroll, offset, target, SCROLL_MS,
                         repeating ? AnimationCurveLinear : AnimationCurveEaseOut))
    {
        offset = target;
        refresh_decorations();
    }
}

static void handle_swap_attempt(const int8_t direction, const bool repeating)
{
    if (direction > 0 && current_is_partial())
    {
        // Reached the end of what has loaded so far. Don't skip past the rest of this message: hurry the text
        // along and stay put; the card simply grows when it lands.
        notification_store_want_details(current_bucket, true);
        return;
    }

    if (!repeating || swap_delay_remaining <= 0)
    {
        if (!swap(direction, false))
        {
            // Nothing there: settle firmly at the edge.
            scroll_to(direction > 0 ? max_scroll() : 0, false);
        }
        swap_delay_remaining = MESSAGE_SWAP_DELAY;
    }
    else
    {
        swap_delay_remaining--;
    }
}

static void attempt_scroll(const int8_t direction, const bool repeating)
{
    // Never drop a press because something is still moving: jump to the end of it and act on the result.
    finish_animation();
    if (current_bucket == 0)
    {
        return;
    }

    const int16_t max = max_scroll();
    if (direction < 0)
    {
        if (offset == 0)
        {
            handle_swap_attempt(-1, repeating);
            return;
        }
        if (offset - FUDGE_PX < SCROLL_PX)
        {
            scroll_to(0, repeating);
        }
        else
        {
            scroll_to(offset - (repeating ? REPEATING_SCROLL_PX : SCROLL_PX), repeating);
        }
        return;
    }

    if (offset >= max)
    {
        handle_swap_attempt(1, repeating);
        return;
    }

    if (offset == 0 && repeating && swap_delay_remaining > 0)
    {
        // Pause at the top of a notification while the button is held.
        swap_delay_remaining--;
        return;
    }

    swap_delay_remaining = MESSAGE_SWAP_DELAY;
    if (max - offset - FUDGE_PX < SCROLL_PX)
    {
        scroll_to(max, repeating);
    }
    else if (offset == 0)
    {
        scroll_to(INITIAL_SCROLL_PX, repeating);
    }
    else
    {
        scroll_to(offset + (repeating ? REPEATING_SCROLL_PX : SCROLL_PX), repeating);
    }
}

// ------------------------------------------------------------------------------------------------ press and hold

static void stop_scroll_in_place(void)
{
    if (anim_kind == AnimScroll && animation != NULL)
    {
        anim_to = offset;
        finish_animation();
    }
}

static void hold_timer_fired(void* context)
{
    (void)context;
    hold_timer = NULL;
    hold_step();
}

static void hold_wait(const uint32_t ms)
{
    if (hold_timer != NULL)
    {
        app_timer_cancel(hold_timer);
    }
    hold_timer = app_timer_register(ms, hold_timer_fired, NULL);
}

static void hold_begin(void* context)
{
    (void)context;
    hold_timer = NULL;
    if (hold_direction == 0 || window == NULL || window_notification_data.menu_displayed)
    {
        return;
    }
    hold_active = true;
    hold_bumped = false;
    // Remember where the press was headed so a press that only just turned into a hold still travels as far.
    hold_press_target = anim_kind == AnimScroll && animation != NULL ? anim_to : offset;
    hold_step();
}

static void hold_step(void)
{
    if (!hold_active || hold_in_step || window == NULL || current_bucket == 0)
    {
        return;
    }
    if (window_notification_data.menu_displayed)
    {
        hold_cancel();
        return;
    }
    if (animation != NULL && anim_kind != AnimScroll)
    {
        // A swap or dismiss is playing; carry on when it lands.
        return;
    }

    hold_in_step = true;
    // Pick up from wherever the press animation has got to, at a steady speed.
    stop_scroll_in_place();

    const int8_t direction = hold_direction;
    const int16_t end = direction > 0 ? max_scroll() : 0;
    if (!current_is_partial())
    {
        // Have the next message's full text ready by the time we get there, so it scrolls on without waiting.
        const NotificationItem* upcoming = neighbor(direction);
        if (upcoming != NULL)
        {
            notification_store_want_details(upcoming->bucket_id, true);
        }
    }
    if (offset != end)
    {
        hold_bumped = false;
        start_animation(AnimScroll, offset, end, duration_for(end - offset, HOLD_SPEED_PX_S), AnimationCurveLinear);
    }
    else if (direction > 0 && current_is_partial())
    {
        // The rest of this message is on its way: wait for it, then keep scrolling.
        notification_store_want_details(current_bucket, true);
        hold_wait(HOLD_RETRY_MS);
    }
    else if (!hold_bumped)
    {
        // Bump: rest at the end of the message for a moment, so it can be read or acted on.
        hold_bumped = true;
        hold_wait(HOLD_BUMP_MS);
    }
    else
    {
        hold_bumped = false;
        swap_at(direction, false, HOLD_SPEED_PX_S);
    }
    hold_in_step = false;
}

static void hold_cancel(void)
{
    hold_active = false;
    hold_direction = 0;
    hold_bumped = false;
    if (hold_timer != NULL)
    {
        app_timer_cancel(hold_timer);
        hold_timer = NULL;
    }
}

static void hold_release(void)
{
    const bool was_active = hold_active;
    const int8_t direction = hold_direction;
    hold_cancel();
    if (!was_active || anim_kind != AnimScroll || animation == NULL)
    {
        // A plain press, or a swap that will finish on its own.
        return;
    }

    stop_scroll_in_place();
    int16_t target = offset + direction * HOLD_RELEASE_PX;
    if ((direction > 0 && target < hold_press_target) || (direction < 0 && target > hold_press_target))
    {
        target = hold_press_target;
    }
    const int16_t max = max_scroll();
    target = target < 0 ? 0 : (target > max ? max : target);
    if (target != offset)
    {
        start_animation(AnimScroll, offset, target, HOLD_RELEASE_MS, AnimationCurveEaseOut);
    }
}

// ------------------------------------------------------------------------------------------------ actions

static bool copy_actions_for_current(void)
{
    const NotificationDetails* details = notification_store_details(current_bucket);
    if (details == NULL)
    {
        return false;
    }

    window_notification_data.currently_selected_bucket = current_bucket;
    window_notification_data.num_actions = details->num_actions;
    if (details->num_actions > 0)
    {
        memcpy(window_notification_data.actions, details->actions, sizeof(Action) * details->num_actions);
    }
    return true;
}

static void on_actions_timeout(void* context)
{
    (void)context;
    actions_timer = NULL;
    if (actions_wanted)
    {
        actions_wanted = false;
        vibes_double_pulse();
    }
}

void detail_window_open_actions(void)
{
    if (current_bucket == 0 || anim_kind == AnimDismiss)
    {
        return;
    }

    if (!copy_actions_for_current())
    {
        // Actions are not here yet. Ask for them now and open the menu as soon as they arrive.
        actions_wanted = true;
        notification_store_want_details(current_bucket, true);
        if (actions_timer == NULL)
        {
            actions_timer = app_timer_register(ACTIONS_WAIT_MS, on_actions_timeout, NULL);
        }
        return;
    }

    cancel_actions_wait();
    window_notification_data.currently_displayed_menu_id = 0;
    window_notification_action_list_show();
}

// ------------------------------------------------------------------------------------------------ input

static void notify_interaction(void)
{
    if (callbacks != NULL && callbacks->interacted != NULL)
    {
        callbacks->interacted();
    }
}

static int8_t direction_of(ClickRecognizerRef recognizer)
{
    return click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP ? -1 : 1;
}

static void raw_scroll_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)context;
    // Only the first press of a click pattern scrolls immediately; the multi-click handler owns the rest.
    if (click_number_of_clicks_counted(recognizer) >= 1)
    {
        return;
    }

    notify_interaction();
    if (window_notification_data.menu_displayed)
    {
        if (direction_of(recognizer) < 0)
        {
            window_notification_action_list_move_up();
        }
        else
        {
            window_notification_action_list_move_down();
        }
        return;
    }
    hold_cancel();
    attempt_scroll(direction_of(recognizer), false);
    hold_direction = direction_of(recognizer);
    hold_timer = app_timer_register(HOLD_START_MS, hold_begin, NULL);
}

static void raw_scroll_up_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    hold_release();
}

static void up_double_click_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    if (window_notification_data.menu_displayed)
    {
        return;
    }
    finish_animation();
    notify_interaction();

    // PebbleOS: if the first click already swapped us onto this card, go to its top; otherwise jump to
    // the top of the previous notification.
    if (card_height(current_bucket) - PBL_DISPLAY_HEIGHT - offset == -PEEK_PX)
    {
        scroll_to(0, false);
    }
    else if (!swap(-1, true))
    {
        scroll_to(0, false);
    }
}

static void down_double_click_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    if (window_notification_data.menu_displayed)
    {
        return;
    }
    finish_animation();
    if (offset != 0 && !current_is_partial())
    {
        notify_interaction();
        swap(1, true);
    }
}

static void select_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    notify_interaction();

    if (window_notification_data.menu_displayed)
    {
        window_notification_action_select();
        return;
    }
    finish_animation();
    detail_window_open_actions();
}

static void back_handler(ClickRecognizerRef recognizer, void* context)
{
    (void)recognizer;
    (void)context;
    notify_interaction();

    if (window_notification_data.menu_displayed)
    {
        window_notification_action_list_hide();
        return;
    }
    cancel_actions_wait();
    if (callbacks != NULL && callbacks->back_pressed != NULL)
    {
        callbacks->back_pressed();
    }
}

static void click_config_provider(void* context)
{
    window_raw_click_subscribe(BUTTON_ID_UP, raw_scroll_handler, raw_scroll_up_handler, context);
    window_multi_click_subscribe(BUTTON_ID_UP, 2, 2, 100, false, up_double_click_handler);

    window_raw_click_subscribe(BUTTON_ID_DOWN, raw_scroll_handler, raw_scroll_up_handler, context);
    window_multi_click_subscribe(BUTTON_ID_DOWN, 2, 2, 100, false, down_double_click_handler);

    window_single_click_subscribe(BUTTON_ID_SELECT, select_handler);
    window_single_click_subscribe(BUTTON_ID_BACK, back_handler);
}

// ------------------------------------------------------------------------------------------------ touch

// Momentum: how quickly a flick slows down, in px/s².
#define FLING_DECELERATION 2600
#define FLING_MIN_MS 180
#define FLING_MAX_MS 900
// How far past the top/bottom of a card you pull before letting go switches notification.
#define PULL_TO_SWAP_PX 56
#define RUBBER_BAND_DIVISOR 3

static TouchGesture touch;
static int16_t drag_base_offset;
static bool dragging;

static void stop_motion_for_touch(void)
{
    if (anim_kind == AnimScroll && animation != NULL)
    {
        // Catch a scrolling/flinging card where it is right now.
        anim_to = offset;
        finish_animation();
    }
    else
    {
        finish_animation();
    }
}

static int16_t rubber_banded(const int16_t raw, const int16_t max)
{
    if (raw < 0)
    {
        return raw / RUBBER_BAND_DIVISOR;
    }
    if (raw > max)
    {
        return max + (raw - max) / RUBBER_BAND_DIVISOR;
    }
    return raw;
}

static void touch_down(void* context)
{
    (void)context;
    notify_interaction();
    hold_cancel();
    stop_motion_for_touch();
    drag_base_offset = offset;
    dragging = false;
}

static void touch_drag_moved(const int16_t dy, void* context)
{
    (void)context;
    if (current_bucket == 0 || anim_kind != AnimNone)
    {
        return;
    }
    if (!dragging)
    {
        dragging = true;
        window_notification_action_list_hide();
    }
    offset = rubber_banded(drag_base_offset - dy, max_scroll());
    refresh_decorations();
}

static void touch_drag_ended(const int16_t dy, const int32_t velocity, void* context)
{
    (void)context;
    if (!dragging || current_bucket == 0)
    {
        dragging = false;
        return;
    }
    dragging = false;

    const int16_t max = max_scroll();
    const int16_t raw = drag_base_offset - dy;

    if (raw < -PULL_TO_SWAP_PX && swap(-1, false))
    {
        return;
    }
    if (raw > max + PULL_TO_SWAP_PX)
    {
        if (current_is_partial())
        {
            notification_store_want_details(current_bucket, true);
        }
        else if (swap(1, true))
        {
            return;
        }
    }

    // Momentum: keep travelling in the flick direction and ease to a stop. Finger moving down (positive
    // velocity) scrolls the content back toward the top.
    const int32_t speed = velocity < 0 ? -velocity : velocity;
    int32_t travel = (speed * speed) / (2 * FLING_DECELERATION);
    if (velocity > 0)
    {
        travel = -travel;
    }
    int32_t target = (int32_t)offset + travel;
    if (target < 0)
    {
        target = 0;
    }
    if (target > max)
    {
        target = max;
    }

    uint32_t duration = (uint32_t)((speed * 1000) / FLING_DECELERATION);
    if (duration < FLING_MIN_MS)
    {
        duration = FLING_MIN_MS;
    }
    if (duration > FLING_MAX_MS)
    {
        duration = FLING_MAX_MS;
    }

    if (target != offset &&
        !start_animation(AnimScroll, offset, (int16_t)target, duration, AnimationCurveEaseOut))
    {
        offset = (int16_t)target;
    }
    refresh_decorations();
}

// PebbleOS: a tap on a notification does nothing (it only stops a moving card, see touch_down) so the
// actions never open by accident; swiping right-to-left opens them.
static void touch_swipe_select(void* context)
{
    (void)context;
    if (!window_notification_data.menu_displayed)
    {
        detail_window_open_actions();
    }
}

static void touch_swipe_back(void* context)
{
    (void)context;
    if (callbacks != NULL && callbacks->back_pressed != NULL)
    {
        callbacks->back_pressed();
    }
}

static void touch_edge_swipe_up(void* context)
{
    (void)context;
    // Flick from the bottom edge: straight to the top of the next notification.
    swap(1, true);
}

static const TouchGestureHandlers touch_handlers = {
    .touch_down = touch_down,
    .tap = NULL,
    .drag_moved = touch_drag_moved,
    .drag_ended = touch_drag_ended,
    .swipe_back = touch_swipe_back,
    .swipe_select = touch_swipe_select,
    .edge_swipe_up = touch_edge_swipe_up,
};

bool detail_window_handle_touch(const TouchEvent* event)
{
    if (window == NULL || window_stack_get_top_window() != window || window_notification_data.menu_displayed)
    {
        return false;
    }
    touch_gesture_feed(&touch, event, &touch_handlers, NULL);
    return true;
}

// ------------------------------------------------------------------------------------------------ window

static void window_load(Window* w)
{
    Layer* root = window_get_root_layer(w);
    const GRect bounds = layer_get_bounds(root);

    card_layer = layer_create(bounds);
    layer_set_update_proc(card_layer, card_layer_update);
    layer_add_child(root, card_layer);

    action_dot_layer = layer_create(bounds);
    layer_set_update_proc(action_dot_layer, action_dot_update);
    layer_add_child(root, action_dot_layer);

    refresh_decorations();
}

static void window_unload(Window* w)
{
    window_notification_action_list_hide();
    cancel_actions_wait();
    hold_cancel();

    Animation* running = animation;
    animation = NULL;
    anim_kind = AnimNone;
    if (running != NULL)
    {
        animation_unschedule(running);
    }
    if (dismiss_sequence != NULL)
    {
        gdraw_command_sequence_destroy(dismiss_sequence);
        dismiss_sequence = NULL;
    }
    dismiss_bucket = 0;

    layer_destroy(action_dot_layer);
    layer_destroy(card_layer);
    action_dot_layer = NULL;
    card_layer = NULL;
    touch_gesture_reset(&touch);
    dragging = false;

    window_destroy(w);
    window = NULL;
    current_bucket = 0;
    window_notification_data.detail_open = false;
    notification_store_pin(0);
}

static void window_appear(Window* w)
{
    (void)w;
    // Coming back from the action menu or dictation: re-show the current card's state.
    if (card_layer != NULL)
    {
        refresh_decorations();
    }
}

void detail_window_set_callbacks(const DetailCallbacks* new_callbacks)
{
    callbacks = new_callbacks;
}

void detail_window_open(const uint8_t bucket_id, const bool animated)
{
    if (window != NULL)
    {
        detail_window_show_bucket(bucket_id);
        return;
    }

    window = window_create();
    if (window == NULL)
    {
        return;
    }

    invalidate_metrics();
    current_bucket = 0;
    window_notification_data.detail_open = true;
    window_set_background_color(window, GColorWhite);
    window_set_click_config_provider(window, click_config_provider);
    // Touch is handled by this window itself (finger-tracked scrolling), not by the system's button emulation.
    window_set_touch_bridge_disabled(window, true);
    window_set_window_handlers(window, (WindowHandlers){
        .load = window_load,
        .unload = window_unload,
        .appear = window_appear,
    });

    settle_on(bucket_id, 0);
    window_stack_push(window, animated);
    refresh_decorations();
}

void detail_window_close(const bool animated)
{
    if (window != NULL)
    {
        window_stack_remove(window, animated);
    }
}

bool detail_window_is_open(void)
{
    return window != NULL;
}

uint8_t detail_window_current_bucket(void)
{
    return window != NULL ? current_bucket : 0;
}

bool detail_window_is_animating(void)
{
    return animation != NULL;
}

void detail_window_show_bucket(const uint8_t bucket_id)
{
    if (window == NULL || bucket_id == current_bucket)
    {
        if (window != NULL && offset != 0)
        {
            finish_animation();
            scroll_to(0, false);
        }
        return;
    }

    finish_animation();
    const int16_t target_index = notification_store_index_of(bucket_id);
    const int16_t index = current_index();
    if (target_index < 0)
    {
        return;
    }

    if (index >= 0 && target_index == index - 1)
    {
        swap(-1, true);
        return;
    }
    if (index >= 0 && target_index == index + 1)
    {
        swap(1, true);
        return;
    }

    // Not a neighbour: slide the target in from above (new notifications live at the top).
    anim_other_bucket = bucket_id;
    anim_start_offset = offset;
    window_notification_action_list_hide();
    if (!start_animation(AnimSwapUp, 0, card_height(bucket_id) + offset, SWAP_MS, AnimationCurveEaseOut))
    {
        settle_on(bucket_id, 0);
        refresh_decorations();
    }
}

void detail_window_play_dismissed(const uint8_t bucket_id)
{
    if (window == NULL || bucket_id != current_bucket)
    {
        notification_store_hide(bucket_id);
        return;
    }

    finish_animation();
    window_notification_action_list_hide();
    dismiss_sequence = gdraw_command_sequence_create_with_resource(RESOURCE_ID_PEBBLEOS_RESULT_DISMISSED_LARGE);
    uint32_t duration = DISMISS_FALLBACK_MS;
    if (dismiss_sequence != NULL)
    {
        const uint32_t total = gdraw_command_sequence_get_total_duration(dismiss_sequence);
        if (total > 0)
        {
            duration = total;
        }
    }
    dismiss_bucket = bucket_id;
    dismiss_elapsed = 0;
    if (!start_animation(AnimDismiss, 0, (int16_t)(duration > 30000 ? 30000 : duration), duration,
                         AnimationCurveLinear))
    {
        complete_dismiss();
    }
}

void detail_window_on_list_changed(const uint8_t* changed_buckets, const uint8_t changed_count)
{
    if (window == NULL)
    {
        return;
    }

    invalidate_metrics();
    if (anim_kind == AnimDismiss && notification_store_index_of(dismiss_bucket) >= 0)
    {
        // Let the dismiss animation finish; it hides the card itself.
        return;
    }

    // Land any running animation first. This can itself change the list (a finished dismiss hides its
    // card), so everything below reads the state fresh.
    finish_animation();
    invalidate_metrics();

    if (notification_store_count() == 0)
    {
        current_bucket = 0;
        window_notification_action_list_hide();
        cancel_actions_wait();
        if (callbacks != NULL && callbacks->emptied != NULL)
        {
            callbacks->emptied();
        }
        return;
    }

    const uint8_t previous_bucket = current_bucket;
    if (notification_store_index_of(previous_bucket) < 0)
    {
        // The card on screen went away (dismissed on the phone or here). Show whatever slid into its place.
        int16_t replacement = last_known_index;
        if (replacement >= notification_store_count())
        {
            replacement = notification_store_count() - 1;
        }
        if (replacement < 0)
        {
            replacement = 0;
        }
        settle_on(notification_store_item(replacement)->bucket_id, 0);
    }
    else
    {
        bool content_changed = false;
        for (uint8_t i = 0; i < changed_count; i++)
        {
            if (changed_buckets[i] == previous_bucket)
            {
                content_changed = true;
            }
        }
        // Keep the reading position, clamped to the (possibly new) card size.
        settle_on(previous_bucket, offset);
        if (content_changed)
        {
            notification_store_want_details(previous_bucket, true);
        }
    }

    refresh_decorations();
}

void detail_window_on_style_changed(void)
{
    invalidate_metrics();
    if (window == NULL)
    {
        return;
    }
    finish_animation();
    const int16_t max = max_scroll();
    if (offset > max)
    {
        offset = max;
    }
    refresh_decorations();
}

void detail_window_on_details_changed(const uint8_t bucket_id)
{
    if (window == NULL)
    {
        return;
    }

    invalidate_metrics_for(bucket_id);
    if (bucket_id == current_bucket)
    {
        const int16_t max = max_scroll();
        if (offset > max)
        {
            offset = max;
        }
        if (actions_wanted)
        {
            detail_window_open_actions();
        }
    }
    refresh_decorations();
}

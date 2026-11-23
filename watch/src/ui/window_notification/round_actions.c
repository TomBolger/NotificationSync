#include "round_actions.h"
#include "../theme.h"

#include "card.h"
#include "touch_gesture.h"

#if PBL_ROUND

#define CENTER_X (PBL_DISPLAY_WIDTH / 2)
#define CENTER_Y (PBL_DISPLAY_HEIGHT / 2)
#define SCREEN_RADIUS (PBL_DISPLAY_WIDTH / 2)

// Where the banner settles: the very same shape turned a quarter round, so it hangs off the left edge exactly
// like it hangs off the top (same big circle, same depth, icon in the same spot relative to it).
#define BANNER_DEPTH (STATUS_BAR_LAYER_HEIGHT + CARD_BANNER_HEIGHT - 1)
#define BUBBLE_RADIUS CARD_ROUND_BANNER_RADIUS
#define BUBBLE_CENTER_X (BANNER_DEPTH - CARD_ROUND_BANNER_RADIUS)
#define BUBBLE_ICON_INSET \
    (STATUS_BAR_LAYER_HEIGHT + CARD_ICON_UPPER_PADDING + CARD_ICON_HEIGHT / 2 - BUBBLE_CENTER_X)
// When the banner is scrolled out of sight, the motion starts from a small circle just above the screen.
#define HIDDEN_START_RADIUS 14
#define HIDDEN_START_Y (-HIDDEN_START_RADIUS - 2)

// Rows follow the text size setting (38 tall at the default size).
#define ROW_HEIGHT (card_scaled_points(24) + 14)
#define ROW_PILL_HEIGHT (ROW_HEIGHT - 6)
// The list starts with its first row two rows above the centre; the selection walks down to the centre, the list
// then scrolls under it, and stops with the last row two rows below the centre, where the selection walks down again.
#define EDGE_ROWS 2
#define TEXT_LEFT (BUBBLE_CENTER_X + BUBBLE_RADIUS + 8)
#define TEXT_RIGHT (PBL_DISPLAY_WIDTH - 10)
#define ROW_EDGE_GAP 8

#define OPEN_MS 320
#define GLIDE_MS 150

static const RoundActionsCallbacks* callbacks;
static Layer* layer;
static GColor color;
static uint8_t icon_id;

// Starting circle (the banner as it was on screen) in polar form around the screen centre.
static int32_t start_angle;
static int32_t start_distance;
static int32_t start_radius;
static int32_t start_icon_inset;
static int16_t banner_cover_bottom;  // rows 0..this are the old banner, hidden as soon as the motion starts

static Animation* motion;
static int32_t progress;  // 0..1000, linear in time; the easing is applied when drawing
static int32_t motion_from;
static int32_t motion_to;
static bool closing;

static Animation* glide;
static int32_t list_pos;  // scroll position in pixels
static int32_t glide_from;
static int32_t glide_to;

static TouchGesture touch;
static int8_t drag_rows_applied;

/** Ease out with a small overshoot (PebbleOS-style bounce). Played backwards, it is the exact reverse. */
static int32_t eased(const int32_t t)
{
    // 1 + (s + 1)(t - 1)^3 + s (t - 1)^2 with s = 1.2, in thousandths.
    const int64_t u = t - 1000;
    const int64_t u2 = u * u / 1000;
    const int64_t u3 = u2 * u / 1000;
    return (int32_t)(1000 + (2200 * u3) / 1000 + (1200 * u2) / 1000);
}

static int32_t lerp(const int32_t from, const int32_t to, const int32_t e)
{
    return from + (to - from) * e / 1000;
}

static int32_t isqrt(int32_t value)
{
    if (value <= 0)
    {
        return 0;
    }
    int32_t root = 0;
    int32_t bit = 1 << 30;
    while (bit > value)
    {
        bit >>= 2;
    }
    while (bit != 0)
    {
        if (value >= root + bit)
        {
            value -= root + bit;
            root = (root >> 1) + bit;
        }
        else
        {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static void draw_rows(GContext* ctx, const int16_t shift)
{
    const uint16_t count = callbacks->count();
    const uint16_t selected = callbacks->selected();
    for (uint16_t i = 0; i < count; i++)
    {
        const int32_t center_y = CENTER_Y + ((int32_t)i - EDGE_ROWS) * ROW_HEIGHT - list_pos;
        if (center_y + ROW_HEIGHT / 2 < 0 || center_y - ROW_HEIGHT / 2 > PBL_DISPLAY_HEIGHT)
        {
            continue;
        }
        // Keep each row inside the round screen at its height.
        const int32_t dy = center_y - CENTER_Y;
        const int32_t half_chord = isqrt(SCREEN_RADIUS * SCREEN_RADIUS - dy * dy);
        int32_t left = CENTER_X - half_chord + ROW_EDGE_GAP;
        int32_t right = CENTER_X + half_chord - ROW_EDGE_GAP;
        left = left > TEXT_LEFT ? left : TEXT_LEFT;
        right = right < TEXT_RIGHT ? right : TEXT_RIGHT;
        if (right - left < 60)
        {
            continue;
        }

        const char* text = callbacks->text(i);
        int16_t points = card_scaled_points(24);
        GFont font = card_scaled_font(24, true);
        const GRect measure_box = GRect(0, 0, 400, ROW_PILL_HEIGHT);
        const GSize size = graphics_text_layout_get_content_size(text, font, measure_box,
                                                                 GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
        if (size.w > right - left - 12)
        {
            // Too long: a step smaller.
            points = card_scaled_points(18);
            font = card_scaled_font(18, true);
        }

        const GRect pill = GRect(left + shift, center_y - ROW_PILL_HEIGHT / 2, right - left, ROW_PILL_HEIGHT);
        if (i == selected)
        {
            graphics_context_set_fill_color(ctx, color);
            graphics_fill_rect(ctx, pill, ROW_PILL_HEIGHT / 2, GCornersAll);
            graphics_context_set_text_color(ctx, GColorWhite);
        }
        else
        {
            graphics_context_set_text_color(ctx, theme_fg());
        }
        // Gothic draws its letters about a quarter of its size below the top of the box.
        graphics_draw_text(ctx, text, font,
                           GRect(pill.origin.x + 8, (int16_t)(center_y - points * 7 / 10), pill.size.w - 16,
                                 points + 8),
                           GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    }
}

static void layer_update(Layer* l, GContext* ctx)
{
    const GRect bounds = layer_get_bounds(l);
    const int32_t e = eased(progress);

    graphics_context_set_fill_color(ctx, theme_bg());
    // The actions come in from the right behind a sweeping edge; the card stays visible to its left.
    int32_t edge = PBL_DISPLAY_WIDTH - PBL_DISPLAY_WIDTH * e / 1000;
    if (edge < 0)
    {
        edge = 0;
    }
    if (progress >= 1000)
    {
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    }
    else
    {
        if (banner_cover_bottom >= 0)
        {
            graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, banner_cover_bottom + 1), 0, GCornerNone);
        }
        if (edge < bounds.size.w)
        {
            graphics_fill_rect(ctx, GRect(edge, 0, bounds.size.w - edge, bounds.size.h), 0, GCornerNone);
        }
    }

    draw_rows(ctx, (int16_t)(PBL_DISPLAY_WIDTH * (1000 - e) / 1000));

    // The banner, swinging around the screen centre from the top to the left edge.
    const int32_t angle = lerp(start_angle, TRIG_MAX_ANGLE / 2, e);
    const int32_t distance = lerp(start_distance, CENTER_X - BUBBLE_CENTER_X, e);
    int32_t radius = lerp(start_radius, BUBBLE_RADIUS, e);
    if (radius < 1)
    {
        radius = 1;
    }
    const int32_t cos_a = cos_lookup(angle);
    const int32_t sin_a = sin_lookup(angle);
    const GPoint center = GPoint(CENTER_X + cos_a * distance / TRIG_MAX_RATIO,
                                 CENTER_Y + sin_a * distance / TRIG_MAX_RATIO);
    graphics_context_set_fill_color(ctx, color);
    card_fill_circle_rows(ctx, center, (int16_t)radius, (int16_t)(center.y - radius),
                          (int16_t)(center.y + radius + 1));

    // The icon sits on the side of the circle facing the screen centre, so it turns with it.
    const int32_t inset = lerp(start_icon_inset, BUBBLE_ICON_INSET, e);
    const GPoint icon_center = GPoint(center.x - cos_a * inset / TRIG_MAX_RATIO,
                                      center.y - sin_a * inset / TRIG_MAX_RATIO);
    int32_t scale = (radius - 6) * 1000 / 24;
    scale = scale > 1000 ? 1000 : scale;
    if (scale > 0)
    {
        card_draw_icon_scaled(ctx, icon_id, icon_center, (int16_t)scale);
    }
}

// ------------------------------------------------------------------------------------------------ motion

static void destroy_layer(void)
{
    if (glide != NULL)
    {
        Animation* g = glide;
        glide = NULL;
        animation_unschedule(g);
    }
    if (layer != NULL)
    {
        layer_destroy(layer);
        layer = NULL;
    }
    touch_gesture_reset(&touch);
    const RoundActionsCallbacks* c = callbacks;
    if (c != NULL && c->closed != NULL)
    {
        c->closed();
    }
}

static void motion_update(Animation* anim, const AnimationProgress p)
{
    (void)anim;
    progress = motion_from + (motion_to - motion_from) * (int32_t)p / ANIMATION_NORMALIZED_MAX;
    if (layer != NULL)
    {
        layer_mark_dirty(layer);
    }
}

static void motion_stopped(Animation* anim, const bool finished, void* context)
{
    (void)finished;
    (void)context;
    if (anim != motion)
    {
        return;
    }
    motion = NULL;
    progress = motion_to;
    if (closing)
    {
        closing = false;
        destroy_layer();
    }
    else if (layer != NULL)
    {
        layer_mark_dirty(layer);
    }
}

static void run_motion(const int32_t to)
{
    if (motion != NULL)
    {
        Animation* old = motion;
        motion = NULL;
        animation_unschedule(old);
    }
    static const AnimationImplementation implementation = {.update = motion_update};
    motion_from = progress;
    motion_to = to;
    const int32_t distance = to > progress ? to - progress : progress - to;
    motion = animation_create();
    if (motion != NULL)
    {
        animation_set_implementation(motion, &implementation);
        animation_set_duration(motion, (uint32_t)(OPEN_MS * distance / 1000) + 1);
        animation_set_curve(motion, AnimationCurveLinear);
        animation_set_handlers(motion, (AnimationHandlers){.stopped = motion_stopped}, NULL);
        if (animation_schedule(motion))
        {
            return;
        }
        motion = NULL;
    }
    // No animation: jump to the end.
    progress = to;
    motion_to = to;
    if (closing)
    {
        closing = false;
        destroy_layer();
    }
}

/** Scroll position for the selected row (see EDGE_ROWS). */
static int32_t scroll_for_selection(void)
{
    const int32_t row = ROW_HEIGHT;
    const int32_t count = callbacks->count();
    int32_t max_scroll = (count - 1 - 2 * EDGE_ROWS) * row;
    max_scroll = max_scroll > 0 ? max_scroll : 0;
    int32_t target = ((int32_t)callbacks->selected() - EDGE_ROWS) * row;
    target = target < 0 ? 0 : target;
    return target > max_scroll ? max_scroll : target;
}

static void glide_update(Animation* anim, const AnimationProgress p)
{
    (void)anim;
    list_pos = glide_from + (glide_to - glide_from) * (int32_t)p / ANIMATION_NORMALIZED_MAX;
    if (layer != NULL)
    {
        layer_mark_dirty(layer);
    }
}

static void glide_stopped(Animation* anim, const bool finished, void* context)
{
    (void)finished;
    (void)context;
    if (anim == glide)
    {
        glide = NULL;
        list_pos = glide_to;
    }
}

void round_actions_selection_changed(const bool animated)
{
    if (layer == NULL)
    {
        return;
    }
    const int32_t target = scroll_for_selection();
    if (glide != NULL)
    {
        Animation* old = glide;
        glide = NULL;
        animation_unschedule(old);
    }
    if (animated && target != list_pos)
    {
        static const AnimationImplementation implementation = {.update = glide_update};
        glide_from = list_pos;
        glide_to = target;
        glide = animation_create();
        if (glide != NULL)
        {
            animation_set_implementation(glide, &implementation);
            animation_set_duration(glide, GLIDE_MS);
            animation_set_curve(glide, AnimationCurveEaseOut);
            animation_set_handlers(glide, (AnimationHandlers){.stopped = glide_stopped}, NULL);
            if (animation_schedule(glide))
            {
                layer_mark_dirty(layer);
                return;
            }
            glide = NULL;
        }
    }
    list_pos = target;
    layer_mark_dirty(layer);
}

// ------------------------------------------------------------------------------------------------ open / close

Layer* round_actions_open(Window* window, const GColor banner_color, const uint8_t banner_icon,
                          const int16_t banner_bottom, const RoundActionsCallbacks* new_callbacks)
{
    if (layer != NULL)
    {
        // Already up (or on its way out): bring it back.
        closing = false;
        run_motion(1000);
        round_actions_selection_changed(false);
        return layer;
    }

    Layer* root = window_get_root_layer(window);
    layer = layer_create(layer_get_bounds(root));
    if (layer == NULL)
    {
        return NULL;
    }
    callbacks = new_callbacks;
    color = banner_color;
    icon_id = banner_icon;

    int32_t start_x = CENTER_X;
    int32_t start_y;
    if (banner_bottom > 4)
    {
        // The banner as it is on screen: a big circle whose lower edge is the banner's curve.
        start_radius = CARD_ROUND_BANNER_RADIUS;
        start_y = banner_bottom - CARD_ROUND_BANNER_RADIUS;
        const int32_t icon_y = banner_bottom - (STATUS_BAR_LAYER_HEIGHT + CARD_BANNER_HEIGHT - 1) +
            STATUS_BAR_LAYER_HEIGHT + CARD_ICON_UPPER_PADDING + CARD_ICON_HEIGHT / 2;
        start_icon_inset = icon_y - start_y;
        banner_cover_bottom = banner_bottom;
    }
    else
    {
        // Scrolled away: grow in from just above the top edge.
        start_radius = HIDDEN_START_RADIUS;
        start_y = HIDDEN_START_Y;
        start_icon_inset = 0;
        banner_cover_bottom = -1;
    }
    start_distance = CENTER_Y - start_y;
    start_angle = atan2_lookup((int16_t)(start_y - CENTER_Y), (int16_t)(start_x - CENTER_X));
    if (start_angle < TRIG_MAX_ANGLE / 2)
    {
        start_angle += TRIG_MAX_ANGLE;
    }

    progress = 0;
    closing = false;
    list_pos = scroll_for_selection();
    drag_rows_applied = 0;
    touch_gesture_reset(&touch);
    layer_set_update_proc(layer, layer_update);
    layer_add_child(root, layer);
    run_motion(1000);
    return layer;
}

void round_actions_close(const bool animated)
{
    if (layer == NULL)
    {
        return;
    }
    closing = true;
    if (animated)
    {
        run_motion(0);
        return;
    }
    if (motion != NULL)
    {
        Animation* old = motion;
        motion = NULL;
        animation_unschedule(old);
    }
    closing = false;
    destroy_layer();
}

bool round_actions_accepts_input(void)
{
    return layer != NULL && !closing;
}

// ------------------------------------------------------------------------------------------------ touch

static void touch_down(void* context)
{
    (void)context;
    drag_rows_applied = 0;
}

static void touch_tap(const GPoint point, void* context)
{
    (void)context;
    if (!round_actions_accepts_input())
    {
        return;
    }
    const int32_t from_center = point.y - CENTER_Y + EDGE_ROWS * ROW_HEIGHT + list_pos + ROW_HEIGHT / 2;
    if (from_center < 0 || point.x < TEXT_LEFT - 10)
    {
        return;
    }
    const int32_t index = from_center / ROW_HEIGHT;
    if (index < callbacks->count() && callbacks->tapped != NULL)
    {
        callbacks->tapped((uint16_t)index);
    }
}

static void touch_drag(const int16_t dy, void* context)
{
    (void)context;
    if (!round_actions_accepts_input() || callbacks->move == NULL)
    {
        return;
    }
    // Finger up moves down the list, a row per row height.
    const int8_t rows = (int8_t)(-dy / ROW_HEIGHT);
    if (rows != drag_rows_applied)
    {
        callbacks->move((int8_t)(rows - drag_rows_applied));
        drag_rows_applied = rows;
    }
}

static void touch_back(void* context)
{
    (void)context;
    if (round_actions_accepts_input() && callbacks->back != NULL)
    {
        callbacks->back();
    }
}

static const TouchGestureHandlers touch_handlers = {
    .touch_down = touch_down,
    .tap = touch_tap,
    .drag_moved = touch_drag,
    .drag_ended = NULL,
    .swipe_back = touch_back,
    .swipe_select = NULL,
    .edge_swipe_up = NULL,
};

bool round_actions_handle_touch(const TouchEvent* event)
{
    if (layer == NULL)
    {
        return false;
    }
    touch_gesture_feed(&touch, event, &touch_handlers, NULL);
    return true;
}

#else

Layer* round_actions_open(Window* window, GColor banner_color, uint8_t banner_icon, int16_t banner_bottom,
                          const RoundActionsCallbacks* new_callbacks)
{
    (void)window;
    (void)banner_color;
    (void)banner_icon;
    (void)banner_bottom;
    (void)new_callbacks;
    return NULL;
}

void round_actions_close(bool animated)
{
    (void)animated;
}

bool round_actions_accepts_input(void)
{
    return false;
}

void round_actions_selection_changed(bool animated)
{
    (void)animated;
}

bool round_actions_handle_touch(const TouchEvent* event)
{
    (void)event;
    return false;
}

#endif

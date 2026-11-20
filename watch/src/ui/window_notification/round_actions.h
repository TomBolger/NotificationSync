#pragma once
#include <pebble.h>

/**
 * Round watches: the notification's actions, drawn by the app instead of the system ActionMenu.
 *
 * Opening swings the card's round banner down and around to the left edge, where it settles as a bubble holding
 * the notification's icon, while the actions slide in to the right of it. Closing plays exactly the same motion
 * backwards, from wherever the opening had got to.
 */
typedef struct
{
    uint16_t (*count)(void);
    const char* (*text)(uint16_t index);
    uint16_t (*selected)(void);
    /** The closing motion finished and the layer is gone. */
    void (*closed)(void);
    /** A tap chose this action. */
    void (*tapped)(uint16_t index);
    /** A drag moved the selection by this many rows. */
    void (*move)(int8_t rows);
    /** A back swipe. */
    void (*back)(void);
} RoundActionsCallbacks;

/**
 * banner_bottom: screen y of the lower edge of the card's banner as it is drawn now (it may be above the screen
 * when the card is scrolled down).
 */
Layer* round_actions_open(Window* window, GColor color, uint8_t icon_id, int16_t banner_bottom,
                          const RoundActionsCallbacks* callbacks);

/** animated: play the opening backwards first. Calls callbacks->closed once the layer is gone. */
void round_actions_close(bool animated);

/** Whether the menu is open and not on its way out. */
bool round_actions_accepts_input(void);

/** The selected action changed: glide the list to it. */
void round_actions_selection_changed(bool animated);

bool round_actions_handle_touch(const TouchEvent* event);

#pragma once
#include <pebble.h>

typedef struct
{
    /** User pressed back. */
    void (*back_pressed)(void);
    /** The detail window has nothing left to show (all notifications gone). */
    void (*emptied)(void);
    /** A notification card settled on screen. */
    void (*card_shown)(uint8_t bucket_id);
    /** Any button press. */
    void (*interacted)(void);
} DetailCallbacks;

void detail_window_set_callbacks(const DetailCallbacks* callbacks);

/** Push the detail window showing this notification. */
void detail_window_open(uint8_t bucket_id, bool animated);
void detail_window_close(bool animated);
bool detail_window_is_open(void);
/** Bucket id of the card currently on screen (0 if closed). */
uint8_t detail_window_current_bucket(void);

/** Bring a notification to the screen, animating from the current card. */
void detail_window_show_bucket(uint8_t bucket_id);

void detail_window_on_list_changed(const uint8_t* changed_buckets, uint8_t changed_count);
void detail_window_on_details_changed(uint8_t bucket_id);

/** Play the PebbleOS "Dismissed" animation for the current card, then drop it. */
void detail_window_play_dismissed(uint8_t bucket_id);

/** Open the action menu once actions for the current card are available. */
void detail_window_open_actions(void);

bool detail_window_is_animating(void);

/** Text size or another layout setting changed: re-measure every card. */
void detail_window_on_style_changed(void);

/** A notification's photo arrived (or turned out not to exist): redraw. */
void detail_window_on_image_changed(uint8_t bucket_id);

/** Feed a raw touch event. Returns false when the detail window is not the one on screen. */
bool detail_window_handle_touch(const TouchEvent* event);

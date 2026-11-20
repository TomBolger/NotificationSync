#pragma once
#include "pebble.h"

void window_notification_action_list_init(const Window* window);
void window_notification_action_list_deinit();

void window_notification_action_list_show();
void window_notification_action_list_hide();
/** Closes without the closing animation (the window is going away). */
void window_notification_action_list_hide_now();
bool window_notification_action_list_handle_touch(const TouchEvent* event);

void window_notification_action_list_move_up();
void window_notification_action_list_move_down();
void window_notification_action_select();
void window_notification_action_list_receive_submenu(const uint8_t* data, size_t data_size);

/** The notification whose Dismiss is on its way to the phone, or 0. */
uint8_t window_notification_action_list_pending_dismiss(void);

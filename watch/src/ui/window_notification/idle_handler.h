#pragma once

#include "pebble.h"

extern bool idle_handler_has_user_interacted_since_app_start;
extern bool idle_handler_has_user_interacted_since_last_vibration;

void idle_handler_register_timers();
void idle_handler_reset_user_interaction();
void idle_handler_notify_user_interacted();
void idle_handler_notify_received_new_vibration();
void idle_handler_notify_notifications_updated();
bool idle_handler_should_keep_current_notification();
uint32_t idle_handler_ms_until_current_notification_release();

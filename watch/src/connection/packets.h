#pragma once

#include "pebble.h"

void send_watch_welcome();
bool send_notification_opened(uint8_t id, bool prefetch);
bool send_mark_read(uint8_t id, void (*on_sent)(bool));
bool send_action_trigger(uint8_t notification_id, uint8_t action_id, uint8_t menu_id, const char* text);
void send_close_me();
void send_close_me_without_animation();
bool send_setting(uint8_t id, uint8_t value);
/** Ask the phone for the photo attached to a notification, at exactly this size (packet 16). */
bool send_image_request(uint8_t bucket_id, uint16_t width, uint16_t height);
void packets_init();

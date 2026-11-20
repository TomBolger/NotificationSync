#pragma once
#include <pebble.h>

/**
 * The watch's own Dark Mode setting (PebbleOS 4.45 and newer, on Pebble Time 2, Pebble Round 2 and Pebble 2 Duo).
 *
 * PebbleOS added dark_mode_service_peek/subscribe for apps in 4.45, but the public SDK this app builds with
 * (4.44) doesn't have them yet, and an app built with a newer SDK would be refused by older firmware. So the
 * calls are made here directly, the way the SDK's own libpebble.a does it, and only when the firmware is new
 * enough to have them. Everywhere else, available() is false and nothing is called.
 */
bool system_dark_mode_available(void);
bool system_dark_mode_peek(void);
void system_dark_mode_subscribe(void (*handler)(bool is_dark));
void system_dark_mode_unsubscribe(void);

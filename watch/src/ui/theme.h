#pragma once
#include <pebble.h>

/**
 * Light or dark look, like the Dark Mode setting PebbleOS 4.45 added for its own notifications: dark puts white
 * text on a black background, while the coloured banners and the list highlight keep their colours.
 * The phone app picks it (Watch theme setting); apps cannot read the watch's own setting with the current SDK.
 */
void theme_set_dark(bool dark);
bool theme_is_dark(void);

/** Card and list background. */
GColor theme_bg(void);
/** Main text, arrows and the action button. */
GColor theme_fg(void);
/** Dimmed text such as the "..." while the rest of a message loads. */
GColor theme_secondary(void);

/**
 * Draws a PebbleOS icon on the page background. As in PebbleOS's notification list, dark mode swaps its black
 * and white so the outline stays visible; `highlighted` rows keep the light version (on black and white watches
 * the highlighted row is black, so there it is the other way around).
 */
void theme_draw_icon(GContext* ctx, GDrawCommandImage* icon, GPoint origin, bool highlighted);

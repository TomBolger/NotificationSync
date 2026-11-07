#include "theme.h"

static bool dark;

void theme_set_dark(const bool is_dark)
{
    dark = is_dark;
}

bool theme_is_dark(void)
{
    return dark;
}

GColor theme_bg(void)
{
    return dark ? GColorBlack : GColorWhite;
}

GColor theme_fg(void)
{
    return dark ? GColorWhite : GColorBlack;
}

GColor theme_secondary(void)
{
    return PBL_IF_COLOR_ELSE(dark ? GColorLightGray : GColorDarkGray, theme_fg());
}

static GColor swap_black_white(const GColor color)
{
    if (gcolor_equal(color, GColorBlack))
    {
        return GColorWhite;
    }
    if (gcolor_equal(color, GColorWhite))
    {
        return GColorBlack;
    }
    return color;
}

static bool invert_command(GDrawCommand* command, uint32_t index, void* context)
{
    (void)index;
    (void)context;
    gdraw_command_set_stroke_color(command, swap_black_white(gdraw_command_get_stroke_color(command)));
    gdraw_command_set_fill_color(command, swap_black_white(gdraw_command_get_fill_color(command)));
    return true;
}

static void invert_icon(GDrawCommandImage* icon)
{
    gdraw_command_list_iterate(gdraw_command_image_get_command_list(icon), invert_command, NULL);
}

void theme_draw_icon(GContext* ctx, GDrawCommandImage* icon, const GPoint origin, const bool highlighted)
{
    // PebbleOS fw/apps/system/notifications.c prv_draw_notification_icon
    bool inverted = dark && !highlighted;
#if PBL_BW
    if (!dark && highlighted)
    {
        inverted = true;
    }
#endif
    if (inverted)
    {
        invert_icon(icon);
    }
    gdraw_command_image_draw(ctx, icon, origin);
    if (inverted)
    {
        invert_icon(icon);
    }
}

#include "window_image.h"

#include "commons/bytes.h"

static uint8_t* bitmap_data = NULL;
static size_t bitmap_data_position = 0;
static GBitmap* bitmap = NULL;
static Layer* drawing_layer = NULL;

// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void image_layer_paint(Layer* layer, GContext* ctx)
{
    const GRect layer_bounds = layer_get_bounds(layer);
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, layer_bounds, 0, GCornerNone);

    if (bitmap == NULL)
    {
        graphics_context_set_text_color(ctx, GColorWhite);
        graphics_draw_text(
            ctx,
            "Loading...",
            fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
            layer_bounds,
            GTextOverflowModeWordWrap,
            GTextAlignmentCenter,
            NULL
        );
    }
    else
    {
        const GRect image_bounds = gbitmap_get_bounds(bitmap);

        const int16_t x = (layer_bounds.size.w - image_bounds.size.w) / 2;
        const int16_t y = (layer_bounds.size.h - image_bounds.size.h) / 2;

        graphics_draw_bitmap_in_rect(ctx, bitmap, GRect(x, y, image_bounds.size.w, image_bounds.size.h));
    }
}


// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void window_load(Window* window)
{
    Layer* window_layer = window_get_root_layer(window);
    drawing_layer = window_layer;
    layer_set_update_proc(window_layer, image_layer_paint);
}

// ReSharper disable once CppParameterMayBeConstPtrOrRef
static void window_unload(Window* window)
{
    if (bitmap != NULL)
    {
        gbitmap_destroy(bitmap);
        bitmap = NULL;
    }
    window_destroy(window);
    drawing_layer = NULL;
}


static size_t bitmap_data_size = 0;

void window_image_show(const uint8_t* image_data, const size_t length)
{
    if (length < 3)
    {
        return;
    }
    const uint8_t flags = image_data[2];
    const bool first_packet = (flags & 0x01) != 0;
    const bool last_packet = (flags & 0x02) != 0;

    if (first_packet)
    {
        bitmap_data_position = 0;

        const size_t bitmap_size_bytes = read_uint16_from_byte_array(image_data, 0);
        if (bitmap_data != NULL)
        {
            free(bitmap_data);
            bitmap_data = NULL;
        }
        bitmap_data = malloc(bitmap_size_bytes);
        bitmap_data_size = bitmap_data != NULL ? bitmap_size_bytes : 0;
        if (bitmap_data == NULL)
        {
            // Not enough memory for this picture: say so instead of crashing.
            vibes_double_pulse();
            return;
        }

        Window* window = window_create();

        window_set_window_handlers(
            window,
            (WindowHandlers)
            {
                .load = window_load,
                .unload = window_unload,
            }
        );

        window_stack_push(window, true);
    }

    if (bitmap_data == NULL)
    {
        // A continuation without its first part (or after a failed allocation).
        return;
    }

    const size_t png_data_length = length - 3;
    if (bitmap_data_position + png_data_length > bitmap_data_size)
    {
        free(bitmap_data);
        bitmap_data = NULL;
        bitmap_data_size = 0;
        return;
    }
    memcpy(&bitmap_data[bitmap_data_position], &image_data[3], png_data_length);
    bitmap_data_position += png_data_length;

    if (last_packet)
    {
        bitmap = gbitmap_create_from_png_data(bitmap_data, bitmap_data_position);
        free(bitmap_data);
        bitmap_data = NULL;
        bitmap_data_size = 0;
        if (drawing_layer != NULL)
        {
            layer_mark_dirty(drawing_layer);
        }
    }
}

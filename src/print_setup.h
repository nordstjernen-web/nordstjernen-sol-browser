/* Nordstjernen — print sheet geometry shared by the renderer and the shell. */

#ifndef NS_PRINT_SETUP_H
#define NS_PRINT_SETUP_H

#define NS_PRINT_A4_WIDTH     (210.0 * 96.0 / 25.4)
#define NS_PRINT_A4_HEIGHT    (297.0 * 96.0 / 25.4)
#define NS_PRINT_MARGIN       (0.5 * 96.0)
#define NS_PRINT_RASTER_SCALE 3.0

typedef struct ns_print_setup {
    double width;
    double height;
    double margin_top;
    double margin_right;
    double margin_bottom;
    double margin_left;
} ns_print_setup;

static inline void
ns_print_setup_default(ns_print_setup *setup)
{
    setup->width = NS_PRINT_A4_WIDTH;
    setup->height = NS_PRINT_A4_HEIGHT;
    setup->margin_top = NS_PRINT_MARGIN;
    setup->margin_right = NS_PRINT_MARGIN;
    setup->margin_bottom = NS_PRINT_MARGIN;
    setup->margin_left = NS_PRINT_MARGIN;
}

#endif

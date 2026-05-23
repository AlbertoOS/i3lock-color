#ifndef _BLUR_H
#define _BLUR_H

#include <cairo.h>

void blur_image_surface(cairo_surface_t *surface, int sigma, int scale);

#endif

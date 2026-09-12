#include <string.h>
#include "all.h"
#include "i3/gradient_border.h"
#include "config_directives.h"

CFGFUN(gradient_border_toggle, const char *value) {
    aiwr_gradient.enabled = (strcmp(value, "enabled") == 0);
}

CFGFUN(gradient_border_color_start, const char *color) {
    aiwr_gradient.active_start = draw_util_hex_to_color(color);
}

CFGFUN(gradient_border_color_end, const char *color) {
    aiwr_gradient.active_end = draw_util_hex_to_color(color);
}

CFGFUN(gradient_border_inactive_start, const char *color) {
    aiwr_gradient.inactive_start = draw_util_hex_to_color(color);
    aiwr_gradient.inactive_set = true;
}

CFGFUN(gradient_border_inactive_end, const char *color) {
    aiwr_gradient.inactive_end = draw_util_hex_to_color(color);
    aiwr_gradient.inactive_set = true;
}

CFGFUN(gradient_border_direction, const char *direction) {
    if (strcmp(direction, "horizontal") == 0) aiwr_gradient.angle = 0;
    else if (strcmp(direction, "vertical") == 0) aiwr_gradient.angle = 90;
    else aiwr_gradient.angle = 45;
}

CFGFUN(gradient_border_angle, const long angle) {
    aiwr_gradient.angle = (int)(((angle % 360) + 360) % 360);
}

CFGFUN(gradient_border_speed, const long speed) {
    aiwr_gradient.speed = (int)(speed < 0 ? 0 : speed);
}

CFGFUN(gradient_border_fps, const long fps) {
    aiwr_gradient.fps = (int)(fps < 1 ? 1 : (fps > 120 ? 120 : fps));
}


/* ===== i3-aiwr: cole em src/config_directives.c, junto dos cfg_overview_* existentes =====
 * Requer no topo do arquivo: #include "i3/overview.h"
 * Requer em include/config_directives.h:
 *   CFGFUN(overview_live_previews, const char *value);
 *   CFGFUN(overview_particles, const long count);
 *   CFGFUN(overview_fps, const long fps);
 *   CFGFUN(overview_border_color, const char *color);
 *   CFGFUN(overview_border_color_end, const char *color);
 *   CFGFUN(overview_border_inactive, const char *color);
 *   CFGFUN(overview_border_width, const long width);
 *   CFGFUN(overview_border_speed, const long speed);
 */

#include <string.h>
#include "all.h"
#include "../include/config_directives.h"

CFGFUN(overview_live_previews, const char *value) {
    overview_config.live_previews = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                     strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(overview_particles, const long count) {
    overview_config.particles = (int)(count < 0 ? 0 : (count > 256 ? 256 : count));
}
CFGFUN(overview_fps, const long fps) {
    overview_config.fps = (int)(fps < 15 ? 15 : (fps > 240 ? 240 : fps));
}
CFGFUN(overview_border_color, const char *color) {
    overview_config.border_start = draw_util_hex_to_color(color);
}
CFGFUN(overview_border_color_end, const char *color) {
    overview_config.border_end = draw_util_hex_to_color(color);
}
CFGFUN(overview_border_inactive, const char *color) {
    overview_config.border_inactive = draw_util_hex_to_color(color);
}
CFGFUN(overview_border_width, const long width) {
    overview_config.border_width = (int)(width < 1 ? 1 : (width > 12 ? 12 : width));
}
CFGFUN(overview_border_speed, const long speed) {
    overview_config.border_speed = (int)(speed < 0 ? 0 : speed);
}
CFGFUN(overview_wallpaper_path, const char *wallpaper_path) {
    FREE(overview_config.wallpaper_path);
    overview_config.wallpaper_path = sstrdup(wallpaper_path);
}
CFGFUN(overview_background_blur, const char *background_blur) {
    long v = strtol(background_blur, NULL, 10);
    overview_config.background_blur = (int)(v < 0 ? 0 : (v > 100 ? 100 : v));
}
CFGFUN(dynamic_workspaces, const char *dynamic_workspaces) {
    overview_config.dynamic_workspaces = (strcmp(dynamic_workspaces, "enabled") == 0 || strcmp(dynamic_workspaces, "yes") == 0 ||
                                          strcmp(dynamic_workspaces, "true") == 0 || strcmp(dynamic_workspaces, "on") == 0);
}
CFGFUN(overview_thumbnail_blur, long int thumbnail_blur) {
    overview_config.thumbnail_blur = (int)(thumbnail_blur < 0 ? 0 : (thumbnail_blur > 100 ? 100 : thumbnail_blur));
}
CFGFUN(workspace_transition, const char *value) {
    workspace_transition_config.enabled = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                           strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(workspace_transition_duration, const long duration_ms) {
    workspace_transition_config.duration_ms = (int)(duration_ms < 0 ? 0 : (duration_ms > 2000 ? 2000 : duration_ms));
}
CFGFUN(workspace_transition_direction, const char *direction) {
    workspace_transition_config.direction = (strcmp(direction, "vertical") == 0) ? WT_VERTICAL : WT_HORIZONTAL;
}
CFGFUN(workspace_transition_fps, const long fps) {
    workspace_transition_config.fps = (int)(fps < 15 ? 15 : (fps > 240 ? 240 : fps));
}
CFGFUN(workspace_transition_type, const char *type) {
    if (strcmp(type, "fade") == 0) workspace_transition_config.type = WT_FADE;
    else if (strcmp(type, "zoom") == 0) workspace_transition_config.type = WT_ZOOM;
    else workspace_transition_config.type = WT_SLIDE;
}
CFGFUN(window_animation, const char *value) {
    window_animation_config.enabled = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                       strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(window_animation_duration, const long duration_ms) {
    window_animation_config.duration_ms = (int)(duration_ms < 0 ? 0 : (duration_ms > 2000 ? 2000 : duration_ms));
}
CFGFUN(window_animation_scale, const long scale) {
    window_animation_config.start_scale = (int)(scale < 10 ? 10 : (scale > 100 ? 100 : scale));
}
CFGFUN(window_animation_fps, const long fps) {
    window_animation_config.fps = (int)(fps < 15 ? 15 : (fps > 240 ? 240 : fps));
}
CFGFUN(workspace_transition_curve, const char *name) {
    FREE(workspace_transition_config.curve);
    workspace_transition_config.curve = sstrdup(name);
}
CFGFUN(resize_live, const char *value) {
    live_resize_config.enabled = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                 strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(resize_live_fps, const long fps) {
    live_resize_config.fps = (int)(fps < 15 ? 15 : (fps > 240 ? 240 : fps));
}
CFGFUN(window_animation_close, const char *value) {
    window_animation_config.close_enabled = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                             strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(window_animation_close_duration, const long duration_ms) {
    window_animation_config.close_duration_ms = (int)(duration_ms < 0 ? 0 : (duration_ms > 2000 ? 2000 : duration_ms));
}
CFGFUN(window_animation_close_scale, const long scale) {
    window_animation_config.close_scale = (int)(scale < 10 ? 10 : (scale > 100 ? 100 : scale));
}
CFGFUN(window_animation_close_curve, const char *name) {
    FREE(window_animation_config.close_curve);
    window_animation_config.close_curve = sstrdup(name);
}
CFGFUN(spring, const char *spec) {
    aiwr_curve_define_spring_spec(spec);
}
CFGFUN(window_animation_opacity, const char *value) {
    window_animation_config.opacity = (strcmp(value, "enabled") == 0 || strcmp(value, "yes") == 0 ||
                                       strcmp(value, "true") == 0 || strcmp(value, "on") == 0);
}
CFGFUN(window_animation_start_opacity, const long pct) {
    window_animation_config.start_opacity = (int)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
}
CFGFUN(window_animation_close_opacity, const long pct) {
    window_animation_config.close_opacity = (int)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
}

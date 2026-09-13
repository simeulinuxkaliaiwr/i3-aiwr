#pragma once
/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: live resize.
 *
 * Upstream i3 drags a preview bar and applies the resize once, at the end.
 * Here the resize is applied during the drag, rate-limited to fps
 * applications per second: each one sends real ConfigureNotify events to the
 * clients, so without the limit a heavy client stalls the whole drag.
 *
 */
#include <stdbool.h>

typedef struct live_resize_config {
    bool enabled;
    int fps; /* applications per second during a drag */
} live_resize_config_t;

extern live_resize_config_t live_resize_config;

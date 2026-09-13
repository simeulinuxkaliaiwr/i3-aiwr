/*
 * vim:ts=4:sw=4:expandtab
 *
 * i3-aiwr: the scrolling layout. See scrolling.h.
 *
 */
#include "all.h"
#include "i3/scrolling.h"
#include "i3/aiwr_anim.h"
#include <math.h>

#define SCLOG(fmt, ...) DLOG("[i3-aiwr] Scroll: " fmt, ##__VA_ARGS__)

static bool in_scroll_render;
static const double width_presets[] = {0.3333, 0.5, 0.6667, 1.0};
static Con *column_of(Con *con);
static void scroll_to_offset(Con *con, double target);
static double scrolling_total_width(Con *con);
static double clamp_offset(Con *con, double offset, double total);

scrolling_config_t scrolling_config = {
    .default_width = 50,
    .duration_ms = 200,
    .curve = NULL,
    .center_focus = false,
};

/* One scroll at a time. Scrolling two columns of the same container
 * simultaneously makes no sense, so a new scroll simply replaces the old. */
static struct {
    Con *con;
    double from, to;
    int anim_id;
} scroll_anim;

void scrolling_forget(Con *con) {
    if (con == NULL) return;
    if (scroll_anim.con == con) {
        if (scroll_anim.anim_id > 0) aiwr_anim_cancel(scroll_anim.anim_id);
        scroll_anim.con = NULL;
        scroll_anim.anim_id = 0;
    }
}

bool scrolling_animating(Con *con) {
    if (scroll_anim.con == NULL) return false;
    for (Con *c = con;c != NULL;c = c->parent) {
        if (c == scroll_anim.con) return true;
    }
    return false;
}

static Con *ws_find_scrolling(Con *ws) {
    if (ws == NULL) return NULL;
    if (ws->layout == L_SCROLLING) return ws;
    Con *c;
    TAILQ_FOREACH (c, &(ws->nodes_head), nodes) {
        Con *r = ws_find_scrolling(c);
        if (r != NULL) return r;
    }
    return NULL;
}

void scrolling_toggle_layout(Con *con) {
    Con *ws = con_get_workspace(con);
    if (ws == NULL) return;

    Con *sc = ws_find_scrolling(ws);
    if (sc != NULL) {
        /* Leaving: in a split the percentages must sum to 1, whereas in
         * scrolling each one is that column's own width. con_fix_percent
         * normalises them. */
        sc->layout = L_SPLITH;
        sc->scroll_offset = 0.0;
        ws->workspace_layout = L_DEFAULT;
        con_fix_percent(sc);
        SCLOG("workspace %s -> splith\n", ws->name);
    } else {
        con_set_layout(ws, L_SCROLLING);
        SCLOG("workspace %s -> scrolling\n", ws->name);
    }
    tree_render();
}

void scrolling_resize_column_px(Con *column, int px) {
    if (column == NULL || column->parent == NULL) return;
    if (column->parent->layout != L_SCROLLING) return;
    const double view = (double)column->parent->rect.width;
    if (view <= 0) return;

    double pct = (column->percent > 0.0) ? column->percent
                                         : scrolling_config.default_width / 100.0;

    pct += (double)px / view;
    if (pct < 0.05) pct = 0.05;
    if (pct > 4.0) pct = 4.0;
    column->percent = pct;
    tree_render();
}

bool scrolling_resize_handled(Con *con, const char *direction, long px, long ppt) {
    Con *column = column_of(con);
    if (column == NULL || column->parent == NULL) return false;

    if (strcmp(direction, "height") == 0 ||
        strcmp(direction, "up") == 0 ||
        strcmp(direction, "down") == 0) {
        return false;
    }

    int delta = (int)ppt;
    if (delta == 0 && px != 0) {
        const int view = column->parent->rect.width;
        if (view <= 0) return false;
        delta = (int)((px * 100) / view);
        if (delta == 0) delta = (px > 0) ? 1 : -1;
    }
    if (delta == 0) return false;

    scrolling_resize_column(column, delta);
    return true;
}

void scrolling_scroll_by(Con *con, int dir) {
    Con *sc = scrolling_container(con);
    if (sc == NULL || sc->rect.width == 0) return;
    const double step = (scrolling_config.default_width / 100.0) * (double)sc->rect.width;
    scroll_to_offset(sc, clamp_offset(sc, sc->scroll_offset + dir * step, scrolling_total_width(sc)));
}

static double scrolling_total_width(Con *con) {
    double total = 0.0;
    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        total += scrolling_column_width(con, child);
    }
    return total;
}

static Con *column_of(Con *con) {
    if (con == NULL) return NULL;
    Con *column = con;
    while (column->parent != NULL && column->parent->layout != L_SCROLLING) {
        if (column->type == CT_WORKSPACE) return NULL;
        column = column->parent;
    }
    return (column->parent != NULL && column->parent->layout == L_SCROLLING) ? column : NULL;
}

static double column_percent(Con *column) {
    return (column->percent > 0.0) ? column->percent
                                   : scrolling_config.default_width / 100.0;
}

void scrolling_toggle_maximize(Con *con) {
    Con *column = column_of(con);
    if (column == NULL) return;

    if (column_percent(column) >= 0.999) {
        double back = column->scroll_prev_percent;
        if (back <= 0.0 || back >= 0.999) back = scrolling_config.default_width / 100.0;
        column->percent = back;
        column->scroll_prev_percent = 0.0;
    } else {
        column->scroll_prev_percent = column_percent(column);
        column->percent = 1.0;
    }
    tree_render();
    scrolling_reveal(column);
}

void scrolling_cycle_width(Con *con, bool backwards) {
    Con *column = column_of(con);
    if (column == NULL) return;

    const int n = (int)(sizeof(width_presets) / sizeof(width_presets[0]));
    double cur = column_percent(column);
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (fabs(width_presets[i] - cur) < 0.02) { idx = i; break; }
        /* A width outside the presets snaps to the next one up. */
        if (width_presets[i] > cur) { idx = backwards ? i : i - 1; break; }
        idx = i;
    }
    idx += backwards ? -1 : 1;
    if (idx < 0) idx = n - 1;
    if (idx >= n) idx = 0;

    column->percent = width_presets[idx];
    column->scroll_prev_percent = 0.0;
    tree_render();
    scrolling_reveal(column);
}

/* Called by the renderer before the columns are positioned. Closing or
 * shrinking a column can leave the offset past its maximum, which would show
 * empty space on the right with no way to scroll back. */
void scrolling_prepare(Con *con) {
    if (con == NULL || con->layout != L_SCROLLING) return;
    /* Mid-animation the target is already computed; do not fight it. */
    if (scroll_anim.con == con) return;
    con->scroll_offset = clamp_offset(con, con->scroll_offset, scrolling_total_width(con));
}

static void scroll_render(void) {
    if (in_scroll_render) return;
    in_scroll_render = true;
    tree_render();
    in_scroll_render = false;
}

Con *scrolling_container(Con *con) {
    for (Con *c = con; c != NULL; c = c->parent) {
        if (c->layout == L_SCROLLING) return c;
        if (c->type == CT_WORKSPACE) return (c->layout == L_SCROLLING) ? c : NULL;
    }
    return NULL;
}

double scrolling_offset(Con *con) {
    return (con != NULL) ? con->scroll_offset : 0.0;
}

/* Width of a column in pixels, given the viewport. */
int scrolling_column_width(Con *con, Con *child) {
    double pct = child->percent;
    if (pct <= 0.0) pct = scrolling_config.default_width / 100.0;
    int w = (int)(con->rect.width * pct);
    return (w < 1) ? 1 : w;
}

/* The x (before the offset) and width of a column within its container. */
static bool column_extent(Con *con, Con *column, double *out_x, double *out_w, double *out_total) {
    double x = 0.0;
    bool found = false;
    Con *child;
    TAILQ_FOREACH (child, &(con->nodes_head), nodes) {
        double w = scrolling_column_width(con, child);
        if (child == column) {
            if (out_x) *out_x = x;
            if (out_w) *out_w = w;
            found = true;
        }
        x += w;
    }
    if (out_total) *out_total = x;
    return found;
}

static double clamp_offset(Con *con, double offset, double total) {
    const double view = (double)con->rect.width;

    /* A strip narrower than the viewport is centred rather than left-aligned.
     * The negative offset is deliberate: the renderer subtracts it. */
    if (total < view) {
        return -(view - total) / 2.0;
    }
    double max = total - view;
    if (max < 0.0) max = 0.0;
    if (offset < 0.0) offset = 0.0;
    if (offset > max) offset = max;
    return offset;
}

static void scroll_step(double p, double e, void *data) {
    Con *con = scroll_anim.con;
    if (con == NULL) return;
    con->scroll_offset = scroll_anim.from + (scroll_anim.to - scroll_anim.from) * e;
    scroll_render();
}

static void scroll_done(void *data) {
    Con *con = scroll_anim.con;
    if (con != NULL) con->scroll_offset = scroll_anim.to;
    scroll_anim.con = NULL;
    scroll_anim.anim_id = 0;
    scroll_render();
}

static void scroll_to_offset(Con *con, double target) {
    if (fabs(target - con->scroll_offset) < 0.5) return;

    if (scroll_anim.anim_id > 0) {
        aiwr_anim_cancel(scroll_anim.anim_id);
        scroll_anim.anim_id = 0;
        scroll_anim.con = NULL;
    }
    if (scrolling_config.duration_ms <= 0) {
        con->scroll_offset = target;
        return;
    }
    scroll_anim.con = con;
    scroll_anim.from = con->scroll_offset;
    scroll_anim.to = target;
    scroll_anim.anim_id = aiwr_anim_start(scrolling_config.duration_ms, scrolling_config.curve,
                                          scroll_step, scroll_done, NULL);
    if (scroll_anim.anim_id == 0) {
        con->scroll_offset = target;
        scroll_anim.con = NULL;
        tree_render();
    }
}

void scrolling_reveal(Con *column) {
    if (column == NULL) return;
    Con *con = (column->parent != NULL && column->parent->layout == L_SCROLLING)
                   ? column->parent
                   : NULL;
    if (con == NULL || con->rect.width == 0) return;

    double x = 0, w = 0, total = 0;
    if (!column_extent(con, column, &x, &w, &total)) return;

    double view = (double)con->rect.width;
    double offset = con->scroll_offset;

    if (scrolling_config.center_focus) {
        offset = x + w / 2.0 - view / 2.0;
    } else {
        /* Only bring it into view: leave the offset alone if the column is
         * already fully visible. */
        if (x < offset) {
            offset = x;
        } else if (x + w > offset + view) {
            offset = x + w - view;
        }
    }
    scroll_to_offset(con, clamp_offset(con, offset, total));
}

void scrolling_on_focus(Con *con) {
    if (con == NULL) return;
    /* Walk up to the column: the focused con may be inside a nested split. */
    Con *column = con;
    while (column->parent != NULL && column->parent->layout != L_SCROLLING) {
        if (column->type == CT_WORKSPACE) return;
        column = column->parent;
    }
    if (column->parent == NULL || column->parent->layout != L_SCROLLING) return;
    scrolling_reveal(column);
}

void scrolling_resize_column(Con *column, int delta_ppt) {
    if (column == NULL || column->parent == NULL) return;
    if (column->parent->layout != L_SCROLLING) return;

    double pct = column->percent;
    if (pct <= 0.0) pct = scrolling_config.default_width / 100.0;
    pct += delta_ppt / 100.0;
    if (pct < 0.05) pct = 0.05;
    if (pct > 4.0) pct = 4.0; /* columns wider than the screen are allowed */
    column->percent = pct;

    tree_render();
    scrolling_reveal(column);
}

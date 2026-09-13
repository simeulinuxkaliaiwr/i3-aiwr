# i3-aiwr

A fork of [i3](https://i3wm.org/) that adds the things a modern desktop is
expected to have, without giving up i3's keyboard-driven tiling: a zoomed-out
workspace overview with live previews, a niri-style scrolling layout, animated
window transitions built on real spring physics, rounded corners, animated
gradient borders, live resizing, and an MRU window switcher.

https://github.com/user-attachments/assets/57611391-6085-4622-af35-3ad745b21d2b

Everything not listed here behaves exactly like upstream i3.

---

## Table of contents

- [Features](#features)
- [Requirements](#requirements)
- [Building](#building)
- [Configuration](#configuration)
  - [Syntax notes](#syntax-notes)
  - [Overview](#overview)
  - [Scrolling layout](#scrolling-layout)
  - [Window animations](#window-animations)
  - [Workspace transitions](#workspace-transitions)
  - [Animation curves](#animation-curves)
  - [Window switcher](#window-switcher)
  - [Rounded corners](#rounded-corners)
  - [Gradient borders](#gradient-borders)
  - [Live resize](#live-resize)
- [Commands](#commands)
- [A complete example](#a-complete-example)
- [How it works](#how-it-works)
- [Known issues](#known-issues)
- [Contributing](#contributing)
- [License](#license)

---

## Features

**Overview.** A zoomed-out view of every workspace, each rendered as a live
thumbnail showing real window contents. Navigate with the keyboard or mouse,
type to filter by window title or class, and drag windows between workspaces
with a preview showing exactly where they will land.

**Scrolling layout.** Workspaces can use a niri-style layout where windows
become columns of independent width laid end to end, extending past the screen
edge. The viewport scrolls to follow focus. Columns keep their own width
rather than sharing space, so opening a window never shrinks the others.

**Window animations.** Windows scale and fade in when opened and out when
closed, driven by cubic-bezier curves or real spring physics.

**Workspace transitions.** Switching workspaces slides, fades or zooms between
them, in any direction, with a configurable curve.

**Window switcher.** Alt+Tab through your windows in most-recently-used order,
across all workspaces, with live previews. Hold the modifier to cycle, release
to commit.

**Rounded corners** on both the frame and the client window, so nothing pokes
through.

**Gradient borders** that animate around the window, with separate colours for
focused and unfocused windows.

**Live resize.** Dragging a border resizes the windows as you drag, instead of
showing an outline and applying it at the end.

---

## Requirements

Beyond upstream i3's dependencies:

- `xcb-composite` — **required**; live previews do not work without it
- `xcb-damage`
- `xcb-shape` — required for rounded corners
- `xcb-xfixes`
- `cairo` >= 1.18

On Arch:

```sh
sudo pacman -S --needed libxcb xcb-util xcb-util-cursor xcb-util-keysyms \
  xcb-util-wm xcb-util-xrm libxkbcommon libxkbcommon-x11 yajl pcre2 \
  cairo pango glib2 libev startup-notification perl meson ninja
```

**A compositor is optional but recommended.** Without one, opacity fading does
nothing (the X server ignores the alpha of ARGB windows) and rounded corners
rely on XShape, which has hard edges rather than antialiased ones. i3-aiwr
detects a compositor at runtime and adapts.

---

## Building

```sh
git clone https://github.com/USER/i3-aiwr.git
cd i3-aiwr
meson setup build
ninja -C build
```

The binary is `build/i3`. To try it without installing, start a second X
server on another VT:

```sh
startx /path/to/i3-aiwr/build/i3 -- :1 vt3
```

To install system-wide:

```sh
sudo ninja -C build install
```

---

## Configuration

Everything goes in your normal i3 config. Every option has a default, so you
only need to set what you want to change.

### Syntax notes

Most i3-aiwr options are **nested**: the feature name, then the setting.

```
overview enabled
overview thumbnail_scale 43
```

A few are flat, because they are standalone settings rather than part of a
group:

```
rounded_corners_radius 20
resize_live_fps 60
workspace_layout scrolling
dynamic_workspaces yes
```

Booleans accept `enabled`, `yes`, `true`, `on` and `1`, and their negations.

---

### Overview

```
overview enabled
overview thumbnail_scale 43        # % of the output size
overview spacing 32                # px between thumbnails
overview animation_duration 240    # ms; 0 disables the zoom
overview background_opacity 50     # 0-100, darkening over the wallpaper
overview background_blur 9         # 0-100
overview thumbnail_blur 35         # blur on unselected thumbnails, 0-100
overview live_previews enabled
overview fps 60
overview particles 0               # background particles; 0 disables

# absolute path to a PNG. cairo only decodes PNG, so convert other formats.
# Without this, i3-aiwr reads _XROOTPMAP_ID, which most wallpaper setters
# (feh, hsetroot) do set. Some, like video wallpapers, do not.
overview wallpaper_path /home/you/Pictures/wall.png

# border of the selected thumbnail and the drop target
overview border_color     #6366f1
overview border_color_end #ec4899
overview border_inactive  #ffffff2e
overview border_width 3
overview border_speed 45           # gradient rotation, degrees/s

bindsym $mod+Tab exec --no-startup-id i3-msg overview toggle
```

**Controls inside the overview**

| Key | Action |
|---|---|
| `Up` `Down` `Left` `Right` `Tab` | navigate |
| `k` `j` `h` `l` | navigate, while the filter is empty |
| `1`–`9` | jump to a workspace, while the filter is empty |
| any printable character | filter by window title, class or workspace name |
| `Backspace` | delete a filter character |
| `Enter` | open the selected workspace |
| `Esc` | clear the filter, then close |
| left click a window | focus it and open its workspace |
| drag a window | move it to another workspace |
| scroll wheel | navigate |

Once you start typing, digits and `hjkl` become filter text — otherwise you
could not search for a window with a number or the letter `j` in its title.
Arrow keys and Tab always navigate.

---

### Scrolling layout

```
workspace_layout scrolling         # applies to workspaces created after this
dynamic_workspaces yes             # show the "+" slot in the overview

scrolling width 50                 # % of the viewport for a new column
scrolling duration 200             # scroll animation, ms
scrolling curve spring
scrolling center_focus disabled    # centre the focused column, or just bring
                                   # it into view

bindsym $mod+z scrolling toggle
bindsym $mod+f scrolling maximize
bindsym $mod+plus scrolling wider
bindsym $mod+minus scrolling narrower
bindsym $mod+bracketleft scrolling left
bindsym $mod+bracketright scrolling right
bindsym --whole-window $mod+button4 scrolling left
bindsym --whole-window $mod+button5 scrolling right
```

**`workspace_layout scrolling` only affects workspaces created after the
config loads.** To convert an existing workspace, use `scrolling toggle`,
which also preserves each column's width across the round trip.

Columns take their width from `scrolling width` as a percentage of the
viewport. Unlike normal tiling, the widths are independent — they do not sum
to 100%, and a column may be wider than the screen. Closing a column does not
resize its neighbours; the strip simply gets shorter. Use `scrolling maximize`
to fill the screen with one column.

`focus left` and `focus right` move between columns, and the view scrolls to
follow. Vertical splits inside a column work normally.

---

### Window animations

```
window_animation enabled
window_animation duration 160      # ms
window_animation scale 60          # % of final size on the first frame
window_animation curve md3_decel
window_animation fps 60
window_animation opacity enabled   # needs a compositor
window_animation start_opacity 0   # % on the first frame

window_animation close enabled
window_animation close duration 140
window_animation close scale 60
window_animation close curve md3_accel
window_animation close opacity 0
```

`scale 100` disables the effect without disabling the module.

Note that a spring curve supplies its own duration, so `duration` is ignored
when `curve` names a spring.

---

### Workspace transitions

```
workspace_transition enabled
workspace_transition duration 200
workspace_transition type slide        # slide, fade or zoom
workspace_transition direction horizontal   # horizontal or vertical
workspace_transition curve md3_decel
workspace_transition fps 60
```

Transitions only run between workspaces on the same output.

---

### Animation curves

Two kinds, usable anywhere a `curve` option appears.

**Bezier** — cubic-bezier in the CSS convention. Only the two control points
matter; y may exceed 1, which is where overshoot comes from.

```
bezier snappy 0.05 0.9 0.1 1.0
bezier snappy, 0.05, 0.9, 0.1, 1.0    # commas optional
```

**Spring** — a damped harmonic oscillator, taking niri's parameters. A spring
has no duration: it runs until it settles within `epsilon`, and that settle
time becomes the animation's duration.

```
spring mine damping-ratio=1.0 stiffness=1000 epsilon=0.0001
spring fast damping-ratio=1.0 stiffness=4000 epsilon=0.001 speed=1.5
```

- `damping-ratio` — 1.0 is critically damped, the fastest approach with no
  overshoot. Below 1 bounces; above 1 is sluggish.
- `stiffness` — higher is faster. 1000 settles in roughly 370ms, 4000 in
  about 185ms.
- `epsilon` — how close counts as arrived. Larger settles sooner.
- `mass` — defaults to 1.
- `speed` — divides the computed settle time.

Each spring logs its computed settle time at load, so you can see what
duration you actually got.

**Built-in bezier curves**, usable without defining anything: `default`,
`linear`, `md3_standard`, `md3_decel`, `md3_accel`, `md2`, `overshot`,
`crazyshot`, `hyprnostretch`, `menu_decel`, `menu_accel`, `easeInOutCirc`,
`easeOutCirc`, `easeOutExpo`, `softAcDecel`.

**Built-in springs**: `spring` (niri's preset: critically damped, stiffness
1000), `spring_snappy`, `spring_soft`, `spring_bouncy`, `spring_wobbly`.

---

### Window switcher

```
switcher enabled
switcher max_items 8
switcher preview enabled

bindsym Mod1+Tab switcher next
bindsym Mod1+Shift+Tab switcher prev
```

The switcher lists windows in most-recently-used order across every
workspace. Hold the modifier and press Tab to cycle; release to commit. Escape
cancels.

**Use a real modifier in the binding.** The switcher reads which modifiers are
physically held when it opens and commits when they are released. Bound to a
bare key, it falls back to a modal list where Enter commits and Escape
cancels.

Windows on your current workspace show live previews. Windows elsewhere show
the last image captured of them. A window that has never been on screen this
session falls back to a coloured tile with its class initial.

---

### Rounded corners

```
rounded_corners enabled
rounded_corners_radius 20
rounded_corners_floating yes
rounded_corners_tiling yes

default_border pixel 4
```

A visible border makes the rounding much more apparent. Clients that set their
own window shape are left alone.

---

### Gradient borders

```
gradient_border enabled
gradient_border color_start "#6366f1ff"
gradient_border color_end   "#ec4899ff"
gradient_border inactive_start "#3a3a4aff"
gradient_border inactive_end   "#55556aff"
gradient_border direction diagonal      # horizontal, vertical or diagonal
gradient_border angle 45                # degrees; overrides direction
gradient_border speed 60                # degrees/s; 0 for static
gradient_border fps 30
```

Without `inactive_start` and `inactive_end`, unfocused windows keep i3's solid
`client.unfocused` colour.

A `for_window [...] border pixel 0` rule suppresses gradient borders for those
windows, since there is no border to paint.

---

### Live resize

```
resize_live yes
resize_live_fps 60
```

With live resize on, dragging a border resizes the windows continuously rather
than showing an outline. Each step sends real `ConfigureNotify` events, so a
slow client can make the drag stutter — lower `resize_live_fps` to 30 if that
happens, or set `resize_live no` for upstream behaviour.

Escape during a drag reverts to the original sizes.

---

## Commands

Usable from a binding or `i3-msg`.

| Command | Effect |
|---|---|
| `overview toggle` | open or close the overview |
| `scrolling toggle` | switch the workspace between scrolling and splith |
| `scrolling maximize` | toggle the focused column to full width |
| `scrolling wider` / `narrower` | cycle the column through preset widths |
| `scrolling left` / `right` | scroll the view without moving focus |
| `switcher next` / `prev` | open or cycle the window switcher |
| `layout scrolling` | set the scrolling layout on the focused container |

---

## A complete example

```
# --- appearance ---
default_border pixel 4

rounded_corners enabled
rounded_corners_radius 12
rounded_corners_floating yes
rounded_corners_tiling yes

gradient_border enabled
gradient_border color_start "#6366f1ff"
gradient_border color_end   "#ec4899ff"
gradient_border direction diagonal
gradient_border speed 60
gradient_border fps 30

# --- curves ---
bezier snappy 0.05 0.9 0.1 1.0
spring close_fast damping-ratio=1.0 stiffness=4000 epsilon=0.001

# --- animations ---
window_animation enabled
window_animation scale 60
window_animation curve md3_decel
window_animation duration 160
window_animation close enabled
window_animation close curve close_fast
window_animation close scale 60

workspace_transition enabled
workspace_transition type slide
workspace_transition duration 200
workspace_transition curve md3_decel

resize_live yes
resize_live_fps 60

# --- overview ---
overview enabled
overview thumbnail_scale 43
overview spacing 32
overview background_opacity 50
overview background_blur 9
overview thumbnail_blur 35
overview border_color     #6366f1
overview border_color_end #ec4899
overview border_width 3
overview border_speed 45
bindsym $mod+Tab exec --no-startup-id i3-msg overview toggle

# --- scrolling ---
workspace_layout scrolling
dynamic_workspaces yes
scrolling width 50
scrolling duration 200
scrolling curve spring
bindsym $mod+z scrolling toggle
bindsym $mod+f scrolling maximize
bindsym $mod+bracketleft scrolling left
bindsym $mod+bracketright scrolling right

# --- switcher ---
switcher enabled
switcher max_items 8
bindsym Mod1+Tab switcher next
bindsym Mod1+Shift+Tab switcher prev
```

---

## How it works

A few notes for anyone reading the source.

**Window capture.** Live previews come from `NameWindowPixmap`, not from
copying the root window. With a compositor running, windows are redirected
offscreen and the root only holds the wallpaper; without one, a root copy only
captures what happens to be visible at that instant. A named pixmap gives the
real contents of a frame and stays valid after the window is unmapped, which
is what makes previews of hidden workspaces possible at all.

**Why hidden workspaces are not live.** An unmapped window does not render, so
its pixmap stops updating. Thumbnails of workspaces you are not on show the
last state you saw. No compositor can change this without keeping every window
rendering all the time.

**One animation clock.** Every animation registers with a single `ev_timer`,
receives its progress already passed through its curve, and the frame ends
with one flush. Separate timers per module produced unevenly spaced frames.

**The scrolling layout.** `L_SCROLLING` is a layout where each child's
`percent` is its own width as a fraction of the viewport, rather than a share
of a whole that sums to 1. The container holds a view offset which the
renderer subtracts from each column's x; X clips whatever falls outside.

---

## Known issues

This is a young fork built by one person. Things that are known to be rough:

- **Opacity fading requires a compositor.** Without one the X server ignores
  the property and windows simply do not fade.
- **The overview can drop frames** on setups with many workspaces and windows,
  particularly with a compositor doing heavy blur. Lowering `overview fps` or
  `overview thumbnail_blur` helps.
- **No preview for windows never seen this session.** The switcher falls back
  to a coloured tile, and overview thumbnails to placeholders.
- **Column widths are not saved across restarts.** A restart restores the
  scrolling layout, but each column returns to the default width.
- **Fullscreen inside a scrolling workspace is untested.**
- **Rounded corners are not antialiased**, because XShape works in whole
  pixels. This would need a compositor doing the rounding in a shader.
- **Per-window move and resize animations do not exist.** An implementation
  was attempted and reverted: it conflicted with the open animation and with
  the scroll animation, and added very little in a scrolling layout.
- **i3-aiwr ships no compositor of its own.** Effects that need real
  compositing — translucency, blur behind windows, shadows — are left to
  picom or similar.

Bug reports are welcome, especially with a log from `i3 -V`.

---

## Contributing

i3-aiwr tracks upstream i3, so changes that touch upstream files are merged
periodically. Keeping new work in new files under `src/` and `include/i3/`
makes those merges far less painful.

The code follows i3's style: four-space indentation, `/** ... */` doc comments
above non-static functions in headers, and comments that explain why rather
than what.

---

## License

BSD-3-Clause, as upstream i3. See [LICENSE](LICENSE).

i3 is © 2009 Michael Stapelberg and contributors.

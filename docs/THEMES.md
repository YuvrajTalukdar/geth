# GETH — Themes

Themes are 24-bit palettes. GETH auto-detects the terminal's color depth
(`truecolor` / `256` / `16`) and falls back gracefully — see
[Color fallback](#color-fallback).

## Built-in themes

| Name | Accent | Character |
|---|---|---|
| `cyberpunk-blue` (default) | `#00E5FF` | Cyan/blue on dark |
| `matrix-green` | `#39FF14` | Terminal green |
| `crimson-red` | `#FF2E4D` | Warm red |
| `amber` | `#FFB000` | Monochrome amber |

Select one with `[general] theme = "…"`, `--theme NAME`, or press `T` in the
UI to cycle (each press saves). `--list-themes` lists every selectable theme
(built-in + custom) and marks the active one.

## Custom themes

Drop a `.toml` file in `$XDG_CONFIG_HOME/geth/themes/` (the filename, without
`.toml`, is the theme name). Start from the shipped example, which documents
every field:

```sh
cp /usr/share/geth/themes/example.toml ~/.config/geth/themes/solar.toml
# (from a source tree: packaging/themes/example.toml)
```

A theme may set any of these (all colors are `#RRGGBB`):

| Field | Meaning |
|---|---|
| `base` | Inherit from another theme (built-in or custom — layering is allowed). Omit for a fully explicit theme. |
| `accent` | The "live" foreground: current values, selected rows, the title |
| `title` | Panel titles (defaults to `accent` when omitted) |
| `text` | Body text: labels, numbers, graph legends |
| `label_dim` | Secondary/dim text: units, hidden markers, the footer |
| `border` | Panel borders |
| `grid` | Graph grid lines |
| `axis_text` | Graph axis labels |
| `warn` | Warning toast / config-error foreground |
| `bg` | Solid-mode screen background (only used when `background = "solid"`) |
| `fill_mix` | `[0, 1]` — how much of the series color the graph "fill" keeps (default `0.45`) |
| `series_1` … `series_6` | The six graph series colors, in order (panels wrap through them) |

A field left out (or set to a bad value) falls back to the base theme's
value, so a theme can override just a few. **Invalid hex values and unknown
keys never crash the app**: each produces a warning toast (and a
`geth: theme: …` line on `stderr`) and that field keeps its inherited value.
An unknown `base`, a missing file, or an unparseable file means the theme
resolves to nothing and is skipped (with a warning), never a crash.

## Background mode

`[general] background` is `"transparent"` (default) or `"solid"`.

- **transparent** — GETH never paints a background color or inverts text; it
  is transparent over any terminal theme. A unit test asserts the rendered
  output contains zero background SGR codes for every theme.
- **solid** — the theme's `bg` color is painted across the whole screen. This
  is the *only* place GETH may paint a cell background.

## Color fallback

For terminals without 24-bit color, each 24-bit palette color is mapped to
the nearest 256-color (or 16-color) entry. The six series colors are mapped
so that **no two distinct series share the same 16-color slot** when distinct
slots exist, keeping adjacent panels distinguishable. A test renders every
built-in theme in all three color modes (truecolor, 256, 16) and asserts the
series stay distinct; another asserts six distinct 16-color slots for every
built-in theme.

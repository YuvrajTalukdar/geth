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
| `fill_mix` | **Deprecated** — no longer used by either chart renderer (the pixel renderer uses fixed gradient strengths, the text renderer no longer fills multi-series panels). Still parsed, so existing themes that set it don't warn |
| `series_1` … `series_6` | The six graph series colors, assigned by device role (see [Series color slots](#series-color-slots)) |

A field left out (or set to a bad value) falls back to the base theme's
value, so a theme can override just a few. **Invalid hex values and unknown
keys never crash the app**: each produces a warning toast (and a
`geth: theme: …` line on `stderr`) and that field keeps its inherited value.
An unknown `base`, a missing file, or an unparseable file means the theme
resolves to nothing and is skipped (with a warning), never a crash.

## Series color slots

The six series colors are assigned **by device role**, so that every series
inside a panel has a distinct color:

| Slot | Role |
|---|---|
| `series_1` | CPU / the primary (first) series of any panel |
| `series_2` | The first discrete GPU (its usage and VRAM panels) |
| `series_3` | The integrated GPU (iGPU) |
| `series_4` | The second discrete GPU |
| `series_5` | Third discrete GPU — or the next-free slot when a panel has two series of one device |
| `series_6` | Fourth discrete GPU — or the next-free slot (continued) |

When a panel holds two series of the *same* device (e.g. the iGPU's GTT and
VRAM, or two CPUs), the second series takes the next free slot. Built-in
themes set slots 5 and 6 as slots 1 and 2 mixed 40% toward white, so the
fallback stays in the theme's hue family.

## Series color validation

The four built-in palettes are checked in the test suite, and **custom
themes are validated at load**: a failing check produces a warning (theme
name, slots, measured value) but the theme still loads.

- **Pair separation** — every pair among slots 1–4:
  - normal vision: ΔE ≥ 15 (OKLab, ×100);
  - under protanopia *and* deuteranopia simulation (Machado 2009 matrices
    on linear sRGB, then OKLab): ΔE ≥ 8.
  - One sanctioned exception: `matrix-green`'s slots 1↔4 (`#39FF14` vs
    `#F0E68C`) measure 7.9 (protan) / 7.1 (deutan) — allowed at ΔE ≥ 7.0 for
    both simulations, because every series carries a labelled legend. The
    exception is keyed on the theme name *and* that exact color pair, so a
    custom theme inheriting the matrix-green palette is covered.
- **Background contrast** — every one of the six slots against the dark
  background `#0B0F14`: WCAG contrast ratio ≥ 3:1 (series are graph lines,
  not text; 3:1 is the WCAG non-text threshold).

Measured margins of the built-ins (smallest per rule): cyberpunk-blue 25.4
ΔE normal / 5.9:1 contrast, matrix-green 21.5 / 7.1 (the exception) / 9.7:1,
crimson-red 22.0 / 5.4:1, amber 18.6 / 6.0:1.

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

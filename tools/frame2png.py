#!/usr/bin/env python3
"""Render one `geth --render-frame` frame to a PNG on an exact cell grid.

Dev tool for docs/screenshots. Captures
    geth --render-frame COLSxROWS --ansi --demo --color truecolor [...]
and draws every cell (glyph, truecolor foreground) on a monospace grid over a
procedurally generated dark, softly blurred gradient background — no external
images. DejaVu Sans Mono covers the box-drawing and braille glyphs geth uses.

Every render runs with a temporary XDG_CONFIG_HOME and GETH_TEST=1, so the
tool can never touch the real config.

Usage:
  frame2png.py <geth-bin> <out.png> --cols C --rows R
               [--theme NAME] [--script EVENTS] [--accent #RRGGBB]
               [--fontsize N] [--font PATH]
"""
import argparse
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

FONT_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono.ttf"
FONT_BOLD_DEFAULT = "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf"
DEFAULT_FG = (215, 220, 228)

# xterm 256-color palette: 16 system + 6x6x6 cube + 24 gray ramp.
def xterm256(n):
    if n < 16:
        base = [
            (0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0),
            (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229),
            (127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0),
            (92, 92, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255),
        ]
        return base[n]
    if n < 232:
        n -= 16
        r, g, b = divmod(n, 36)
        g, b = divmod(g, 6)
        vals = (0, 95, 135, 175, 215, 255)
        return (vals[r], vals[g], vals[b])
    v = 8 + (n - 232) * 10
    return (v, v, v)


def parse_hex(s):
    s = s.lstrip("#")
    if len(s) != 6:
        raise ValueError(f"bad color {s!r}")
    return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def parse_ansi(data, cols, rows):
    """Parse an ANSI frame stream into a grid of (char, fg_or_None, bold)."""
    blank = (" ", None, False)
    grid = [[blank] * cols for _ in range(rows)]
    fg, bold = None, False
    x = y = 0

    def put(ch):
        nonlocal x, fg, bold
        if 0 <= x < cols and 0 <= y < rows:
            grid[y][x] = (ch, fg, bold)
        x += 1

    def sgr(seq):
        nonlocal fg, bold
        params = seq[2:-1].decode("latin1").split(";")
        i = 0
        while i < len(params):
            p = params[i] or "0"
            if p == "0":
                fg, bold = None, False
            elif p == "1":
                bold = True
            elif p == "39":
                fg = None
            elif p == "38":
                if i + 4 <= len(params) and params[i + 1] == "2":
                    fg = (int(params[i + 2]), int(params[i + 3]), int(params[i + 4]))
                    i += 4
                elif i + 3 <= len(params) and params[i + 1] == "5":
                    fg = xterm256(int(params[i + 2]))
                    i += 2
            elif p == "48":
                # background: geth never paints one; just consume the params
                if i + 4 <= len(params) and params[i + 1] == "2":
                    i += 4
                elif i + 3 <= len(params) and params[i + 1] == "5":
                    i += 2
            elif p.isdigit() and 30 <= int(p) <= 37:
                fg = xterm256(int(p) - 30)
            i += 1

    def csi_move(seq):
        nonlocal x, y
        fin = seq[-1]
        parts = [p for p in seq[2:-1].decode("latin1").split(";")]
        n1 = int(parts[0]) if parts and parts[0].isdigit() else None
        n2 = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else None
        if fin == ord("H") or fin == ord("f"):
            y = (n1 or 1) - 1
            x = (n2 or 1) - 1
        elif fin == ord("A"):
            y = max(0, y - (n1 or 1))
        elif fin == ord("B"):
            y = min(rows - 1, y + (n1 or 1))
        elif fin == ord("C"):
            x += n1 or 1
        elif fin == ord("D"):
            x = max(0, x - (n1 or 1))
        elif fin == ord("G"):
            x = (n1 or 1) - 1
        elif fin == ord("d"):
            y = (n1 or 1) - 1
        elif fin == ord("J") or fin == ord("K"):
            mode = (parts[0] if parts else "") or "0"
            if fin == ord("K"):
                rng = range(x, cols) if mode in ("0", "") else range(cols)
                for xx in rng:
                    grid[y][xx] = blank
            else:
                if mode in ("2", "3"):
                    for yy in range(rows):
                        for xx in range(cols):
                            grid[yy][xx] = blank
                else:
                    for xx in range(x, cols):
                        grid[y][xx] = blank
                    for yy in range(y + 1, rows):
                        for xx in range(cols):
                            grid[yy][xx] = blank

    i, n = 0, len(data)
    while i < n:
        b = data[i]
        if b == 0x1B and i + 1 < n and data[i + 1] == ord("["):
            j = i + 2
            while j < n and not (0x40 <= data[j] <= 0x7E):
                j += 1
            if j >= n:
                break
            seq = data[i:j + 1]
            if seq[-1] == ord("m"):
                sgr(seq)
            elif seq[-1] in b"HfABCDEFGdJK":
                csi_move(seq)
            # everything else (modes, queries): no effect on the grid
            i = j + 1
        elif b == 0x0D:
            x = 0
            i += 1
        elif b == 0x0A:
            y += 1
            x = 0
            i += 1
        elif b == 0:
            i += 1
        elif 0x20 <= b < 0x7F:
            put(chr(b))
            i += 1
        elif b >= 0x80:
            # UTF-8 multi-byte glyph
            width = 1
            if b >= 0xF0:
                width = 4
            elif b >= 0xE0:
                width = 3
            elif b >= 0xC0:
                width = 2
            if i + width <= n:
                try:
                    put(data[i:i + width].decode("utf-8"))
                    i += width
                    continue
                except UnicodeDecodeError:
                    pass
            i += 1
        else:
            i += 1
    return grid


def render_grid(grid, cols, rows, accent, fontsize, font_path):
    font = ImageFont.truetype(font_path, fontsize)
    font_bold = None
    if os.path.exists(FONT_BOLD_DEFAULT):
        font_bold = ImageFont.truetype(FONT_BOLD_DEFAULT, fontsize)

    adv = int(round(font.getlength("M")))
    cw = adv
    ch = int(round(fontsize * 1.28))
    # vertical centering: DejaVu's ink box for 'M'
    lb, tb, rb, bb = font.getbbox("M")
    off_x = max(0, (cw - (rb - lb)) // 2 - lb)
    off_y = max(0, (ch - (bb - tb)) // 2 - tb)

    W, H = cw * cols, ch * rows
    img = make_background(W, H, accent)
    draw = ImageDraw.Draw(img)
    for y in range(rows):
        for x in range(cols):
            ch_, fg, bold = grid[y][x]
            if ch_ == " ":
                continue
            f = font_bold if (bold and font_bold) else font
            draw.text((x * cw + off_x, y * ch + off_y), ch_, font=f,
                      fill=fg or DEFAULT_FG)
    return img


def make_background(w, h, accent):
    """Dark vertical gradient + a few large soft glows, heavily blurred."""
    top, bottom = (9, 11, 16), (24, 28, 38)
    base = Image.new("RGB", (w, h))
    px = base.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(top[0] + (bottom[0] - top[0]) * t)
        g = int(top[1] + (bottom[1] - top[1]) * t)
        b = int(top[2] + (bottom[2] - top[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    spots = [
        (int(w * 0.18), int(h * 0.22), int(min(w, h) * 0.55), accent, 46),
        (int(w * 0.85), int(h * 0.80), int(min(w, h) * 0.70), accent, 34),
        (int(w * 0.60), int(h * 0.05), int(min(w, h) * 0.40), (90, 110, 160), 26),
    ]
    for cx, cy, rad, color, alpha in spots:
        gd.ellipse([cx - rad, cy - rad, cx + rad, cy + rad],
                   fill=(*color, alpha))
    glow = glow.filter(ImageFilter.GaussianBlur(120))
    return Image.alpha_composite(base.convert("RGBA"), glow).convert("RGB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bin")
    ap.add_argument("out")
    ap.add_argument("--cols", type=int, required=True)
    ap.add_argument("--rows", type=int, required=True)
    ap.add_argument("--theme", default="")
    ap.add_argument("--script", default="")
    ap.add_argument("--accent", default="#4a6fa5")
    ap.add_argument("--fontsize", type=int, default=22)
    ap.add_argument("--font", default=FONT_DEFAULT)
    ap.add_argument("--warmup", type=int, default=60)
    args = ap.parse_args()

    if not os.path.exists(args.font):
        sys.exit(f"font not found: {args.font}")
    accent = parse_hex(args.accent)

    # Temp config dir + test guard: never touch the real ~/.config.
    xdg = tempfile.mkdtemp(prefix="geth-f2p-")
    env = dict(os.environ)
    env["XDG_CONFIG_HOME"] = xdg
    env["GETH_TEST"] = "1"
    env["TERM"] = "xterm-256color"

    cmd = [args.bin, "--render-frame", f"{args.cols}x{args.rows}", "--ansi",
           "--demo", "--color", "truecolor", "--no-save",
           "--warmup", str(args.warmup)]
    if args.theme:
        cmd += ["--theme", args.theme]
    if args.script:
        cmd += ["--script", args.script]
    r = subprocess.run(cmd, capture_output=True, env=env)
    if r.returncode != 0:
        sys.exit(f"geth failed ({r.returncode}):\n{r.stderr.decode(errors='replace')}")

    grid = parse_ansi(r.stdout, args.cols, args.rows)
    img = render_grid(grid, args.cols, args.rows, accent, args.fontsize, args.font)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    img.save(args.out, "PNG", optimize=True)
    size = os.path.getsize(args.out)
    print(f"{args.out}: {args.cols}x{args.rows} cells -> {img.width}x{img.height} px, "
          f"{size/1024:.1f} KB {'' if size < 500 * 1024 else '*** TOO BIG (>=500 KB)'}")
    if size >= 500 * 1024:
        sys.exit(1)


if __name__ == "__main__":
    main()

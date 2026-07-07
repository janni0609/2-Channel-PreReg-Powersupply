#!/usr/bin/env python3
# Author + preview the "Option 1c" clean condensed big-digit font for the
# 2-channel PSU OLED readout, then emit a C header (BigDigits.h).
#
# Glyphs are authored as ASCII art (row-major, '#'=ink '.'=off) so they can be
# eyeballed. Digits are monospace (DW wide); '.' / space use a narrower advance.

H = 16                 # glyph cell height (rows); ink fills rows 0..15
BASELINE = 15          # bottom ink row -> screen baseline
DW = 11                # digit ink width

GLYPHS = {
'0': [
"...#####...",
"..#######..",
".###...###.",
".##.....##.",
"###.....###",
"###.....###",
"###.....###",
"###.....###",
"###.....###",
"###.....###",
"###.....###",
"###.....###",
".##.....##.",
".###...###.",
"..#######..",
"...#####...",
],
'1': [
"....###....",
"...####....",
"..#####....",
".##.###....",
"##..###....",
"....###....",
"....###....",
"....###....",
"....###....",
"....###....",
"....###....",
"....###....",
"....###....",
"..#######..",
"..#######..",
"..#######..",
],
'2': [
"..######...",
".########..",
"##.....###.",
"##......###",
"........###",
".......###.",
"......###..",
".....###...",
"....###....",
"...###.....",
"..###......",
".###.......",
"###........",
"##########.",
"##########.",
"##########.",
],
'3': [
".########..",
".########..",
"......###..",
"......###..",
"......###..",
".....####..",
"..######...",
"..######...",
".....####..",
"......###..",
"......###..",
"......###..",
"##.....###.",
"###...###..",
".########..",
"..######...",
],
'4': [
"......###..",
".....####..",
"....##.##..",
"...##..##..",
"..##...##..",
".##....##..",
"##.....##..",
"##########.",
"##########.",
".......##..",
".......##..",
".......##..",
".......##..",
".......##..",
".......##..",
".......##..",
],
'5': [
".#########.",
".#########.",
".##........",
".##........",
".##........",
".#######...",
".########..",
"......###..",
".......###.",
".......###.",
".......###.",
".......###.",
"##.....###.",
"###...###..",
".########..",
"..######...",
],
'6': [
"...######..",
"..########.",
".###.......",
".##........",
"###........",
"###........",
"###.####...",
"##########.",
"####...###.",
"###.....###",
"###.....###",
"###.....###",
".##.....##.",
".###...###.",
"..#######..",
"...#####...",
],
'7': [
"##########.",
"##########.",
"##########.",
".......###.",
"......###..",
"......###..",
".....###...",
".....###...",
"....###....",
"....###....",
"...###.....",
"...###.....",
"..###......",
"..###......",
".###.......",
".###.......",
],
'8': [
"..######...",
".########..",
"###....###.",
"##......##.",
"##......##.",
".###..###..",
"..######...",
"..######...",
".###..###..",
"###....###.",
"##......##.",
"##......##.",
"##......##.",
"###....###.",
".########..",
"..######...",
],
'9': [
"...#####...",
"..#######..",
".###...###.",
"###.....###",
"###.....###",
"###.....###",
".###...####",
"..########.",
".......###.",
".......###.",
"......###..",
"......###..",
".....###...",
".....###...",
"....###....",
"...###.....",
],
'.': [
"....",
"....",
"....",
"....",
"....",
"....",
"....",
"....",
"....",
"....",
"....",
"....",
".##.",
"###.",
"###.",
".##.",
],
'-': [
"...........",
"...........",
"...........",
"...........",
"...........",
"...........",
"..######...",
"..######...",
"...........",
"...........",
"...........",
"...........",
"...........",
"...........",
"...........",
"...........",
],
' ': ["......" for _ in range(H)],
}

def advance(ch):
    if ch == '.':
        return 5
    if ch == ' ':
        return 6
    return DW + 1        # digits: 11 ink + 1 spacing

def str_width(s):
    return sum(advance(c) for c in s)

class Canvas:
    def __init__(self, w=256, h=64):
        self.w, self.h = w, h
        self.buf = [[0]*w for _ in range(h)]
    def px(self, x, y, v):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.buf[y][x] = v
    def hline(self, x0, x1, y, v):
        for x in range(x0, x1+1): self.px(x, y, v)
    def vline(self, x, y0, y1, v):
        for y in range(y0, y1+1): self.px(x, y, v)
    def big(self, x, ytop, s, v=15):
        for ch in s:
            g = GLYPHS[ch]
            for r,row in enumerate(g):
                for c,px in enumerate(row):
                    if px == '#':
                        self.px(x+c, ytop+r, v)
            x += advance(ch)
        return x
    def show(self, x0=0, x1=256):
        for y in range(self.h):
            line=[]
            for x in range(x0, min(x1,self.w)):
                v=self.buf[y][x]
                line.append('#' if v>=12 else ('+' if v>=6 else ('.' if v>=1 else ' ')))
            print("".join(line))

def cname(ch):
    return {'0':'D0','1':'D1','2':'D2','3':'D3','4':'D4','5':'D5','6':'D6',
            '7':'D7','8':'D8','9':'D9','.':'DOT','-':'DASH',' ':'SPACE'}[ch]

def emit_header(path):
    order = ['0','1','2','3','4','5','6','7','8','9','.','-',' ']
    lines = []
    ap = lines.append
    ap("#pragma once")
    ap("#include <SSD1322.h>")
    ap("")
    ap("// ============================================================================")
    ap("//  BigDigits - clean condensed 16px numeric font (\"Option 1c\") for the PSU")
    ap("//  main-readout big numbers. Covers '0'-'9', '.', '-', ' '. Digits are")
    ap("//  monospace so tabular values stay column-aligned; '.'/'-'/' ' are narrower.")
    ap("//  Generated by tools/bigfont.py - edit the glyph art there, not here,")
    ap("//  then re-run: python tools/bigfont.py  (writes src/BigDigits.h).")
    ap("//")
    ap("//  Each glyph is HEIGHT rows; row bit c (LSB = leftmost column) is ink.")
    ap("//  Draw left-anchored at the glyph-cell TOP row; the ink baseline is row")
    ap("//  BASELINE, so place text with yTop = baselineY - BigDigits::BASELINE.")
    ap("// ============================================================================")
    ap("namespace BigDigits {")
    ap(f"static const int HEIGHT   = {H};")
    ap(f"static const int BASELINE = {BASELINE};  // bottom ink row within the cell")
    ap("")
    # glyph bitmaps
    for ch in order:
        g = GLYPHS[ch]
        w = len(g[0])
        vals = []
        for row in g:
            bits = 0
            for c, px in enumerate(row):
                if px == '#':
                    bits |= (1 << c)
            vals.append(bits)
        arr = ", ".join(f"0x{v:04X}" for v in vals)
        ap(f"static const uint16_t {cname(ch)}[{H}] = {{ {arr} }};")
    ap("")
    ap("struct Glyph { uint8_t width; uint8_t advance; const uint16_t* rows; };")
    ap("")
    ap("// Look up a glyph by character; unsupported chars fall back to a blank space.")
    ap("inline Glyph glyphFor(char ch) {")
    for ch in order:
        g = GLYPHS[ch]
        w = len(g[0])
        adv = advance(ch)
        lit = "' '" if ch == ' ' else (f"'{ch}'")
        ap(f"    if (ch == {lit}) return {{ {w}, {adv}, {cname(ch)} }};")
    ap(f"    return {{ 6, 6, SPACE }};")
    ap("}")
    ap("")
    ap("// Total advance width in pixels of string s (for right-alignment).")
    ap("inline int textWidth(const char* s) {")
    ap("    int w = 0;")
    ap("    for (; *s; ++s) w += glyphFor(*s).advance;")
    ap("    return w;")
    ap("}")
    ap("")
    ap("// Draw s left-anchored with its cell top at (x, yTop) in the given grey level.")
    ap("inline void draw(SSD1322& d, int x, int yTop, const char* s, uint8_t color) {")
    ap("    for (; *s; ++s) {")
    ap("        Glyph g = glyphFor(*s);")
    ap("        for (int r = 0; r < HEIGHT; ++r) {")
    ap("            uint16_t bits = g.rows[r];")
    ap("            for (int c = 0; c < g.width; ++c)")
    ap("                if (bits & (1u << c)) d.setPixel(x + c, yTop + r, color);")
    ap("        }")
    ap("        x += g.advance;")
    ap("    }")
    ap("}")
    ap("")
    ap("// Draw s right-anchored so its rightmost advance column sits at x=rightX.")
    ap("inline void drawRight(SSD1322& d, int rightX, int yTop, const char* s, uint8_t color) {")
    ap("    draw(d, rightX + 1 - textWidth(s), yTop, s, color);")
    ap("}")
    ap("")
    ap("}  // namespace BigDigits")
    ap("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines))
    print(f"wrote {path}")

def main():
    for k,g in GLYPHS.items():
        assert len(g)==H, f"{k!r} has {len(g)} rows"
        w=len(g[0])
        for row in g:
            assert len(row)==w, f"{k!r} ragged: {row!r}"

    cv = Canvas()
    def draw_channel(CX, v_meas, i_meas):
        vs = f"{v_meas:.3f}"
        cv.big(CX+113-str_width(vs), 32-BASELINE, vs, 15)   # baseline y=32
        cs = f"{i_meas:.4f}"
        cv.big(CX+113-str_width(cs), 50-BASELINE, cs, 15)   # baseline y=50
        cv.hline(CX+5, CX+121, 52, 5)
    cv.vline(127, 6, 57, 5)
    draw_channel(0,   12.472, 0.8231)
    draw_channel(128, 4.021,  2.0000)
    cv.show()
    print()
    cv2 = Canvas(w=200, h=H)
    cv2.big(2, 0, "0123456789.-", 15)
    cv2.show(0, 160)

    import os
    # This script lives in <project>/tools/; the header ships in <project>/src/.
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "src", "BigDigits.h")
    emit_header(os.path.normpath(out))

if __name__ == "__main__":
    main()

#include "SSD1322.h"
#include <string.h>

// -- Font ------------------------------------------------------------------------
// 5x7 bitmap font covering the 95 printable ASCII characters (0x20 ' ' - 0x7E '~').
// Each entry holds 5 column bytes for one character.  Within each byte bit 0
// is the top pixel and bit 6 is the bottom pixel (LSB = top row).
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 0x20 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 0x21 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 0x22 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 0x23 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 0x24 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 0x25 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 0x26 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 0x27 '\''
    {0x00,0x1C,0x22,0x41,0x00}, // 0x28 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 0x29 ')'
    {0x14,0x08,0x3E,0x08,0x14}, // 0x2A '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 0x2B '+'
    {0x00,0x50,0x30,0x00,0x00}, // 0x2C ','
    {0x08,0x08,0x08,0x08,0x08}, // 0x2D '-'
    {0x00,0x60,0x60,0x00,0x00}, // 0x2E '.'
    {0x20,0x10,0x08,0x04,0x02}, // 0x2F '/'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x30 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 0x31 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 0x32 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 0x33 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 0x34 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 0x35 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 0x36 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 0x37 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 0x38 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 0x39 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 0x3A ':'
    {0x00,0x56,0x36,0x00,0x00}, // 0x3B ';'
    {0x08,0x14,0x22,0x41,0x00}, // 0x3C '<'
    {0x14,0x14,0x14,0x14,0x14}, // 0x3D '='
    {0x00,0x41,0x22,0x14,0x08}, // 0x3E '>'
    {0x02,0x01,0x51,0x09,0x06}, // 0x3F '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 0x40 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 0x41 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 0x42 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 0x43 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 0x44 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 0x45 'E'
    {0x7F,0x09,0x09,0x09,0x01}, // 0x46 'F'
    {0x3E,0x41,0x49,0x49,0x7A}, // 0x47 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 0x48 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 0x49 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 0x4A 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 0x4B 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 0x4C 'L'
    {0x7F,0x02,0x0C,0x02,0x7F}, // 0x4D 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 0x4E 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 0x4F 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 0x50 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 0x51 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 0x52 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 0x53 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 0x54 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 0x55 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 0x56 'V'
    {0x3F,0x40,0x38,0x40,0x3F}, // 0x57 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 0x58 'X'
    {0x07,0x08,0x70,0x08,0x07}, // 0x59 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 0x5A 'Z'
    {0x00,0x7F,0x41,0x41,0x00}, // 0x5B '['
    {0x02,0x04,0x08,0x10,0x20}, // 0x5C '\'
    {0x00,0x41,0x41,0x7F,0x00}, // 0x5D ']'
    {0x04,0x02,0x01,0x02,0x04}, // 0x5E '^'
    {0x40,0x40,0x40,0x40,0x40}, // 0x5F '_'
    {0x00,0x01,0x02,0x04,0x00}, // 0x60 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 0x61 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 0x62 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 0x63 'c'
    {0x38,0x44,0x44,0x48,0x7F}, // 0x64 'd'
    {0x38,0x54,0x54,0x54,0x18}, // 0x65 'e'
    {0x08,0x7E,0x09,0x01,0x02}, // 0x66 'f'
    {0x0C,0x52,0x52,0x52,0x3E}, // 0x67 'g'
    {0x7F,0x08,0x04,0x04,0x78}, // 0x68 'h'
    {0x00,0x44,0x7D,0x40,0x00}, // 0x69 'i'
    {0x20,0x40,0x44,0x3D,0x00}, // 0x6A 'j'
    {0x7F,0x10,0x28,0x44,0x00}, // 0x6B 'k'
    {0x00,0x41,0x7F,0x40,0x00}, // 0x6C 'l'
    {0x7C,0x04,0x18,0x04,0x78}, // 0x6D 'm'
    {0x7C,0x08,0x04,0x04,0x78}, // 0x6E 'n'
    {0x38,0x44,0x44,0x44,0x38}, // 0x6F 'o'
    {0x7C,0x14,0x14,0x14,0x08}, // 0x70 'p'
    {0x08,0x14,0x14,0x18,0x7C}, // 0x71 'q'
    {0x7C,0x08,0x04,0x04,0x08}, // 0x72 'r'
    {0x48,0x54,0x54,0x54,0x20}, // 0x73 's'
    {0x04,0x3F,0x44,0x40,0x20}, // 0x74 't'
    {0x3C,0x40,0x40,0x40,0x3C}, // 0x75 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, // 0x76 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, // 0x77 'w'
    {0x44,0x28,0x10,0x28,0x44}, // 0x78 'x'
    {0x0C,0x50,0x50,0x50,0x3E}, // 0x79 'y'
    {0x44,0x64,0x54,0x4C,0x44}, // 0x7A 'z'
    {0x00,0x08,0x36,0x41,0x00}, // 0x7B '{'
    {0x00,0x00,0x7F,0x00,0x00}, // 0x7C '|'
    {0x00,0x41,0x36,0x08,0x00}, // 0x7D '}'
    {0x10,0x08,0x08,0x10,0x08}, // 0x7E '~'
};

// -- Constructor -----------------------------------------------------------------

SSD1322::SSD1322(SPIClassRP2040 &spi,
                 uint8_t pinCS, uint8_t pinDC, uint8_t pinRES,
                 uint8_t pinCLK, uint8_t pinDIN)
    : _spi(spi),
      _pinCS(pinCS), _pinDC(pinDC), _pinRES(pinRES),
      _pinCLK(pinCLK), _pinDIN(pinDIN)
{}

// -- Private SPI helpers ---------------------------------------------------------

// Pull DC low (command mode), assert CS, clock out the byte, deassert CS.
void SSD1322::sendCommand(uint8_t cmd) {
    digitalWrite(_pinDC, LOW);
    digitalWrite(_pinCS, LOW);
    _spi.transfer(cmd);
    digitalWrite(_pinCS, HIGH);
}

// Pull DC high (data mode), assert CS, clock out the byte, deassert CS.
void SSD1322::sendData(uint8_t data) {
    digitalWrite(_pinDC, HIGH);
    digitalWrite(_pinCS, LOW);
    _spi.transfer(data);
    digitalWrite(_pinCS, HIGH);
}

// Program the GDDRAM write window (0x15 = column address, 0x75 = row address,
// 0x5C = write RAM).  colStart/colEnd are display-relative column units;
// COL_OFFSET is added to convert to the hardware column address.
// One column unit spans 4 pixels (2 bytes) in the framebuffer.
void SSD1322::setWindow(uint8_t colStart, uint8_t colEnd,
                        uint8_t rowStart, uint8_t rowEnd) {
    sendCommand(0x15);
    sendData(COL_OFFSET + colStart);
    sendData(COL_OFFSET + colEnd);
    sendCommand(0x75);
    sendData(rowStart);
    sendData(rowEnd);
    sendCommand(0x5C);  // enter write-RAM mode; data bytes follow immediately
}

// -- Public API ------------------------------------------------------------------

void SSD1322::begin() {
    // Configure the manually-driven control pins with safe idle states.
    pinMode(_pinCS,  OUTPUT); digitalWrite(_pinCS,  HIGH);  // deasserted
    pinMode(_pinDC,  OUTPUT); digitalWrite(_pinDC,  LOW);
    pinMode(_pinRES, OUTPUT); digitalWrite(_pinRES, HIGH);  // not in reset

    // Bind SPI to the display clock/data pins and open the bus.  RX (MISO) is
    // parked on an unused, SPI1-capable pin (GPIO8) because the display is
    // write-only; CS is driven manually above, so hardware CS stays disabled.
    _spi.setSCK(_pinCLK);
    _spi.setTX(_pinDIN);
    _spi.setRX(8);
    _spi.begin();
    _spi.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));

    // Let the panel's VDD/VCC rails settle before touching RES. On a cold or
    // brown-out reset the RP2350 boots faster than the OLED supply ramps; on a
    // warm reset the rails are already up. This delay covers the cold case and
    // is what makes startup reliable regardless of how the MCU was reset.
    delay(100);

    // Hardware reset: hold RES low long enough to discharge any RC/cap network
    // the breakout board places on the reset line (a 1 ms pulse can leave it
    // half-charged and skip the reset entirely), then release and wait for the
    // internal voltage regulator to stabilise (>=50 ms per datasheet).
    digitalWrite(_pinRES, LOW);  delay(10);
    digitalWrite(_pinRES, HIGH); delay(100);

    // Initialisation sequence (refer to SSD1322 datasheet for register details).
    sendCommand(0xFD); sendData(0x12);          // unlock command interface
    sendCommand(0xAE);                           // display off (sleep mode)
    sendCommand(0xB3); sendData(0x91);          // set display clock: divider=1, oscillator=0x9
    sendCommand(0xCA); sendData(0x3F);          // multiplex ratio = 64 COM lines
    sendCommand(0xA2); sendData(0x00);          // display offset = 0 rows
    sendCommand(0xA1); sendData(0x00);          // display start line = 0
    sendCommand(0xA0); sendData(0x14); sendData(0x11); // remap: nibble remap + COM split
    sendCommand(0xB5); sendData(0x00);          // GPIO pins disabled
    sendCommand(0xAB); sendData(0x01);          // internal VDD regulator enabled
    sendCommand(0xB4); sendData(0xA0); sendData(0xFD); // display enhancement A
    sendCommand(0xC1); sendData(0xFF);          // contrast = maximum (255)
    sendCommand(0xC7); sendData(0x0F);          // master contrast = maximum (15)
    sendCommand(0xB9);                           // use default linear grey-scale table
    sendCommand(0xB1); sendData(0xE2);          // phase 1 = 5 clocks, phase 2 = 14 clocks
    sendCommand(0xD1); sendData(0xA2); sendData(0x20); // display enhancement B
    sendCommand(0xBB); sendData(0x1F);          // pre-charge voltage = 0.6 x VCC
    sendCommand(0xB6); sendData(0x08);          // second pre-charge period = 8 clocks
    sendCommand(0xBE); sendData(0x07);          // COM deselect voltage = 0.86 x VCC
    sendCommand(0xA6);                           // normal display (not inverted)
    sendCommand(0xA9);                           // exit partial display mode
    sendCommand(0xAF);                           // display on
    delay(150);                                  // let panel VCC stabilise before drawing
}

// Fill the entire framebuffer with one grey level (call flush() to show it).
void SSD1322::clear(uint8_t gray4) {
    // Pack the nibble into both halves of every byte (two pixels per byte).
    memset(_framebuf, (gray4 << 4) | gray4, sizeof(_framebuf));
}

// Transfer the full 8 192-byte framebuffer to the display in one SPI burst.
void SSD1322::flush() {
    setWindow(0, 63, 0, HEIGHT - 1);
    digitalWrite(_pinDC, HIGH);
    digitalWrite(_pinCS, LOW);
    for (uint16_t i = 0; i < sizeof(_framebuf); i++)
        _spi.transfer(_framebuf[i]);
    digitalWrite(_pinCS, HIGH);
}

// Transfer only the rows and columns inside the given rectangle, reducing SPI
// traffic when most of the screen is static.
//
// The SSD1322 column address unit covers 4 pixels (2 bytes), so x and w are
// snapped outward to the nearest 4-pixel boundary before the window is set.
// The caller does not need to pre-align; any (x, w) pair is accepted.
void SSD1322::flushRect(int x, int y, int w, int h) {
    int x0 = x & ~3;                    // round x down to 4-pixel boundary
    int x1 = (x + w + 3) & ~3;         // round x+w up to 4-pixel boundary
    uint8_t colStart  = (uint8_t)(x0 / 4);
    uint8_t colEnd    = (uint8_t)(x1 / 4 - 1);
    uint8_t rowEnd    = (uint8_t)(y + h - 1);
    int     byteStart = x0 / 2;         // byte offset within each framebuffer row
    int     byteCount = (x1 - x0) / 2; // bytes per row to send

    setWindow(colStart, colEnd, (uint8_t)y, rowEnd);
    digitalWrite(_pinDC, HIGH);
    digitalWrite(_pinCS, LOW);
    for (int row = y; row <= (int)rowEnd; row++) {
        const uint8_t *p = _framebuf + row * (WIDTH / 2) + byteStart;
        for (int b = 0; b < byteCount; b++)
            _spi.transfer(p[b]);
    }
    digitalWrite(_pinCS, HIGH);
}

// -- Pixel -----------------------------------------------------------------------

// With nibble remapping enabled (A[2]=1 in the remap register), the GDDRAM
// layout is sequential: the upper nibble of each byte holds the even-x pixel
// and the lower nibble holds the odd-x pixel.
void SSD1322::setPixel(int x, int y, uint8_t gray4) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    uint16_t idx = (uint16_t)y * (WIDTH / 2) + x / 2;
    if (x & 1)
        _framebuf[idx] = (_framebuf[idx] & 0xF0) | (gray4 & 0x0F);  // odd x -> lower nibble
    else
        _framebuf[idx] = (_framebuf[idx] & 0x0F) | (gray4 << 4);    // even x -> upper nibble
}

// -- Lines -----------------------------------------------------------------------

// Integer Bresenham line algorithm - works for all octants.
void SSD1322::drawLine(int x0, int y0, int x1, int y1, uint8_t color) {
    int dx =  abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        setPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// -- Rectangles ------------------------------------------------------------------

// Draws four lines forming a hollow rectangle outline.
void SSD1322::drawRect(int x, int y, int w, int h, uint8_t color) {
    drawLine(x,         y,         x + w - 1, y,         color);  // top
    drawLine(x,         y + h - 1, x + w - 1, y + h - 1, color);  // bottom
    drawLine(x,         y,         x,         y + h - 1, color);  // left
    drawLine(x + w - 1, y,         x + w - 1, y + h - 1, color);  // right
}

void SSD1322::fillRect(int x, int y, int w, int h, uint8_t color) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            setPixel(col, row, color);
}

// -- Text ------------------------------------------------------------------------

// Render one character at (x, y).  Characters outside the printable ASCII range
// are replaced with '?'.  Each character is 5 px wide + 1 px spacing column.
void SSD1322::drawChar(int x, int y, char c, uint8_t fg, uint8_t bg) {
    if (c < 0x20 || c > 0x7E) c = '?';
    const uint8_t *g = font5x7[(uint8_t)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            setPixel(x + col, y + row, (bits >> row) & 1 ? fg : bg);
    }
    // Clear the trailing spacing column so characters don't bleed into each other.
    for (int row = 0; row < 7; row++)
        setPixel(x + 5, y + row, bg);
}

// Render a null-terminated string; each character advances x by 6 px.
void SSD1322::drawText(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    while (*s) { drawChar(x, y, *s++, fg, bg); x += 6; }
}

// Render a string with each font pixel blown up to a scale x scale block.
// At scale=2: character width = 12 px, character height = 14 px.
// The spacing column is also scaled so character pitch = 6 x scale px.
void SSD1322::drawTextScaled(int x, int y, const char *s, uint8_t fg, uint8_t bg, uint8_t scale) {
    while (*s) {
        char c = *s++;
        if (c < 0x20 || c > 0x7E) c = '?';
        const uint8_t *g = font5x7[(uint8_t)c - 0x20];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                uint8_t color = (bits >> row) & 1 ? fg : bg;
                // Expand each source pixel into a scale x scale block.
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        setPixel(x + col*scale + sx, y + row*scale + sy, color);
            }
        }
        // Clear the scaled spacing column (scale px wide, 7 x scale px tall).
        for (int sc = 0; sc < scale; sc++)
            for (int row = 0; row < 7*scale; row++)
                setPixel(x + 5*scale + sc, y + row, bg);
        x += 6 * scale;
    }
}

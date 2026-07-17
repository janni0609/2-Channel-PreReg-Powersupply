#pragma once
#include <Arduino.h>
#include <SPI.h>

// Driver for the Solomon Systech SSD1322 256x64 4-bit grayscale OLED controller,
// driven over 4-wire SPI. All drawing calls write into an 8 192-byte RAM
// framebuffer; nothing appears on screen until flush() or flushRect() transfers
// the data over SPI.
//
// Colour values are 4-bit grayscale (0 = off, 15 = maximum brightness).
//
// This variant targets the RP2350 (Waveshare RP2350-Tiny brain) and lets the
// caller pick which SPI peripheral to use. On the RP2350 the front-panel display
// clock/data land on GPIO10/GPIO11, which are the SPI1 SCK/TX functions, so the
// brain passes the SPI1 instance here.
//
// The OLED module ties R/W and E/RD on-board for 4-wire SPI, so those pins are
// not driven by the MCU and are absent from this API.
class SSD1322 {
public:
    static const int WIDTH  = 256;  // visible pixels horizontally
    static const int HEIGHT = 64;   // visible pixels vertically

    // spi : the SPIClassRP2040 peripheral wired to pinCLK/pinDIN (SPI1 on the brain).
    // Pins: CS=chip-select, DC=data/command, RES=reset,
    //       CLK=SPI clock, DIN=SPI MOSI.
    SSD1322(SPIClassRP2040 &spi,
            uint8_t pinCS, uint8_t pinDC, uint8_t pinRES,
            uint8_t pinCLK, uint8_t pinDIN);

    // Configure GPIO, start SPI at 10 MHz, reset the chip and send the full
    // initialisation sequence.  Must be called once before any other method.
    void begin();

    // Fill the entire framebuffer with gray4 (0-15).  Call flush() to show it.
    void clear(uint8_t gray4 = 0);

    // Push the complete 8 192-byte framebuffer to the display over SPI.
    void flush();

    // Push only the pixels inside the rectangle (x, y, w, h) to the display.
    // x and w are automatically aligned outward to the nearest 4-pixel boundary
    // required by the SSD1322 column-address scheme.  Use this instead of flush()
    // when only a small region of the framebuffer has changed.
    void flushRect(int x, int y, int w, int h);

    // -- Framebuffer primitives -------------------------------------------------
    // All primitives write into the local framebuffer only.
    // Call flush() or flushRect() afterwards to make changes visible.

    // Write a single pixel.  Out-of-bounds coordinates are silently ignored.
    void setPixel(int x, int y, uint8_t gray4);

    // Bresenham line between (x0,y0) and (x1,y1).
    void drawLine(int x0, int y0, int x1, int y1, uint8_t color);

    // Hollow rectangle outline.
    void drawRect(int x, int y, int w, int h, uint8_t color);

    // Filled rectangle.
    void fillRect(int x, int y, int w, int h, uint8_t color);

    // Draw a null-terminated string using the built-in 5x7 font.
    // Each character occupies 6 px (5 data + 1 spacing column).
    void drawText(int x, int y, const char *s, uint8_t fg, uint8_t bg = 0);

    // Same as drawText but each font pixel is rendered as a scale x scale block.
    // At scale=2 each character is 12 px wide and 14 px tall.
    void drawTextScaled(int x, int y, const char *s, uint8_t fg, uint8_t bg, uint8_t scale);

private:
    // The SSD1322 GDDRAM starts at hardware column 0x1C for this display module,
    // mapping hardware column 0x1C -> visible pixel 0.
    static const uint8_t COL_OFFSET = 0x1C;

    SPIClassRP2040 &_spi;
    uint8_t _pinCS, _pinDC, _pinRES, _pinCLK, _pinDIN;

    // 4-bit grayscale: 2 pixels per byte, 128 bytes per row x 64 rows = 8 192 bytes.
    // Upper nibble = even pixel (x even), lower nibble = odd pixel (x odd).
    uint8_t _framebuf[WIDTH / 2 * HEIGHT];

    // Send a command byte (DC low).
    void sendCommand(uint8_t cmd);

    // Send a data byte (DC high).
    void sendData(uint8_t data);

    // Set the GDDRAM write window using SSD1322 column/row address commands.
    // colStart/colEnd are in display-relative column units (1 unit = 4 pixels = 2 bytes).
    // The hardware offset COL_OFFSET is added internally.
    void setWindow(uint8_t colStart, uint8_t colEnd, uint8_t rowStart, uint8_t rowEnd);

    // Render one character from font5x7 into the framebuffer.
    void drawChar(int x, int y, char c, uint8_t fg, uint8_t bg);
};

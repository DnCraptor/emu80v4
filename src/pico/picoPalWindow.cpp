#include "picoPalWindow.h"
#include "graphics.h"

void PalWindow::bringToFront() {
// TODO:
}

void PalWindow::applyParams() {
    /// TODO:
}

void PalWindow::drawEnd() {
    /// TODO:
}

void PalWindow::drawImage(uint32_t* pixels, int imageWidth, int imageHeight, double aspectRatio, bool blend, bool useAlpha) {
    /// TODO:
}

void PalWindow::drawFill(uint32_t color)
{
    uint8_t red = (color & 0xFF0000) >> 16;
    uint8_t green = (color & 0xFF00) >> 8;
    uint8_t blue = color & 0xFF;
    /// TODO:
}

void PalWindow::getSize(int& width, int& height)
{
    width = DISP_WIDTH;
    height = DISP_HEIGHT;
}

PalWindow::PalWindow()
{
    /// TODO:
}

PalWindow::~PalWindow()
{
    /// TODO:
}

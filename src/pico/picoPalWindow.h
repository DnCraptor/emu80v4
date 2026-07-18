#ifndef PIOCPALWINDOW_H
#define PIOCPALWINDOW_H

#include <string>
#include <stdint.h>

class PalWindow
{
    public:
    struct PalWindowParams {
        bool visible;
    };

        PalWindow();
        virtual ~PalWindow();
        void initPalWindow() {}

        virtual void calcDstRect(int srcWidth, int srcHeight,  double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY) = 0;

    protected:
///        SDL_Window* m_window = nullptr;
///        SDL_Renderer* m_renderer = nullptr;
        PalWindowParams m_params;

        void getSize(int& width, int& height);
        void applyParams();

        void drawFill(uint32_t color);
        void drawImage(uint32_t* pixels, int imageWidth, int imageHeight, double aspectratio,
                       bool blend = false, bool useAlpha = false);
        void drawEnd();
        void screenshotRequest(const std::string& ssFileName);

};


#endif // PICOPALWINDOW_H

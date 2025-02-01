#ifndef PIOCPALWINDOW_H
#define PIOCPALWINDOW_H

#include <string>
#include <map>

#include "../EmuTypes.h"
#include "../PalKeys.h"

class DebugWindow;

class PalWindow
{
    public:
    virtual DebugWindow* asDebugWindow() { return nullptr; }

    enum PalWindowStyle {
        PWS_FIXED,
        PWS_RESIZABLE,
        PWS_FULLSCREEN
    };

    struct PalWindowParams {
        PalWindowStyle style;
        SmoothingType smoothing;
        bool vsync;
        bool visible;
        int width;
        int height;
        std::string title;
    };

        PalWindow();
        virtual ~PalWindow();
        void initPalWindow() {}

        static PalWindow* windowById(uint32_t id);
        void bringToFront();
        void maximize();
        void focusChanged(bool isFocused);

        virtual void mouseClick(int x, int y, PalMouseKey key) {}
        virtual void mouseDrag(int x, int y) = 0;

        virtual std::string getPlatformObjectName() = 0;
        EmuWindowType getWindowType() {return m_windowType;}
        virtual void calcDstRect(int srcWidth, int srcHeight,  double srcAspectRatio, int wndWidth, int wndHeight, int& dstWidth, int& dstHeight, int& dstX, int& dstY) = 0;

    protected:
///        SDL_Window* m_window = nullptr;
///        SDL_Renderer* m_renderer = nullptr;
        PalWindowParams m_params;

        void setTitle(const std::string& title);
        void getSize(int& width, int& height);
        void applyParams();

        void drawFill(uint32_t color);
        void drawImage(uint32_t* pixels, int imageWidth, int imageHeight, double aspectratio,
                       bool blend = false, bool useAlpha = false);
        void drawEnd();
        void screenshotRequest(const std::string& ssFileName);

        EmuWindowType m_windowType = EWT_UNDEFINED;
};


#endif // PICOPALWINDOW_H

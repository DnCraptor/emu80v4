#ifndef PIOCPALWINDOW_H
#define PIOCPALWINDOW_H

#include <string>
#include <stdint.h>

class PalWindow
{
    public:
        virtual ~PalWindow() = default;

    protected:
        void screenshotRequest(const std::string& ssFileName);
};

#endif // PICOPALWINDOW_H

#ifndef PICOMENU_H
#define PICOMENU_H

#include "../PalKeys.h"

void palOpenMainMenu();
void palCloseMainMenu();
bool palMainMenuIsOpen();
bool palMainMenuHandleKey(PalKeyCode keyCode, bool isPressed);

#endif // PICOMENU_H

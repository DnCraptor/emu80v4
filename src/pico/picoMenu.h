#ifndef PICOMENU_H
#define PICOMENU_H

#include "../PalKeys.h"

void palOpenMainMenu();
void palCloseMainMenu();
bool palMainMenuIsOpen();
bool palMainMenuHandleKey(PalKeyCode keyCode, bool isPressed);

// Отсчёт автоповтора навигации. Вызывается из основного цикла: событий
// повтора ни HID-, ни PS/2-слой не присылают.
void palMainMenuTick();

// Модальное сообщение в общем стиле меню: по центру, с заголовком и тенью.
// Ожидание клавиши остаётся на вызывающем.
void palMessageBox(const char* title, const char* text);

#endif // PICOMENU_H

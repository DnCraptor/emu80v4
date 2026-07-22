#ifndef PICOMENU_H
#define PICOMENU_H

#include "../PalKeys.h"

char* appendText(char* dst, const char* text);
char* appendUnsigned(char* dst, unsigned value);

void palOpenMainMenu();
void palCloseMainMenu();
bool palMainMenuIsOpen();
bool palMainMenuHandleKey(PalKeyCode keyCode, bool isPressed);

// Отсчёт автоповтора навигации. Вызывается из основного цикла: событий
// повтора ни HID-, ни PS/2-слой не присылают.
void palMainMenuTick();

// Пока меню открыто, кнопки сдвига двигают его, а не картинку
void palMainMenuShift(int dx, int dy);

// Автоповтор для клавиш сдвига; реализация в Main.cpp
void palShiftKeysTick();

// Опрос клавиатуры и USB из основного цикла: нужен, когда ядро 1 занято
// кодированием HDMI. Реализация в Main.cpp.
void palInputTick();
int palMainMenuOriginX();
int palMainMenuOriginY();

// Модальное сообщение в общем стиле меню: по центру, с заголовком и тенью.
// Ожидание клавиши остаётся на вызывающем.
void palMessageBox(const char* title, const char* text);

#endif // PICOMENU_H

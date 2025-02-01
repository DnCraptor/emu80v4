﻿/*
 *  Emu80 v. 4.x
 *  © Viktor Pykhonin <pyk@mail.ru>, 2017-2024
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Platform Abstraction Layer, PalFile class


#ifdef PAL_QT
  #include "qt/qtPalFile.h"
#endif // PAL_QT

#ifdef PORT_VERSION
  #include "pico/ffPalFile.h"
#endif // PORT_VERSION

#ifdef PAL_SDL
  #ifndef PAL_WASM
    #include "sdl/sdlPalFile.h"
  #else
    #include "wasm/wasmPalFile.h"
  #endif // !PAL_WASM
#endif // PAL_SDL

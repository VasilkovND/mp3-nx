// mp3-nx - the port's options menu (ZL + ZR + Minus): see mp3_menu.cpp.

#pragma once

namespace rex {
class Runtime;
}

namespace mp3::menu {

// Loads the saved options, takes the controller filter and shows the menu when asked; call once the runtime exists.
void Instalar(rex::Runtime& runtime);

}  // namespace mp3::menu

#pragma once

#include <SDL3/SDL.h>
#include <cstdint>

namespace MacWindow {
int activeDisplayCount();
bool hasKeyboardFocus(SDL_Window* window);
bool fullscreenTopInset(Uint32 displayId, int* top);
void logGeometry(SDL_Window* window);
int unobscuredToolbarLeft(SDL_Window* window, int currentLeft, int toolbarWidth);
void tabletCursor(SDL_Window* window, const unsigned char* pixels, unsigned width,
                  unsigned height, unsigned hotX, unsigned hotY, std::uint64_t generation,
                  int x, int y, bool visible);
void hideTabletCursor(SDL_Window* window);
void showReconnectStatus(SDL_Window* window, const char* text, bool warning);
void hideReconnectStatus(SDL_Window* window);
void openInputMonitoringSettings();
}

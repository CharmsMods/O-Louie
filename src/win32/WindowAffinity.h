#pragma once

#include <windows.h>

#include <string>

namespace olouie::win32 {

bool TryExcludeFromCapture(HWND window, std::wstring* error);

}  // namespace olouie::win32

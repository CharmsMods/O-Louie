#include "win32/WindowAffinity.h"

#include <string>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace olouie::win32 {

bool TryExcludeFromCapture(HWND window, std::wstring* error) {
  if (window == nullptr) {
    if (error != nullptr) {
      *error = L"SetWindowDisplayAffinity was skipped because the window is null.";
    }
    return false;
  }

  if (SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
    return true;
  }

  if (error != nullptr) {
    *error = L"SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed with "
             L"Win32 error " +
             std::to_wstring(GetLastError()) + L".";
  }

  return false;
}

}  // namespace olouie::win32

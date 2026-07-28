#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace olouie::settings {

struct ParsedHotkey {
  UINT modifiers = 0;
  UINT virtual_key = 0;
  std::wstring canonical_label;
};

struct HotkeyParseResult {
  bool succeeded = false;
  ParsedHotkey hotkey;
  std::wstring message;

  bool Succeeded() const noexcept;
};

HotkeyParseResult ParseHotkey(std::wstring_view text);

}  // namespace olouie::settings

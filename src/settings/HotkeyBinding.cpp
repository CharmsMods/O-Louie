#include "settings/HotkeyBinding.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

namespace olouie::settings {
namespace {

std::wstring Trim(std::wstring_view value) {
  size_t start = 0;
  while (start < value.size() && std::iswspace(value[start]) != 0) {
    ++start;
  }
  size_t end = value.size();
  while (end > start && std::iswspace(value[end - 1]) != 0) {
    --end;
  }
  return std::wstring(value.substr(start, end - start));
}

std::wstring Upper(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) { return std::towupper(ch); });
  return value;
}

HotkeyParseResult Fail(std::wstring message) {
  HotkeyParseResult result;
  result.message = std::move(message);
  return result;
}

bool ParseFunctionKey(const std::wstring& token, UINT* virtual_key,
                      std::wstring* label) {
  if (token.size() < 2 || token.front() != L'F') {
    return false;
  }
  int number = 0;
  for (size_t index = 1; index < token.size(); ++index) {
    if (token[index] < L'0' || token[index] > L'9') {
      return false;
    }
    number = number * 10 + static_cast<int>(token[index] - L'0');
  }
  if (number < 1 || number > 24 || number == 12) {
    return false;
  }
  *virtual_key = static_cast<UINT>(VK_F1 + number - 1);
  *label = L"F" + std::to_wstring(number);
  return true;
}

bool ParseKey(const std::wstring& token, UINT* virtual_key,
              std::wstring* label) {
  if (token.size() == 1) {
    const wchar_t ch = token.front();
    if ((ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9')) {
      *virtual_key = static_cast<UINT>(ch);
      *label = token;
      return true;
    }
  }

  if (ParseFunctionKey(token, virtual_key, label)) {
    return true;
  }

  struct NamedKey {
    const wchar_t* name;
    UINT virtual_key;
    const wchar_t* label;
  };
  constexpr NamedKey kNamedKeys[] = {
      {L"SPACE", VK_SPACE, L"Space"},
      {L"INSERT", VK_INSERT, L"Insert"},
      {L"DELETE", VK_DELETE, L"Delete"},
      {L"HOME", VK_HOME, L"Home"},
      {L"END", VK_END, L"End"},
      {L"PAGEUP", VK_PRIOR, L"PageUp"},
      {L"PAGEDOWN", VK_NEXT, L"PageDown"},
      {L"UP", VK_UP, L"Up"},
      {L"DOWN", VK_DOWN, L"Down"},
      {L"LEFT", VK_LEFT, L"Left"},
      {L"RIGHT", VK_RIGHT, L"Right"},
  };
  for (const auto& named : kNamedKeys) {
    if (token == named.name) {
      *virtual_key = named.virtual_key;
      *label = named.label;
      return true;
    }
  }
  return false;
}

}  // namespace

bool HotkeyParseResult::Succeeded() const noexcept {
  return succeeded;
}

HotkeyParseResult ParseHotkey(std::wstring_view text) {
  std::vector<std::wstring> tokens;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t separator = text.find(L'+', start);
    const size_t end = separator == std::wstring_view::npos ? text.size()
                                                            : separator;
    auto token = Upper(Trim(text.substr(start, end - start)));
    if (token.empty()) {
      return Fail(L"Hotkeys must use nonempty '+'-separated keys.");
    }
    tokens.push_back(std::move(token));
    if (separator == std::wstring_view::npos) {
      break;
    }
    start = separator + 1;
  }

  UINT modifiers = MOD_NOREPEAT;
  bool control = false;
  bool alt = false;
  bool shift = false;
  bool windows = false;
  UINT virtual_key = 0;
  std::wstring key_label;

  for (const auto& token : tokens) {
    if (token == L"CTRL" || token == L"CONTROL") {
      if (control) {
        return Fail(L"A hotkey modifier may appear only once.");
      }
      control = true;
      modifiers |= MOD_CONTROL;
    } else if (token == L"ALT") {
      if (alt) {
        return Fail(L"A hotkey modifier may appear only once.");
      }
      alt = true;
      modifiers |= MOD_ALT;
    } else if (token == L"SHIFT") {
      if (shift) {
        return Fail(L"A hotkey modifier may appear only once.");
      }
      shift = true;
      modifiers |= MOD_SHIFT;
    } else if (token == L"WIN" || token == L"WINDOWS") {
      if (windows) {
        return Fail(L"A hotkey modifier may appear only once.");
      }
      windows = true;
      modifiers |= MOD_WIN;
    } else {
      if (virtual_key != 0) {
        return Fail(L"A hotkey must contain exactly one non-modifier key.");
      }
      if (!ParseKey(token, &virtual_key, &key_label)) {
        if (token == L"F12") {
          return Fail(L"F12 is reserved and cannot be used by O'Louie.");
        }
        return Fail(L"The hotkey key is not supported: " + token + L".");
      }
    }
  }

  if (virtual_key == 0) {
    return Fail(L"A hotkey needs one non-modifier key.");
  }
  if (!control && !alt && !shift && !windows) {
    return Fail(L"A global hotkey needs at least one modifier.");
  }

  std::wstring canonical;
  const auto append = [&canonical](std::wstring_view token) {
    if (!canonical.empty()) {
      canonical += L'+';
    }
    canonical += token;
  };
  if (control) {
    append(L"Ctrl");
  }
  if (alt) {
    append(L"Alt");
  }
  if (shift) {
    append(L"Shift");
  }
  if (windows) {
    append(L"Win");
  }
  append(key_label);

  HotkeyParseResult result;
  result.succeeded = true;
  result.hotkey.modifiers = modifiers;
  result.hotkey.virtual_key = virtual_key;
  result.hotkey.canonical_label = std::move(canonical);
  return result;
}

}  // namespace olouie::settings

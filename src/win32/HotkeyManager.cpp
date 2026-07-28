#include "win32/HotkeyManager.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace olouie::win32 {
namespace {

void SetError(std::wstring* error, std::wstring message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

HotkeyRegistrationApi HotkeyRegistrationApi::Win32() {
  HotkeyRegistrationApi api;
  api.register_hotkey = [](HWND owner, int id, UINT modifiers,
                           UINT virtual_key) {
    return RegisterHotKey(owner, id, modifiers, virtual_key) != FALSE;
  };
  api.unregister_hotkey = [](HWND owner, int id) {
    return UnregisterHotKey(owner, id) != FALSE;
  };
  api.last_error = [] { return GetLastError(); };
  return api;
}

HotkeyManager::HotkeyManager(HotkeyRegistrationApi registration_api)
    : registration_api_(std::move(registration_api)) {}

HotkeyManager::~HotkeyManager() {
  UnregisterAll();
}

void HotkeyManager::SetCommandSink(CommandSink sink) {
  command_sink_ = std::move(sink);
}

bool HotkeyManager::Register(HWND owner, const HotkeyBinding& binding,
                             std::wstring* error) {
  if (owner == nullptr) {
    SetError(error, L"Hotkey registration needs a valid window.");
    return false;
  }

  if (binding.id == 0 || binding.virtual_key == 0 ||
      bindings_.contains(binding.id)) {
    SetError(error, L"Hotkey binding is incomplete.");
    return false;
  }

  if (!registration_api_.register_hotkey ||
      !registration_api_.register_hotkey(owner, binding.id,
                                         binding.modifiers,
                                         binding.virtual_key)) {
    const DWORD last_error = registration_api_.last_error
                                 ? registration_api_.last_error()
                                 : ERROR_GEN_FAILURE;
    SetError(error, L"RegisterHotKey failed with Win32 error " +
                        std::to_wstring(last_error) + L".");
    return false;
  }

  owner_ = owner;
  bindings_[binding.id] = binding;
  return true;
}

bool HotkeyManager::ReplaceAll(HWND owner,
                               const std::vector<HotkeyBinding>& bindings,
                               std::wstring* error) {
  if (owner == nullptr) {
    SetError(error, L"Hotkey registration needs a valid window.");
    return false;
  }
  if (!ValidateBindings(bindings, error)) {
    return false;
  }

  const HWND previous_owner = owner_;
  const auto previous_bindings = bindings_;
  if (previous_owner != nullptr && registration_api_.unregister_hotkey) {
    for (const auto& [id, binding] : previous_bindings) {
      (void)binding;
      registration_api_.unregister_hotkey(previous_owner, id);
    }
  }
  bindings_.clear();
  owner_ = nullptr;

  std::vector<int> registered_ids;
  for (const auto& binding : bindings) {
    if (registration_api_.register_hotkey &&
        registration_api_.register_hotkey(owner, binding.id,
                                           binding.modifiers,
                                           binding.virtual_key)) {
      registered_ids.push_back(binding.id);
      continue;
    }

    const DWORD registration_error = registration_api_.last_error
                                         ? registration_api_.last_error()
                                         : ERROR_GEN_FAILURE;
    if (registration_api_.unregister_hotkey) {
      for (const int id : registered_ids) {
        registration_api_.unregister_hotkey(owner, id);
      }
    }

    bool restored = true;
    std::map<int, HotkeyBinding> restored_bindings;
    if (previous_owner != nullptr) {
      for (const auto& [id, previous] : previous_bindings) {
        if (!registration_api_.register_hotkey ||
            !registration_api_.register_hotkey(
                previous_owner, id, previous.modifiers,
                previous.virtual_key)) {
          restored = false;
          break;
        }
        restored_bindings[id] = previous;
      }
    }

    if (!restored && previous_owner != nullptr &&
        registration_api_.unregister_hotkey) {
      for (const auto& [id, previous] : restored_bindings) {
        (void)previous;
        registration_api_.unregister_hotkey(previous_owner, id);
      }
      restored_bindings.clear();
    }
    owner_ = restored ? previous_owner : nullptr;
    bindings_ = restored ? previous_bindings : restored_bindings;

    std::wstring message = L"Hotkey '" + binding.label +
                           L"' could not be registered (Win32 error " +
                           std::to_wstring(registration_error) + L").";
    if (restored) {
      message += L" Previous hotkeys remain active.";
    } else {
      message += L" Previous hotkeys could not be restored.";
    }
    SetError(error, std::move(message));
    return false;
  }

  owner_ = owner;
  for (const auto& binding : bindings) {
    bindings_[binding.id] = binding;
  }
  return true;
}

void HotkeyManager::UnregisterAll() {
  if (owner_ == nullptr) {
    bindings_.clear();
    return;
  }

  for (const auto& [id, binding] : bindings_) {
    (void)binding;
    if (registration_api_.unregister_hotkey) {
      registration_api_.unregister_hotkey(owner_, id);
    }
  }

  bindings_.clear();
  owner_ = nullptr;
}

bool HotkeyManager::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;

  if (message != WM_HOTKEY) {
    return false;
  }

  const auto found = bindings_.find(static_cast<int>(wparam));
  if (found != bindings_.end() && command_sink_) {
    command_sink_(found->second.command);
  }

  return true;
}

std::vector<HotkeyBinding> HotkeyManager::Bindings() const {
  std::vector<HotkeyBinding> result;
  result.reserve(bindings_.size());
  for (const auto& [id, binding] : bindings_) {
    (void)id;
    result.push_back(binding);
  }
  return result;
}

bool HotkeyManager::ValidateBindings(
    const std::vector<HotkeyBinding>& bindings, std::wstring* error) const {
  if (bindings.empty()) {
    SetError(error, L"At least one hotkey binding is required.");
    return false;
  }

  std::set<int> ids;
  std::set<std::pair<UINT, UINT>> keys;
  for (const auto& binding : bindings) {
    if (binding.id == 0 || binding.virtual_key == 0 ||
        binding.label.empty()) {
      SetError(error, L"Hotkey binding is incomplete.");
      return false;
    }
    if (!ids.insert(binding.id).second) {
      SetError(error, L"Hotkey ids must be unique.");
      return false;
    }
    const auto key = std::make_pair(
        binding.modifiers & ~static_cast<UINT>(MOD_NOREPEAT),
        binding.virtual_key);
    if (!keys.insert(key).second) {
      SetError(error, L"Hotkey combinations must be unique.");
      return false;
    }
  }
  return true;
}

}  // namespace olouie::win32

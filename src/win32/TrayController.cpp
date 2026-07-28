#include "win32/TrayController.h"

#include <cwchar>
#include <iterator>
#include <string>

namespace olouie::win32 {
namespace {

constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT_PTR kToggleRecordingMenuId = 1000;
constexpr UINT_PTR kOpenSettingsMenuId = 1001;
constexpr UINT_PTR kExitMenuId = 1002;
constexpr UINT_PTR kSaveLast30SecondsMenuId = 1003;
constexpr UINT_PTR kSaveLast5MinutesMenuId = 1004;
constexpr UINT_PTR kAddBookmarkMenuId = 1005;
constexpr UINT_PTR kSaveSecondPresetMenuId = 1006;
constexpr UINT_PTR kSaveCustomClipMenuId = 1007;
constexpr UINT_PTR kRecoverRecordingMenuId = 1008;
constexpr UINT_PTR kDiscardRecoveryMenuId = 1009;

constexpr GUID kTrayGuid = {0x98c947dd,
                            0x2a9d,
                            0x46a7,
                            {0xa7, 0x54, 0x57, 0x96, 0xc1, 0x83, 0x9d, 0x9b}};

void CopyText(wchar_t* destination, size_t destination_count,
              std::wstring_view source) {
  const std::wstring text(source);
  wcsncpy_s(destination, destination_count, text.c_str(), _TRUNCATE);
}

}  // namespace

TrayController::~TrayController() {
  Destroy();
}

bool TrayController::Create(HWND owner, std::wstring_view tooltip) {
  if (created_) {
    return true;
  }

  owner_ = owner;
  notify_icon_ = {};
  notify_icon_.cbSize = sizeof(notify_icon_);
  notify_icon_.hWnd = owner_;
  notify_icon_.uID = 1;
  notify_icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
  notify_icon_.uCallbackMessage = kTrayCallbackMessage;
  notify_icon_.guidItem = kTrayGuid;
  notify_icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  CopyText(notify_icon_.szTip, std::size(notify_icon_.szTip), tooltip);

  if (!Shell_NotifyIconW(NIM_ADD, &notify_icon_)) {
    return false;
  }

  notify_icon_.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &notify_icon_);

  created_ = true;
  return true;
}

void TrayController::Destroy() {
  if (created_) {
    Shell_NotifyIconW(NIM_DELETE, &notify_icon_);
    created_ = false;
  }
}

void TrayController::SetCommandSink(CommandSink sink) {
  command_sink_ = std::move(sink);
}

bool TrayController::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)wparam;

  if (message != kTrayCallbackMessage) {
    return false;
  }

  switch (LOWORD(lparam)) {
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
      ShowContextMenu();
      break;
    case NIN_SELECT:
    case WM_LBUTTONDBLCLK:
      Emit(app::AppCommand::OpenSettings);
      break;
    default:
      break;
  }

  return true;
}

bool TrayController::HandleCommand(WPARAM command_id) {
  switch (LOWORD(command_id)) {
    case kToggleRecordingMenuId:
      Emit(app::AppCommand::ToggleRecording);
      return true;
    case kOpenSettingsMenuId:
      Emit(app::AppCommand::OpenSettings);
      return true;
    case kSaveLast30SecondsMenuId:
      Emit(app::AppCommand::SaveLast30Seconds);
      return true;
    case kSaveSecondPresetMenuId:
      Emit(app::AppCommand::SaveSecondPreset);
      return true;
    case kSaveLast5MinutesMenuId:
      Emit(app::AppCommand::SaveLast5Minutes);
      return true;
    case kSaveCustomClipMenuId:
      Emit(app::AppCommand::SaveCustomClip);
      return true;
    case kAddBookmarkMenuId:
      Emit(app::AppCommand::AddBookmark);
      return true;
    case kRecoverRecordingMenuId:
      Emit(app::AppCommand::RecoverRecording);
      return true;
    case kDiscardRecoveryMenuId:
      Emit(app::AppCommand::DiscardRecovery);
      return true;
    case kExitMenuId:
      Emit(app::AppCommand::Exit);
      return true;
    default:
      return false;
  }
}

void TrayController::ShowInfo(std::wstring_view title,
                              std::wstring_view message) {
  if (!created_ || !notifications_enabled_) {
    return;
  }

  NOTIFYICONDATAW info = notify_icon_;
  info.uFlags = NIF_INFO | NIF_GUID;
  info.dwInfoFlags = NIIF_INFO;
  CopyText(info.szInfoTitle, std::size(info.szInfoTitle), title);
  CopyText(info.szInfo, std::size(info.szInfo), message);
  Shell_NotifyIconW(NIM_MODIFY, &info);
}

void TrayController::SetRecordingState(TrayRecordingState state) noexcept {
  recording_state_ = state;
}

void TrayController::SetNotificationsEnabled(bool enabled) noexcept {
  notifications_enabled_ = enabled;
}

void TrayController::SetClipPresetDurations(int first_seconds,
                                            int second_seconds,
                                            int third_seconds,
                                            int custom_seconds) noexcept {
  if (first_seconds > 0) {
    first_clip_preset_seconds_ = first_seconds;
  }
  if (second_seconds > 0) {
    second_clip_preset_seconds_ = second_seconds;
  }
  if (third_seconds > 0) {
    third_clip_preset_seconds_ = third_seconds;
  }
  if (custom_seconds > 0) {
    custom_clip_seconds_ = custom_seconds;
  }
}

void TrayController::SetRecoveryState(size_t exportable_count,
                                      size_t discardable_count,
                                      bool busy) noexcept {
  recovery_exportable_count_ = exportable_count;
  recovery_discardable_count_ = discardable_count;
  recovery_busy_ = busy;
}

void TrayController::ShowContextMenu() {
  if (owner_ == nullptr) {
    return;
  }

  POINT cursor{};
  GetCursorPos(&cursor);

  HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }

  const wchar_t* recording_label = L"Start Recording";
  UINT recording_flags = MF_STRING;
  switch (recording_state_) {
    case TrayRecordingState::Idle:
      break;
    case TrayRecordingState::Starting:
      recording_label = L"Cancel Recording Start";
      break;
    case TrayRecordingState::Recording:
      recording_label = L"Stop and Save Full Recording";
      break;
    case TrayRecordingState::Stopping:
      recording_label = L"Stopping and Saving...";
      recording_flags |= MF_GRAYED;
      break;
  }

  AppendMenuW(menu, recording_flags, kToggleRecordingMenuId, recording_label);
  const UINT clip_flags = recording_state_ == TrayRecordingState::Recording
                              ? MF_STRING
                              : MF_STRING | MF_GRAYED;
  const std::wstring first_clip_label =
      L"Save Last " + std::to_wstring(first_clip_preset_seconds_) +
      L" Seconds";
  const std::wstring third_clip_label =
      L"Save Last " + std::to_wstring(third_clip_preset_seconds_) +
      L" Seconds";
  const std::wstring second_clip_label =
      L"Save Last " + std::to_wstring(second_clip_preset_seconds_) +
      L" Seconds";
  const std::wstring custom_clip_label =
      L"Save Last " + std::to_wstring(custom_clip_seconds_) +
      L" Seconds (Custom)";
  AppendMenuW(menu, clip_flags, kSaveLast30SecondsMenuId,
              first_clip_label.c_str());
  AppendMenuW(menu, clip_flags, kSaveSecondPresetMenuId,
              second_clip_label.c_str());
  AppendMenuW(menu, clip_flags, kSaveLast5MinutesMenuId,
              third_clip_label.c_str());
  AppendMenuW(menu, clip_flags, kSaveCustomClipMenuId,
              custom_clip_label.c_str());
  AppendMenuW(menu, clip_flags, kAddBookmarkMenuId,
              L"Add Bookmark and Save Clip");
  if (recovery_busy_ || recovery_exportable_count_ != 0 ||
      recovery_discardable_count_ != 0) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (recovery_busy_) {
      AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
                  L"Checking interrupted recordings...");
    } else {
      if (recovery_exportable_count_ != 0) {
        const std::wstring label =
            recovery_exportable_count_ == 1
                ? L"Recover Interrupted Recording"
                : L"Recover Next Interrupted Recording (" +
                      std::to_wstring(recovery_exportable_count_) + L")";
        AppendMenuW(menu, MF_STRING, kRecoverRecordingMenuId,
                    label.c_str());
      }
      if (recovery_discardable_count_ != 0) {
        const std::wstring label =
            recovery_discardable_count_ == 1
                ? L"Discard Interrupted Recording"
                : L"Discard Next Interrupted Recording (" +
                      std::to_wstring(recovery_discardable_count_) + L")";
        AppendMenuW(menu, MF_STRING, kDiscardRecoveryMenuId,
                    label.c_str());
      }
    }
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kOpenSettingsMenuId, L"Open Settings");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExitMenuId, L"Exit");

  SetForegroundWindow(owner_);
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, owner_, nullptr);
  DestroyMenu(menu);
  PostMessageW(owner_, WM_NULL, 0, 0);
}

void TrayController::Emit(app::AppCommand command) {
  if (command_sink_) {
    command_sink_(command);
  }
}

}  // namespace olouie::win32

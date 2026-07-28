#pragma once

namespace olouie::app {

enum class AppCommand {
  OpenSettings,
  Exit,
  ToggleRecording,
  SaveLast30Seconds,
  SaveSecondPreset,
  SaveLast5Minutes,
  SaveCustomClip,
  AddBookmark,
  RecoverRecording,
  DiscardRecovery,
};

}  // namespace olouie::app

#include "ui/SettingsUi.h"

#include <imgui.h>

#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

#include "settings/HotkeyBinding.h"

namespace olouie::ui {
namespace {

std::string ToUtf8(std::wstring_view text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                       text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

bool FromUtf8(std::string_view text, std::wstring* output) {
  if (output == nullptr) {
    return false;
  }
  if (text.empty()) {
    output->clear();
    return true;
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0);
  if (size <= 0) {
    return false;
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(),
                          size) <= 0) {
    return false;
  }
  *output = std::move(result);
  return true;
}

template <size_t Size>
void CopyBuffer(std::array<char, Size>* destination, std::string_view source) {
  destination->fill('\0');
  const size_t count = std::min(source.size(), Size - 1);
  std::memcpy(destination->data(), source.data(), count);
}

template <size_t Size>
std::string_view BufferView(const std::array<char, Size>& buffer) {
  return std::string_view(buffer.data(), strnlen_s(buffer.data(), Size));
}

std::string MonitorLabel(const graphics::MonitorInfo& monitor) {
  std::wstring label = monitor.device_name + L"  " +
                       std::to_wstring(monitor.Width()) + L" x " +
                       std::to_wstring(monitor.Height());
  if (monitor.primary) {
    label += L"  (Primary)";
  }
  return ToUtf8(label);
}

void SectionLabel(const char* label) {
  ImGui::Spacing();
  ImGui::TextDisabled("%s", label);
  ImGui::Separator();
  ImGui::Spacing();
}

std::string Decimal(double value, const char* suffix = "") {
  std::ostringstream text;
  text << std::fixed << std::setprecision(2) << value << suffix;
  return text.str();
}

std::string Bitrate(uint64_t bits_per_second) {
  if (bits_per_second == 0) {
    return "Unavailable";
  }
  return Decimal(static_cast<double>(bits_per_second) / 1000000.0, " Mbps");
}

std::string Milliseconds(uint64_t nanoseconds) {
  return Decimal(static_cast<double>(nanoseconds) / 1000000.0, " ms");
}

void DiagnosticRow(const char* label, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextDisabled("%s", label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextWrapped("%s", value.c_str());
}

void SelectableDiagnosticBlock(const char* label, const std::string& value) {
  ImGui::TextDisabled("%s", label);
  ImGui::PushID(label);
  std::string buffer = value.empty() ? "Unavailable" : value;
  ImGui::InputTextMultiline("##Value", buffer.data(), buffer.size() + 1,
                            ImVec2(-1.0f, ImGui::GetTextLineHeight() * 2.2f),
                            ImGuiInputTextFlags_ReadOnly);
  ImGui::PopID();
}

}  // namespace

SettingsUi::~SettingsUi() {
  Shutdown();
}

void SettingsUi::Configure(HINSTANCE instance, ApplySink apply_sink,
                           DiagnosticSink diagnostic_sink,
                           MicMonitorStartSink mic_monitor_start_sink,
                           MicMonitorStopSink mic_monitor_stop_sink,
                           MicMonitorSnapshotSource mic_monitor_snapshot_source) {
  instance_ = instance;
  apply_sink_ = std::move(apply_sink);
  diagnostic_sink_ = std::move(diagnostic_sink);
  mic_monitor_start_sink_ = std::move(mic_monitor_start_sink);
  mic_monitor_stop_sink_ = std::move(mic_monitor_stop_sink);
  mic_monitor_snapshot_source_ = std::move(mic_monitor_snapshot_source);
}

bool SettingsUi::Open(const settings::AppSettings& current,
                      const settings::AppSettings& defaults,
                      std::wstring* error,
                      SettingsUiInitialTab initial_tab) {
  if (instance_ == nullptr || !apply_sink_) {
    if (error != nullptr) {
      *error = L"Settings UI is not configured.";
    }
    return false;
  }

  if (host_.visible()) {
    return host_.Show(error);
  }

  draft_ = current;
  defaults_ = defaults;
  monitors_ = graphics::EnumerateMonitors();
  RefreshAudioOutputs();
  SyncBuffersFromDraft();
  status_text_.clear();
  status_is_error_ = false;
  select_audio_tab_ = initial_tab == SettingsUiInitialTab::Audio;
  select_diagnostics_tab_ = initial_tab == SettingsUiInitialTab::Diagnostics;

  if (!host_.created()) {
    if (!host_.Create(
            instance_, L"O'Louie Settings", [this] { Render(); },
            [this] { status_text_.clear(); },
            [this](std::wstring_view message) {
              if (diagnostic_sink_) {
                diagnostic_sink_(message);
              }
              SetStatus(std::wstring(message), true);
            },
            error)) {
      return false;
    }
  }
  return host_.Show(error);
}

void SettingsUi::SetDiagnosticsSnapshot(
    diagnostics::DiagnosticsSnapshot snapshot) {
  diagnostics_ = std::move(snapshot);
}

void SettingsUi::Close() {
  StopMicMonitor();
  host_.Hide();
}

void SettingsUi::Shutdown() {
  StopMicMonitor();
  host_.Destroy();
}

bool SettingsUi::visible() const noexcept {
  return host_.visible();
}

HWND SettingsUi::hwnd() const noexcept {
  return host_.hwnd();
}

ImGuiDx11HostStats SettingsUi::host_stats() const noexcept {
  return host_.stats();
}

void SettingsUi::Render() {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
  ImGui::SetNextWindowSize(io.DisplaySize);
  constexpr ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin("##O'LouieSettings", nullptr, flags);

  ImGui::TextUnformatted("O'Louie Settings");
  ImGui::SameLine();
  ImGui::TextDisabled("Recorder configuration");
  ImGui::Spacing();

  bool diagnostics_tab_active = false;
  if (ImGui::BeginTabBar("SettingsTabs", ImGuiTabBarFlags_None)) {
    if (ImGui::BeginTabItem("General")) {
      RenderGeneralTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Video")) {
      RenderVideoTab();
      ImGui::EndTabItem();
    }
    const ImGuiTabItemFlags audio_flags =
        select_audio_tab_ ? ImGuiTabItemFlags_SetSelected
                          : ImGuiTabItemFlags_None;
    if (ImGui::BeginTabItem("Audio", nullptr, audio_flags)) {
      select_audio_tab_ = false;
      RenderAudioTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Clips / Bookmarks")) {
      RenderClipsTab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Hotkeys")) {
      RenderHotkeysTab();
      ImGui::EndTabItem();
    }
    const ImGuiTabItemFlags diagnostics_flags =
        select_diagnostics_tab_ ? ImGuiTabItemFlags_SetSelected
                                : ImGuiTabItemFlags_None;
    if (ImGui::BeginTabItem("Diagnostics", nullptr, diagnostics_flags)) {
      diagnostics_tab_active = true;
      select_diagnostics_tab_ = false;
      RenderDiagnosticsTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  const float footer_height = ImGui::GetFrameHeightWithSpacing() * 2.2f;
  const float available = ImGui::GetContentRegionAvail().y;
  if (available > footer_height) {
    ImGui::Dummy(ImVec2(0.0f, available - footer_height));
  }
  ImGui::Separator();
  if (!status_text_.empty()) {
    const ImVec4 color = status_is_error_
                             ? ImVec4(0.96f, 0.42f, 0.38f, 1.0f)
                             : ImVec4(0.42f, 0.86f, 0.68f, 1.0f);
    ImGui::TextColored(color, "%s", status_text_.c_str());
  } else {
    ImGui::TextUnformatted(" ");
  }

  if (diagnostics_tab_active) {
    if (ImGui::Button("Close", ImVec2(90.0f, 0.0f))) {
      Close();
    }
  } else {
    if (ImGui::Button("Save", ImVec2(110.0f, 0.0f))) {
      SaveDraft();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 0.0f))) {
      Close();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset Defaults", ImVec2(125.0f, 0.0f))) {
      StopMicMonitor();
      draft_ = defaults_;
      SyncBuffersFromDraft();
      SetStatus(L"Defaults restored in the editor.", false);
    }
  }

  ImGui::End();
}

void SettingsUi::RenderGeneralTab() {
  SectionLabel("Output");
  ImGui::SetNextItemWidth(-105.0f);
  ImGui::InputText("##OutputDirectory", output_directory_.data(),
                   output_directory_.size());
  ImGui::SameLine();
  if (ImGui::Button("Browse...", ImVec2(95.0f, 0.0f))) {
    PickOutputDirectory();
  }

  SectionLabel("Behavior");
  ImGui::Checkbox("Start with Windows", &draft_.start_with_windows);
  ImGui::Checkbox("Show recording notifications",
                  &draft_.show_overlay_notifications);
}

void SettingsUi::RenderVideoTab() {
  SectionLabel("Capture");
  const char* preview = "Primary monitor";
  std::string selected_label;
  for (const auto& monitor : monitors_) {
    if (!draft_.video.monitor_device_name.empty() &&
        monitor.device_name == draft_.video.monitor_device_name) {
      selected_label = MonitorLabel(monitor);
      preview = selected_label.c_str();
      break;
    }
  }
  ImGui::SetNextItemWidth(420.0f);
  if (ImGui::BeginCombo("Monitor", preview)) {
    const bool primary = draft_.video.monitor_device_name.empty();
    if (ImGui::Selectable("Primary monitor", primary)) {
      draft_.video.monitor_device_name.clear();
    }
    for (const auto& monitor : monitors_) {
      const std::string label = MonitorLabel(monitor);
      const bool selected =
          monitor.device_name == draft_.video.monitor_device_name;
      if (ImGui::Selectable(label.c_str(), selected)) {
        draft_.video.monitor_device_name = monitor.device_name;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::Checkbox("Capture cursor", &draft_.video.capture_cursor);

  SectionLabel("Encoding");
  int resolution = draft_.video.resolution_mode ==
                           settings::ResolutionMode::Source
                       ? 0
                       : 1;
  if (ImGui::RadioButton("Source size", resolution == 0)) {
    draft_.video.resolution_mode = settings::ResolutionMode::Source;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("Custom size", resolution == 1)) {
    draft_.video.resolution_mode = settings::ResolutionMode::Custom;
  }
  const bool custom =
      draft_.video.resolution_mode == settings::ResolutionMode::Custom;
  ImGui::BeginDisabled(!custom);
  ImGui::SetNextItemWidth(130.0f);
  ImGui::InputInt("Width", &draft_.video.custom_width, 2, 16);
  ImGui::SetNextItemWidth(130.0f);
  ImGui::InputInt("Height", &draft_.video.custom_height, 2, 16);
  ImGui::EndDisabled();
  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderInt("Frame rate", &draft_.video.fps, 1, 240, "%d FPS");
  ImGui::SetNextItemWidth(180.0f);
  ImGui::SliderInt("Bitrate", &draft_.video.bitrate_mbps, 1, 500,
                   "%d Mbps");
  float gop = static_cast<float>(draft_.video.gop_seconds);
  ImGui::SetNextItemWidth(180.0f);
  if (ImGui::SliderFloat("Keyframe interval", &gop, 0.25f, 10.0f,
                         "%.2f s")) {
    draft_.video.gop_seconds = gop;
  }
  ImGui::SetNextItemWidth(280.0f);
  if (ImGui::BeginCombo("Encoder", "Media Foundation hardware H.264")) {
    ImGui::Selectable("Media Foundation hardware H.264", true);
    ImGui::EndCombo();
  }

  SectionLabel("Performance");
  const bool capture_first =
      draft_.video.performance_mode ==
      performance::CapturePerformanceMode::CaptureFirst;
  const char* performance_preview =
      capture_first ? "Capture First" : "Balanced";
  ImGui::SetNextItemWidth(220.0f);
  if (ImGui::BeginCombo("Recording priority", performance_preview)) {
    if (ImGui::Selectable("Balanced", !capture_first)) {
      draft_.video.performance_mode =
          performance::CapturePerformanceMode::Balanced;
    }
    if (ImGui::Selectable("Capture First", capture_first)) {
      draft_.video.performance_mode =
          performance::CapturePerformanceMode::CaptureFirst;
    }
    ImGui::EndCombo();
  }
  ImGui::TextWrapped(
      capture_first
          ? "Prioritizes capture, audio service, and video encode work and "
            "requests low-latency, faster hardware encoding. This can trade "
            "some encoder quality for speed and reduce in-game frame rate "
            "slightly under heavy load."
          : "Uses normal multimedia scheduling and leaves optional encoder "
            "latency/speed tuning unchanged.");
}

void SettingsUi::RenderAudioTab() {
  const float reserved_footer = ImGui::GetFrameHeightWithSpacing() * 3.4f;
  ImGui::BeginChild("AudioBody", ImVec2(0.0f, -reserved_footer), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  SectionLabel("Sources");
  ImGui::Checkbox("System audio", &draft_.audio.system_mix);
  ImGui::Checkbox("Microphone", &draft_.audio.mic);

  SectionLabel("Microphone check");
  const audio::MicMonitorSnapshot monitor =
      mic_monitor_snapshot_source_ ? mic_monitor_snapshot_source_()
                                   : audio::MicMonitorSnapshot{};
  const bool monitor_active =
      monitor.state == audio::MicMonitorState::Starting ||
      monitor.state == audio::MicMonitorState::Monitoring ||
      monitor.state == audio::MicMonitorState::Stopping;
  const bool recorder_busy =
      diagnostics_.recorder_state == record::VideoRecorderState::Starting ||
      diagnostics_.recorder_state == record::VideoRecorderState::Recording ||
      diagnostics_.recorder_state == record::VideoRecorderState::Stopping;

  const audio::AudioEndpointInfo* default_output = nullptr;
  const audio::AudioEndpointInfo* selected_output = nullptr;
  for (const auto& endpoint : audio_outputs_) {
    if (endpoint.is_default) {
      default_output = &endpoint;
    }
    if (!draft_.audio.mic_check_output_device_id.empty() &&
        endpoint.id == draft_.audio.mic_check_output_device_id) {
      selected_output = &endpoint;
    }
  }

  std::string output_preview;
  if (draft_.audio.mic_check_output_device_id.empty()) {
    output_preview = default_output == nullptr
                         ? "Windows Default (unavailable)"
                         : "Windows Default - " +
                               ToUtf8(default_output->name);
  } else if (selected_output != nullptr) {
    output_preview = ToUtf8(selected_output->name);
  } else {
    output_preview = "Saved output unavailable (uses Windows Default)";
  }

  ImGui::BeginDisabled(monitor_active);
  ImGui::SetNextItemWidth(-100.0f);
  if (ImGui::BeginCombo("##MicCheckOutput", output_preview.c_str())) {
    const bool default_selected =
        draft_.audio.mic_check_output_device_id.empty();
    std::string default_label =
        default_output == nullptr
            ? "Windows Default (unavailable)"
            : "Windows Default - " + ToUtf8(default_output->name);
    if (ImGui::Selectable(default_label.c_str(), default_selected)) {
      draft_.audio.mic_check_output_device_id.clear();
      SetStatus(L"Mic check will use Windows Default after saving.", false);
    }
    if (!draft_.audio.mic_check_output_device_id.empty() &&
        selected_output == nullptr) {
      ImGui::Selectable(
          "Saved output unavailable (Windows Default fallback)", true,
          ImGuiSelectableFlags_Disabled);
    }
    ImGui::Separator();
    for (const auto& endpoint : audio_outputs_) {
      const bool selected =
          endpoint.id == draft_.audio.mic_check_output_device_id;
      std::string label = ToUtf8(endpoint.name);
      if (endpoint.is_default) {
        label += " (Windows Default)";
      }
      ImGui::PushID(label.c_str());
      if (ImGui::Selectable(label.c_str(), selected)) {
        draft_.audio.mic_check_output_device_id = endpoint.id;
        SetStatus(L"Mic-check output selected; press Save to keep it.",
                  false);
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh", ImVec2(90.0f, 0.0f))) {
    RefreshAudioOutputs();
  }
  ImGui::EndDisabled();
  ImGui::TextDisabled("Playback output (mic check only)");

  if (!audio_output_enumeration_error_.empty()) {
    ImGui::TextColored(ImVec4(0.96f, 0.42f, 0.38f, 1.0f), "%s",
                       ToUtf8(audio_output_enumeration_error_).c_str());
  } else if (audio_outputs_.empty()) {
    ImGui::TextColored(ImVec4(0.96f, 0.42f, 0.38f, 1.0f),
                       "No active output devices are available.");
  } else if (!draft_.audio.mic_check_output_device_id.empty() &&
             selected_output == nullptr) {
    ImGui::TextColored(
        ImVec4(0.96f, 0.68f, 0.28f, 1.0f),
        "Saved output is unavailable; mic check will use Windows Default.");
  }

  ImGui::Spacing();
  ImGui::TextColored(ImVec4(0.96f, 0.68f, 0.28f, 1.0f),
                     "Use headphones to prevent speaker feedback.");

  if (recorder_busy) {
    ImGui::BeginDisabled();
    ImGui::Button("Start Mic Check", ImVec2(150.0f, 0.0f));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("Unavailable while recording is active.");
  } else if (monitor.state == audio::MicMonitorState::Starting) {
    if (ImGui::Button("Stop Mic Check", ImVec2(150.0f, 0.0f))) {
      StopMicMonitor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Starting...");
  } else if (monitor.state == audio::MicMonitorState::Monitoring) {
    if (ImGui::Button("Stop Mic Check", ImVec2(150.0f, 0.0f))) {
      StopMicMonitor();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Monitoring");
  } else if (monitor.state == audio::MicMonitorState::Stopping) {
    ImGui::BeginDisabled();
    ImGui::Button("Stopping...", ImVec2(150.0f, 0.0f));
    ImGui::EndDisabled();
  } else {
    const bool outputs_unavailable = audio_outputs_.empty();
    ImGui::BeginDisabled(outputs_unavailable || !mic_monitor_start_sink_);
    const char* label = monitor.state == audio::MicMonitorState::Failed
                            ? "Retry Mic Check"
                            : "Start Mic Check";
    if (ImGui::Button(label, ImVec2(150.0f, 0.0f))) {
      audio::MicMonitorOptions options;
      options.output_device_id = draft_.audio.mic_check_output_device_id;
      const auto command = mic_monitor_start_sink_(options);
      SetStatus(command.message, !command.Accepted());
    }
    ImGui::EndDisabled();
  }

  const float meter_fraction =
      std::clamp((monitor.peak_dbfs + 60.0f) / 60.0f, 0.0f, 1.0f);
  char meter_text[32]{};
  sprintf_s(meter_text, "%.1f dBFS", monitor.peak_dbfs);
  const ImVec4 meter_color =
      monitor.clipping
          ? ImVec4(0.96f, 0.25f, 0.20f, 1.0f)
          : monitor.peak_dbfs > -6.0f
                ? ImVec4(0.96f, 0.68f, 0.20f, 1.0f)
                : ImVec4(0.30f, 0.82f, 0.56f, 1.0f);
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, meter_color);
  ImGui::ProgressBar(meter_fraction, ImVec2(-1.0f, 0.0f), meter_text);
  ImGui::PopStyleColor();
  if (monitor.clipping) {
    ImGui::TextColored(ImVec4(0.96f, 0.25f, 0.20f, 1.0f),
                       "Input is clipping.");
  }

  if (!monitor.active_output_device_name.empty()) {
    ImGui::TextWrapped("Playing through: %s",
                       ToUtf8(monitor.active_output_device_name).c_str());
  }
  if (!monitor.message.empty() &&
      monitor.state != audio::MicMonitorState::Idle) {
    const ImVec4 message_color =
        monitor.state == audio::MicMonitorState::Failed
            ? ImVec4(0.96f, 0.42f, 0.38f, 1.0f)
            : monitor.using_fallback_output
                  ? ImVec4(0.96f, 0.68f, 0.28f, 1.0f)
                  : ImVec4(0.65f, 0.72f, 0.78f, 1.0f);
    ImGui::TextColored(message_color, "%s",
                       ToUtf8(monitor.message).c_str());
  }
  if (monitor.underrun_count != 0 || monitor.overflow_count != 0) {
    ImGui::TextDisabled("Playback underruns: %llu   Queue overflows: %llu",
                        static_cast<unsigned long long>(
                            monitor.underrun_count),
                        static_cast<unsigned long long>(
                            monitor.overflow_count));
  }

  SectionLabel("Tracks");
  draft_.audio.separate_tracks = true;
  ImGui::BeginDisabled();
  ImGui::Checkbox("Keep enabled sources as separate tracks",
                  &draft_.audio.separate_tracks);
  ImGui::EndDisabled();
  ImGui::TextDisabled("Separate tracks are required for reliable source "
                      "capture.");
  ImGui::BeginDisabled();
  int sample_rate = draft_.audio.sample_rate;
  ImGui::SetNextItemWidth(160.0f);
  ImGui::InputInt("Sample rate", &sample_rate, 0, 0);
  ImGui::EndDisabled();
  ImGui::EndChild();
}

void SettingsUi::RenderClipsTab() {
  SectionLabel("Clip presets");
  if (draft_.clips.presets_seconds.size() != 3) {
    draft_.clips.presets_seconds = {30, 60, 300};
  }
  for (size_t index = 0; index < draft_.clips.presets_seconds.size(); ++index) {
    ImGui::PushID(static_cast<int>(index));
    ImGui::SetNextItemWidth(150.0f);
    const std::string label = "Preset " + std::to_string(index + 1) + " (seconds)";
    ImGui::InputInt(label.c_str(), &draft_.clips.presets_seconds[index], 1,
                    10);
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(150.0f);
  ImGui::InputInt("Custom duration (seconds)",
                  &draft_.clips.custom_seconds, 1, 10);

  SectionLabel("Bookmarks");
  ImGui::SetNextItemWidth(150.0f);
  ImGui::InputInt("Pre-roll (seconds)",
                  &draft_.clips.bookmark_pre_seconds, 1, 10);
  ImGui::SetNextItemWidth(150.0f);
  ImGui::BeginDisabled();
  ImGui::InputInt("Post-roll (seconds)",
                  &draft_.clips.bookmark_post_seconds, 0, 0);
  ImGui::EndDisabled();
}

void SettingsUi::RenderHotkeysTab() {
  SectionLabel("Global shortcuts");
  ImGui::SetNextItemWidth(280.0f);
  ImGui::InputText("Toggle recording", toggle_recording_.data(),
                   toggle_recording_.size());
  ImGui::SetNextItemWidth(280.0f);
  ImGui::InputText("Save preset 1", save_first_preset_.data(),
                   save_first_preset_.size());
  ImGui::SetNextItemWidth(280.0f);
  ImGui::InputText("Save preset 3", save_third_preset_.data(),
                   save_third_preset_.size());
  ImGui::SetNextItemWidth(280.0f);
  ImGui::InputText("Add bookmark", bookmark_.data(), bookmark_.size());
}

void SettingsUi::RenderDiagnosticsTab() {
  const float reserved_footer = ImGui::GetFrameHeightWithSpacing() * 3.4f;
  ImGui::BeginChild("DiagnosticsBody", ImVec2(0.0f, -reserved_footer), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  SectionLabel("Runtime");
  if (ImGui::BeginTable("RuntimeFacts", 2,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Fact", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
    DiagnosticRow("Recorder",
                  ToUtf8(record::VideoRecorderStateName(
                      diagnostics_.recorder_state)));
    DiagnosticRow("Recovery",
                  ToUtf8(record::RecordingRecoveryStateName(
                      diagnostics_.recovery_state)));
    DiagnosticRow("Monitor", ToUtf8(diagnostics_.monitor_identity));
    DiagnosticRow("Encoder", ToUtf8(diagnostics_.encoder_identity));
    DiagnosticRow("FPS requested", Decimal(diagnostics_.requested_fps));
    DiagnosticRow("FPS negotiated", Decimal(diagnostics_.negotiated_fps));
    DiagnosticRow("FPS observed", Decimal(diagnostics_.observed_fps));
    DiagnosticRow("Bitrate requested",
                  Bitrate(diagnostics_.requested_bitrate_bps));
    DiagnosticRow("Bitrate negotiated",
                  Bitrate(diagnostics_.negotiated_bitrate_bps));
    DiagnosticRow("Bitrate observed",
                  Bitrate(diagnostics_.observed_bitrate_bps));
    const auto& stats = diagnostics_.recorder_stats;
    DiagnosticRow("Captured / accepted frames",
                  std::to_string(stats.captured_frame_count) + " / " +
                      std::to_string(stats.accepted_frame_count));
    DiagnosticRow("Rate-limited / dropped / encoded",
                  std::to_string(stats.rate_limited_frame_count) + " / " +
                      std::to_string(stats.dropped_frame_count) + " / " +
                      std::to_string(stats.encoded_frame_count));
    DiagnosticRow(
        "Video queue current / peak / cap",
        std::to_string(stats.runtime.queued_video_frame_count) + " / " +
            std::to_string(stats.runtime.peak_queued_video_frame_count) +
            " / " + std::to_string(stats.runtime.video_queue_capacity));
    DiagnosticRow(
        "Video queue age current / max",
        Milliseconds(stats.runtime.video_queue_oldest_frame_age_ns) + " / " +
            Milliseconds(stats.runtime.video_queue_maximum_frame_age_ns));
    DiagnosticRow(
        "Queue overflows / stale recoveries",
        std::to_string(stats.runtime.video_queue_overflow_event_count) +
            " / " +
            std::to_string(stats.runtime.video_queue_backlog_recovery_count));
    DiagnosticRow(
        "Queue drops newest / oldest / backlog",
        std::to_string(stats.runtime.video_queue_dropped_newest_count) +
            " / " +
            std::to_string(stats.runtime.video_queue_dropped_oldest_count) +
            " / " +
            std::to_string(stats.runtime.video_queue_dropped_backlog_count));
    DiagnosticRow(
        "Last queue overflow",
        ToUtf8(capture::VideoFrameQueueOverflowReasonName(
            stats.runtime.video_queue_last_overflow_reason)));
    const auto& texture_pool = stats.runtime.video_texture_pool;
    DiagnosticRow(
        "Capture textures in use / peak / cap",
        std::to_string(texture_pool.in_use_texture_count) + " / " +
            std::to_string(texture_pool.peak_in_use_texture_count) + " / " +
            std::to_string(texture_pool.capacity));
    DiagnosticRow(
        "Capture textures created / reused / exhausted",
        std::to_string(texture_pool.created_texture_count) + " / " +
            std::to_string(texture_pool.reused_texture_count) + " / " +
            std::to_string(
                stats.runtime.video_texture_pool_exhausted_frame_count));
    DiagnosticRow(
        "Converter views input create / reuse",
        std::to_string(stats.runtime.video_converter.input_view_create_count) +
            " / " +
            std::to_string(stats.runtime.video_converter.input_view_reuse_count));
    DiagnosticRow(
        "Converter views output create / reuse",
        std::to_string(stats.runtime.video_converter.output_view_create_count) +
            " / " +
            std::to_string(stats.runtime.video_converter.output_view_reuse_count));
    DiagnosticRow("Video / audio packets",
                  std::to_string(stats.encoded_packet_count) + " / " +
                      std::to_string(stats.encoded_audio_packet_count));
    DiagnosticRow(
        "Audio queue current / peak / track cap",
        std::to_string(stats.runtime.queued_audio_block_count) + " / " +
            std::to_string(stats.runtime.peak_queued_audio_block_count) +
            " / " +
            std::to_string(stats.runtime.audio_queue_capacity_per_track));
    DiagnosticRow("Export commands / outstanding",
                  std::to_string(
                      stats.runtime.pending_export_command_count) +
                      " / " +
                      std::to_string(stats.runtime.outstanding_export_count));
    DiagnosticRow("Exports saved / failed",
                  std::to_string(stats.runtime.saved_export_count) + " / " +
                      std::to_string(stats.runtime.failed_export_count));
    DiagnosticRow("Exports rejected",
                  std::to_string(stats.runtime.rejected_export_count));
    DiagnosticRow("Recovery candidates",
                  std::to_string(diagnostics_.recoverable_session_count) +
                      " recoverable / " +
                      std::to_string(diagnostics_.discardable_session_count) +
                      " discardable");
    ImGui::EndTable();
  }

  SectionLabel("Audio tracks");
  if (diagnostics_.audio_tracks.empty()) {
    ImGui::TextDisabled("No audio tracks configured.");
  } else {
    for (const auto& track : diagnostics_.audio_tracks) {
      const std::string name = ToUtf8(track.name);
      ImGui::BulletText("%s  |  track %u  |  %s", name.c_str(),
                        track.track_id,
                        track.packet_bearing ? "packet-bearing"
                                             : "no packets yet");
    }
  }

  SectionLabel("Paths and status");
  SelectableDiagnosticBlock("Session directory",
                            ToUtf8(diagnostics_.session_directory.wstring()));
  SelectableDiagnosticBlock(
      "Recording output",
      ToUtf8(diagnostics_.recording_output_path.wstring()));
  SelectableDiagnosticBlock(
      "Configured output directory",
      ToUtf8(diagnostics_.configured_output_directory.wstring()));
  if (!diagnostics_.latest_export_state.empty()) {
    SelectableDiagnosticBlock(
        "Latest export",
        ToUtf8(diagnostics_.latest_export_state + L": " +
               diagnostics_.latest_export_path.wstring()));
  }
  if (!diagnostics_.first_actionable_failure.empty()) {
    SelectableDiagnosticBlock(
        "First actionable failure",
        ToUtf8(diagnostics_.first_actionable_failure));
  }

  if (ImGui::Button("Copy report")) {
    ImGui::SetClipboardText(
        ToUtf8(diagnostics::FormatDiagnosticsReport(diagnostics_)).c_str());
  }
  ImGui::EndChild();
}

void SettingsUi::SyncBuffersFromDraft() {
  CopyBuffer(&output_directory_, ToUtf8(draft_.output_directory.native()));
  CopyBuffer(&toggle_recording_,
             ToUtf8(draft_.hotkeys.toggle_recording));
  CopyBuffer(&save_first_preset_,
             ToUtf8(draft_.hotkeys.save_last_30s));
  CopyBuffer(&save_third_preset_, ToUtf8(draft_.hotkeys.save_last_5m));
  CopyBuffer(&bookmark_, ToUtf8(draft_.hotkeys.bookmark));
}

bool SettingsUi::SyncDraftFromBuffers(std::wstring* error) {
  std::wstring output;
  std::wstring toggle;
  std::wstring first;
  std::wstring third;
  std::wstring bookmark;
  if (!FromUtf8(BufferView(output_directory_), &output) ||
      !FromUtf8(BufferView(toggle_recording_), &toggle) ||
      !FromUtf8(BufferView(save_first_preset_), &first) ||
      !FromUtf8(BufferView(save_third_preset_), &third) ||
      !FromUtf8(BufferView(bookmark_), &bookmark)) {
    if (error != nullptr) {
      *error = L"A settings text field contains invalid UTF-8.";
    }
    return false;
  }

  const auto parsed_toggle = settings::ParseHotkey(toggle);
  const auto parsed_first = settings::ParseHotkey(first);
  const auto parsed_third = settings::ParseHotkey(third);
  const auto parsed_bookmark = settings::ParseHotkey(bookmark);
  const settings::HotkeyParseResult* parsed[] = {
      &parsed_toggle, &parsed_first, &parsed_third, &parsed_bookmark};
  for (const auto* value : parsed) {
    if (!value->Succeeded()) {
      if (error != nullptr) {
        *error = value->message;
      }
      return false;
    }
  }

  draft_.output_directory = std::filesystem::path(output);
  draft_.hotkeys.toggle_recording =
      parsed_toggle.hotkey.canonical_label;
  draft_.hotkeys.save_last_30s = parsed_first.hotkey.canonical_label;
  draft_.hotkeys.save_last_5m = parsed_third.hotkey.canonical_label;
  draft_.hotkeys.bookmark = parsed_bookmark.hotkey.canonical_label;
  return true;
}

bool SettingsUi::SaveDraft() {
  std::wstring error;
  if (!SyncDraftFromBuffers(&error) ||
      !settings::Validate(draft_, &error)) {
    SetStatus(std::move(error), true);
    return false;
  }
  if (!apply_sink_(draft_, &error)) {
    SetStatus(std::move(error), true);
    return false;
  }
  SyncBuffersFromDraft();
  SetStatus(L"Settings saved and applied.", false);
  return true;
}

void SettingsUi::PickOutputDirectory() {
  winrt::com_ptr<IFileOpenDialog> dialog;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(dialog.put())))) {
    SetStatus(L"Could not open the output folder picker.", true);
    return;
  }
  DWORD options = 0;
  if (FAILED(dialog->GetOptions(&options)) ||
      FAILED(dialog->SetOptions(options | FOS_PICKFOLDERS |
                                FOS_FORCEFILESYSTEM))) {
    SetStatus(L"Could not configure the output folder picker.", true);
    return;
  }
  dialog->SetTitle(L"Choose O'Louie output folder");
  const HRESULT shown = dialog->Show(host_.hwnd());
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return;
  }
  winrt::com_ptr<IShellItem> item;
  if (FAILED(shown) || FAILED(dialog->GetResult(item.put()))) {
    SetStatus(L"Could not read the selected output folder.", true);
    return;
  }
  PWSTR raw_path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) ||
      raw_path == nullptr) {
    SetStatus(L"The selected output folder has no filesystem path.", true);
    return;
  }
  const std::wstring path(raw_path);
  CoTaskMemFree(raw_path);
  CopyBuffer(&output_directory_, ToUtf8(path));
  SetStatus(L"Output folder selected.", false);
}

void SettingsUi::RefreshAudioOutputs() {
  std::wstring error;
  std::vector<audio::AudioEndpointInfo> endpoints;
  if (!audio::EnumerateActiveAudioEndpoints(audio::AudioEndpointFlow::Render,
                                             &endpoints, &error)) {
    audio_outputs_.clear();
    audio_output_enumeration_error_ = std::move(error);
    return;
  }
  audio_outputs_ = std::move(endpoints);
  audio_output_enumeration_error_.clear();
}

void SettingsUi::StopMicMonitor() {
  if (!mic_monitor_stop_sink_ || !mic_monitor_snapshot_source_) {
    return;
  }
  const auto snapshot = mic_monitor_snapshot_source_();
  if (snapshot.state == audio::MicMonitorState::Starting ||
      snapshot.state == audio::MicMonitorState::Monitoring ||
      snapshot.state == audio::MicMonitorState::Stopping) {
    mic_monitor_stop_sink_();
  }
}

void SettingsUi::SetStatus(std::wstring message, bool error) {
  status_text_ = ToUtf8(message);
  status_is_error_ = error;
}

}  // namespace olouie::ui

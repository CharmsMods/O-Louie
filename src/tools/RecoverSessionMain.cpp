#include <filesystem>
#include <iostream>
#include <system_error>

#include "record/RecordingRecovery.h"

namespace {

bool SamePath(const std::filesystem::path& left,
              const std::filesystem::path& right) {
  std::error_code left_error;
  std::error_code right_error;
  const auto canonical_left =
      std::filesystem::weakly_canonical(left, left_error);
  const auto canonical_right =
      std::filesystem::weakly_canonical(right, right_error);
  return !left_error && !right_error && canonical_left == canonical_right;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  if (argc != 3) {
    std::wcerr << L"Usage: O'LouieRecoverSession <session-directory> "
                  L"<output-directory>\n";
    return 2;
  }

  const std::filesystem::path session_directory = argv[1];
  const std::filesystem::path output_directory = argv[2];
  olouie::record::RecordingRecoveryScanOptions options;
  options.session_root_directory = session_directory.parent_path();
  options.output_directory = output_directory;

  const auto scan = olouie::record::ScanRecordingSessions(options);
  if (!scan.succeeded) {
    std::wcerr << L"Recovery scan failed: " << scan.first_error << L'\n';
    return 1;
  }

  for (const auto& candidate : scan.candidates) {
    if (!SamePath(candidate.session_directory, session_directory)) {
      continue;
    }
    std::wcout << L"Candidate: "
               << olouie::record::RecordingRecoveryKindName(candidate.kind)
               << L", packets=" << candidate.packet_count
               << L", trailing_bytes=" << candidate.trailing_packet_bytes
               << L'\n';
    const auto result = olouie::record::ExportRecoveredRecording(candidate);
    if (!result.Succeeded()) {
      std::wcerr << L"Recovery failed: " << result.message << L'\n';
      return 1;
    }
    std::wcout << L"Recovered recording: " << result.output_path.wstring()
               << L'\n';
    return 0;
  }

  std::wcerr << L"The requested session was not found by the recovery scan.\n";
  return 1;
}

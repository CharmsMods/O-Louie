#pragma once

#include <filesystem>
#include <string>
#include <system_error>

namespace olouie::record {

enum class DiskWriteFaultKind {
  None,
  NoSpace,
  AccessDenied,
  WriteProtected,
  PathUnavailable,
  DestinationExists,
  IoFailure,
};

enum class DiskWriteSubsystem {
  None,
  PacketStore,
  SessionManifest,
  Mp4Mux,
};

enum class DiskWriteOperation {
  None,
  CreateDirectories,
  InspectPath,
  OpenFile,
  OpenTemporaryFile,
  Append,
  WriteContents,
  WriteHeader,
  WritePacket,
  WriteTrailer,
  Flush,
  Close,
  RemoveStalePartial,
  RemoveFailedPartial,
  AtomicPublish,
};

struct DiskWriteFault {
  DiskWriteFaultKind kind = DiskWriteFaultKind::None;
  DiskWriteSubsystem subsystem = DiskWriteSubsystem::None;
  DiskWriteOperation operation = DiskWriteOperation::None;
  std::filesystem::path path;
  std::error_code error_code;
  int backend_error = 0;
  std::wstring detail;

  bool Failed() const noexcept;
};

DiskWriteFault MakeDiskWriteFault(
    DiskWriteSubsystem subsystem,
    DiskWriteOperation operation,
    const std::filesystem::path& path,
    std::error_code error_code = {},
    int backend_error = 0,
    std::wstring detail = {});

std::wstring DescribeDiskWriteFault(const DiskWriteFault& fault);

bool AtomicPublishFile(const std::filesystem::path& temporary_path,
                       const std::filesystem::path& final_path,
                       bool allow_overwrite,
                       DiskWriteSubsystem subsystem,
                       DiskWriteFault* fault);

const wchar_t* DiskWriteFaultKindName(DiskWriteFaultKind kind) noexcept;
const wchar_t* DiskWriteSubsystemName(DiskWriteSubsystem subsystem) noexcept;
const wchar_t* DiskWriteOperationName(DiskWriteOperation operation) noexcept;

}  // namespace olouie::record

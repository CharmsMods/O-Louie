#include "record/DiskWriteFault.h"

#include <windows.h>

#include <cerrno>
#include <utility>

namespace olouie::record {
namespace {

bool Matches(const std::error_code& error, std::errc condition) {
  return error && error == std::make_error_condition(condition);
}

DiskWriteFaultKind Classify(std::error_code error, int backend_error) {
  const bool windows_system_error =
      error && error.category() == std::system_category();
  if (Matches(error, std::errc::no_space_on_device) ||
      backend_error == -ENOSPC ||
      (windows_system_error &&
       (error.value() == ERROR_DISK_FULL ||
        error.value() == ERROR_HANDLE_DISK_FULL))) {
    return DiskWriteFaultKind::NoSpace;
  }
  if (Matches(error, std::errc::permission_denied) ||
      backend_error == -EACCES ||
      (windows_system_error && error.value() == ERROR_ACCESS_DENIED)) {
    return DiskWriteFaultKind::AccessDenied;
  }
  if (Matches(error, std::errc::read_only_file_system) ||
      backend_error == -EROFS ||
      (windows_system_error && error.value() == ERROR_WRITE_PROTECT)) {
    return DiskWriteFaultKind::WriteProtected;
  }
  if (Matches(error, std::errc::no_such_file_or_directory) ||
      Matches(error, std::errc::not_a_directory) ||
      backend_error == -ENOENT ||
      (windows_system_error &&
       (error.value() == ERROR_FILE_NOT_FOUND ||
        error.value() == ERROR_PATH_NOT_FOUND ||
        error.value() == ERROR_INVALID_NAME ||
        error.value() == ERROR_DIRECTORY ||
        error.value() == ERROR_BAD_PATHNAME))) {
    return DiskWriteFaultKind::PathUnavailable;
  }
  if (Matches(error, std::errc::file_exists) ||
      backend_error == -EEXIST ||
      (windows_system_error &&
       (error.value() == ERROR_FILE_EXISTS ||
        error.value() == ERROR_ALREADY_EXISTS))) {
    return DiskWriteFaultKind::DestinationExists;
  }
  return DiskWriteFaultKind::IoFailure;
}

std::wstring ErrorText(const DiskWriteFault& fault) {
  if (fault.error_code) {
    const std::string narrow = fault.error_code.message();
    return std::wstring(narrow.begin(), narrow.end());
  }
  if (fault.backend_error != 0) {
    return L"backend error " + std::to_wstring(fault.backend_error);
  }
  return {};
}

}  // namespace

bool DiskWriteFault::Failed() const noexcept {
  return kind != DiskWriteFaultKind::None;
}

DiskWriteFault MakeDiskWriteFault(
    DiskWriteSubsystem subsystem,
    DiskWriteOperation operation,
    const std::filesystem::path& path,
    std::error_code error_code,
    int backend_error,
    std::wstring detail) {
  DiskWriteFault fault;
  fault.kind = Classify(error_code, backend_error);
  fault.subsystem = subsystem;
  fault.operation = operation;
  fault.path = path;
  fault.error_code = error_code;
  fault.backend_error = backend_error;
  fault.detail = std::move(detail);
  return fault;
}

std::wstring DescribeDiskWriteFault(const DiskWriteFault& fault) {
  if (!fault.Failed()) {
    return {};
  }

  std::wstring message = DiskWriteSubsystemName(fault.subsystem);
  message += L" ";
  message += DiskWriteOperationName(fault.operation);
  message += L" failed";
  if (!fault.path.empty()) {
    message += L" at '" + fault.path.wstring() + L"'";
  }
  message += L": ";
  message += DiskWriteFaultKindName(fault.kind);

  const auto error_text = ErrorText(fault);
  if (!error_text.empty()) {
    message += L" (" + error_text;
    if (fault.error_code) {
      message += L", code " + std::to_wstring(fault.error_code.value());
    }
    message += L")";
  }
  message += L".";
  if (!fault.detail.empty()) {
    message += L" " + fault.detail;
  }
  return message;
}

bool AtomicPublishFile(const std::filesystem::path& temporary_path,
                       const std::filesystem::path& final_path,
                       bool allow_overwrite,
                       DiskWriteSubsystem subsystem,
                       DiskWriteFault* fault) {
  if (fault != nullptr) {
    *fault = {};
  }

  const DWORD temporary_attributes =
      GetFileAttributesW(temporary_path.c_str());
  if (temporary_attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = std::error_code(static_cast<int>(GetLastError()),
                                       std::system_category());
    if (fault != nullptr) {
      *fault = MakeDiskWriteFault(
          subsystem, DiskWriteOperation::InspectPath, temporary_path, error,
          0, L"The temporary file was not available for publication.");
    }
    return false;
  }

  if (!allow_overwrite) {
    const DWORD final_attributes = GetFileAttributesW(final_path.c_str());
    if (final_attributes != INVALID_FILE_ATTRIBUTES) {
      if (fault != nullptr) {
        *fault = MakeDiskWriteFault(
            subsystem, DiskWriteOperation::AtomicPublish, final_path,
            std::make_error_code(std::errc::file_exists));
      }
      return false;
    }
    const DWORD inspect_error = GetLastError();
    if (inspect_error != ERROR_FILE_NOT_FOUND &&
        inspect_error != ERROR_PATH_NOT_FOUND) {
      if (fault != nullptr) {
        *fault = MakeDiskWriteFault(
            subsystem, DiskWriteOperation::InspectPath, final_path,
            std::error_code(static_cast<int>(inspect_error),
                            std::system_category()));
      }
      return false;
    }
  }

  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (allow_overwrite) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  if (!MoveFileExW(temporary_path.c_str(), final_path.c_str(), flags)) {
    if (fault != nullptr) {
      *fault = MakeDiskWriteFault(
          subsystem, DiskWriteOperation::AtomicPublish, final_path,
          std::error_code(static_cast<int>(GetLastError()),
                          std::system_category()));
    }
    return false;
  }
  return true;
}

const wchar_t* DiskWriteFaultKindName(DiskWriteFaultKind kind) noexcept {
  switch (kind) {
    case DiskWriteFaultKind::None:
      return L"none";
    case DiskWriteFaultKind::NoSpace:
      return L"the destination is out of disk space";
    case DiskWriteFaultKind::AccessDenied:
      return L"access was denied";
    case DiskWriteFaultKind::WriteProtected:
      return L"the destination is write-protected";
    case DiskWriteFaultKind::PathUnavailable:
      return L"the destination path is unavailable";
    case DiskWriteFaultKind::DestinationExists:
      return L"the destination already exists";
    case DiskWriteFaultKind::IoFailure:
      return L"an I/O error occurred";
  }
  return L"an unknown disk error occurred";
}

const wchar_t* DiskWriteSubsystemName(DiskWriteSubsystem subsystem) noexcept {
  switch (subsystem) {
    case DiskWriteSubsystem::None:
      return L"Disk output";
    case DiskWriteSubsystem::PacketStore:
      return L"PacketStore";
    case DiskWriteSubsystem::SessionManifest:
      return L"Session manifest";
    case DiskWriteSubsystem::Mp4Mux:
      return L"MP4 mux";
  }
  return L"Disk output";
}

const wchar_t* DiskWriteOperationName(DiskWriteOperation operation) noexcept {
  switch (operation) {
    case DiskWriteOperation::None:
      return L"write";
    case DiskWriteOperation::CreateDirectories:
      return L"directory creation";
    case DiskWriteOperation::InspectPath:
      return L"path inspection";
    case DiskWriteOperation::OpenFile:
      return L"file open";
    case DiskWriteOperation::OpenTemporaryFile:
      return L"temporary-file open";
    case DiskWriteOperation::Append:
      return L"append";
    case DiskWriteOperation::WriteContents:
      return L"content write";
    case DiskWriteOperation::WriteHeader:
      return L"header write";
    case DiskWriteOperation::WritePacket:
      return L"packet write";
    case DiskWriteOperation::WriteTrailer:
      return L"trailer write";
    case DiskWriteOperation::Flush:
      return L"flush";
    case DiskWriteOperation::Close:
      return L"close";
    case DiskWriteOperation::RemoveStalePartial:
      return L"stale-partial cleanup";
    case DiskWriteOperation::RemoveFailedPartial:
      return L"failed-partial cleanup";
    case DiskWriteOperation::AtomicPublish:
      return L"atomic publication";
  }
  return L"write";
}

}  // namespace olouie::record

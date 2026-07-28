cmake_minimum_required(VERSION 3.26)

project(OLouie VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "O'Louie is a Windows-only native desktop application.")
endif()

option(OLOUIE_BUILD_TESTS "Build O'Louie smoke tests." ON)
option(OLOUIE_ENABLE_FFMPEG "Enable the explicit LGPL dynamic FFmpeg MP4 mux backend." OFF)
set(OLOUIE_IMGUI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/_deps/imgui" CACHE PATH "Root of the pinned official Dear ImGui source tree.")
set(OLOUIE_FFMPEG_ROOT "" CACHE PATH "Root of an LGPL dynamic FFmpeg build with include/ and lib/ folders.")
set(OLOUIE_FFMPEG_INCLUDE_DIR "" CACHE PATH "FFmpeg include directory. Defaults to OLOUIE_FFMPEG_ROOT/include when empty.")
set(OLOUIE_FFMPEG_LIBRARY_DIR "" CACHE PATH "FFmpeg import-library directory. Defaults to OLOUIE_FFMPEG_ROOT/lib when empty.")
set(OLOUIE_FFMPEG_BINARY_DIR "" CACHE PATH "FFmpeg runtime DLL directory. Defaults to OLOUIE_FFMPEG_ROOT/bin when empty.")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(OLOUIE_ENABLE_FFMPEG)
  if(OLOUIE_FFMPEG_INCLUDE_DIR STREQUAL "")
    if(OLOUIE_FFMPEG_ROOT STREQUAL "")
      message(FATAL_ERROR "OLOUIE_ENABLE_FFMPEG requires OLOUIE_FFMPEG_ROOT or OLOUIE_FFMPEG_INCLUDE_DIR.")
    endif()
    set(OLOUIE_FFMPEG_INCLUDE_DIR "${OLOUIE_FFMPEG_ROOT}/include")
  endif()

  if(OLOUIE_FFMPEG_LIBRARY_DIR STREQUAL "")
    if(OLOUIE_FFMPEG_ROOT STREQUAL "")
      message(FATAL_ERROR "OLOUIE_ENABLE_FFMPEG requires OLOUIE_FFMPEG_ROOT or OLOUIE_FFMPEG_LIBRARY_DIR.")
    endif()
    set(OLOUIE_FFMPEG_LIBRARY_DIR "${OLOUIE_FFMPEG_ROOT}/lib")
  endif()

  if(OLOUIE_FFMPEG_BINARY_DIR STREQUAL "")
    if(OLOUIE_FFMPEG_ROOT STREQUAL "")
      message(FATAL_ERROR "OLOUIE_ENABLE_FFMPEG requires OLOUIE_FFMPEG_ROOT or OLOUIE_FFMPEG_BINARY_DIR.")
    endif()
    set(OLOUIE_FFMPEG_BINARY_DIR "${OLOUIE_FFMPEG_ROOT}/bin")
  endif()

  foreach(ffmpeg_header
          libavformat/avformat.h
          libavcodec/avcodec.h
          libavutil/avutil.h
          libswresample/swresample.h)
    if(NOT EXISTS "${OLOUIE_FFMPEG_INCLUDE_DIR}/${ffmpeg_header}")
      message(FATAL_ERROR "OLOUIE_ENABLE_FFMPEG could not find ${ffmpeg_header} under ${OLOUIE_FFMPEG_INCLUDE_DIR}.")
    endif()
  endforeach()

  find_library(OLOUIE_FFMPEG_AVFORMAT_LIBRARY NAMES avformat PATHS "${OLOUIE_FFMPEG_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
  find_library(OLOUIE_FFMPEG_AVCODEC_LIBRARY NAMES avcodec PATHS "${OLOUIE_FFMPEG_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
  find_library(OLOUIE_FFMPEG_AVUTIL_LIBRARY NAMES avutil PATHS "${OLOUIE_FFMPEG_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)
  find_library(OLOUIE_FFMPEG_SWRESAMPLE_LIBRARY NAMES swresample PATHS "${OLOUIE_FFMPEG_LIBRARY_DIR}" NO_DEFAULT_PATH REQUIRED)

  set(OLOUIE_FFMPEG_RUNTIME_DLLS)
  foreach(ffmpeg_runtime avformat avcodec avutil swresample)
    file(GLOB ffmpeg_runtime_dlls CONFIGURE_DEPENDS "${OLOUIE_FFMPEG_BINARY_DIR}/${ffmpeg_runtime}*.dll")
    list(LENGTH ffmpeg_runtime_dlls ffmpeg_runtime_dll_count)
    if(ffmpeg_runtime_dll_count EQUAL 0)
      message(FATAL_ERROR "OLOUIE_ENABLE_FFMPEG could not find a runtime DLL matching ${ffmpeg_runtime}*.dll under ${OLOUIE_FFMPEG_BINARY_DIR}.")
    endif()
    list(SORT ffmpeg_runtime_dlls)
    list(GET ffmpeg_runtime_dlls 0 ffmpeg_runtime_dll)
    list(APPEND OLOUIE_FFMPEG_RUNTIME_DLLS "${ffmpeg_runtime_dll}")
  endforeach()
endif()

foreach(imgui_source
        imgui.h
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
        backends/imgui_impl_win32.h
        backends/imgui_impl_win32.cpp
        backends/imgui_impl_dx11.h
        backends/imgui_impl_dx11.cpp
        LICENSE.txt)
  if(NOT EXISTS "${OLOUIE_IMGUI_ROOT}/${imgui_source}")
    message(FATAL_ERROR "Pinned Dear ImGui source is incomplete: ${OLOUIE_IMGUI_ROOT}/${imgui_source}. Run tools/PrepareDearImGui.ps1.")
  endif()
endforeach()

file(STRINGS "${OLOUIE_IMGUI_ROOT}/imgui.h" OLOUIE_IMGUI_VERSION_LINE
     REGEX "^#define IMGUI_VERSION +\"[0-9.]+\"$")
if(NOT OLOUIE_IMGUI_VERSION_LINE MATCHES
   "^#define IMGUI_VERSION[ \t]+\"1\\.92\\.8\"$")
  message(FATAL_ERROR "OLOUIE_IMGUI_ROOT must contain pinned Dear ImGui 1.92.8. Run tools/PrepareDearImGui.ps1.")
endif()

add_library(olouie_imgui STATIC
  "${OLOUIE_IMGUI_ROOT}/imgui.cpp"
  "${OLOUIE_IMGUI_ROOT}/imgui_draw.cpp"
  "${OLOUIE_IMGUI_ROOT}/imgui_tables.cpp"
  "${OLOUIE_IMGUI_ROOT}/imgui_widgets.cpp"
  "${OLOUIE_IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
  "${OLOUIE_IMGUI_ROOT}/backends/imgui_impl_dx11.cpp"
)

target_include_directories(olouie_imgui PUBLIC
  "${OLOUIE_IMGUI_ROOT}"
  "${OLOUIE_IMGUI_ROOT}/backends"
)

target_compile_definitions(olouie_imgui PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_compile_options(olouie_imgui PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

add_library(olouie_core STATIC
  src/app/AppCommand.h
  src/app/AppHost.cpp
  src/app/AppHost.h
  src/product/ProductIdentity.h
  src/audio/AacEncodeSink.cpp
  src/audio/AacEncodeSink.h
  src/audio/AacEncoder.cpp
  src/audio/AacEncoder.h
  src/audio/AacPacketStore.cpp
  src/audio/AacPacketStore.h
  src/audio/AudioCaptureEncodeBridge.cpp
  src/audio/AudioCaptureEncodeBridge.h
  src/audio/AudioCaptureEncodeSetup.cpp
  src/audio/AudioCaptureEncodeSetup.h
  src/audio/AudioCaptureEncodeSmoke.cpp
  src/audio/AudioCaptureEncodeSmoke.h
  src/audio/AudioLiveCaptureEncode.cpp
  src/audio/AudioLiveCaptureEncode.h
  src/audio/AudioRecordingSession.cpp
  src/audio/AudioRecordingSession.h
  src/audio/AudioRecordingMetadata.cpp
  src/audio/AudioRecordingMetadata.h
  src/audio/AudioCaptureManager.cpp
  src/audio/AudioCaptureManager.h
  src/audio/AudioCaptureSmoke.cpp
  src/audio/AudioCaptureSmoke.h
  src/audio/AudioEncodeSessionBinding.cpp
  src/audio/AudioEncodeSessionBinding.h
  src/audio/AudioEncodeSession.cpp
  src/audio/AudioEncodeSession.h
  src/audio/AudioEncodeWorker.cpp
  src/audio/AudioEncodeWorker.h
  src/audio/AudioEndpointManager.cpp
  src/audio/AudioEndpointManager.h
  src/audio/AudioResampler.cpp
  src/audio/AudioResampler.h
  src/audio/AudioSource.cpp
  src/audio/AudioSource.h
  src/audio/AudioSourceRouter.cpp
  src/audio/AudioSourceRouter.h
  src/audio/AudioSourceSessionDispatcher.cpp
  src/audio/AudioSourceSessionDispatcher.h
  src/audio/AudioTrackEncodeChain.cpp
  src/audio/AudioTrackEncodeChain.h
  src/audio/AudioTrackPlan.cpp
  src/audio/AudioTrackPlan.h
  src/audio/CapturedPcmSink.cpp
  src/audio/CapturedPcmSink.h
  src/audio/MicMonitorSession.cpp
  src/audio/MicMonitorSession.h
  src/audio/CapturedPcmSessionSink.cpp
  src/audio/CapturedPcmSessionSink.h
  src/audio/CapturedPcmQueueWriter.cpp
  src/audio/CapturedPcmQueueWriter.h
  src/audio/PcmAudio.cpp
  src/audio/PcmAudio.h
  src/audio/PreparedPcmQueue.cpp
  src/audio/PreparedPcmQueue.h
  src/audio/WasapiLoopbackCapture.cpp
  src/audio/WasapiLoopbackCapture.h
  src/audio/WasapiCaptureSource.cpp
  src/audio/WasapiCaptureSource.h
  src/audio/WasapiMicCapture.cpp
  src/audio/WasapiMicCapture.h
  src/capture/BgraTexturePool.cpp
  src/capture/BgraTexturePool.h
  src/capture/CapturedVideoFrameSink.cpp
  src/capture/CapturedVideoFrameSink.h
  src/capture/VideoFrameCadence.cpp
  src/capture/VideoFrameCadence.h
  src/capture/VideoFrameQueue.cpp
  src/capture/VideoFrameQueue.h
  src/capture/WgcMonitorCapture.cpp
  src/capture/WgcMonitorCapture.h
  src/capture/WgcMonitorCaptureSession.cpp
  src/capture/WgcMonitorCaptureSession.h
  src/diagnostics/DiagnosticsSnapshot.cpp
  src/diagnostics/DiagnosticsSnapshot.h
  src/encode/H264PacketStore.cpp
  src/encode/H264PacketStore.h
  src/encode/MfHardwareH264EncoderProbe.cpp
  src/encode/MfHardwareH264EncoderProbe.h
  src/encode/MfHardwareH264EncoderSession.cpp
  src/encode/MfHardwareH264EncoderSession.h
  src/encode/VideoCaptureEncodeBridge.cpp
  src/encode/VideoCaptureEncodeBridge.h
  src/encode/VideoEncodeChain.cpp
  src/encode/VideoEncodeChain.h
  src/encode/VideoEncodeWorker.cpp
  src/encode/VideoEncodeWorker.h
  src/encode/VideoEncodeThread.cpp
  src/encode/VideoEncodeThread.h
  src/encode/VideoRecordingBootstrap.cpp
  src/encode/VideoRecordingBootstrap.h
  src/encode/VideoLiveCaptureEncode.cpp
  src/encode/VideoLiveCaptureEncode.h
  src/encode/VideoRecordingMetadata.cpp
  src/encode/VideoRecordingMetadata.h
  src/encode/VideoRecordingRunSession.cpp
  src/encode/VideoRecordingRunSession.h
  src/encode/VideoRecordingSetup.cpp
  src/encode/VideoRecordingSetup.h
  src/encode/VideoRecordingSession.cpp
  src/encode/VideoRecordingSession.h
  src/graphics/D3D11DeviceContext.cpp
  src/graphics/D3D11DeviceContext.h
  src/graphics/D3D11DeviceFault.cpp
  src/graphics/D3D11DeviceFault.h
  src/graphics/DisplayManager.cpp
  src/graphics/DisplayManager.h
  src/graphics/GpuBgraToNv12.cpp
  src/graphics/GpuBgraToNv12.h
  src/app/SingleInstance.cpp
  src/app/SingleInstance.h
  src/logging/Logger.cpp
  src/logging/Logger.h
  src/logging/RuntimePaths.cpp
  src/logging/RuntimePaths.h
  src/logging/RuntimePathMigration.cpp
  src/logging/RuntimePathMigration.h
  src/performance/CapturePerformance.cpp
  src/performance/CapturePerformance.h
  src/performance/MultimediaThreadScheduling.cpp
  src/performance/MultimediaThreadScheduling.h
  src/record/Bookmark.cpp
  src/record/Bookmark.h
  src/record/ActiveRecordingBookmark.cpp
  src/record/ActiveRecordingBookmark.h
  src/record/ActiveRecordingClip.cpp
  src/record/ActiveRecordingClip.h
  src/record/ClipExportQueue.cpp
  src/record/ClipExportQueue.h
  src/record/DiskWriteFault.cpp
  src/record/DiskWriteFault.h
  src/record/Mp4Muxer.cpp
  src/record/Mp4Muxer.h
  src/record/MuxPlan.cpp
  src/record/MuxPlan.h
  src/record/PacketStore.cpp
  src/record/PacketStore.h
  src/record/RecordingRecovery.cpp
  src/record/RecordingRecovery.h
  src/record/SessionManifest.cpp
  src/record/SessionManifest.h
  src/record/SessionClock.cpp
  src/record/SessionClock.h
  src/record/Timebase.cpp
  src/record/Timebase.h
  src/record/VideoExportPlan.cpp
  src/record/VideoExportPlan.h
  src/record/VideoRecorderPipeline.cpp
  src/record/VideoRecorderPipeline.h
  src/record/VideoRecorderClip.cpp
  src/record/VideoRecorderClip.h
  src/record/VideoRecorderSession.cpp
  src/record/VideoRecorderSession.h
  src/settings/Settings.cpp
  src/settings/Settings.h
  src/settings/HotkeyBinding.cpp
  src/settings/HotkeyBinding.h
  src/settings/SettingsStore.cpp
  src/settings/SettingsStore.h
  src/ui/ImGuiDx11Host.cpp
  src/ui/ImGuiDx11Host.h
  src/ui/SettingsUi.cpp
  src/ui/SettingsUi.h
  src/win32/HiddenWindow.cpp
  src/win32/HiddenWindow.h
  src/win32/HotkeyManager.cpp
  src/win32/HotkeyManager.h
  src/win32/StartupRegistration.cpp
  src/win32/StartupRegistration.h
  src/win32/TrayController.cpp
  src/win32/TrayController.h
  src/win32/WindowAffinity.cpp
  src/win32/WindowAffinity.h
)

target_include_directories(olouie_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(olouie_core PUBLIC
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_compile_options(olouie_core PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive- /EHsc /utf-8>
)

target_link_libraries(olouie_core PUBLIC
  olouie_imgui
  advapi32
  avrt
  d3d11
  dxgi
  mf
  mfplat
  mfuuid
  ole32
  shell32
  user32
  uuid
  windowsapp
  wmcodecdspuuid
)

if(OLOUIE_ENABLE_FFMPEG)
  target_compile_definitions(olouie_core PUBLIC OLOUIE_FFMPEG_CONFIGURED=1)
  target_include_directories(olouie_core PUBLIC "${OLOUIE_FFMPEG_INCLUDE_DIR}")
  target_link_libraries(olouie_core PUBLIC
    "${OLOUIE_FFMPEG_AVFORMAT_LIBRARY}"
    "${OLOUIE_FFMPEG_AVCODEC_LIBRARY}"
    "${OLOUIE_FFMPEG_AVUTIL_LIBRARY}"
    "${OLOUIE_FFMPEG_SWRESAMPLE_LIBRARY}"
  )
else()
  target_compile_definitions(olouie_core PUBLIC OLOUIE_FFMPEG_CONFIGURED=0)
endif()

configure_file(
  src/win32/AppVersion.rc.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/AppVersion.rc"
  @ONLY
)

add_executable(OLouie WIN32
  src/main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/AppVersion.rc"
)

target_link_libraries(OLouie PRIVATE olouie_core)

add_executable(OLouieCaptureSmoke
  src/tools/CaptureSmokeMain.cpp
)

target_link_libraries(OLouieCaptureSmoke PRIVATE olouie_core)

add_executable(OLouieAudioSmoke
  src/tools/AudioSmokeMain.cpp
)

target_link_libraries(OLouieAudioSmoke PRIVATE olouie_core)

add_executable(OLouieEncodeSmoke
  src/tools/EncodeSmokeMain.cpp
)

target_link_libraries(OLouieEncodeSmoke PRIVATE olouie_core)

add_executable(OLouieSettingsUiSmoke
  src/tools/SettingsUiSmokeMain.cpp
)

target_link_libraries(OLouieSettingsUiSmoke PRIVATE olouie_core)

add_executable(OLouieRuntimeControl
  src/tools/RuntimeControlMain.cpp
)

target_link_libraries(OLouieRuntimeControl PRIVATE user32)

add_executable(OLouieDisplayModeExercise
  src/tools/DisplayModeExerciseMain.cpp
)

target_link_libraries(OLouieDisplayModeExercise PRIVATE user32)

add_executable(OLouieLongSessionStimulus WIN32
  src/tools/LongSessionStimulusMain.cpp
)

target_link_libraries(OLouieLongSessionStimulus PRIVATE shell32 user32 gdi32)

add_executable(OLouieRecoverSession
  src/tools/RecoverSessionMain.cpp
)

target_link_libraries(OLouieRecoverSession PRIVATE olouie_core)

if(OLOUIE_BUILD_TESTS)
  enable_testing()

  add_executable(OLouieSmokeTests
    tests/SmokeTests.cpp
  )

  target_link_libraries(OLouieSmokeTests PRIVATE olouie_core)

  add_test(NAME OLouieSmokeTests COMMAND OLouieSmokeTests)

  add_executable(OLouieRecordTests
    tests/RecordTests.cpp
  )

  target_link_libraries(OLouieRecordTests PRIVATE olouie_core)

  add_test(NAME OLouieRecordTests COMMAND OLouieRecordTests)

  add_executable(OLouieAudioTests
    tests/AudioTests.cpp
  )

  target_link_libraries(OLouieAudioTests PRIVATE olouie_core)

  add_test(NAME OLouieAudioTests COMMAND OLouieAudioTests)

  add_executable(OLouieEncodeTests
    tests/EncodeTests.cpp
  )

  target_link_libraries(OLouieEncodeTests PRIVATE olouie_core)

  add_test(NAME OLouieEncodeTests COMMAND OLouieEncodeTests)

  find_program(OLOUIE_POWERSHELL_EXECUTABLE NAMES powershell.exe powershell pwsh.exe pwsh)
  if(OLOUIE_POWERSHELL_EXECUTABLE)
    add_test(
      NAME OLouieFfmpegGateToolTests
      COMMAND "${OLOUIE_POWERSHELL_EXECUTABLE}"
              -NoProfile
              -ExecutionPolicy
              Bypass
              -File
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/TestFfmpegGateTools.ps1"
    )
    add_test(
      NAME OLouieLongSessionToolTests
      COMMAND "${OLOUIE_POWERSHELL_EXECUTABLE}"
              -NoProfile
              -ExecutionPolicy
              Bypass
              -File
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/TestLongSessionExerciseTools.ps1"
    )
  else()
    message(WARNING "PowerShell was not found; skipping PowerShell tool tests.")
  endif()
endif()

foreach(olouie_named_target
        OLouie
        OLouieCaptureSmoke
        OLouieAudioSmoke
        OLouieEncodeSmoke
        OLouieSettingsUiSmoke
        OLouieRuntimeControl
        OLouieDisplayModeExercise
        OLouieLongSessionStimulus
        OLouieRecoverSession
        OLouieSmokeTests
        OLouieRecordTests
        OLouieAudioTests
        OLouieEncodeTests)
  if(TARGET ${olouie_named_target})
    string(REGEX REPLACE "^OLouie" "O'Louie" olouie_output_name
           "${olouie_named_target}")
    set_target_properties(${olouie_named_target}
                          PROPERTIES OUTPUT_NAME "${olouie_output_name}")
  endif()
endforeach()

if(OLOUIE_ENABLE_FFMPEG)
  foreach(olouie_runtime_target
          OLouie
          OLouieCaptureSmoke
          OLouieAudioSmoke
          OLouieEncodeSmoke
          OLouieSettingsUiSmoke
          OLouieRecoverSession
          OLouieSmokeTests
          OLouieRecordTests
          OLouieAudioTests
          OLouieEncodeTests)
    if(TARGET ${olouie_runtime_target})
      add_custom_command(
        TARGET ${olouie_runtime_target}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${OLOUIE_FFMPEG_RUNTIME_DLLS}
                $<TARGET_FILE_DIR:${olouie_runtime_target}>
        COMMENT "Copying configured FFmpeg runtime DLLs for ${olouie_runtime_target}"
      )
    endif()
  endforeach()
endif()

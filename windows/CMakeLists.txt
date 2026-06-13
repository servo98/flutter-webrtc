cmake_minimum_required(VERSION 3.15)
set(PROJECT_NAME "flutter_webrtc")
project(${PROJECT_NAME} LANGUAGES CXX C)  # [chatpapol] C para RNNoise/speex

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

# Add the libwebrtc dependency
include("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/CMakeLists.txt")

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "flutter_webrtc_plugin")

add_definitions(-DLIB_WEBRTC_API_DLL)
add_definitions(-DRTC_DESKTOP_DEVICE)

# [chatpapol] supresor de ruido RNNoise (motor + adaptador + libs vendorizadas)
set(CHATPAPOL_RNNOISE_C
  "../third_party/rnnoise/src/celt_lpc.c"
  "../third_party/rnnoise/src/denoise.c"
  "../third_party/rnnoise/src/kiss_fft.c"
  "../third_party/rnnoise/src/pitch.c"
  "../third_party/rnnoise/src/rnn.c"
  "../third_party/rnnoise/src/rnn_data.c"
  "../third_party/rnnoise/src/rnn_reader.c")
set(CHATPAPOL_SPEEX_C "../third_party/speex_resampler/resample.c")
set(CHATPAPOL_DSP_CPP
  "../common/cpp/src/rnnoise_engine.cc")
# [chatpapol] efectos de voz (voicefx) — DSP autocontenido (sin deps externas).
# Incluye el adaptador rnnoise_processor.cc (llama a la C ABI de voicefx).
set(CHATPAPOL_VOICEFX_CPP
  "../common/cpp/src/rnnoise_processor.cc"
  "../common/cpp/src/per_user_eq.cc"
  "../common/cpp/voicefx/voicefx.cpp"
  "../common/cpp/voicefx/effects.cpp"
  "../common/cpp/voicefx/pitch.cpp")

add_library(${PLUGIN_NAME} SHARED
  ${CHATPAPOL_RNNOISE_C}
  ${CHATPAPOL_SPEEX_C}
  ${CHATPAPOL_DSP_CPP}
  ${CHATPAPOL_VOICEFX_CPP}
  "../common/cpp/src/flutter_common.cc"
  "../common/cpp/src/flutter_data_channel.cc"
  "../common/cpp/src/flutter_data_packet_cryptor.cc"
  "../common/cpp/src/flutter_frame_cryptor.cc"
  "../common/cpp/src/flutter_media_stream.cc"
  "../common/cpp/src/flutter_utf8_sanitize.cc"
  "../common/cpp/src/flutter_peerconnection.cc"
  "../common/cpp/src/flutter_frame_capturer.cc"
  "../common/cpp/src/flutter_video_renderer.cc"
  "../common/cpp/src/flutter_screen_capture.cc"
  "../common/cpp/src/flutter_webrtc.cc"
  "../common/cpp/src/flutter_webrtc_base.cc"
  "application_loopback_capturer.cc"
  "loopback_capturer_factory.cc"
  "../common/cpp/src/mic_capturer_common.cc"
  "mic_capturer_windows.cc"
  "flutter_webrtc_plugin.cc"
  "task_runner_windows.cc"
)

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/voicefx/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/svpng"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rnnoise/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rnnoise/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/speex_resampler"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/speex_resampler/speex"
)

# [chatpapol] defines por-archivo + silenciar warnings del código de terceros
# (apply_standard_settings activa /WX; /w evita que rnnoise/speex rompan el build).
set_source_files_properties(${CHATPAPOL_RNNOISE_C} PROPERTIES
  COMPILE_DEFINITIONS "RNNOISE_EXPORT=;_CRT_SECURE_NO_WARNINGS;_USE_MATH_DEFINES"
  COMPILE_OPTIONS "/w")
set_source_files_properties(${CHATPAPOL_SPEEX_C} PROPERTIES
  COMPILE_DEFINITIONS "OUTSIDE_SPEEX;FLOATING_POINT;EXPORT=;RANDOM_PREFIX=rnnoise_rs;_CRT_SECURE_NO_WARNINGS"
  COMPILE_OPTIONS "/w")
set_source_files_properties(${CHATPAPOL_DSP_CPP} PROPERTIES
  COMPILE_DEFINITIONS "RANDOM_PREFIX=rnnoise_rs;OUTSIDE_SPEEX;FLOATING_POINT;RNNOISE_EXPORT="
  COMPILE_OPTIONS "/w")
# voicefx: DSP propio; /w para que /WX no rompa el build por warnings benignos.
# VOICEFX_BUILD => VFX_EXPORT = dllexport (no dllimport): se compila DENTRO del
# plugin, así que los símbolos se definen aquí (evita los LNK4217).
set_source_files_properties(${CHATPAPOL_VOICEFX_CPP} PROPERTIES
  COMPILE_DEFINITIONS "VOICEFX_BUILD"
  COMPILE_OPTIONS "/w")

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE 
  flutter
  flutter_wrapper_plugin
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/lib/libwebrtc.dll.lib"
  avrt.lib
  ksuser.lib
  mmdevapi.lib
  ole32.lib
  runtimeobject.lib
  uuid.lib
  winmm.lib
)

# List of absolute paths to libraries that should be bundled with the plugin
set(flutter_webrtc_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/lib/libwebrtc.dll"
  PARENT_SCOPE
)

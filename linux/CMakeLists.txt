cmake_minimum_required(VERSION 3.10)
set(PROJECT_NAME "flutter_webrtc")
project(${PROJECT_NAME} LANGUAGES CXX C)  # [chatpapol] C para RNNoise/speex

if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
  cmake_policy(SET CMP0135 NEW)
endif()

# Add the libwebrtc dependency
include("${CMAKE_CURRENT_SOURCE_DIR}/../third_party/CMakeLists.txt")

set(PLUGIN_NAME "${PROJECT_NAME}_plugin")
set (CMAKE_EXPORT_COMPILE_COMMANDS ON )
set(CMAKE_CXX_STANDARD 17)

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
  "../common/cpp/src/rnnoise_engine.cc"
  "../common/cpp/src/rnnoise_processor.cc")
# [chatpapol] efectos de voz (voicefx) — DSP autocontenido (sin deps externas)
set(CHATPAPOL_VOICEFX_CPP
  "../common/cpp/voicefx/voicefx.cpp"
  "../common/cpp/voicefx/effects.cpp"
  "../common/cpp/voicefx/pitch.cpp")

add_library(${PLUGIN_NAME} SHARED
  ${CHATPAPOL_RNNOISE_C}
  ${CHATPAPOL_SPEEX_C}
  ${CHATPAPOL_DSP_CPP}
  ${CHATPAPOL_VOICEFX_CPP}
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
  "../common/cpp/src/flutter_common.cc"
  "loopback_capturer_factory.cc"
  "flutter_webrtc_plugin.cc"
  "flutter/core_implementations.cc"
  "flutter/standard_codec.cc"
  "flutter/plugin_registrar.cc"
  "task_runner_linux.cc"
)

include_directories(
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/flutter/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/voicefx/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/svpng"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rnnoise/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rnnoise/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/speex_resampler"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/speex_resampler/speex"
)

# [chatpapol] defines por-archivo + silenciar warnings del código de terceros.
set_source_files_properties(${CHATPAPOL_RNNOISE_C} PROPERTIES
  COMPILE_DEFINITIONS "RNNOISE_EXPORT=;_USE_MATH_DEFINES"
  COMPILE_OPTIONS "-w")
set_source_files_properties(${CHATPAPOL_SPEEX_C} PROPERTIES
  COMPILE_DEFINITIONS "OUTSIDE_SPEEX;FLOATING_POINT;EXPORT=;RANDOM_PREFIX=rnnoise_rs"
  COMPILE_OPTIONS "-w")
set_source_files_properties(${CHATPAPOL_DSP_CPP} PROPERTIES
  COMPILE_DEFINITIONS "RANDOM_PREFIX=rnnoise_rs;OUTSIDE_SPEEX;FLOATING_POINT;RNNOISE_EXPORT="
  COMPILE_OPTIONS "-w")
# voicefx: DSP propio, sin -Werror para no romper el build por warnings benignos.
set_source_files_properties(${CHATPAPOL_VOICEFX_CPP} PROPERTIES
  COMPILE_OPTIONS "-w")

apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../common/cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/include"
)
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter)
target_link_libraries(${PLUGIN_NAME} PRIVATE PkgConfig::GTK)


target_link_libraries(${PLUGIN_NAME} PRIVATE 
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/lib/libwebrtc.so"
)

# List of absolute paths to libraries that should be bundled with the plugin
set(flutter_webrtc_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/libwebrtc/lib/libwebrtc.so"
  PARENT_SCOPE
)

# Add $ORIGIN to RPATH so that lib/libflutter_webrtc_plugin.so can find lib/libwebrtc.so at runtime
set_property(
    TARGET ${PLUGIN_NAME}
    PROPERTY BUILD_RPATH
    "\$ORIGIN"
)

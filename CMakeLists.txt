# claude-speak — single self-contained speak.exe: PocketTTS.cpp inference +
# WASAPI playback + a pulsing on-screen orb. No Python, no server, no ffplay.
#
# Build (Windows):  build.bat
# Build (generic):  cmake -B .build -DCMAKE_BUILD_TYPE=Release && cmake --build .build

cmake_minimum_required(VERSION 3.28)
project(claude_speak VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT WIN32)
    message(FATAL_ERROR "claude-speak's orb overlay and audio output are Win32-only")
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ── PocketTTS.cpp: the inference engine, compiled straight into our binary ──
# It is a single source file, so we just fetch that file (pinned) rather than
# pulling in its whole project. PTT_SHARED_LIB drops its main() and keeps the
# ptt_* C API we call from speak.cpp.
set(POCKETTTS_REF "e801e7d6c2692121a39e80ae525cb5265174a495")
set(POCKETTTS_SRC ${CMAKE_BINARY_DIR}/pocket_tts.cpp)
if(NOT EXISTS ${POCKETTTS_SRC})
    message(STATUS "Downloading pocket_tts.cpp @ ${POCKETTTS_REF}")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/VolgaGerm/PocketTTS.cpp/${POCKETTTS_REF}/pocket_tts.cpp"
        ${POCKETTTS_SRC} TLS_VERIFY ON STATUS dl_status)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        file(REMOVE ${POCKETTTS_SRC})
        message(FATAL_ERROR "Failed to download pocket_tts.cpp: ${dl_status}")
    endif()
endif()

# ── ONNX Runtime (prebuilt) ─────────────────────────────────────────────────
set(ONNXRUNTIME_VERSION "1.23.2")
FetchContent_Declare(onnxruntime
    URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(onnxruntime)
set(ONNXRUNTIME_ROOT ${onnxruntime_SOURCE_DIR})

# ── SentencePiece (static) ──────────────────────────────────────────────────
set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(SPM_PROTOBUF_PROVIDER "internal" CACHE STRING "" FORCE)
set(SPM_ABSL_PROVIDER "internal" CACHE STRING "" FORCE)
set(SPM_ENABLE_NFKC_COMPILE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(sentencepiece
    GIT_REPOSITORY https://github.com/google/sentencepiece.git
    GIT_TAG v0.2.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(sentencepiece)
target_compile_options(sentencepiece-static PRIVATE -w)

# ── dr_libs (header-only audio decoding) ────────────────────────────────────
FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(dr_libs)

# ── speak.exe ───────────────────────────────────────────────────────────────
# speak.cpp #includes pocket_tts.cpp, so they compile as one translation unit:
# that is what lets us use upstream's TTSServer (daemon mode) as well as its
# ptt_* C API. ${CMAKE_BINARY_DIR} is where the fetched source landed.
add_executable(speak speak.cpp)
target_compile_definitions(speak PRIVATE PTT_SHARED_LIB _CRT_SECURE_NO_WARNINGS)
target_include_directories(speak PRIVATE
    ${CMAKE_BINARY_DIR}
    ${ONNXRUNTIME_ROOT}/include
    ${sentencepiece_SOURCE_DIR}/src
    ${dr_libs_SOURCE_DIR}
)
target_link_libraries(speak PRIVATE
    ${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib
    sentencepiece-static
    ws2_32 ole32 gdi32 user32 dwmapi
    windowscodecs   # WIC, for decoding the avatar PNGs the enricher downloads
    windowscodecs   # WIC, for decoding the avatar PNGs the enricher downloads
    delayimp
)

# Delay-load the 14 MB inference DLL: when a daemon is already running, the
# client path never touches ONNX Runtime, so it should not pay to map it in.
target_link_options(speak PRIVATE /DELAYLOAD:onnxruntime.dll)

# onnxruntime.dll is the one file that must sit beside the exe.
add_custom_command(TARGET speak POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll $<TARGET_FILE_DIR:speak>
)

message(STATUS "")
message(STATUS "=== claude-speak ===")
message(STATUS "Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "ONNX Runtime:  ${ONNXRUNTIME_VERSION}")
message(STATUS "PocketTTS.cpp: ${POCKETTTS_SRC}")
message(STATUS "====================")
message(STATUS "")

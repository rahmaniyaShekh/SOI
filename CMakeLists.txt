# 3.20 is what Visual Studio 2019 Build Tools bundles; nothing here needs more.
cmake_minimum_required(VERSION 3.20)
project(soi-share LANGUAGES CXX VERSION 1.0.0)

if(NOT WIN32)
    message(FATAL_ERROR "soi-share targets Windows (Media Foundation + GDI BitBlt).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT so the executable runs on a clean machine with no Visual C++
# redistributable installed. Every fetched dependency inherits this.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(OPENSSL_USE_STATIC_LIBS ON)
set(OPENSSL_MSVC_STATIC_RT  ON)

option(SOI_BUILD_APP      "Build the full streaming app (needs OpenSSL for libdatachannel)" ON)
option(SOI_BUILD_SELFTEST "Build the self-test binary" ON)

# Any libdatachannel release >= 0.20 exposes the API used here
# (H264RtpPacketizer(Separator, ...), MediaHandler::addToChain, Track::setMediaHandler).
set(SOI_LIBDATACHANNEL_TAG "v0.22.2" CACHE STRING "libdatachannel git tag")

include(FetchContent)
include(CheckCXXSourceCompiles)

# ---------------------------------------------------------------------------
# <format> availability.
#
# Probe by compiling rather than sniffing versions: <format> landed in MSVC
# 19.30, GCC 13 and Clang 17, and the feature-test macro is not reliable across
# all three. {fmt} is fetched only when the probe fails.
# ---------------------------------------------------------------------------
if(MSVC)
    set(CMAKE_REQUIRED_FLAGS "/std:c++20")
else()
    set(CMAKE_REQUIRED_FLAGS "-std=c++20")
endif()
# Probe the exact construct util/Log.h relies on, not just std::format(). MSVC
# 19.29 (VS2019 16.11) ships std::format but NOT std::format_string, so a naive
# probe passes and the build then fails deep inside the Windows SDK headers.
check_cxx_source_compiles([[
    #include <format>
    #include <string>
    #include <utility>
    template <class... A> using FS = std::format_string<A...>;
    template <class... A> std::string wrap(FS<A...> f, A&&... a) {
        return std::format(f, std::forward<A>(a)...);
    }
    int main() { return (int)wrap("{}-{}", 42, 7).size(); }
]] SOI_HAVE_STD_FORMAT)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT SOI_HAVE_STD_FORMAT)
    message(STATUS "<format> unavailable on this toolchain -- fetching {fmt}")
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        10.2.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(fmt)
endif()

# ---------------------------------------------------------------------------
# miniz -- raw DEFLATE, matching the browser's CompressionStream('deflate-raw')
# ---------------------------------------------------------------------------
set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS      OFF CACHE BOOL "" FORCE)
set(BUILD_HEADER_ONLY  OFF CACHE BOOL "" FORCE)
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)

FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(miniz)

# ---------------------------------------------------------------------------
# soi-core -- everything that does not need a WebRTC stack.
# Kept separate so the capture/encode/signalling layers stay testable without
# OpenSSL or libdatachannel present.
# ---------------------------------------------------------------------------
add_library(soi-core STATIC
    src/app/LocalHandover.cpp
    src/app/Rendezvous.cpp
    src/app/Service.cpp
    src/capture/BitBltCapture.cpp
    src/capture/CaptureFactory.cpp
    src/capture/CaptureProtect.cpp
    src/capture/DxgiCapture.cpp
    src/capture/FrameBuffer.cpp
    src/capture/WgcCapture.cpp
    src/encode/ColorConvert.cpp
    src/encode/H264Encoder.cpp
    src/encode/Quality.cpp
    src/net/SignalBlob.cpp
    src/util/Log.cpp
)

target_include_directories(soi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${miniz_SOURCE_DIR}
    ${miniz_BINARY_DIR}
)

target_link_libraries(soi-core PUBLIC
    miniz
    mfplat mfuuid mfreadwrite wmcodecdspuuid strmiids
    ole32 oleaut32 uuid
    user32 gdi32 shcore dwmapi shell32
    # Capture backends: d3d11/dxgi for Desktop Duplication, runtimeobject for the
    # WinRT activation Windows.Graphics.Capture needs. Both are inbox on every
    # supported Windows, so this adds no deployment requirement.
    d3d11 dxgi runtimeobject
    bcrypt
    ws2_32 iphlpapi winhttp
)

target_compile_definitions(soi-core PUBLIC
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    # Windows 10 APIs: SetProcessDpiAwarenessContext, WDA_EXCLUDEFROMCAPTURE,
    # CREATE_WAITABLE_TIMER_HIGH_RESOLUTION.
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007
)

if(NOT SOI_HAVE_STD_FORMAT)
    target_compile_definitions(soi-core PUBLIC SOI_USE_FMTLIB)
    target_link_libraries(soi-core PUBLIC fmt::fmt)
endif()

if(MSVC)
    target_compile_options(soi-core PUBLIC /W4 /permissive- /utf-8 /MP /EHsc)
else()
    target_compile_options(soi-core PUBLIC -Wall -Wextra -msse4.1)
endif()

# ---------------------------------------------------------------------------
# soi-selftest
# ---------------------------------------------------------------------------
if(SOI_BUILD_SELFTEST)
    add_executable(soi-selftest tests/selftest.cpp)
    # d3d11/dxgi come through soi-core now that Desktop Duplication is a capture
    # backend, but the test also uses them directly to verify capture protection
    # against the path most screen recorders actually use.
    target_link_libraries(soi-selftest PRIVATE soi-core d3d11 dxgi)
    if(MSVC)
        target_link_options(soi-selftest PRIVATE /SUBSYSTEM:CONSOLE)
    endif()
endif()

# ---------------------------------------------------------------------------
# soi-share -- the full app. Needs libdatachannel, which needs OpenSSL.
# ---------------------------------------------------------------------------
if(SOI_BUILD_APP)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        message(WARNING
            "OpenSSL not found -- skipping the streaming app (soi-core and "
            "soi-selftest still build). Install it, e.g. "
            "'vcpkg install openssl:x64-windows', and configure with "
            "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake")
        set(SOI_BUILD_APP OFF)
    endif()
endif()

if(SOI_BUILD_APP)
    set(NO_EXAMPLES  ON  CACHE BOOL "" FORCE)
    set(NO_TESTS     ON  CACHE BOOL "" FORCE)
    set(NO_MEDIA     OFF CACHE BOOL "" FORCE)   # we need the media/RTP layer
    set(NO_WEBSOCKET ON  CACHE BOOL "" FORCE)   # serverless: no websocket signalling
    set(USE_GNUTLS   OFF CACHE BOOL "" FORCE)
    set(USE_NICE     OFF CACHE BOOL "" FORCE)   # libjuice is lighter and enough

    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG        ${SOI_LIBDATACHANNEL_TAG}
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(libdatachannel)

    add_executable(soi-share
        src/main.cpp
        src/net/Streamer.cpp
    )
    target_link_libraries(soi-share PRIVATE
        soi-core
        LibDataChannel::LibDataChannel
        ws2_32 iphlpapi
    )
    if(MSVC)
        target_link_options(soi-share PRIVATE /SUBSYSTEM:CONSOLE)
    endif()

    # Keep viewer.html next to the executable so --open-viewer finds it.
    add_custom_command(TARGET soi-share POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/viewer/viewer.html
            $<TARGET_FILE_DIR:soi-share>/viewer.html
        COMMENT "Staging viewer.html"
    )
endif()

message(STATUS "soi: std::format=${SOI_HAVE_STD_FORMAT}  app=${SOI_BUILD_APP}  selftest=${SOI_BUILD_SELFTEST}")

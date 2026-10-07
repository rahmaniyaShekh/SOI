# 3.20 is what Visual Studio 2019 Build Tools bundles; nothing here needs more.
cmake_minimum_required(VERSION 3.20)

# macOS: the oldest release the binary runs on. 10.15 is the first with the
# Screen Recording permission API (CGPreflightScreenCaptureAccess) and covers
# every Mac that runs a supported browser; Apple silicon starts at 11.0 anyway.
# Must be set before project().
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
endif()

project(soi-share LANGUAGES CXX VERSION 1.1.0)

if(APPLE)
    enable_language(OBJCXX)
elseif(NOT WIN32)
    message(FATAL_ERROR "soi-share targets Windows (Media Foundation) and macOS (VideoToolbox).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)
# Static CRT so the executable runs on a clean machine with no Visual C++
# redistributable installed. Every fetched dependency inherits this.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(OPENSSL_USE_STATIC_LIBS ON)
set(OPENSSL_MSVC_STATIC_RT  ON)

# Stamped into the exe: `soi-share version`, the updater's comparison, and the
# file's VERSIONINFO. The release workflow passes the git tag (v1.2.3 -> 1.2.3).
set(SOI_VERSION "${PROJECT_VERSION}" CACHE STRING "Version stamped into the binaries")
if(NOT SOI_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)([-+][0-9A-Za-z.+-]*)?$")
    message(FATAL_ERROR "SOI_VERSION must look like 1.2.3 (got '${SOI_VERSION}')")
endif()
set(SOI_VERSION_MAJOR ${CMAKE_MATCH_1})
set(SOI_VERSION_MINOR ${CMAKE_MATCH_2})
set(SOI_VERSION_PATCH ${CMAKE_MATCH_3})
# Where `soi-share update` looks for releases.
set(SOI_GITHUB_REPO "rahmaniyaShekh/SOI" CACHE STRING "owner/name of the GitHub repository")

option(SOI_BUILD_APP      "Build the full streaming app (needs OpenSSL for libdatachannel)" ON)
option(SOI_BUILD_SELFTEST "Build the self-test binary" ON)

# Any libdatachannel release >= 0.20 exposes the API used here
# (H264RtpPacketizer(Separator, ...), MediaHandler::addToChain, Track::setMediaHandler).
set(SOI_LIBDATACHANNEL_TAG "v0.22.2" CACHE STRING "libdatachannel git tag")

include(FetchContent)
include(CheckCXXSourceCompiles)

# CMake 4 refuses projects that declare cmake_minimum_required below 3.5, and
# miniz 3.0.2 and libdatachannel's bundled usrsctp still do. This lets them
# configure under their old policies; CMake 3.x ignores the variable.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

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
#
# Apple's libc++ is the other trap: std::format compiles, but formatting a
# double needs a to_chars that only exists from macOS 13.3, so with an older
# deployment target the build fails on the first "{:.1f}". {fmt} it is there.
if(APPLE)
    set(SOI_HAVE_STD_FORMAT OFF)
else()
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
endif()
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
set(SOI_CORE_COMMON
    src/app/Control.cpp
    src/app/Lifecycle.cpp
    src/app/LocalHandover.cpp
    src/app/Rendezvous.cpp
    src/app/Service.cpp
    src/app/Update.cpp
    src/capture/CaptureFactory.cpp
    src/encode/ColorConvert.cpp
    src/encode/Quality.cpp
    src/net/SignalBlob.cpp
    src/util/Crypto.cpp
    src/util/Json.cpp
    src/util/Log.cpp
    src/util/Platform.cpp
)

if(WIN32)
    set(SOI_CORE_PLATFORM
        src/app/Install.cpp
        src/capture/BitBltCapture.cpp
        src/capture/CaptureProtect.cpp
        src/capture/DxgiCapture.cpp
        src/capture/FrameBuffer.cpp
        src/capture/WgcCapture.cpp
        src/encode/H264Encoder.cpp
        src/gpu/GpuPipeline.cpp
        src/util/Crypto_win.cpp
        src/util/Platform_win.cpp
    )
else()
    # viewer.html and the license notices, compiled in as byte arrays: what the
    # Windows build gets from soi-share.rc, so the binary alone is complete.
    set(SOI_EMBEDDED_CPP ${CMAKE_CURRENT_BINARY_DIR}/embedded_resources.cpp)
    add_custom_command(
        OUTPUT  ${SOI_EMBEDDED_CPP}
        COMMAND ${CMAKE_COMMAND}
                -DVIEWER=${CMAKE_CURRENT_SOURCE_DIR}/viewer/viewer.html
                -DNOTICES=${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
                -DOUTPUT=${SOI_EMBEDDED_CPP}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/viewer/viewer.html
                ${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
                ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedResources.cmake
        COMMENT "Embedding viewer.html and THIRD_PARTY_NOTICES.md"
        VERBATIM
    )
    set(SOI_CORE_PLATFORM
        src/app/Install_posix.cpp
        src/app/Keychain_mac.cpp
        src/capture/mac/CaptureProtectMac.mm
        src/capture/mac/CgImageCapture.mm
        src/capture/mac/MacCaptureCommon.mm
        src/capture/mac/SckCapture.mm
        src/capture/mac/StreamCapture.mm
        src/encode/H264EncoderMac.mm
        src/gpu/GpuPipelineMac.mm
        src/util/Crypto_openssl.cpp
        src/util/HttpCurl.cpp
        src/util/Platform_posix.cpp
        ${SOI_EMBEDDED_CPP}
    )
endif()

add_library(soi-core STATIC ${SOI_CORE_COMMON} ${SOI_CORE_PLATFORM})

target_include_directories(soi-core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${miniz_SOURCE_DIR}
    ${miniz_BINARY_DIR}
)

target_compile_definitions(soi-core PUBLIC
    SOI_VERSION="${SOI_VERSION}"
    SOI_GITHUB_REPO="${SOI_GITHUB_REPO}"
)

if(WIN32)
    target_link_libraries(soi-core PUBLIC
        miniz
        mfplat mfuuid mfreadwrite wmcodecdspuuid strmiids
        ole32 oleaut32 uuid
        user32 gdi32 shcore dwmapi shell32
        # d3d11/dxgi for Desktop Duplication, runtimeobject for the WinRT activation
        # Windows.Graphics.Capture needs, d3dcompiler for the NV12 conversion shader
        # the GPU pipeline compiles at run time. All three ship with Windows, so this
        # adds no deployment requirement.
        d3d11 dxgi runtimeobject d3dcompiler
        bcrypt
        ws2_32 iphlpapi winhttp
        # Install / update: DPAPI for the saved token, the registry for PATH.
        crypt32 advapi32
    )

    target_compile_definitions(soi-core PUBLIC
        UNICODE _UNICODE
        WIN32_LEAN_AND_MEAN NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        # Windows 10 APIs: SetProcessDpiAwarenessContext, WDA_EXCLUDEFROMCAPTURE,
        # CREATE_WAITABLE_TIMER_HIGH_RESOLUTION.
        WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000007
    )
else()
    # OpenSSL is linked statically for libdatachannel already; the signalling
    # crypto uses it too (util/Crypto_openssl.cpp). Everything else is part of
    # macOS itself -- the frameworks below and /usr/lib/libcurl -- so the binary
    # needs nothing installed. ScreenCaptureKit is weak-linked: it does not
    # exist before macOS 12.3, where the other capture backends take over.
    set(OPENSSL_USE_STATIC_LIBS ON)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(soi-core PUBLIC
        miniz
        OpenSSL::Crypto
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework VideoToolbox"
        "-framework IOSurface"
        "-framework AppKit"
        "-framework Metal"
        "-framework Security"
        "-framework CFNetwork"
        "-weak_framework ScreenCaptureKit"
        curl
    )
    # ARC for the Objective-C++ files. And any call to an API newer than the
    # deployment target that is not behind @available is a build error, not a
    # crash on someone's older Mac.
    target_compile_options(soi-core PUBLIC
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
        -Werror=unguarded-availability)
endif()

if(NOT SOI_HAVE_STD_FORMAT)
    target_compile_definitions(soi-core PUBLIC SOI_USE_FMTLIB)
    target_link_libraries(soi-core PUBLIC fmt::fmt)
endif()

if(MSVC)
    target_compile_options(soi-core PUBLIC /W4 /permissive- /utf-8 /MP /EHsc)
else()
    # SIMD paths are selected per function at run time (encode/ColorConvert.cpp),
    # so no -m flag here: the x86_64 slice still runs on any Intel Mac.
    target_compile_options(soi-core PUBLIC -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# soi-selftest
# ---------------------------------------------------------------------------
if(SOI_BUILD_SELFTEST)
    add_executable(soi-selftest tests/selftest.cpp)
    # d3d11/dxgi come through soi-core now that Desktop Duplication is a capture
    # backend, but the test also uses them directly to verify capture protection
    # against the path most screen recorders actually use.
    if(WIN32)
        target_link_libraries(soi-selftest PRIVATE soi-core d3d11 dxgi)
    else()
        # The macOS encoder test decodes what it encoded, with VideoToolbox.
        target_sources(soi-selftest PRIVATE tests/selftest_mac.mm)
        target_link_libraries(soi-selftest PRIVATE soi-core)
    endif()
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

    # Compiled into the exe so it alone is a complete install (ids in
    # app/Lifecycle.h):
    #   101  viewer.html -- the LAN handover page. One placed next to the exe
    #        still takes precedence.
    #   102  THIRD_PARTY_NOTICES.md -- `soi-share licenses`, and copied beside
    #        the installed exe by `soi-share install`.
    # plus VERSIONINFO, so Explorer's Properties > Details shows the version.
    # (macOS gets the same two files from embedded_resources.cpp in soi-core.)
    if(WIN32)
        set(SOI_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        set(SOI_APP_RC ${CMAKE_CURRENT_BINARY_DIR}/soi-share.rc)
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/soi-share.rc.in ${SOI_APP_RC} @ONLY)
        set_source_files_properties(${SOI_APP_RC} PROPERTIES OBJECT_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/viewer/viewer.html;${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md")
    endif()

    add_executable(soi-share
        src/main.cpp
        src/net/Streamer.cpp
        ${SOI_APP_RC}
    )
    target_link_libraries(soi-share PRIVATE
        soi-core
        LibDataChannel::LibDataChannel
    )
    if(WIN32)
        target_link_libraries(soi-share PRIVATE ws2_32 iphlpapi)
    endif()
    if(MSVC)
        # No debug information in the shipped exe (Release links without
        # /DEBUG, so there is no PDB path either); drop unreferenced code and
        # fold identical functions.
        target_link_options(soi-share PRIVATE /SUBSYSTEM:CONSOLE
            $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>)
    elseif(APPLE)
        # Drop unreferenced code, and local symbols from the shipped binary.
        target_link_options(soi-share PRIVATE -Wl,-dead_strip
            $<$<CONFIG:Release>:-Wl,-x>)
    else()
        target_link_options(soi-share PRIVATE -static -static-libgcc -static-libstdc++ -s)
    endif()

    # Keep viewer.html next to the executable so --open-viewer finds it.
    add_custom_command(TARGET soi-share POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/viewer/viewer.html
            $<TARGET_FILE_DIR:soi-share>/viewer.html
        COMMENT "Staging viewer.html"
    )
endif()

message(STATUS "soi: version=${SOI_VERSION}  repo=${SOI_GITHUB_REPO}")
message(STATUS "soi: std::format=${SOI_HAVE_STD_FORMAT}  app=${SOI_BUILD_APP}  selftest=${SOI_BUILD_SELFTEST}")

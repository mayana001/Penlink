cmake_minimum_required(VERSION 3.20)
project(PenLinkHost CXX)

# A Windows FFmpeg development build with NVENC, D3D11VA and libx264.
set(FFMPEG_DIR "" CACHE PATH "FFmpeg root (include/ and lib/)")

add_executable(penlink-host main.cpp)
target_compile_features(penlink-host PRIVATE cxx_std_17)
target_compile_definitions(penlink-host PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
if(MSVC)
    target_compile_options(penlink-host PRIVATE /utf-8)
endif()
target_include_directories(penlink-host PRIVATE "${FFMPEG_DIR}/include")
target_link_directories(penlink-host PRIVATE "${FFMPEG_DIR}/lib")
target_link_libraries(penlink-host PRIVATE avcodec avutil swscale d3d11 dxgi ws2_32 user32 winmm runtimeobject)

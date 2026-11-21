cmake_minimum_required(VERSION 3.20)
project(PenLinkHost CXX)
set(CMAKE_CXX_STANDARD 17)

# Путь к dev-сборке FFmpeg с NVENC и D3D11VA (папки include/ и lib/), например BtbN "win64-gpl-shared"
set(FFMPEG_DIR "" CACHE PATH "FFmpeg root")

add_executable(penlink-host main.cpp)
target_include_directories(penlink-host PRIVATE ${FFMPEG_DIR}/include)
target_link_directories(penlink-host PRIVATE ${FFMPEG_DIR}/lib)
target_link_libraries(penlink-host PRIVATE avcodec avutil d3d11 dxgi ws2_32 user32 winmm)
target_link_libraries(penlink-host PRIVATE winmm)

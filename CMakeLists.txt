cmake_minimum_required(VERSION 3.20)
project(psdthumb VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies via FetchContent ---
include(FetchContent)

# lcms2
FetchContent_Declare(
    lcms2
    GIT_REPOSITORY https://github.com/mm2/Little-CMS.git
    GIT_TAG        master
)
set(LCMS2_BUILD_SHARED FALSE CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lcms2)

# zlib
FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(zlib)

# stb_image_write - download single header
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

# --- Engine library ---
add_library(psdengine STATIC
    src/psd_parser.cpp
    src/color_engine.cpp
    src/downscaler.cpp
    src/image_writer.cpp
)
target_include_directories(psdengine PUBLIC src)
target_link_libraries(psdengine PUBLIC lcms2 zlibstatic)
target_include_directories(psdengine PUBLIC ${stb_SOURCE_DIR})

# --- CLI executable ---
add_executable(psdthumb src/main.cpp)
target_link_libraries(psdthumb PRIVATE psdengine)

# --- Diagnostic tool ---
add_executable(diag_resources src/diag_resources.cpp)
target_link_libraries(diag_resources PRIVATE psdengine)

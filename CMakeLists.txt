cmake_minimum_required(VERSION 3.20)
project(rocktrainer LANGUAGES C CXX)

# Default to modern; we'll bump to C++26/latest if supported.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SANITIZERS "Enable ASAN/UBSAN" OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
    if(BUILD_SANITIZERS)
        add_link_options(-fsanitize=address,undefined)
        add_compile_options(-fsanitize=address,undefined)
    endif()
endif()

include(CheckCXXCompilerFlag)

# Try C++26 (2c) on GCC/Clang and "latest" on MSVC.
check_cxx_compiler_flag("-std=c++2c" HAS_STD_CXX2C)
check_cxx_compiler_flag("/std:c++latest" HAS_STD_MSVC_LATEST)

set(RT_USE_CXX2C OFF)
set(RT_USE_MSVC_LATEST OFF)
if(HAS_STD_CXX2C)
    set(RT_USE_CXX2C ON)
elseif(MSVC AND HAS_STD_MSVC_LATEST)
    set(RT_USE_MSVC_LATEST ON)
endif()

find_package(PkgConfig REQUIRED)

# --- Dependencies via pkg-config ---
pkg_check_modules(SDL2       REQUIRED sdl2)
pkg_check_modules(PORTAUDIO  portaudio-2.0)
pkg_check_modules(AUBIO      aubio)

# nlohmann_json (header-only). Prefer find_package; fallback to pkg-config include dir
find_package(nlohmann_json QUIET)
if (NOT nlohmann_json_FOUND)
    pkg_check_modules(NLOHMANN REQUIRED nlohmann_json)
endif()

# --- Target ---
add_executable(rocktrainer
        src/main.cpp
        src/chart_mss.cpp
)

# Includes
target_include_directories(rocktrainer PRIVATE
        ${SDL2_INCLUDE_DIRS}
)
if (PORTAUDIO_FOUND AND AUBIO_FOUND)
    target_include_directories(rocktrainer PRIVATE
            ${PORTAUDIO_INCLUDE_DIRS}
            ${AUBIO_INCLUDE_DIRS}
    )
endif()

if (NOT nlohmann_json_FOUND)
    target_include_directories(rocktrainer PRIVATE ${NLOHMANN_INCLUDE_DIRS})
endif()

# Library search paths
target_link_directories(rocktrainer PRIVATE
        ${SDL2_LIBRARY_DIRS}
)
if (PORTAUDIO_FOUND AND AUBIO_FOUND)
    target_link_directories(rocktrainer PRIVATE
            ${PORTAUDIO_LIBRARY_DIRS}
            ${AUBIO_LIBRARY_DIRS}
    )
endif()

# Compiler flags suggested by pkg-config (e.g., defines)
target_compile_options(rocktrainer PRIVATE
        ${SDL2_CFLAGS_OTHER}
)
if (PORTAUDIO_FOUND AND AUBIO_FOUND)
    target_compile_options(rocktrainer PRIVATE
            ${PORTAUDIO_CFLAGS_OTHER}
            ${AUBIO_CFLAGS_OTHER}
    )
endif()

# Linker flags suggested by pkg-config (critical on macOS: frameworks)
target_link_options(rocktrainer PRIVATE
        ${SDL2_LDFLAGS_OTHER}
)
if (PORTAUDIO_FOUND AND AUBIO_FOUND)
    target_link_options(rocktrainer PRIVATE
            ${PORTAUDIO_LDFLAGS_OTHER}
            ${AUBIO_LDFLAGS_OTHER}
    )
endif()

# Libraries
target_link_libraries(rocktrainer PRIVATE
        ${SDL2_LIBRARIES}
)
if (PORTAUDIO_FOUND AND AUBIO_FOUND)
    target_link_libraries(rocktrainer PRIVATE
            ${PORTAUDIO_LIBRARIES}
            ${AUBIO_LIBRARIES}
    )
    target_compile_definitions(rocktrainer PRIVATE RT_ENABLE_AUDIO)
else()
    message(WARNING "PortAudio or aubio not found; building without audio support")
endif()

if (nlohmann_json_FOUND)
    target_link_libraries(rocktrainer PRIVATE nlohmann_json::nlohmann_json)
endif()

# Prefer C++26 if available
if (RT_USE_CXX2C)
    target_compile_options(rocktrainer PRIVATE -std=c++2c)
    target_compile_definitions(rocktrainer PRIVATE ROCKTRAINER_CPP2C=1)
elseif (RT_USE_MSVC_LATEST)
    target_compile_options(rocktrainer PRIVATE /std:c++latest)
    target_compile_definitions(rocktrainer PRIVATE ROCKTRAINER_CPP2C=1)
endif()

enable_testing()

add_executable(title_test tests/title_test.cpp src/chart_mss.cpp)
target_include_directories(title_test PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_directories(title_test PRIVATE ${SDL2_LIBRARY_DIRS})
target_compile_options(title_test PRIVATE ${SDL2_CFLAGS_OTHER})
target_link_options(title_test PRIVATE ${SDL2_LDFLAGS_OTHER})
target_link_libraries(title_test PRIVATE ${SDL2_LIBRARIES})
if (nlohmann_json_FOUND)
    target_link_libraries(title_test PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(title_test PRIVATE ${NLOHMANN_INCLUDE_DIRS})
endif()
add_test(NAME TitleTest COMMAND title_test)

add_executable(mss_parser_test tests/mss_parser_test.cpp src/chart_mss.cpp)
if (nlohmann_json_FOUND)
    target_link_libraries(mss_parser_test PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(mss_parser_test PRIVATE ${NLOHMANN_INCLUDE_DIRS})
endif()
add_test(NAME MSSParserTest COMMAND mss_parser_test)

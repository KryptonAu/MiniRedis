cmake_minimum_required(VERSION 3.20)
project(MiniRedis VERSION 0.1.0 LANGUAGES CXX)

# ---- C++ Standard ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Compiler check: require GCC >= 12 ----
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS "12.0")
        message(FATAL_ERROR "GCC >= 12 is required. Found: ${CMAKE_CXX_COMPILER_VERSION}")
    endif()
    message(STATUS "GCC ${CMAKE_CXX_COMPILER_VERSION} — OK")
else()
    message(WARNING "Only GCC >= 12 is tested. Your compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ---- Build options ----
option(MINIREDIS_BUILD_TESTS "Build unit tests" ON)
option(MINIREDIS_USE_SANITIZERS "Enable Address/UB sanitizers" OFF)

# ---- Dependencies: stdexec (FetchContent) ----
include(FetchContent)
FetchContent_Declare(
    stdexec
    GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
    GIT_TAG        origin/main   # Pin to a stable tag in production
    GIT_SHALLOW    TRUE
)
# stdexec is header-only, suppress its tests/examples
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(stdexec)

# ---- Sanitizers (Debug only) ----
if(MINIREDIS_USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

# ---- Compiler flags ----
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wno-unknown-warning-option
)

# ---- Data Structures library ----
add_library(miniredis_ds STATIC
    src/ds/listpack.cpp
    src/ds/intset.cpp
    src/ds/quicklist.cpp
)
target_include_directories(miniredis_ds PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(miniredis_ds PUBLIC cxx_std_20)

# ---- Types library ----
add_library(miniredis_types STATIC
    src/types/numeric_parse.cpp
    src/types/string_value.cpp
    src/types/list_value.cpp
    src/types/set_value.cpp
    src/types/hash_value.cpp
    src/types/zset_value.cpp
    src/types/value.cpp
    src/types/type_conversion.cpp
)
target_include_directories(miniredis_types PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis_types PUBLIC miniredis_ds)
target_compile_features(miniredis_types PUBLIC cxx_std_20)

# ---- Executable ----
add_executable(miniredis src/main.cpp)
target_include_directories(miniredis PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(miniredis PRIVATE miniredis_types miniredis_ds stdexec pthread)
target_compile_features(miniredis PRIVATE cxx_std_20)

# ---- Tests ----
if(MINIREDIS_BUILD_TESTS)
    enable_testing()
    # Google Test via FetchContent
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

# ---- clang-format target (Google C++ Style) ----
find_program(CLANG_FORMAT clang-format)
if(CLANG_FORMAT)
    file(GLOB_RECURSE FORMAT_SOURCES
        ${CMAKE_SOURCE_DIR}/src/*.cpp
        ${CMAKE_SOURCE_DIR}/src/*.h
    )
    add_custom_target(format
        COMMAND ${CLANG_FORMAT} -i -style=Google ${FORMAT_SOURCES}
        COMMENT "Running clang-format (Google style)"
    )
    add_custom_target(format-check
        COMMAND ${CLANG_FORMAT} --dry-run --Werror -style=Google ${FORMAT_SOURCES}
        COMMENT "Checking clang-format (Google style)"
    )
endif()

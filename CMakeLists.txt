# ============================================================
#  CMakeLists.txt  –  Animal Company Mod Menu
#  Output: libACModMenu.so  (arm64-v8a)
#
#  Requirements:
#    NDK  : r23 or later
#    CMake: 3.18 or later
#
#  Toolchain invocation (from project root):
#    cmake -B build \
#          -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake \
#          -DANDROID_ABI=arm64-v8a \
#          -DANDROID_PLATFORM=android-29 \
#          -DANDROID_STL=c++_shared \
#          -DCMAKE_BUILD_TYPE=Release
#    cmake --build build
#
#  The resulting libACModMenu.so will be in build/.
# ============================================================

cmake_minimum_required(VERSION 3.18)
project(ACModMenu LANGUAGES CXX)

# ── C++ standard ─────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Enforce arm64 ────────────────────────────────────────────
if(NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "This mod only targets arm64-v8a.  "
        "Pass -DANDROID_ABI=arm64-v8a to CMake.")
endif()

# ── Source files ─────────────────────────────────────────────
add_library(ACModMenu SHARED
    mod.cpp
    # Add additional translation units here if the project grows:
    # src/toggle_fly.cpp
    # src/toggle_godmode.cpp
    # etc.
)

# ── Include directories ───────────────────────────────────────
target_include_directories(ACModMenu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}          # BNMSetup.h lives here
    ${CMAKE_CURRENT_SOURCE_DIR}/BNM/include  # BNM public headers
    ${CMAKE_CURRENT_SOURCE_DIR}/Dobby/include  # Dobby (optional)
)

# ── Compiler definitions ──────────────────────────────────────
target_compile_definitions(ACModMenu PRIVATE
    ANDROID                   # platform guard used in some third-party headers
    __ANDROID__               # same, different spelling
    # Uncomment to enable verbose debug logging:
    # AC_MOD_VERBOSE
)

# ── Compiler flags ────────────────────────────────────────────
target_compile_options(ACModMenu PRIVATE
    -Wall
    -Wextra
    -Wno-unused-parameter     # IL2CPP stubs leave many params unused
    -fvisibility=hidden       # hide all symbols by default; only JNI_OnLoad is exported
    -ffunction-sections       # dead-strip unused functions
    -fdata-sections
    # ARM-specific optimisation
    -march=armv8-a
    -O2
    # Reduce binary size on release
    $<$<CONFIG:Release>:-Oz>
    $<$<CONFIG:Release>:-fomit-frame-pointer>
)

# ── Linker flags ──────────────────────────────────────────────
target_link_options(ACModMenu PRIVATE
    -Wl,--gc-sections         # strip unreferenced sections
    -Wl,--exclude-libs,ALL    # do not re-export symbols from static deps
    # Ensure JNI_OnLoad is always visible to the Android linker
    -Wl,--export-dynamic-symbol,JNI_OnLoad
    -Wl,-z,nocopyreloc
)

# ── System libraries ──────────────────────────────────────────
# android  : ANativeActivity, asset manager, etc.
# log      : __android_log_print  (used by LOGI/LOGW/LOGE macros)
# dl       : dlopen / dlsym
# m        : standard math (sqrt, fabs, …)
target_link_libraries(ACModMenu PRIVATE
    android
    log
    dl
    m
)

# ── Optional: Dobby (inline hook library) ─────────────────────
#  Build Dobby separately and place libdobby.a in the Dobby/libs
#  directory, then uncomment the block below.
#
# set(DOBBY_LIB_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Dobby/libs/arm64-v8a/libdobby.a)
# if(EXISTS ${DOBBY_LIB_PATH})
#     target_link_libraries(ACModMenu PRIVATE ${DOBBY_LIB_PATH})
#     target_compile_definitions(ACModMenu PRIVATE DOBBY_AVAILABLE=1)
#     message(STATUS "Dobby found and linked.")
# else()
#     message(WARNING "Dobby not found at ${DOBBY_LIB_PATH}. Raw hooks disabled.")
# endif()

# ── Optional: BNM (ByNameModding) ────────────────────────────
#  BNM is header-only so no static library is required, but it
#  does need to find its own source file (BNM.cpp) if the user
#  builds the non-header-only variant.
#
# set(BNM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/BNM/BNM.cpp)
# if(EXISTS ${BNM_SRC})
#     target_sources(ACModMenu PRIVATE ${BNM_SRC})
#     target_compile_definitions(ACModMenu PRIVATE BNM_AVAILABLE=1)
#     message(STATUS "BNM source found.")
# endif()

# ── Strip debug info from Release builds ─────────────────────
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_custom_command(TARGET ACModMenu POST_BUILD
        COMMAND ${CMAKE_STRIP} --strip-unneeded
                $<TARGET_FILE:ACModMenu>
        COMMENT "Stripping debug symbols from libACModMenu.so"
    )
endif()

# ── Print build summary ───────────────────────────────────────
message(STATUS "-----------------------------------------------------")
message(STATUS " Target  : libACModMenu.so")
message(STATUS " ABI     : ${ANDROID_ABI}")
message(STATUS " Platform: ${ANDROID_PLATFORM}")
message(STATUS " Build   : ${CMAKE_BUILD_TYPE}")
message(STATUS " NDK     : ${ANDROID_NDK}")
message(STATUS "-----------------------------------------------------")

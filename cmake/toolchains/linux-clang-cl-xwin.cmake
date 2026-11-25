# Cross-compilation toolchain: Linux host -> Windows x64 (MSVC ABI via clang-cl + lld-link + xwin)
#
# Environment / Cache overrides:
#   LLVM_MINGW_ROOT : Directory containing bin/clang, bin/lld-link, bin/llvm-lib, bin/llvm-rc
#                     (defaults to $HOME/.local/share/wwmi-build-deps/llvm-mingw or system PATH)
#   XWIN_ROOT       : Directory containing splatted MSVC CRT & Windows SDK (crt/include, sdk/include/..., lib/x86_64)
#                     (defaults to $HOME/.local/share/wwmi-build-deps/xwin-sdk or $HOME/.xwin)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES LLVM_MINGW_ROOT XWIN_ROOT)

# Locate LLVM / clang-cl toolchain
if(NOT DEFINED LLVM_MINGW_ROOT)
    if(DEFINED ENV{LLVM_MINGW_ROOT} AND EXISTS "$ENV{LLVM_MINGW_ROOT}/bin/clang")
        set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}")
    elseif(EXISTS "$ENV{HOME}/.local/share/wwmi-build-deps/llvm-mingw/bin/clang")
        set(LLVM_MINGW_ROOT "$ENV{HOME}/.local/share/wwmi-build-deps/llvm-mingw")
    endif()
endif()

if(DEFINED LLVM_MINGW_ROOT AND EXISTS "${LLVM_MINGW_ROOT}/bin/clang")
    set(_WWMI_CLANG_BIN "${LLVM_MINGW_ROOT}/bin/clang")
    set(_WWMI_LLD_LINK  "${LLVM_MINGW_ROOT}/bin/lld-link")
    set(_WWMI_LLVM_LIB  "${LLVM_MINGW_ROOT}/bin/llvm-lib")
    set(_WWMI_LLVM_RC   "${LLVM_MINGW_ROOT}/bin/llvm-rc")
else()
    find_program(_WWMI_CLANG_BIN NAMES clang)
    find_program(_WWMI_LLD_LINK  NAMES lld-link)
    find_program(_WWMI_LLVM_LIB  NAMES llvm-lib)
    find_program(_WWMI_LLVM_RC   NAMES llvm-rc)
endif()

if(NOT _WWMI_CLANG_BIN OR NOT _WWMI_LLD_LINK OR NOT _WWMI_LLVM_LIB OR NOT _WWMI_LLVM_RC)
    message(FATAL_ERROR "Missing LLVM Windows MSVC cross-tools (clang, lld-link, llvm-lib, llvm-rc). Set LLVM_MINGW_ROOT or install LLVM.")
endif()

# Locate xwin MSVC CRT + Windows SDK splat
if(NOT DEFINED XWIN_ROOT)
    if(DEFINED ENV{XWIN_ROOT} AND EXISTS "$ENV{XWIN_ROOT}/crt/include")
        set(XWIN_ROOT "$ENV{XWIN_ROOT}")
    elseif(EXISTS "$ENV{HOME}/.local/share/wwmi-build-deps/xwin-sdk/crt/include")
        set(XWIN_ROOT "$ENV{HOME}/.local/share/wwmi-build-deps/xwin-sdk")
    elseif(EXISTS "$ENV{HOME}/.xwin/crt/include")
        set(XWIN_ROOT "$ENV{HOME}/.xwin")
    endif()
endif()

if(NOT DEFINED XWIN_ROOT OR NOT EXISTS "${XWIN_ROOT}/crt/include")
    message(FATAL_ERROR "Missing xwin MSVC CRT/SDK splat. Run `xwin --accept-license splat --output ~/.xwin` or set XWIN_ROOT.")
endif()

set(_WWMI_COMPAT_INC "${CMAKE_CURRENT_LIST_DIR}/../include-compat")
cmake_path(NORMAL_PATH _WWMI_COMPAT_INC)

set(_WWMI_SDK_INCLUDES
    "${XWIN_ROOT}/crt/include"
    "${XWIN_ROOT}/sdk/include/ucrt"
    "${XWIN_ROOT}/sdk/include/um"
    "${XWIN_ROOT}/sdk/include/shared"
    "${XWIN_ROOT}/sdk/include/winrt"
)
set(_WWMI_SDK_LIBS
    "${XWIN_ROOT}/crt/lib/x86_64"
    "${XWIN_ROOT}/sdk/lib/ucrt/x86_64"
    "${XWIN_ROOT}/sdk/lib/um/x86_64"
)

# Generate wrapper scripts inside the build directory so CMake compiler probes pass cleanly
set(_WWMI_WRAP_DIR "${CMAKE_BINARY_DIR}/_toolchain_wrappers")
file(MAKE_DIRECTORY "${_WWMI_WRAP_DIR}")

set(_WWMI_IMSVC_ARGS "")
set(_WWMI_RC_INC_ARGS "")
foreach(_inc IN LISTS _WWMI_SDK_INCLUDES)
    string(APPEND _WWMI_IMSVC_ARGS " \"/imsvc${_inc}\"")
    string(APPEND _WWMI_RC_INC_ARGS " /I\"${_inc}\"")
endforeach()

set(_WWMI_LIBPATH_ARGS "")
foreach(_lib IN LISTS _WWMI_SDK_LIBS)
    string(APPEND _WWMI_LIBPATH_ARGS " /libpath:\"${_lib}\"")
endforeach()

set(_WWMI_CLANG_CL_WRAP "${_WWMI_WRAP_DIR}/clang-cl")
if(NOT EXISTS "${_WWMI_CLANG_CL_WRAP}")
    file(WRITE "${_WWMI_CLANG_CL_WRAP}"
        "#!/bin/sh\nexec \"${_WWMI_CLANG_BIN}\" --driver-mode=cl --target=x86_64-pc-windows-msvc \"/I${_WWMI_COMPAT_INC}\"${_WWMI_IMSVC_ARGS} \"$@\"\n"
    )
    file(CHMOD "${_WWMI_CLANG_CL_WRAP}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

set(_WWMI_LLVM_RC_WRAP "${_WWMI_WRAP_DIR}/llvm-rc")
if(NOT EXISTS "${_WWMI_LLVM_RC_WRAP}")
    file(WRITE "${_WWMI_LLVM_RC_WRAP}"
        "#!/bin/sh\nexec \"${_WWMI_LLVM_RC}\" /C 65001 \"$@\"\n"
    )
    file(CHMOD "${_WWMI_LLVM_RC_WRAP}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()

set(CMAKE_C_COMPILER   "${_WWMI_CLANG_CL_WRAP}" CACHE FILEPATH "clang-cl C wrapper" FORCE)
set(CMAKE_CXX_COMPILER "${_WWMI_CLANG_CL_WRAP}" CACHE FILEPATH "clang-cl CXX wrapper" FORCE)
set(CMAKE_RC_COMPILER  "${_WWMI_LLVM_RC_WRAP}"  CACHE FILEPATH "llvm-rc UTF-8 wrapper" FORCE)
set(CMAKE_AR           "${_WWMI_LLVM_LIB}"      CACHE FILEPATH "llvm-lib archiver" FORCE)
set(CMAKE_LINKER       "${_WWMI_LLD_LINK}"      CACHE FILEPATH "lld-link linker" FORCE)
set(CMAKE_MT           ""                       CACHE FILEPATH "Disable mt.exe on cross-builds" FORCE)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RC_FLAGS_INIT "${_WWMI_RC_INC_ARGS} /I\"${_WWMI_COMPAT_INC}\"")
set(CMAKE_EXE_LINKER_FLAGS_INIT "/manifest:no${_WWMI_LIBPATH_ARGS}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "/manifest:no${_WWMI_LIBPATH_ARGS}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "/manifest:no${_WWMI_LIBPATH_ARGS}")

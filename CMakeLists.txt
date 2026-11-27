cmake_minimum_required(VERSION 3.23)
project(wwmi_dx12_rt VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(WWMI_BUILD_WIN_SETUP "Build zero-dependency Windows GUI/CLI Setup executable" ON)
option(WWMI_BUILD_RUNTIME   "Build ReShade host + wwmi_dx12 + zz_wwmi_rt_pack when third_party/ is present" ON)
option(WWMI_BUILD_LINUX_UTF8_PRELOAD "Build Linux Wine UTF-8 filename locale shim (.so)" ON)

set(WWMI_UPSTREAM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
    "Path to fetched upstream ReShade / wwmi-dx12 / 3Dmigoto / dxc dependencies (populated via scripts/bootstrap_deps.py)")

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(WIN32)
    enable_language(RC)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Allow standalone LLVM clang-cl (e.g. Clang 20/21) to pair with newer MSVC STL headers (e.g. VS 18 / MSVC 14.52+)
        add_compile_definitions(_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
    endif()
endif()

# Common x86_64 SIMD intrinsics required on Clang-CL (InterlockedCompareExchange128, SSE4.1, BMI/LZCNT)
set(_WWMI_CLANG_SIMD_FLAGS
    $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-clang:-mcx16>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-clang:-msse4.1>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-clang:-mbmi>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang>:-clang:-mlzcnt>
)

# ---------------------------------------------------------------------------
# 1. Windows Native Setup & Manager (WWMI-DX12-RT-Setup.exe)
#    Builds out-of-the-box on any Windows MSVC / clang-cl environment even
#    before third_party/ is bootstrapped.
# ---------------------------------------------------------------------------
if(WIN32 AND WWMI_BUILD_WIN_SETUP)
    add_executable(WWMI-DX12-RT-Setup WIN32
        tools/win_setup/wwmi_dx12_setup_win32.cpp
    )
    target_compile_definitions(WWMI-DX12-RT-Setup PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    )
    if(MSVC)
        target_compile_options(WWMI-DX12-RT-Setup PRIVATE "$<$<COMPILE_LANGUAGE:C,CXX>:/utf-8;/W3;/EHsc>")
    endif()
    target_link_libraries(WWMI-DX12-RT-Setup PRIVATE
        kernel32 user32 gdi32 shell32 ole32 advapi32 shlwapi comctl32 bcrypt
    )
endif()

# ---------------------------------------------------------------------------
# 2. Linux Wine UTF-8 Filename Locale Guard (lib/wine-filename-utf8.so)
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE AND WWMI_BUILD_LINUX_UTF8_PRELOAD)
    add_library(wine-filename-utf8 SHARED
        source/linux/wine-filename-utf8.c
    )
    set_target_properties(wine-filename-utf8 PROPERTIES
        PREFIX ""
        OUTPUT_NAME "wine-filename-utf8"
    )
    target_link_libraries(wine-filename-utf8 PRIVATE dl)
endif()

# ---------------------------------------------------------------------------
# 3. Full D3D12 + DXR Runtime Targets (Windows native or Linux clang-cl cross)
#    - ReShade             -> ReShade64.dll (packaged as ReShade64-game-forward.dll)
#    - migoto_engine       -> static library (3Dmigoto + DX12 bridge)
#    - wwmi_dx12           -> wwmi_dx12.addon64
#    - zz_wwmi_rt_pack     -> zz_wwmi_rt_pack.addon64
# ---------------------------------------------------------------------------
if(WIN32 AND WWMI_BUILD_RUNTIME
   AND EXISTS "${WWMI_UPSTREAM_ROOT}/reshade/include/reshade.hpp"
   AND EXISTS "${WWMI_UPSTREAM_ROOT}/wwmi-dx12/reshade_addon"
   AND EXISTS "${WWMI_UPSTREAM_ROOT}/3Dmigoto/DirectX11/CommandList.cpp")

    set(_WWMI_STAGE_DIR "${CMAKE_BINARY_DIR}/_staged")
    # Add/remove and content edits in either overlay regenerate CMake before
    # Ninja evaluates the staged sources. The same staging pass serves initial configure.
    file(GLOB_RECURSE _WWMI_OVERLAY_INPUTS CONFIGURE_DEPENDS LIST_DIRECTORIES false
        "${CMAKE_CURRENT_SOURCE_DIR}/source/host/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/main/*"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_WWMI_OVERLAY_INPUTS}
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_sources.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_support.py"
    )
    message(STATUS "Staging upstream + repository sources into ${_WWMI_STAGE_DIR} ...")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_sources.py"
            --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
            --upstream-root "${WWMI_UPSTREAM_ROOT}"
            --stage-root "${_WWMI_STAGE_DIR}"
        RESULT_VARIABLE _stage_rc
    )
    if(NOT _stage_rc EQUAL 0)
        message(FATAL_ERROR "stage_sources.py failed with exit code ${_stage_rc}")
    endif()

    set(_STAGED_RESHADE "${_WWMI_STAGE_DIR}/reshade")
    set(_STAGED_WWMI    "${_WWMI_STAGE_DIR}/wwmi-dx12")
    set(_STAGED_3DM     "${_WWMI_STAGE_DIR}/3dm_src")
    set(_MIGOTO_DIRECTXTK "${WWMI_UPSTREAM_ROOT}/3Dmigoto/DirectXTK")
    set(_SHARED_INC     "${CMAKE_CURRENT_SOURCE_DIR}/source/include")

    # ---- 3A. Paired ReShade Host (ReShade64.dll) ----
    set(WWMI_GLAD_OFFLINE_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/cmake/run_glad_offline.py" CACHE FILEPATH "" FORCE)
    set(WWMI_PYTHON_EXECUTABLE   "${Python3_EXECUTABLE}" CACHE FILEPATH "" FORCE)

    add_subdirectory("${_STAGED_RESHADE}" "${CMAKE_BINARY_DIR}/reshade_host")

    target_include_directories(ReShade PRIVATE "${_SHARED_INC}")
    target_compile_definitions(ReShade PRIVATE RESHADE_LOCALIZATION UNICODE)
    target_link_options(ReShade PRIVATE
        /MANIFEST:NO
        /DELAYLOAD:D3DCompiler_47.dll
        /DELAYLOAD:WinMM.dll
        /DELAYLOAD:XInput9_1_0.dll
    )
    target_link_libraries(ReShade PRIVATE delayimp.lib)

    if(CMAKE_HOST_UNIX)
        # xwin on case-sensitive Linux hosts provides canonical lowercase .lib filenames
        set_property(TARGET Windows PROPERTY INTERFACE_LINK_LIBRARIES
            d3dcompiler.lib dxguid.lib shlwapi.lib version.lib wininet.lib
            winmm.lib ws2_32.lib xinput9_1_0.lib
        )
    endif()

    # ---- 3B. 3Dmigoto Execution Engine + DX12 Bridge (migoto_engine) ----
    set(_MIGOTO_ENGINE_SRCS
        "${_STAGED_3DM}/CommandList.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_ini.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_override.cpp"
        "${_STAGED_3DM}/ResourceHash.cpp"
        "${_STAGED_3DM}/HackerInputLayout.cpp"
        "${_STAGED_3DM}/profiling.cpp"
        "${_STAGED_3DM}/input.cpp"
        "${_STAGED_3DM}/util.cpp"
        "${_STAGED_3DM}/lock.cpp"
        "${_STAGED_3DM}/cursor.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_globals.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_context.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_bridge.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_context_impl.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_device.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_device_impl.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_wrap.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_pipeline.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_skcap.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/mod_ui_shield.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_api.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/hacker_context_shim.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_vars.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_publish.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_parse_index.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_dispatch.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_keygate.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/port_skel_copies.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/frame_analysis_format.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/frame_analysis_engine.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_dump_readback.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dump_watchdog.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dump_preview.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/dx12_input_layout.cpp"
        "${_STAGED_WWMI}/reshade_addon/migoto/frame_analysis_driver.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_shape_source.cpp"
    )

    add_library(migoto_engine STATIC
        ${_MIGOTO_ENGINE_SRCS}
        "${_MIGOTO_DIRECTXTK}/Src/DDSTextureLoader.cpp"
        "${_MIGOTO_DIRECTXTK}/Src/WICTextureLoader.cpp"
        "${_MIGOTO_DIRECTXTK}/Src/ScreenGrab.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/source/client/crc32c.cpp"
    )
    set_target_properties(migoto_engine PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(migoto_engine PRIVATE
        UNICODE _UNICODE _HAS_STD_BYTE=0 MIGOTO_DX=11
        PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC CRC32C_STATIC
        _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1
        _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES_COUNT=1
        _NO_CRT_STDIO_INLINE=1
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    )
    target_include_directories(migoto_engine PRIVATE
        "${_SHARED_INC}"
        "${_STAGED_WWMI}"
        "${_STAGED_WWMI}/reshade_addon"
        "${_STAGED_WWMI}/reshade_addon/migoto"
        "${_STAGED_3DM}"
        "${_STAGED_WWMI}/third_party/3Dmigoto"
        "${_STAGED_WWMI}/third_party/3Dmigoto/HLSLDecompiler"
        "${_STAGED_WWMI}/third_party/3Dmigoto/D3D_Shaders"
        "${_MIGOTO_DIRECTXTK}/Inc"
        "${_MIGOTO_DIRECTXTK}/Src"
        "${_STAGED_WWMI}/third_party/3Dmigoto/crc32c-hw-1.0.5/include"
        "${_STAGED_WWMI}/third_party/3Dmigoto/crc32c-hw-1.0.5/src"
        "${_STAGED_WWMI}/third_party/3Dmigoto/pcre2"
    )
    target_compile_options(migoto_engine PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/O2;/EHa;/W3;/utf-8>"
        $<$<COMPILE_LANG_AND_ID:CXX,Clang>:/clang:-Wno-error=c++11-narrowing>
        $<$<COMPILE_LANG_AND_ID:CXX,Clang>:/clang:-fno-operator-names>
        ${_WWMI_CLANG_SIMD_FLAGS}
    )

    foreach(_mig_src IN LISTS _MIGOTO_ENGINE_SRCS)
        set_property(SOURCE "${_mig_src}" APPEND PROPERTY
            COMPILE_OPTIONS "/FI${_STAGED_WWMI}/reshade_addon/migoto/port_compat.h"
        )
    endforeach()

    # ---- 3C. WWMI-DX12 Provider Add-on (wwmi_dx12.addon64) ----
    add_library(wwmi_dx12 SHARED
        "${_STAGED_WWMI}/reshade_addon/wwmi_addon.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/layout_registry.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/shader_alias.cpp"
        "${_STAGED_WWMI}/reshade_addon/hooks/pipeline_hooks.cpp"
        "${_STAGED_WWMI}/reshade_addon/hooks/cmd_bind_hooks.cpp"
        "${_STAGED_WWMI}/reshade_addon/hooks/resource_tracker.cpp"
        "${_STAGED_WWMI}/reshade_addon/rabbitfx/rabbitfx_bind.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/port_section.cpp"
        "${_STAGED_WWMI}/reshade_addon/dispatch/dispatch_hook.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/dispatch_diag.cpp"
        "${_STAGED_WWMI}/reshade_addon/dispatch/dispatch_overrides.cpp"
        "${_STAGED_WWMI}/reshade_addon/dispatch/skin_cache_learn.cpp"
        "${_STAGED_WWMI}/reshade_addon/present/present_hook.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/present_diag.cpp"
        "${_STAGED_WWMI}/reshade_addon/present/present_engine.cpp"
        "${_STAGED_WWMI}/reshade_addon/present/presence_tick.cpp"
        "${_STAGED_WWMI}/reshade_addon/present/present_mod_load.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/device_watch.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_hook.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_claim.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/draw_probes.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_shadow.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_texswap.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_inject.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_restore.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_comp.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/draw_section_draw.cpp"
        "${_STAGED_WWMI}/reshade_addon/observation/binding_client/wwmi_binding_client.cpp"
        "${_STAGED_WWMI}/reshade_addon/observation/producer/producer_bridge.cpp"
        "${_STAGED_WWMI}/reshade_addon/observation/producer/semantic_capture.cpp"
        "${_STAGED_WWMI}/reshade_addon/observation/wwmi_executed_draw.cpp"
        "${_STAGED_WWMI}/reshade_addon/draw/section_ps_stamp.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/claim_state.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/stall_watchdog.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/removal_pin.cpp"
        "${_STAGED_WWMI}/reshade_addon/hooks/dxil_psv.cpp"
        "${_STAGED_WWMI}/reshade_addon/rabbitfx/rfx_dxil.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/texture_module.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_lod.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_look.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_dhash.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_dhash_table.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_crawl.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_defer.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_replace.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_encode.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_budget.cpp"
        "${_STAGED_WWMI}/reshade_addon/textures/tex_gc.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/tex_timing.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/skeleton.cpp"
        "${_STAGED_WWMI}/reshade_addon/ui/overlay.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/wwmi_runtime.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/buffer_relocation.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_identity.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_resolve.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_queue.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_store.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_replay.cpp"
        "${_STAGED_WWMI}/reshade_addon/shapekeys/sk_submit_order.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/wwmi_mod.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/wwmi_mod_engine.cpp"
        "${_STAGED_WWMI}/reshade_addon/mods/ib_hash.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/crash_handler.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/gpu_timer.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/perf_monitor.cpp"
        "${_STAGED_WWMI}/reshade_addon/instrumentation/vram_meter.cpp"
        "${_STAGED_WWMI}/reshade_addon/hud/uid_hide.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_stub.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_capture.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/private-stream-pso.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_raster_gate.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_localvf_inputs.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_mod_source.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_cpu_window.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_program_contracts.cpp"
        "${_STAGED_WWMI}/reshade_addon/rt/rt_program_runtime.cpp"
        "${_STAGED_WWMI}/src/mod_loader.cpp"
        "${_STAGED_WWMI}/src/dxil_cb.cpp"
        "${_STAGED_WWMI}/src/gpu_progress.cpp"
        "${_STAGED_WWMI}/src/log.cpp"
        "${_STAGED_WWMI}/reshade_addon/wwmi_dx12.rc"
    )
    set_target_properties(wwmi_dx12 PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(wwmi_dx12 PRIVATE
        IMGUI_DISABLE_INCLUDE_IMCONFIG_H
        WWMI_RESEARCH_PRESERVE_GAME_SETTINGS=1
    )
    target_include_directories(wwmi_dx12 PRIVATE
        "${_SHARED_INC}"
        "${_STAGED_WWMI}"
        "${_STAGED_WWMI}/reshade_addon"
        "${_STAGED_WWMI}/reshade_addon/rt"
        "${_STAGED_WWMI}/reshade_addon/migoto"
        "${_STAGED_WWMI}/reshade_addon/shapekeys"
        "${_STAGED_WWMI}/reshade_addon/hooks"
        "${_STAGED_WWMI}/reshade_addon/rabbitfx"
        "${_STAGED_WWMI}/reshade_addon/present"
        "${_STAGED_WWMI}/reshade_addon/textures"
        "${_STAGED_WWMI}/reshade_addon/sdk/include"
        "${_STAGED_WWMI}/reshade_addon/sdk/deps/imgui"
        "${_STAGED_WWMI}/src"
        "${_STAGED_3DM}"
        "${_STAGED_WWMI}/third_party/3Dmigoto"
        "${_STAGED_WWMI}/third_party/3Dmigoto/HLSLDecompiler"
        "${_STAGED_WWMI}/third_party/3Dmigoto/D3D_Shaders"
        "${_STAGED_RESHADE}/deps/d3d12/include/directx"
    )
    target_compile_options(wwmi_dx12 PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/O2;/EHsc;/W3;/utf-8>"
        ${_WWMI_CLANG_SIMD_FLAGS}
    )
    set_property(SOURCE
        "${_STAGED_WWMI}/reshade_addon/observation/wwmi_executed_draw.cpp"
        "${_STAGED_WWMI}/reshade_addon/observation/producer/producer_bridge.cpp"
        APPEND PROPERTY COMPILE_OPTIONS /EHsc /Zc:threadSafeInit /U_HAS_EXCEPTIONS
    )
    target_link_options(wwmi_dx12 PRIVATE
        /MANIFEST:NO
        /OPT:REF
        /INCREMENTAL:NO
        /EXPORT:WWMI_RtModAcquirePostMultiplierShape_v1
    )
    target_link_libraries(wwmi_dx12 PRIVATE
        migoto_engine
        legacy_stdio_definitions.lib
        user32.lib gdi32.lib ole32.lib advapi32.lib
        d3d12.lib d3d11.lib d3dcompiler.lib dxgi.lib
        xinput.lib dbghelp.lib shlwapi.lib psapi.lib
        "${_STAGED_WWMI}/third_party/3Dmigoto/pcre2/pcre2-8-64.lib"
    )

    # ---- 3D. Standalone DXR Ray Tracing + Hybrid Raster Consumer (zz_wwmi_rt_pack.addon64) ----
    add_library(zz_wwmi_rt_pack SHARED
        source/client/rt_pack.cpp
        source/client/hsin_runtime.cpp
        source/client/hsin_inputs.cpp
        source/client/hsin_geometry.cpp
        source/client/hsin_patches.cpp
        source/client/native_draw_identity.cpp
        source/client/shapekey_position_input.cpp
        source/client/crc32c.cpp
    )
    set_target_properties(zz_wwmi_rt_pack PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_definitions(zz_wwmi_rt_pack PRIVATE
        NOMINMAX
        CRC32C_STATIC
    )
    target_include_directories(zz_wwmi_rt_pack PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/source/client"
        "${_SHARED_INC}"
        "${_STAGED_RESHADE}/include"
        "${_STAGED_RESHADE}/deps/d3d12/include/directx"
        "${_STAGED_WWMI}/third_party/3Dmigoto/crc32c-hw-1.0.5/include"
        "${_STAGED_WWMI}/third_party/3Dmigoto/crc32c-hw-1.0.5/src"
    )
    target_compile_options(zz_wwmi_rt_pack PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:/O2;/EHsc;/W3;/utf-8>"
        ${_WWMI_CLANG_SIMD_FLAGS}
    )
    target_link_options(zz_wwmi_rt_pack PRIVATE
        /MANIFEST:NO
        /OPT:REF
        /OPT:ICF
        /INCREMENTAL:NO
    )
    target_link_libraries(zz_wwmi_rt_pack PRIVATE
        d3d12.lib dxgi.lib dxguid.lib kernel32.lib user32.lib
    )
endif()

enable_testing()
add_test(
    NAME python_unit_tests
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -v
)

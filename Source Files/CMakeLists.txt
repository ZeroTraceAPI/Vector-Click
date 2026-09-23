cmake_minimum_required(VERSION 3.24)
project(VectorClick VERSION 1.0.0.0 LANGUAGES CXX)

configure_file(
    Windows/version_config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/version_config.h
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep every MSVC-built project target on the same static runtime. CMake's
# default for targets without this setting is the DLL runtime, which can mix
# /MD library objects with Vector Click's intended /MT executable objects.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(vectorclick_core STATIC
    Core/diagnostic_report.cpp
    Core/elevated_restart_settings.cpp
    Core/json_object.cpp
    Core/numeric_edit_history.cpp
    Core/settings.cpp
    Core/settings_history.cpp
    Core/validation.cpp
)
target_include_directories(vectorclick_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(vectorclick_presentation STATIC
    Windows/ui_presentation_state.cpp
    Windows/ui_layout.cpp
)
target_include_directories(vectorclick_presentation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vectorclick_presentation PUBLIC vectorclick_core)

if(MSVC)
    target_compile_options(vectorclick_presentation PRIVATE
        /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2>
    )
else()
    target_compile_options(vectorclick_presentation PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fstack-protector-strong
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
endif()

if(MSVC)
    target_compile_options(vectorclick_core PRIVATE
        /W4 /permissive- /EHsc /utf-8
        $<$<CONFIG:Release>:/O2>
    )
else()
    target_compile_options(vectorclick_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fstack-protector-strong
        $<$<CONFIG:Release>:-Os>
        $<$<CONFIG:Release>:-ffunction-sections>
        $<$<CONFIG:Release>:-fdata-sections>
    )
endif()

if(WIN32)
    enable_language(RC)
    add_executable(VectorClick WIN32
        Windows/main.cpp
        Windows/mingw_entrypoint.cpp
        Windows/controller.cpp
        Windows/capture_exclusion.cpp
        Windows/click_position_indicator.cpp
        Windows/centered_message_box.cpp
        Windows/shared_image_loader.cpp
        Windows/click_worker.cpp
        Windows/input_backend_dispatcher.cpp
        Windows/send_input_backend.cpp
        Windows/unicode_input_backend.cpp
        Windows/targeted_unicode_input_backend.cpp
        Windows/window_message_backend.cpp
        Windows/hotkey_thread.cpp
        Windows/main_window.cpp
        Windows/process_priority_manager.cpp
        Windows/profile_store.cpp
        Windows/profile_manager.cpp
        Windows/run_feedback_presenter.cpp
        Windows/numeric_field_presenter.cpp
        Windows/diagnostics_display.cpp
        Windows/safety_shield.cpp
        Windows/tooltip_manager.cpp
        Windows/ui_theme.cpp
        Windows/admin_utils.cpp
        Windows/startup_options.cpp
        Windows/settings_store.cpp
        Windows/target_window.cpp
        Windows/target_window_picker.cpp
        Windows/VectorClick.rc
    )
    target_include_directories(VectorClick PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    set_target_properties(VectorClick PROPERTIES OUTPUT_NAME "Vector Click")
    target_link_libraries(VectorClick PRIVATE vectorclick_presentation comctl32 comdlg32 shell32 user32 gdi32 gdiplus advapi32 uxtheme dwmapi winmm)
    target_compile_definitions(VectorClick PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        _WIN32_WINNT=0x0A00 WINVER=0x0A00
    )

    if(MSVC)
        target_compile_options(VectorClick PRIVATE
            /W4 /permissive- /EHsc /utf-8
            $<$<CONFIG:Release>:/O1>
        )
        target_link_options(VectorClick PRIVATE
            /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
        )
    else()
        target_compile_options(VectorClick PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fstack-protector-strong
            $<$<CONFIG:Release>:-Os>
            $<$<CONFIG:Release>:-ffunction-sections>
            $<$<CONFIG:Release>:-fdata-sections>
        )
        # MinGW-compatible toolchains require -municode when the GUI entry point is wWinMain.
        target_link_options(VectorClick PRIVATE
            -municode
            -Wl,--subsystem,windows,--dynamicbase,--nxcompat,--high-entropy-va
            $<$<CONFIG:Release>:-Wl,--gc-sections,--strip-all>
        )
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(vectorclick_core_tests Tests/core_tests.cpp)
    target_link_libraries(vectorclick_core_tests PRIVATE vectorclick_presentation)
    if(MSVC)
        target_compile_options(vectorclick_core_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(vectorclick_core_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
    add_test(NAME vectorclick_core_tests COMMAND vectorclick_core_tests)
endif()

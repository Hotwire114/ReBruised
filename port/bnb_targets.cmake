# Black & Bruised executable: the ps2recomp runtime (ps2_runtime, built by this tree) plus the code ps2_recomp.exe
# generated from the player's SLES_516.20, compiled in batches (tools/make_batches.py -> build/batches).
get_property(_bnb_file GLOBAL PROPERTY BNB_TARGETS_FILE)
get_filename_component(BNB_ROOT "${_bnb_file}/../.." ABSOLUTE)
file(GLOB BNB_BATCHES CONFIGURE_DEPENDS "${BNB_ROOT}/build/batches/batch_*.cpp")
if(NOT BNB_BATCHES)
    message(WARNING "Black & Bruised: no build/batches/batch_*.cpp yet (run tools/make_batches.py); target skipped")
    return()
endif()

add_executable(BlackAndBruised "${BNB_ROOT}/port/main.cpp" "${BNB_ROOT}/port/button_prompts.cpp" "${BNB_ROOT}/port/disc_setup.cpp" "${BNB_ROOT}/port/app_icon.rc" ${BNB_BATCHES})
# Lifted VU microprograms (tools/build_program.ps1 -> tools/vu/vu_lift.py); without them VU runs in the exact engine.
file(GLOB BNB_VULIFT_PARTS CONFIGURE_DEPENDS "${BNB_ROOT}/build/vulift/vulift_part*.cpp")
if(BNB_VULIFT_PARTS)
    target_sources(BlackAndBruised PRIVATE ${BNB_VULIFT_PARTS})
    target_include_directories(BlackAndBruised PRIVATE "${CMAKE_SOURCE_DIR}/ps2xRuntime/include")
    message(STATUS "Black & Bruised: lifted VU programs ${BNB_VULIFT_PARTS}")
endif()
target_compile_features(BlackAndBruised PRIVATE cxx_std_20)
target_include_directories(BlackAndBruised PRIVATE "${BNB_ROOT}/generated" "${BNB_ROOT}/port/include")
target_link_libraries(BlackAndBruised PRIVATE ps2_runtime)
if(PS2X_ENABLE_DEBUG_UI)
    # same as ps2EntryRunner (ps2xRuntime/CMakeLists.txt): the debug panel lives in the program, not ps2_runtime
    target_sources(BlackAndBruised PRIVATE "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_link_libraries(BlackAndBruised PRIVATE rlImGui imgui)
    target_compile_definitions(BlackAndBruised PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()
# Generated code: same settings the Deadly Alliance build used (huge functions; -O1 keeps compile time sane).
set_source_files_properties(${BNB_BATCHES} PROPERTIES COMPILE_OPTIONS "-O1;-g0;-fno-strict-aliasing;-fwrapv;-Wno-everything")
if(MINGW)
    target_compile_options(BlackAndBruised PRIVATE -msse4.1)
    # raylib defines CloseWindow/ShowCursor, which user32's import members also carry once something pulls them
    # (seen when FFmpeg is linked): keep the first definition (raylib's).
    target_link_options(BlackAndBruised PRIVATE -Wl,--stack,67108864 -Wl,--allow-multiple-definition)
endif()
set_target_properties(BlackAndBruised PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(BlackAndBruised)
endif()
get_filename_component(BNB_COMPILER_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
foreach(dll libc++.dll libunwind.dll libwinpthread-1.dll)
    if(EXISTS "${BNB_COMPILER_BIN}/${dll}")
        add_custom_command(TARGET BlackAndBruised POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${BNB_COMPILER_BIN}/${dll}" "$<TARGET_FILE_DIR:BlackAndBruised>")
    endif()
endforeach()

# Launcher: animated game-style front end (port/launcher), raylib only. BlackAndBruised-Launcher.cmd starts it.
add_executable(BlackAndBruisedLauncher WIN32 "${BNB_ROOT}/port/launcher/launcher.cpp" "${BNB_ROOT}/port/launcher/launch_win.cpp" "${BNB_ROOT}/port/disc_setup.cpp"
               "${BNB_ROOT}/port/app_icon.rc")
target_compile_features(BlackAndBruisedLauncher PRIVATE cxx_std_20)
target_link_libraries(BlackAndBruisedLauncher PRIVATE raylib comdlg32 shell32 advapi32)
set_target_properties(BlackAndBruisedLauncher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(MINGW)
    target_link_options(BlackAndBruisedLauncher PRIVATE -Wl,--allow-multiple-definition)
endif()
add_custom_command(TARGET BlackAndBruisedLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:BlackAndBruisedLauncher>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BNB_ROOT}/port/launcher/assets/rebruised-logo-alpha.png"
            "$<TARGET_FILE_DIR:BlackAndBruisedLauncher>/assets/rebruised-logo-alpha.png")

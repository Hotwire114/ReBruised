# Passed as -DCMAKE_PROJECT_INCLUDE to the ps2recomp tree (source/ps2recomp): once the top-level project is
# configured, adds the Black & Bruised executable (runtime + recompiled game code in one program, llvm-mingw build).
get_property(_bnb_deferred GLOBAL PROPERTY BNB_TARGETS_DEFERRED)
if(NOT _bnb_deferred AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set_property(GLOBAL PROPERTY BNB_TARGETS_DEFERRED ON)
    if(WIN32)
        enable_language(RC) # app icon (port/app_icon.rc); not allowed in the deferred call below
    endif()
    set_property(GLOBAL PROPERTY BNB_TARGETS_FILE "${CMAKE_CURRENT_LIST_DIR}/bnb_targets.cmake")
    function(bnb_add_targets)
        get_property(_file GLOBAL PROPERTY BNB_TARGETS_FILE)
        include("${_file}")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL bnb_add_targets)
endif()

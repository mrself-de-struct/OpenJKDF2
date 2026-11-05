macro(plat_initialize)
    message( STATUS "Targeting Win32 MinGW (Windows XP / OpenGL 1.1)" )

    set(BIN_NAME "${PROJECT_NAME}-32")
    string(TOLOWER ${BIN_NAME} BIN_NAME)

    add_compile_definitions(
        WINVER=0x0501 _WIN32_WINNT=0x0501  # Windows XP
        WIN32_STANDALONE
        WIN64_STANDALONE # HACK
        WIN32            #TODO: Test `_WIN32` instead
        WIN32_MINGW      #TODO: Test `_WIN32 && __MINGW32__` instead
    )

    if (NOT DEFINED GITHUB_RUNNER_COMPILE)
        set(TARGET_USE_PHYSFS TRUE)
    endif()
    set(OPENJKDF2_NO_ASAN TRUE)
    set(TARGET_USE_LIBSMACKER TRUE)
    set(TARGET_USE_LIBSMUSHER TRUE)
    set(TARGET_USE_SDL2 TRUE)
    set(TARGET_USE_OPENGL11 TRUE)  # Legacy fixed-function GL 1.1 renderer
    if (NOT DEFINED GITHUB_RUNNER_COMPILE)
        set(TARGET_USE_OPENAL TRUE)
    endif()
    set(TARGET_POSIX TRUE)
    set(TARGET_WIN32 TRUE)
    set(TARGET_NO_BLOBS TRUE)
    set(TARGET_CAN_JKGM FALSE)  # jkgm needs modern GL/shaders
    set(TARGET_USE_CURL FALSE)
    set(TARGET_COMPILE_FREEGLUT FALSE)  # No GLUT/GLEW on the GL 1.1 path
    set(TARGET_FIND_OPENAL FALSE)
    # Real DirectPlay for interop with vanilla JK.EXE. Disable with -DTARGET_USE_DPLAY=OFF.
    if(NOT DEFINED TARGET_USE_DPLAY)
        set(TARGET_USE_DPLAY TRUE)
    endif()


    add_compile_definitions(main=SDL_main _MBCS)
    # TODO: Bump to O2 eventually. MinGW likes to replace memset with calls to itself...
    add_compile_options(-pthread -Wall -Wno-unused-variable -Wno-parentheses -Wno-missing-braces)
    # GCC 14+ made some long-standing C warnings errors by default. OpenJKDF2's
    # Win32 stand-in declarations (src/jk.h) trip -Wint-conversion and
    # -Wincompatible-pointer-types, e.g. in jkGUIRend.c; keep them warnings.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=return-mismatch>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-int>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=declaration-missing-parameter-type>)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL Debug)
        add_compile_options(-Og)
    else()
        add_compile_options(-O2)
    endif()
endmacro()

macro(plat_specific_deps)
    set(SDL2_COMMON_LIBS SDL::SDL)
endmacro()

macro(plat_link_and_package)
    set_target_properties(${BIN_NAME} PROPERTIES
        ENABLE_EXPORTS FALSE
        WINDOWS_EXPORT_ALL_SYMBOLS FALSE
        VERSION ${CMAKE_SYSTEM_VERSION}
    )
    if(CMAKE_BUILD_TYPE STREQUAL Release OR
       CMAKE_BUILD_TYPE STREQUAL MinSizeRel OR
       CMAKE_BUILD_TYPE STREQUAL RelWithDebInfo)
        # TODO: Implement WinMain() for this to work nicely
        set_target_properties(${BIN_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(CMAKE_BUILD_TYPE STREQUAL Debug)
        set_target_properties(${BIN_NAME} PROPERTIES WIN32_EXECUTABLE FALSE)
    endif()

    # NOTE: no GLEW / GLUT here -- OpenGL 1.1 entry points come straight from opengl32,
    # and extension procs (EXT_paletted_texture) are resolved via SDL_GL_GetProcAddress.
    # libssp is statically linked in the toolchain's standard libraries, so it is
    # intentionally not listed here (linking -lssp re-adds libssp-0.dll).
    target_link_libraries(sith_engine PRIVATE mingw32 ${SDL2_COMMON_LIBS} version imm32 setupapi gdi32 winmm imm32 ole32 oleaut32 shell32 winmm user32 crypt32 advapi32) # mingw32 must precede SDL::SDL so its runtime startup provides the entry point SDL3's built-in WinMain expects

    if (TARGET_USE_OPENAL)
        # SDL::Mixer before its Vorbis/Ogg deps so the static link resolves ov_*.
        target_link_libraries(sith_engine PRIVATE SDL::Mixer ${SDL_MIXER_DEPS})
        target_link_libraries(sith_engine PRIVATE OpenAL::OpenAL)
    endif()
    target_link_libraries(sith_engine PRIVATE nlohmann_json::nlohmann_json)
    if(TARGET_USE_DPLAY)
        target_link_libraries(sith_engine PRIVATE ZLIB::ZLIB) # Added: map/mod downloads in compressed blocks (jkweb_xfer.c)
    endif()
    if(TARGET_USE_DPLAY)
        # Added: zlib.h from the zlib built here (ZLIB::ZLIB's static target carries no include path)
        target_include_directories(sith_engine PRIVATE ${ZLIB_INCLUDE_DIRS})
    endif()
    if(TARGET_USE_PHYSFS)
        target_link_libraries(sith_engine PRIVATE PhysFS::PhysFS_s)
        target_link_libraries(${BIN_NAME} PRIVATE PhysFS::PhysFS_s)
    endif()
    # Windows system import libs last, so they satisfy symbols pulled in by any of
    # the static archives above (link order matters for import-stub archives too).
    target_link_libraries(sith_engine PRIVATE opengl32 ws2_32 uuid ole32 shell32 shlwapi advapi32 user32 gdi32 winmm imm32 version setupapi oleaut32 crypt32)

    # OpenAL ships as a DLL so it can be swapped for another OpenAL implementation.
    if (TARGET_USE_OPENAL)
        add_custom_command(
            TARGET ${BIN_NAME}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${PROJECT_BINARY_DIR}/openal/bin/OpenAL32.dll ${PROJECT_BINARY_DIR}
        )
    endif()

endmacro()

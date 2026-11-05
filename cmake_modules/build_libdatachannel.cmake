# WebRTC data channels for the web lobby transport (jkweb_rtc.c), via
# libdatachannel. Uses an installed copy if CMake can find one, otherwise
# downloads and builds a pinned release (data channels only, no media).
#
# Sets WEBRTC_LIBS (link these into sith_engine) and defines JKWEB_WEBRTC.

find_package(LibDataChannel CONFIG QUIET)
if(TARGET LibDataChannel::LibDataChannelStatic)
    set(WEBRTC_LIBS LibDataChannel::LibDataChannelStatic)
elseif(TARGET LibDataChannel::LibDataChannel)
    set(WEBRTC_LIBS LibDataChannel::LibDataChannel)
else()
    message(STATUS "Going to build “libdatachannel 0.24.6” (WebRTC data channels) from GitHub")
    include(FetchContent)
    set(NO_MEDIA ON CACHE BOOL "" FORCE)
    set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
    set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
    set(NO_TESTS ON CACHE BOOL "" FORCE)
    FetchContent_Declare(libdatachannel
        GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
        GIT_TAG v0.24.6
        GIT_SHALLOW TRUE
        GIT_SUBMODULES_RECURSE TRUE)

    # Build it with its own settings, not the engine's: the engine's global
    # definitions (USE_MBEDTLS, HAVE_CONFIG_H... meant for the bundled curl)
    # and flags (-fshort-wchar...) would change how it compiles.
    function(jk_add_libdatachannel)
        get_directory_property(_defs COMPILE_DEFINITIONS)
        set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "")
        set(CMAKE_C_FLAGS "")
        set(CMAKE_CXX_FLAGS "")
        FetchContent_MakeAvailable(libdatachannel)
        set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_defs}")
    endfunction()
    jk_add_libdatachannel()
    set(WEBRTC_LIBS datachannel-static)
endif()

add_compile_definitions(JKWEB_WEBRTC)

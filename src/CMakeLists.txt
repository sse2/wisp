set(WISP_CORE_SOURCES
    plat/plat_common.c
    core/config.c
    core/core.c
    audio/ring.c
    audio/output.c
    audio/source.c
    audio/decoders.c
    audio/engine.c
    net/http.c
    subsonic/subsonic.c
    subsonic/sync.c
    model/fold.c
    model/model.c
    cache/cache.c)

if(WIN32)
    list(APPEND WISP_CORE_SOURCES plat/plat_win.c)
else()
    list(APPEND WISP_CORE_SOURCES plat/plat_posix.c)
endif()

add_library(wisp_core STATIC ${WISP_CORE_SOURCES})
target_include_directories(wisp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(wisp_core PUBLIC Threads::Threads)
target_link_libraries(wisp_core PRIVATE wisp_warnings miniaudio ma_backends mbedtls wisp_cjson)

if(WIN32)
    target_link_libraries(wisp_core PUBLIC shell32 ole32 advapi32 crypt32)
endif()

set(WISP_UI_SOURCES main.c ui/screen.c ui/app.c)
if(WIN32)
    list(APPEND WISP_UI_SOURCES ui/tty_win.c)
else()
    list(APPEND WISP_UI_SOURCES ui/tty_posix.c)
endif()

add_executable(wisp ${WISP_UI_SOURCES})
target_link_libraries(wisp PRIVATE wisp_core wisp_warnings)

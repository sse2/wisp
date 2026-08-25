add_library(miniaudio STATIC impl/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC miniaudio)
if(APPLE)
    target_link_libraries(miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
elseif(NOT WIN32)
    target_link_libraries(miniaudio PUBLIC ${CMAKE_DL_LIBS} m pthread)
endif()

add_library(wisp_ogg STATIC ogg/src/bitwise.c ogg/src/framing.c)
target_include_directories(wisp_ogg PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/ogg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ogg_config)

file(GLOB WISP_VORBIS_SRC ${CMAKE_CURRENT_SOURCE_DIR}/vorbis/lib/*.c)
list(FILTER WISP_VORBIS_SRC EXCLUDE REGEX "(barkmel|psytune|tone|misc)\\.c$")
add_library(wisp_vorbis STATIC ${WISP_VORBIS_SRC})
target_include_directories(wisp_vorbis
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vorbis/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vorbis/lib)
target_link_libraries(wisp_vorbis PUBLIC wisp_ogg)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(opus)

add_library(wisp_opusfile STATIC
    opusfile/src/info.c
    opusfile/src/internal.c
    opusfile/src/opusfile.c
    opusfile/src/stream.c)
target_include_directories(wisp_opusfile
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/opusfile/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/opusfile/src)
target_compile_definitions(wisp_opusfile PRIVATE OP_DISABLE_HTTP)
target_link_libraries(wisp_opusfile PUBLIC opus wisp_ogg)

set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(_wisp_saved_sim "${CMAKE_C_SIMULATE_ID}")
set(CMAKE_C_SIMULATE_ID "")
add_subdirectory(mbedtls)
set(CMAKE_C_SIMULATE_ID "${_wisp_saved_sim}")

add_library(wisp_cjson STATIC cjson/cJSON.c)
target_include_directories(wisp_cjson PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cjson)
target_compile_definitions(wisp_cjson PUBLIC CJSON_HIDE_SYMBOLS)

add_library(ma_backends STATIC
    miniaudio/extras/decoders/libvorbis/miniaudio_libvorbis.c
    miniaudio/extras/decoders/libopus/miniaudio_libopus.c)
target_include_directories(ma_backends PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/extras/decoders/libvorbis
    ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/extras/decoders/libopus)
target_link_libraries(ma_backends PUBLIC miniaudio wisp_vorbis wisp_opusfile)

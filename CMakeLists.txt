cmake_minimum_required(VERSION 3.20)
project(HaloInAssaultCube LANGUAGES C CXX)

if(NOT APPLE)
    message(FATAL_ERROR "This build targets macOS.")
endif()

find_package(SDL2 CONFIG REQUIRED)
find_package(SDL2_image CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
find_library(OPENAL_FRAMEWORK OpenAL REQUIRED)
find_path(VORBIS_INCLUDE_DIR vorbis/vorbisfile.h REQUIRED)
find_path(OGG_INCLUDE_DIR ogg/ogg.h REQUIRED)
find_library(VORBISFILE_LIBRARY vorbisfile REQUIRED)

set(ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source/enet")
add_library(enet STATIC
    ${ENET_DIR}/callbacks.c
    ${ENET_DIR}/compress.c
    ${ENET_DIR}/host.c
    ${ENET_DIR}/list.c
    ${ENET_DIR}/packet.c
    ${ENET_DIR}/peer.c
    ${ENET_DIR}/protocol.c
    ${ENET_DIR}/unix.c
)
target_include_directories(enet PUBLIC "${ENET_DIR}/include")

set(CLIENT_SOURCES
    audiomanager.cpp
    autodownload.cpp
    bot/ac_bot.cpp
    bot/ac_bot_ai.cpp
    bot/bot.cpp
    bot/bot_ai.cpp
    bot/bot_util.cpp
    bot/bot_waypoint.cpp
    bot/botmanager.cpp
    client.cpp
    clientgame.cpp
    clients2c.cpp
    command.cpp
    console.cpp
    crypto.cpp
    docs.cpp
    editing.cpp
    entities.cpp
    halo.cpp
    halo_collision.cpp
    halo_counter.cpp
    halo_script.cpp
    http.cpp
    log.cpp
    main.cpp
    menus.cpp
    oggstream.cpp
    openal.cpp
    packetqueue.cpp
    physics.cpp
    protocol.cpp
    rendercubes.cpp
    rendergl.cpp
    renderhud.cpp
    rendermodel.cpp
    renderparticles.cpp
    rendertext.cpp
    rndmap.cpp
    scoreboard.cpp
    server.cpp
    serverbrowser.cpp
    serverms.cpp
    shadow.cpp
    soundlocation.cpp
    soundscheduler.cpp
    stream.cpp
    texture.cpp
    tools.cpp
    water.cpp
    weapon.cpp
    wizard.cpp
    world.cpp
    worldio.cpp
    worldlight.cpp
    worldocull.cpp
    worldrender.cpp
    zip.cpp
)
list(TRANSFORM CLIENT_SOURCES PREPEND "source/src/")

add_executable(halo-ac ${CLIENT_SOURCES})
set_target_properties(halo-ac PROPERTIES CXX_STANDARD 14 CXX_STANDARD_REQUIRED YES)
target_include_directories(halo-ac PRIVATE source/src source/src/bot source/include
    "${VORBIS_INCLUDE_DIR}" "${OGG_INCLUDE_DIR}")
target_compile_definitions(halo-ac PRIVATE GL_SILENCE_DEPRECATION)
target_compile_options(halo-ac PRIVATE -fsigned-char -fno-strict-aliasing)
target_link_libraries(halo-ac PRIVATE enet SDL2::SDL2 SDL2_image::SDL2_image ZLIB::ZLIB
    "${OPENGL_FRAMEWORK}" "${OPENAL_FRAMEWORK}" "${VORBISFILE_LIBRARY}")

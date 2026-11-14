cmake_minimum_required(VERSION 3.20)
project(HaloInAssaultCube LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
option(HALO_BUILD_QA "Build the halo-ac-qa verification client used by tests/" OFF)

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
    halo_collision.cpp
    halo_counter.cpp
    halo_script.cpp
    halo_ai.cpp
    halo_hud.cpp
    halo_weapons.cpp
    halo_world.cpp
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

# Everything except halo.cpp is shared by the game and the QA client. halo.cpp
# is compiled per executable because the QA hooks join its translation unit.
add_library(halo-engine OBJECT ${CLIENT_SOURCES})
target_include_directories(halo-engine PUBLIC source/src source/src/bot source/include
    "${VORBIS_INCLUDE_DIR}" "${OGG_INCLUDE_DIR}")
target_compile_definitions(halo-engine PUBLIC GL_SILENCE_DEPRECATION)
target_compile_options(halo-engine PUBLIC -fsigned-char -fno-strict-aliasing)
target_link_libraries(halo-engine PUBLIC enet SDL2::SDL2 SDL2_image::SDL2_image ZLIB::ZLIB
    "${OPENGL_FRAMEWORK}" "${OPENAL_FRAMEWORK}" "${VORBISFILE_LIBRARY}")

add_executable(halo-ac source/src/halo.cpp)
target_link_libraries(halo-ac PRIVATE halo-engine)

if(HALO_BUILD_QA)
    add_executable(halo-ac-qa source/src/halo.cpp tests/native/b30_trace_qa.cpp)
    target_compile_definitions(halo-ac-qa PRIVATE HALO_QA)
    target_include_directories(halo-ac-qa PRIVATE tests/native)
    target_link_libraries(halo-ac-qa PRIVATE halo-engine)
endif()

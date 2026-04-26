##
# Turbo Room Library
##

set(TURBO_ROOM_SOURCES
    turbo_room_provider.h
    turbo_room.h
    turbo_room.c
    provider_static.c
    provider_token_hmac.c
)

add_library(turbo_room STATIC ${TURBO_ROOM_SOURCES})

target_link_libraries(turbo_room turbo_forward turbo_common ssl crypto)

target_include_directories(turbo_room PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../forward
    ${CMAKE_CURRENT_SOURCE_DIR}/../netif
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

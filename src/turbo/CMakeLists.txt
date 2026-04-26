##
# Turbo Extensions
##

add_subdirectory(common)
add_subdirectory(netif)
add_subdirectory(forward)
add_subdirectory(room)
add_subdirectory(api)

# Top-level turbo.c (init/deinit + process_packet)
add_library(turbo_core STATIC turbo.c turbo.h)
target_link_libraries(turbo_core turbo_netif turbo_forward turbo_room turbo_common)
target_include_directories(turbo_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/netif
    ${CMAKE_CURRENT_SOURCE_DIR}/forward
    ${CMAKE_CURRENT_SOURCE_DIR}/room
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)

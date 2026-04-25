##
# Turbo Forward Library
##

# Source files
set(TURBO_FORWARD_SOURCES
    turbo_room.h
    turbo_room.c
    turbo_switch.h
    turbo_switch.c
    turbo_fec.h
    turbo_fec.c
)

# Create static library
add_library(turbo_forward STATIC ${TURBO_FORWARD_SOURCES})

# Link dependencies
target_link_libraries(turbo_forward turbo_network)

# Include directories
target_include_directories(turbo_forward PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../netif
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

if(TURBO_AFXDP)
    target_compile_definitions(turbo_forward PRIVATE TURBO_AFXDP)
endif()
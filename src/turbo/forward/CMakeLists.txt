##
# Turbo Forward Library
##

set(TURBO_FORWARD_SOURCES
    turbo_fastpath.h
    turbo_fastpath.c
    turbo_shaper.h
    turbo_shaper.c
    turbo_switch.h
    turbo_switch.c
    turbo_fec.h
    turbo_fec.c
)

add_library(turbo_forward STATIC ${TURBO_FORWARD_SOURCES})

target_link_libraries(turbo_forward turbo_netif turbo_common)

target_include_directories(turbo_forward PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../netif
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)

if(TURBO_AFXDP)
    target_compile_definitions(turbo_forward PRIVATE TURBO_AFXDP)
endif()
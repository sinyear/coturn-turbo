##
# Turbo network backend library
##

find_package(PkgConfig)

if(TURBO_AFXDP)
    pkg_check_modules(LIBBPF REQUIRED libbpf)
    pkg_check_modules(LIBXDP REQUIRED libxdp)
    if(LIBBPF_FOUND AND LIBXDP_FOUND)
        message(STATUS "libbpf found: ${LIBBPF_VERSION}")
        message(STATUS "libxdp found: ${LIBXDP_VERSION}")
        include_directories(${LIBBPF_INCLUDE_DIRS} ${LIBXDP_INCLUDE_DIRS})
        link_directories(${LIBBPF_LIBRARY_DIRS} ${LIBXDP_LIBRARY_DIRS})
    endif()
endif()

set(TURBO_NETIF_SOURCES
    turbo_netif.h
    turbo_netif.c
    turbo_port.h
    turbo_port.c
    turbo_mempool.h
    turbo_mempool.c
)

if(TURBO_AFXDP)
    list(APPEND TURBO_NETIF_SOURCES turbo_af_xdp.c turbo_af_xdp.h)
endif()

add_library(turbo_network STATIC ${TURBO_NETIF_SOURCES})

target_include_directories(turbo_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
)
target_link_libraries(turbo_network PUBLIC turbo_utils)

if(TURBO_AFXDP)
    target_compile_definitions(turbo_network PRIVATE TURBO_AFXDP)
    target_link_libraries(turbo_network PUBLIC ${LIBBPF_LIBRARIES} ${LIBXDP_LIBRARIES})
endif()

# XDP BPF program
if(TURBO_AFXDP)
    find_program(CLANG clang)
    if(CLANG)
        set(XDP_PROG_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/xdp_prog.c)
        set(XDP_PROG_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdp_prog.o)
        add_custom_command(
            OUTPUT ${XDP_PROG_OUTPUT}
            COMMAND ${CLANG} -O2 -target bpf -I/usr/include -c
                    ${XDP_PROG_SOURCE} -o ${XDP_PROG_OUTPUT}
            DEPENDS ${XDP_PROG_SOURCE}
            COMMENT "Compiling XDP BPF program"
            VERBATIM
        )
        add_custom_target(xdp_prog ALL DEPENDS ${XDP_PROG_OUTPUT})
        install(FILES ${XDP_PROG_OUTPUT} DESTINATION share/turnserver OPTIONAL)
    else()
        message(WARNING "clang not found — XDP BPF program will not be compiled.")
    endif()
endif()

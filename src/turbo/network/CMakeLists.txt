##
# Turbo Network Backend Library
##

# Find required packages for different backends
find_package(PkgConfig)

# DPDK backend
if(TURN_USE_DPDK)
    pkg_check_modules(DPDK REQUIRED libdpdk)
    if(DPDK_FOUND)
        message(STATUS "DPDK found: ${DPDK_VERSION}")
        include_directories(${DPDK_INCLUDE_DIRS})
        link_directories(${DPDK_LIBRARY_DIRS})
    endif()
endif()

# AF_XDP backend
if(TURN_USE_AFXDP)
    pkg_check_modules(LIBBPF REQUIRED libbpf)
    pkg_check_modules(LIBXDP REQUIRED libxdp)
    if(LIBBPF_FOUND AND LIBXDP_FOUND)
        message(STATUS "libbpf found: ${LIBBPF_VERSION}")
        message(STATUS "libxdp found: ${LIBXDP_VERSION}")
        include_directories(${LIBBPF_INCLUDE_DIRS} ${LIBXDP_INCLUDE_DIRS})
        link_directories(${LIBBPF_LIBRARY_DIRS} ${LIBXDP_LIBRARY_DIRS})
    endif()
endif()

# Source files
set(TURBO_NETWORK_SOURCES
    turbo_netif.h
    turbo_netif.c
    turbo_port.h
    turbo_port.c
    turbo_mempool.h
    turbo_mempool.c
)

# Add backend-specific sources
if(TURN_USE_DPDK)
    list(APPEND TURBO_NETWORK_SOURCES turbo_dpdk.c)
endif()

if(TURN_USE_AFXDP)
    list(APPEND TURBO_NETWORK_SOURCES turbo_af_xdp.c)
endif()

# Create static library
add_library(turbo_network STATIC ${TURBO_NETWORK_SOURCES})

# Set compile definitions
if(TURN_USE_DPDK)
    target_compile_definitions(turbo_network PRIVATE TURN_USE_DPDK)
endif()

if(TURN_USE_AFXDP)
    target_compile_definitions(turbo_network PRIVATE TURN_USE_AFXDP)
endif()

# Link libraries
if(TURN_USE_DPDK)
    target_link_libraries(turbo_network ${DPDK_LIBRARIES})
endif()

if(TURN_USE_AFXDP)
    target_link_libraries(turbo_network ${LIBBPF_LIBRARIES} ${LIBXDP_LIBRARIES})
endif()

# Include directories
target_include_directories(turbo_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# XDP BPF program compilation (AF_XDP backend only)
if(TURN_USE_AFXDP)
    # Find clang for BPF compilation
    find_program(CLANG clang)
    if(CLANG)
        set(XDP_PROG_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/xdp_prog.c)
        set(XDP_PROG_OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/xdp_prog.o)

        add_custom_command(
            OUTPUT ${XDP_PROG_OUTPUT}
            COMMAND ${CLANG} -O2 -target bpf -c
                    ${XDP_PROG_SOURCE}
                    -o ${XDP_PROG_OUTPUT}
            DEPENDS ${XDP_PROG_SOURCE}
            COMMENT "Compiling XDP BPF program"
            VERBATIM
        )
        add_custom_target(xdp_prog ALL DEPENDS ${XDP_PROG_OUTPUT})
        install(FILES ${XDP_PROG_OUTPUT} DESTINATION share/turnserver OPTIONAL)
    else()
        message(WARNING "clang not found - XDP BPF program will not be compiled")
    endif()
endif()
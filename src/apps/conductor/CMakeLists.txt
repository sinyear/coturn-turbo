##
# SPDX-License-Identifier: BSD-3-Clause
#
# https://opensource.org/license/bsd-3-clause
#
# Copyright (C) 2021 Kang Lin <kl222@126.com>
#
# All rights reserved.
##

project(conductor)

# Source files
set(CONDUCTOR_SOURCES
    main.c
    room_manager.c
    api_server.c
    signal_handler.c
    redis_sync.c
)

set(CONDUCTOR_HEADERS
    api_server.h
)

# Dependencies
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBEVENT REQUIRED
    libevent
    libevent_core
    libevent_extra
)

pkg_check_modules(JSONC REQUIRED json-c)

pkg_check_modules(OPENSSL REQUIRED openssl)

# Optional: hiredis for Redis sync
pkg_check_modules(HIREDIS hiredis)
if(HIREDIS_FOUND)
    list(APPEND conductor_DEFINITIONS HAVE_HIREDIS)
    list(APPEND conductor_LIBS ${HIREDIS_LIBRARIES})
    list(APPEND conductor_INCLUDE_DIRS ${HIREDIS_INCLUDE_DIRS})
endif()

# Threading
find_package(Threads REQUIRED)

# Build executable
add_executable(${PROJECT_NAME} ${CONDUCTOR_SOURCES} ${CONDUCTOR_HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${LIBEVENT_INCLUDE_DIRS}
    ${JSONC_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
    ${conductor_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${LIBEVENT_LIBRARIES}
    ${JSONC_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    ${conductor_LIBS}
    Threads::Threads
)

target_compile_definitions(${PROJECT_NAME} PRIVATE ${conductor_DEFINITIONS})

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

INSTALL(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
)

install(DIRECTORY
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
    COMPONENT Runtime
)

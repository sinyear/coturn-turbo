##
# Turbo Admin HTTP API
##

find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)

set(TURBO_API_SOURCES
    turbo_api.h
    turbo_api.c
)

add_library(turbo_api STATIC ${TURBO_API_SOURCES})
target_include_directories(turbo_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../forward
    ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/relay
    ${CMAKE_CURRENT_SOURCE_DIR}/../../apps/common
    ${JSONC_INCLUDE_DIRS}
)
target_compile_definitions(turbo_api PRIVATE TURBO_FEATURES)
target_link_libraries(turbo_api PUBLIC turbo_forward turbo_utils ${JSONC_LIBRARIES})

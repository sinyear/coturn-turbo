##
# Turbo common utilities
##

set(TURBO_COMMON_SOURCES
    turbo_hash.h
    turbo_hash.c
    turbo_json.h
    turbo_json.c
    turbo_rcu.h
    turbo_rcu.c
)

add_library(turbo_utils STATIC ${TURBO_COMMON_SOURCES})
target_include_directories(turbo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

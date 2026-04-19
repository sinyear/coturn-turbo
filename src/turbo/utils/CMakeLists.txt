##
# Turbo Utils Library
##

# Find json-c for turbo_json
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED json-c)

# Source files
set(TURBO_UTILS_SOURCES
    turbo_hash.h
    turbo_hash.c
    turbo_json.h
    turbo_json.c
    turbo_rcu.h
    turbo_rcu.c
)

# Create static library
add_library(turbo_utils STATIC ${TURBO_UTILS_SOURCES})

target_include_directories(turbo_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(turbo_utils PRIVATE ${JSONC_INCLUDE_DIRS})
target_link_libraries(turbo_utils ${JSONC_LIBRARIES})

cmake_minimum_required(VERSION 3.15)
project(snoop_capture)

include(FindPkgConfig)

# N-API / cmake-js integration
include_directories(${CMAKE_JS_INC})

add_library(${PROJECT_NAME} SHARED
    src/addon.c
)

# Platform-specific sources and libraries
if(UNIX AND NOT APPLE)
    pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
    pkg_check_modules(DBUS REQUIRED dbus-1)

    target_sources(${PROJECT_NAME} PRIVATE src/pipewire.c)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${PIPEWIRE_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME}
        ${PIPEWIRE_LIBRARIES}
        ${DBUS_LIBRARIES}
        pthread)
elseif(WIN32)
    # Phase 5: target_sources(${PROJECT_NAME} PRIVATE src/dxgi.c)
elseif(APPLE)
    # Phase 5: target_sources(${PROJECT_NAME} PRIVATE src/macos.c)
endif()

# N-API setup
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".node"
)
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8)

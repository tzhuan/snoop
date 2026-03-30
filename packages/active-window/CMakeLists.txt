cmake_minimum_required(VERSION 3.15)
project(snoop_active_window)

# N-API / cmake-js integration
include_directories(${CMAKE_JS_INC})

add_library(${PROJECT_NAME} SHARED
    src/addon.c
)

# Platform-specific sources and libraries
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_sources(${PROJECT_NAME} PRIVATE src/x11.c)
    target_include_directories(${PROJECT_NAME} PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} ${X11_LIBRARIES})
elseif(WIN32)
    # Phase 5: target_sources(${PROJECT_NAME} PRIVATE src/windows.c)
elseif(APPLE)
    # Phase 5: target_sources(${PROJECT_NAME} PRIVATE src/darwin.c)
endif()

# N-API setup
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ".node"
)
target_link_libraries(${PROJECT_NAME} ${CMAKE_JS_LIB})

# Use N-API version 8 for broad compatibility
target_compile_definitions(${PROJECT_NAME} PRIVATE NAPI_VERSION=8)

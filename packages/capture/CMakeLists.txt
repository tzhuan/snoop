cmake_minimum_required(VERSION 3.15)
project(snoop_capture)

include(FindPkgConfig)

# N-API / cmake-js integration
include_directories(${CMAKE_JS_INC})

# Helper: configure a capture .node target
macro(setup_capture_target target_name)
    set_target_properties(${target_name} PROPERTIES
        PREFIX ""
        SUFFIX ".node"
    )
    target_link_libraries(${target_name} ${CMAKE_JS_LIB})
    target_compile_definitions(${target_name} PRIVATE NAPI_VERSION=8)
endmacro()

if(UNIX AND NOT APPLE)
    # Two separate .node files on Linux so each only links its own deps.
    # index.js tries pipewire first, falls back to x11.

    # PipeWire backend (Wayland + GNOME X11)
    pkg_check_modules(PIPEWIRE libpipewire-0.3)
    pkg_check_modules(DBUS dbus-1)
    if(PIPEWIRE_FOUND AND DBUS_FOUND)
        add_library(snoop_capture_pipewire SHARED src/addon.c src/pipewire.c)
        target_include_directories(snoop_capture_pipewire PRIVATE
            ${PIPEWIRE_INCLUDE_DIRS}
            ${DBUS_INCLUDE_DIRS})
        target_link_libraries(snoop_capture_pipewire
            ${PIPEWIRE_LIBRARIES}
            ${DBUS_LIBRARIES}
            pthread)
        setup_capture_target(snoop_capture_pipewire)
    endif()

    # X11 backend (XShm)
    find_package(X11 REQUIRED)
    add_library(snoop_capture_x11 SHARED src/addon.c src/x11.c)
    target_include_directories(snoop_capture_x11 PRIVATE ${X11_INCLUDE_DIR})
    target_link_libraries(snoop_capture_x11 ${X11_LIBRARIES} Xext pthread)
    setup_capture_target(snoop_capture_x11)

elseif(WIN32)
    add_library(snoop_capture SHARED src/addon.c src/dxgi.c)
    target_link_libraries(snoop_capture d3d11 dxgi)
    setup_capture_target(snoop_capture)

elseif(APPLE)
    add_library(snoop_capture SHARED src/addon.c src/darwin.c)
    target_link_libraries(snoop_capture
        "-framework CoreGraphics"
        "-framework IOSurface")
    setup_capture_target(snoop_capture)

endif()

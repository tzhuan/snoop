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

    # ext-image-copy-capture-v1 backend (opt-in, Wayland only)
    pkg_check_modules(WAYLAND_CLIENT wayland-client)
    if(WAYLAND_CLIENT_FOUND)
        # Generate protocol C code from XML
        find_program(WAYLAND_SCANNER wayland-scanner)
        if(WAYLAND_SCANNER)
            set(PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/protocol)
            add_custom_command(
                OUTPUT ${PROTO_DIR}/ext-image-copy-capture-v1-protocol.c
                       ${PROTO_DIR}/ext-image-copy-capture-v1-client-protocol.h
                COMMAND ${WAYLAND_SCANNER} private-code
                    ${PROTO_DIR}/ext-image-copy-capture-v1.xml
                    ${PROTO_DIR}/ext-image-copy-capture-v1-protocol.c
                COMMAND ${WAYLAND_SCANNER} client-header
                    ${PROTO_DIR}/ext-image-copy-capture-v1.xml
                    ${PROTO_DIR}/ext-image-copy-capture-v1-client-protocol.h
                DEPENDS ${PROTO_DIR}/ext-image-copy-capture-v1.xml
            )
            add_custom_command(
                OUTPUT ${PROTO_DIR}/ext-image-capture-source-v1-protocol.c
                       ${PROTO_DIR}/ext-image-capture-source-v1-client-protocol.h
                COMMAND ${WAYLAND_SCANNER} private-code
                    ${PROTO_DIR}/ext-image-capture-source-v1.xml
                    ${PROTO_DIR}/ext-image-capture-source-v1-protocol.c
                COMMAND ${WAYLAND_SCANNER} client-header
                    ${PROTO_DIR}/ext-image-capture-source-v1.xml
                    ${PROTO_DIR}/ext-image-capture-source-v1-client-protocol.h
                DEPENDS ${PROTO_DIR}/ext-image-capture-source-v1.xml
            )
        endif()

        add_library(snoop_capture_eicc SHARED
            src/addon.c
            src/eicc.c
            ${PROTO_DIR}/ext-image-copy-capture-v1-protocol.c
            ${PROTO_DIR}/ext-image-capture-source-v1-protocol.c
        )
        target_include_directories(snoop_capture_eicc PRIVATE
            ${WAYLAND_CLIENT_INCLUDE_DIRS}
            ${PROTO_DIR})
        target_link_libraries(snoop_capture_eicc
            ${WAYLAND_CLIENT_LIBRARIES}
            pthread)
        setup_capture_target(snoop_capture_eicc)
    endif()

elseif(WIN32)
    # BitBlt backend (default) — multi-monitor for free, no DirectX capture
    add_library(snoop_capture_bitblt SHARED src/addon.c src/bitblt.c)
    target_link_libraries(snoop_capture_bitblt gdi32 user32)
    setup_capture_target(snoop_capture_bitblt)

    # DXGI backend (opt-in) — captures DirectX games, per-monitor streams
    add_library(snoop_capture_dxgi SHARED src/addon.c src/dxgi.c)
    target_link_libraries(snoop_capture_dxgi d3d11 dxgi)
    setup_capture_target(snoop_capture_dxgi)

elseif(APPLE)
    enable_language(OBJC)
    add_library(snoop_capture SHARED src/addon.c src/darwin.m)
    set_source_files_properties(src/darwin.m PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(snoop_capture
        "-framework ScreenCaptureKit"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework CoreGraphics")
    setup_capture_target(snoop_capture)

endif()

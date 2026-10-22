cmake_minimum_required(VERSION 3.25)
project(impshot VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json REQUIRED)
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET
    wayland-client wayland-egl egl glesv2 xkbcommon pangocairo cairo libpng zxing)
pkg_get_variable(WL_PROTOCOLS wayland-protocols pkgdatadir)
pkg_get_variable(WL_SCANNER wayland-scanner wayland_scanner)

# --- Wayland protocols -> C glue ---------------------------------------------
set(PROTO_DIR ${CMAKE_BINARY_DIR}/protocols)
file(MAKE_DIRECTORY ${PROTO_DIR})
set(PROTO_SRCS)
foreach(xml
        ${WL_PROTOCOLS}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml
        ${WL_PROTOCOLS}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml
        ${WL_PROTOCOLS}/staging/cursor-shape/cursor-shape-v1.xml
        ${WL_PROTOCOLS}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml
        ${WL_PROTOCOLS}/stable/viewporter/viewporter.xml
        ${WL_PROTOCOLS}/staging/alpha-modifier/alpha-modifier-v1.xml
        ${WL_PROTOCOLS}/stable/tablet/tablet-v2.xml
        ${WL_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml
        ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
    get_filename_component(name ${xml} NAME_WE)
    add_custom_command(
        OUTPUT ${PROTO_DIR}/${name}.h ${PROTO_DIR}/${name}.c
        COMMAND ${WL_SCANNER} client-header ${xml} ${PROTO_DIR}/${name}.h
        COMMAND ${WL_SCANNER} private-code ${xml} ${PROTO_DIR}/${name}.c
        DEPENDS ${xml})
    list(APPEND PROTO_SRCS ${PROTO_DIR}/${name}.c)
endforeach()

# --- GLSL -> C++ header with raw strings --------------------------------------
file(GLOB SHADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/shaders/*.glsl)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/shaders.hpp
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_BINARY_DIR}/shaders.hpp -DDIR=${CMAKE_SOURCE_DIR}/shaders
            -P ${CMAKE_SOURCE_DIR}/cmake/embed_shaders.cmake
    DEPENDS ${SHADERS} ${CMAKE_SOURCE_DIR}/cmake/embed_shaders.cmake)

add_executable(impshot
    src/main.cpp
    src/app.cpp
    src/capture.cpp
    src/renderer.cpp
    src/canvas.cpp
    src/toolbar.cpp
    src/export.cpp
    src/hypr.cpp
    src/recognize.cpp
    src/pin.cpp
    src/keyboard.cpp
    ${PROTO_SRCS}
    ${CMAKE_BINARY_DIR}/shaders.hpp)
target_include_directories(impshot PRIVATE src ${PROTO_DIR} ${CMAKE_BINARY_DIR})
target_link_libraries(impshot PRIVATE PkgConfig::DEPS nlohmann_json::nlohmann_json)
target_compile_definitions(impshot PRIVATE IMPSHOT_VERSION="${PROJECT_VERSION}")
target_compile_options(impshot PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wno-sign-conversion
    -fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE)
# сгенерированный wayland-scanner код не наш — без предупреждений
set_source_files_properties(${PROTO_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
target_link_options(impshot PRIVATE -pie -Wl,-z,relro,-z,now)

install(TARGETS impshot RUNTIME DESTINATION bin)

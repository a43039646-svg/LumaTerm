cmake_minimum_required(VERSION 3.16)
project(LumaTerm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(VTE REQUIRED IMPORTED_TARGET vte-2.91)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0)

add_executable(lumaterm src/main.cpp)
target_link_libraries(lumaterm PRIVATE PkgConfig::GTK3 PkgConfig::VTE PkgConfig::GSTREAMER)
target_compile_options(lumaterm PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(lumaterm PRIVATE LUMATERM_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/icon.png")

add_executable(lumaterm-settings src/settings.cpp)
target_link_libraries(lumaterm-settings PRIVATE PkgConfig::GTK3)
target_compile_options(lumaterm-settings PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(lumaterm-settings PRIVATE LUMATERM_ICON_PATH="${CMAKE_CURRENT_SOURCE_DIR}/icon.png")

install(TARGETS lumaterm RUNTIME DESTINATION bin)
install(TARGETS lumaterm-settings RUNTIME DESTINATION bin)
install(FILES icon.png DESTINATION share/icons/hicolor/512x512/apps RENAME lumaterm.png)
install(FILES data/lumaterm.desktop DESTINATION share/applications)
install(FILES data/lumaterm-settings.desktop DESTINATION share/applications)

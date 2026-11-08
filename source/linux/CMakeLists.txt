cmake_minimum_required(VERSION 3.20)
project(D2ArmLinux VERSION 0.1.6 LANGUAGES CXX)

set(D2_DISPLAY_VERSION "${PROJECT_VERSION}")
if("$ENV{GITHUB_REF_NAME}" MATCHES "^v(.+)$")
    set(D2_DISPLAY_VERSION "${CMAKE_MATCH_1}")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
find_package(X11 REQUIRED)
find_library(XTST_LIBRARY Xtst REQUIRED)
find_library(XFIXES_LIBRARY Xfixes REQUIRED)

qt_add_executable(d2arm-linux
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/gamepad.cpp
    src/gamepad.h
)
target_link_libraries(d2arm-linux PRIVATE Qt6::Widgets PkgConfig::SDL2 X11::X11 ${XTST_LIBRARY} ${XFIXES_LIBRARY})
target_compile_definitions(d2arm-linux PRIVATE D2_VERSION="${D2_DISPLAY_VERSION}")
install(TARGETS d2arm-linux RUNTIME DESTINATION bin)
install(FILES share/d2arm-linux.desktop DESTINATION share/applications)
install(FILES share/d2arm-linux.svg DESTINATION share/icons/hicolor/scalable/apps)
install(DIRECTORY assets/ DESTINATION share/d2arm-linux/assets USE_SOURCE_PERMISSIONS OPTIONAL)
install(FILES ../LICENSE DESTINATION share/licenses/d2arm-linux RENAME LICENSE-MIT.txt)
install(FILES ../winlator/LICENSE DESTINATION share/licenses/d2arm-linux RENAME LICENSE-LGPL-2.1.txt)

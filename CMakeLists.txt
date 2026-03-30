cmake_minimum_required(VERSION 3.20)
project(rdp2exec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREERDP REQUIRED IMPORTED_TARGET freerdp3)
pkg_check_modules(FREERDP_CLIENT REQUIRED IMPORTED_TARGET freerdp-client3)
pkg_check_modules(WINPR REQUIRED IMPORTED_TARGET winpr3)

add_library(rdp2exec_client SHARED
  src/plugin/rdp2exec_client.cpp
)

target_include_directories(rdp2exec_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rdp2exec_client PRIVATE PkgConfig::FREERDP PkgConfig::FREERDP_CLIENT PkgConfig::WINPR pthread)
set_target_properties(rdp2exec_client PROPERTIES
  OUTPUT_NAME "rdp2exec-client"
  PREFIX "lib"
)

install(TARGETS rdp2exec_client LIBRARY DESTINATION lib/freerdp3)
install(PROGRAMS src/launcher/rdp2exec.py DESTINATION bin)
install(FILES src/windows/rdp2exec_bridge.cpp DESTINATION share/rdp2exec/windows-src)

project(sandbox)
cmake_minimum_required(VERSION 2.8.12)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup()

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_PREFIX_PATH "C:\\Qt\\5.8\\msvc2015_64\\")

# Find the QtWidgets library
find_package(Qt5Widgets REQUIRED)

# Add the alia library.
file (GLOB_RECURSE alia_source_files ${CMAKE_SOURCE_DIR}/alia/*.[chi]pp)

add_executable(sandbox WIN32 main.cpp ${alia_source_files})

# Use the Widgets module from Qt 5.
qt5_use_modules(sandbox Widgets)

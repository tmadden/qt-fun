project(qt_fun)
cmake_minimum_required(VERSION 3.1.0)

set(CMAKE_VERBOSE_MAKEFILE TRUE)

include(${CMAKE_BINARY_DIR}/conanbuildinfo.cmake)
conan_basic_setup(KEEP_RPATHS)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
# Instruct CMake to run moc automatically when needed
set(CMAKE_AUTOMOC ON)
# Create code from a list of Qt designer ui files
set(CMAKE_AUTOUIC ON)

# Find the Qt libraries
find_package(Qt5Core CONFIG REQUIRED)
find_package(Qt5Widgets REQUIRED)

# Add the alia library.
file (GLOB_RECURSE alia_source_files ${CMAKE_SOURCE_DIR}/alia/*.[chi]pp)

add_executable(${PROJECT_NAME} WIN32 main.cpp ${alia_source_files})
qt5_use_modules(${PROJECT_NAME} Widgets)

# configure_file(${CMAKE_CURRENT_BINARY_DIR}/qt.conf
#                ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qt.conf COPYONLY)

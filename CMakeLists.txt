cmake_minimum_required(VERSION 3.27)
cmake_policy(SET CMP0144 NEW)
project(urpc VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0" CACHE STRING "Debug flags" FORCE)
set(CMAKE_C_FLAGS_DEBUG "-g -O0" CACHE STRING "Debug flags" FORCE)

add_compile_definitions(DEV_STAGE=${DEV_STAGE})

message(STATUS "DEV_STAGE is set to ${DEV_STAGE}")

include(FetchContent)

FetchContent_Declare(
        uvent
        GIT_REPOSITORY https://github.com/Usub-development/uvent.git
        GIT_TAG main
)
FetchContent_Declare(
        ureflect
        GIT_REPOSITORY https://github.com/Usub-development/ureflect.git
        GIT_TAG main
)

FetchContent_MakeAvailable(uvent)
FetchContent_MakeAvailable(ureflect)

file(GLOB_RECURSE URPC_HEADERS CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/urpc/*.h
        ${CMAKE_CURRENT_SOURCE_DIR}/include/urpc/*.hpp
)
file(GLOB_RECURSE URPC_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/urpc/*.cpp
)

add_library(urpc ${URPC_SOURCES} ${URPC_HEADERS})
add_library(usub::urpc ALIAS urpc)

target_include_directories(urpc
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/urpc>
)

target_link_libraries(urpc
        PUBLIC
        usub::uvent
        usub::ureflect
)

set_target_properties(urpc PROPERTIES
        EXPORT_NAME urpc
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

option(URPC_BUILD_EXAMPLES "Build urpc example executables" ON)
if (URPC_BUILD_EXAMPLES)
    add_executable(urpc_example examples/main.cpp
            examples/interceptor_demo.cpp)
    target_link_libraries(urpc_example PRIVATE urpc)
    target_compile_definitions(urpc_example PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_ping_inbox examples/main_ping_inbox.cpp)
    target_link_libraries(urpc_example_ping_inbox PRIVATE urpc)
    target_compile_definitions(urpc_example_ping_inbox PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_interceptors examples/interceptor_demo.cpp)
    target_link_libraries(urpc_example_interceptors PRIVATE urpc)
    target_compile_definitions(urpc_example_interceptors PRIVATE DEV_STAGE=${DEV_STAGE})
endif()

install(TARGETS urpc
        EXPORT urpcTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/urpc
)

install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/urpc
        FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
)

configure_package_config_file(
        cmake/urpcConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/urpcConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/urpc
        NO_CHECK_REQUIRED_COMPONENTS_MACRO
)

write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/urpcConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/urpcConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/urpcConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/urpc
)

install(EXPORT urpcTargets
        NAMESPACE urpc::
        FILE urpcTargets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/urpc
)
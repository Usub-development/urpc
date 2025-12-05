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

option(URPC_LOGS "Use URPC_LOGS" OFF)
option(URPC_BUILD_CLI "Build urpc_cli command-line tool" ON)

message(STATUS "URPC_LOGS = ${URPC_LOGS}")
message(STATUS "URPC_BUILD_CLI = ${URPC_BUILD_CLI}")

find_package(OpenSSL REQUIRED)

include(CTest)
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
FetchContent_MakeAvailable(uvent ureflect)

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
        OpenSSL::Crypto
        OpenSSL::SSL
)

if (URPC_LOGS)
    FetchContent_Declare(
            ulog
            GIT_REPOSITORY https://github.com/Usub-development/ulog.git
            GIT_TAG main
    )
    FetchContent_MakeAvailable(ulog)
    target_link_libraries(urpc
            PUBLIC
            usub::ulog
    )
endif ()

if (URPC_BUILD_CLI)
    FetchContent_Declare(
            ulog
            GIT_REPOSITORY https://github.com/Usub-development/ulog.git
            GIT_TAG main
    )
    FetchContent_MakeAvailable(ulog)
    target_link_libraries(urpc
            PUBLIC
            usub::ulog
    )

    add_executable(urpc_cli bin/urpc_cli.cpp)
    target_link_libraries(urpc_cli PRIVATE urpc usub::ulog)
    target_compile_definitions(urpc_cli PRIVATE DEV_STAGE=${DEV_STAGE})

    install(TARGETS urpc_cli
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif ()

target_compile_definitions(urpc PUBLIC
        $<$<BOOL:${URPC_LOGS}>:URPC_LOGS>
)

set_target_properties(urpc PROPERTIES
        EXPORT_NAME urpc
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
)

option(URPC_BUILD_EXAMPLES "Build urpc example executables" ON)
if (URPC_BUILD_EXAMPLES)
    add_executable(urpc_example examples/main.cpp)
    target_link_libraries(urpc_example PRIVATE urpc)
    target_compile_definitions(urpc_example PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_load_imitation examples/main_load.cpp)
    target_link_libraries(urpc_example_load_imitation PRIVATE urpc)
    target_compile_definitions(urpc_example_load_imitation PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_server_mtls examples/main_server_mtls.cpp)
    target_link_libraries(urpc_example_server_mtls PRIVATE urpc)
    target_compile_definitions(urpc_example_server_mtls PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_server_aes examples/main_aes.cpp)
    target_link_libraries(urpc_example_server_aes PRIVATE urpc)
    target_compile_definitions(urpc_example_server_aes PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_client examples/main_client.cpp)
    target_link_libraries(urpc_example_client PRIVATE urpc)
    target_compile_definitions(urpc_example_client PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_client_pool examples/main_client_pool.cpp)
    target_link_libraries(urpc_example_client_pool PRIVATE urpc)
    target_compile_definitions(urpc_example_client_pool PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_client_pool_expand examples/main_client_pool_expand.cpp)
    target_link_libraries(urpc_example_client_pool_expand PRIVATE urpc)
    target_compile_definitions(urpc_example_client_pool_expand PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_stress_client examples/main_client_stress.cpp)
    target_link_libraries(urpc_stress_client PRIVATE urpc)
    target_compile_definitions(urpc_stress_client PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_multi examples/main_server_multi.cpp)
    target_link_libraries(urpc_example_multi PRIVATE urpc)
    target_compile_definitions(urpc_example_multi PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_example_client_multi examples/main_multi_methods_single_client.cpp)
    target_link_libraries(urpc_example_client_multi PRIVATE urpc)
    target_compile_definitions(urpc_example_client_multi PRIVATE DEV_STAGE=${DEV_STAGE})

    add_executable(urpc_stress_client_multi examples/main_multi_methods_stress.cpp)
    target_link_libraries(urpc_stress_client_multi PRIVATE urpc)
    target_compile_definitions(urpc_stress_client_multi PRIVATE DEV_STAGE=${DEV_STAGE})
endif ()

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

option(URPC_BUILD_TESTS "Build urpc tests" ON)
if (BUILD_TESTING AND URPC_BUILD_TESTS)
    # GoogleTest
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.15.2
            FIND_PACKAGE_ARGS NAMES GTest
    )
    find_package(GTest QUIET)
    if (NOT GTest_FOUND)
        FetchContent_MakeAvailable(googletest)
        set(GTest_LIBS GTest::gtest GTest::gtest_main)
    else ()
        set(GTest_LIBS GTest::gtest GTest::gtest_main)
    endif ()

    file(GLOB_RECURSE URPC_TESTS CONFIGURE_DEPENDS tests/*.cpp)

    #    add_executable(urpc_tests ${URPC_TESTS})
    #    target_link_libraries(urpc_tests PRIVATE urpc ${GTest_LIBS})
    #    target_compile_definitions(urpc_tests PRIVATE DEV_STAGE=${DEV_STAGE})
    #    target_compile_features(urpc_tests PRIVATE cxx_std_23)
    #
    #    include(GoogleTest)
    #    gtest_discover_tests(urpc_tests
    #            WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    #            DISCOVERY_TIMEOUT 60
    #    )
endif ()
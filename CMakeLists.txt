cmake_minimum_required(VERSION 3.22)
set(SC_MODULE rest-server)
project(simply-cpp-${SC_MODULE} LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_REST_SERVER_BUILD_TESTS "Build simply-cpp-rest-server tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.4.3)
include(cmake/oatpp.cmake)

set(SOURCE_OBJECTS include/rest_server.h)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(rest_server PUBLIC_LINK_LIBRARIES sc::sc-core)
target_include_directories(sc-obj-rest_server PRIVATE "${oatpp_SOURCE_DIR}/src")

add_sc_libraries(DESCRIPTION "simply-cpp REST server")
target_compile_features(sc-rest-server PUBLIC cxx_std_20)
target_compile_features(sc-rest-server-shared PUBLIC cxx_std_20)
target_link_libraries(sc-rest-server-shared PRIVATE oatpp)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake cmake/oatpp.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-rest-server
        COMPONENT development)
install(FILES tests/sc_test.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT development)

if (SC_REST_SERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-rest-server-test)
endif ()

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.4.3)")

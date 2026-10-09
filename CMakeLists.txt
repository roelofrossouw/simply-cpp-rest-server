cmake_minimum_required(VERSION 3.22)
set(SC_MODULE rest-server)
project(simply-cpp-${SC_MODULE} LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_REST_SERVER_BUILD_TESTS "Build simply-cpp-rest-server tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

# The library needs 1.4.3; the demo also uses sc::getenv(), sc::datetime, sc::console::subheading,
# and an sc::rest that can be called more than once (1.5.13).
find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.5.21)
find_or_install_package(oatpp simply-cpp-oatpp-dev oatpp)
find_or_install_package(nlohmann_json nlohmann-json3-dev nlohmann_json)
find_or_install_package(OpenSSL libssl-dev openssl)

set(SOURCE_OBJECTS include/rest_server.h)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(rest_server
        LINK_LIBRARIES oatpp::oatpp nlohmann_json::nlohmann_json OpenSSL::Crypto
        PUBLIC_LINK_LIBRARIES sc::sc-core oatpp::oatpp nlohmann_json::nlohmann_json OpenSSL::Crypto)

add_sc_libraries(DESCRIPTION "simply-cpp REST server")
target_compile_features(sc-rest-server PUBLIC cxx_std_20)
target_compile_features(sc-rest-server-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-rest-server
        COMPONENT development)

if (SC_REST_SERVER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-rest-server-test)
endif ()

add_sc_example(sc-rest-server-demo)

set(CPACK_PACKAGE_DIRECTORY "${CMAKE_BINARY_DIR}")
package_sc_module(
        DEPENDS simply-cpp-oatpp
        DEV_DEPENDS "simply-cpp-dev (>= 1.5.21~)" simply-cpp-oatpp-dev nlohmann-json3-dev libssl-dev)

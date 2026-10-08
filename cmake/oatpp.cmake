include(FetchContent)

if (NOT TARGET oatpp)
    set(OATPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(OATPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(OATPP_LINK_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(sc_rest_server_policy_version_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    FetchContent_Declare(
            oatpp
            GIT_REPOSITORY https://github.com/oatpp/oatpp.git
            GIT_TAG 1.3.1
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(oatpp)
    set(CMAKE_POLICY_VERSION_MINIMUM "${sc_rest_server_policy_version_minimum}")
endif ()

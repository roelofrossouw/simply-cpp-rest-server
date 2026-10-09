# Keep project()/cmake_minimum_required() in the top-level CMakeLists.txt.
# Only the post-project C++/PIC defaults are safe here.
# This file is also loaded by downstream consumers via find_package(), where SC_MODULE is unset.
if (SC_MODULE)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    option(SC_BUILD_EXAMPLES "Build and install the module's examples" ON)
endif ()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)
include(CMakeParseArguments)

# Keep runtime search paths valid for relocatable installs.
set(CMAKE_SKIP_BUILD_RPATH FALSE)
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
if (APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
elseif (UNIX)
    set(CMAKE_INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif ()

set(SC_HELPERS_VERSION 24)
set(SC_VERSION_FILE "VERSION.txt")
set(SC_VERSION_DEFAULT "1.0.0")

function(parse_sc_version text output)
    string(REGEX REPLACE "^[vV]" "" candidate "${text}")
    # CMake regexes have no {m,n} repetition, so the optional parts are spelled out.
    if (candidate MATCHES "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?(\\.[0-9]+)?$")
        set(${output} "${candidate}" PARENT_SCOPE)
    else ()
        set(${output} "" PARENT_SCOPE)
    endif ()
endfunction()

function(get_sc_version)
    set(version_file "${CMAKE_CURRENT_SOURCE_DIR}/${SC_VERSION_FILE}")
    set(version "")

    find_package(Git QUIET)
    if (Git_FOUND)
        execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE git_tag
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE git_result
        )

        if (git_result EQUAL 0)
            parse_sc_version("${git_tag}" version)
            if ("${version}" STREQUAL "")
                message(WARNING "Ignoring git tag '${git_tag}': not a version number")
            endif ()
        endif ()
    endif ()

    if (NOT "${version}" STREQUAL "")
        message(STATUS "Version ${version} from git tag '${git_tag}'")
        write_sc_version_file("${version}" "${version_file}")
    else ()
        read_sc_version_file("${version_file}" version)
    endif ()

    if ("${version}" STREQUAL "")
        set(version "${SC_VERSION_DEFAULT}")
        message(WARNING "No usable git tag and no ${SC_VERSION_FILE}; defaulting to ${version}.")
    endif ()

    set(SC_VERSION "${version}" PARENT_SCOPE)
endfunction()

function(write_sc_version_file version version_file)
    set(staged "${CMAKE_CURRENT_BINARY_DIR}/${SC_VERSION_FILE}")
    file(WRITE "${staged}" "${version}\n")
    file(COPY_FILE "${staged}" "${version_file}" ONLY_IF_DIFFERENT RESULT copy_error)
    if (copy_error)
        message(WARNING "Could not update ${version_file}: ${copy_error}."
                " A build without git will fall back to whatever it already holds.")
    endif ()
endfunction()

function(read_sc_version_file version_file output)
    set(${output} "" PARENT_SCOPE)
    if (NOT EXISTS "${version_file}")
        return()
    endif ()

    file(READ "${version_file}" contents)
    string(STRIP "${contents}" contents)
    parse_sc_version("${contents}" version)
    if ("${version}" STREQUAL "")
        message(WARNING "Ignoring ${version_file}: '${contents}' is not a version number")
        return()
    endif ()

    message(STATUS "Version ${version} from ${SC_VERSION_FILE} (no usable git tag)")
    set(${output} "${version}" PARENT_SCOPE)
endfunction()

# Try the requested package name and the lowercase form, and report success under the caller's spelling.
macro(sc_find_package_any_case package)
    find_package(${package} QUIET ${ARGN})

    string(TOLOWER "${package}" SC_ANY_CASE_LOWER)
    if (NOT ${package}_FOUND AND NOT "${SC_ANY_CASE_LOWER}" STREQUAL "${package}")
        find_package(${SC_ANY_CASE_LOWER} QUIET ${ARGN})
        if (${SC_ANY_CASE_LOWER}_FOUND)
            message(STATUS "${package} resolved as ${SC_ANY_CASE_LOWER}")
            set(${package}_FOUND TRUE)
            if (NOT ${package}_VERSION)
                set(${package}_VERSION "${${SC_ANY_CASE_LOWER}_VERSION}")
            endif ()
        endif ()
    endif ()
endmacro()

# Ensure the simply-cpp package source is registered before installing our own packages.
function(sc_ensure_package_source)
    if (APPLE)
        execute_process(COMMAND brew --repository OUTPUT_VARIABLE SC_BREW_REPO OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if (NOT SC_BREW_REPO OR EXISTS "${SC_BREW_REPO}/Library/Taps/roelofrossouw/homebrew-sc")
            return()
        endif ()
    elseif (UNIX AND EXISTS "/usr/bin/apt")
        if (EXISTS "/etc/apt/sources.list.d/simply-cpp.list")
            return()
        endif ()
    else ()
        return()
    endif ()

    message(STATUS "Registering the simply-cpp package source")
    execute_process(COMMAND bash -c "curl -fsSL https://apt.roelof.co.za/setup.sh | bash")
endfunction()

# find_or_install_package(<package> <apt name> <brew name> [COMPONENTS ...])
# A macro so find_package() result variables land in the caller's scope.
macro(find_or_install_package package apt_name brew_name)
    cmake_parse_arguments(SC_PACKAGE "" "" "COMPONENTS" ${ARGN})
    set(SC_PACKAGE_ARGS)
    if (SC_PACKAGE_COMPONENTS)
        set(SC_PACKAGE_ARGS COMPONENTS ${SC_PACKAGE_COMPONENTS})
    endif ()

    message(STATUS "Detecting ${package}")
    #sc_find_package_any_case(${package} ${SC_PACKAGE_ARGS})
    find_package(${package} QUIET ${ARGN})

    if (NOT ${package}_FOUND)
        sc_ensure_package_source()
        if (UNIX AND EXISTS "/usr/bin/apt")
            message(STATUS "${package} not found, attempting apt installation...")
            execute_process(COMMAND sudo apt -y install ${apt_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()
        if (APPLE)
            message(STATUS "${package} not found, attempting brew installation...")
            execute_process(COMMAND brew install ${brew_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()

        # Re-check the package that was asked for.
        sc_find_package_any_case(${package} ${SC_PACKAGE_ARGS})
        if (NOT ${package}_FOUND)
            message(FATAL_ERROR "Failed to install or locate ${package}"
                    " (install result=${SC_PACKAGE_INSTALL_RESULT})")
        endif ()
    endif ()

    message(STATUS "${package} found - ${${package}_VERSION}")
endmacro()

# sc_forget_stale_pkg_config(<prefix>)
# pkg_check_modules() keeps what it found in the cache and doesn't look again. Homebrew keeps
# each version of a package in its own Cellar folder and removes the old one on upgrade, so a
# cached library path can stop existing (make: "No rule to make target .../Cellar/<package>/
# <old version>/lib/..."). When one has, forget the result so pkg-config is asked again.
function(sc_forget_stale_pkg_config prefix)
    get_cmake_property(cached_variables CACHE_VARIABLES)
    set(stale FALSE)
    foreach (variable IN LISTS cached_variables)
        if (variable MATCHES "^pkgcfg_lib_${prefix}_" AND ${variable} AND NOT EXISTS "${${variable}}")
            set(stale TRUE)
        endif ()
    endforeach ()
    if (NOT stale)
        return()
    endif ()
    message(STATUS "${prefix}: the cached location is gone (upgraded?), looking again")
    foreach (variable IN LISTS cached_variables)
        if (variable MATCHES "^pkgcfg_lib_${prefix}_")
            unset(${variable} CACHE)
        endif ()
    endforeach ()
    unset(__pkg_config_checked_${prefix} CACHE)
endfunction()

macro(config_or_install_package package apt_name brew_name)
    cmake_parse_arguments(SC_PACKAGE "" "" "COMPONENTS" ${ARGN})
    set(SC_PACKAGE_ARGS)
    if (SC_PACKAGE_COMPONENTS)
        set(SC_PACKAGE_ARGS COMPONENTS ${SC_PACKAGE_COMPONENTS})
    endif ()
    string(TOLOWER "${package}" package_lower)

    message(STATUS "Detecting ${package}")
    find_package(PkgConfig REQUIRED)
    sc_forget_stale_pkg_config(${package})
    pkg_check_modules(${package} IMPORTED_TARGET ${package_lower})

    # Tesseract ships pkg-config rather than a CMake config on both platforms.
    #find_package(PkgConfig REQUIRED)
    #pkg_check_modules(Tesseract IMPORTED_TARGET tesseract)
    #if (NOT Tesseract_FOUND)
    #    if (UNIX AND EXISTS "/usr/bin/apt")
    #        message(STATUS "tesseract not found, attempting apt installation...")
    #        execute_process(COMMAND sudo apt -y install libtesseract-dev)
    #    elseif (APPLE)
    #        message(STATUS "tesseract not found, attempting brew installation...")
    #        execute_process(COMMAND brew install tesseract)
    #    endif ()
    #    pkg_check_modules(Tesseract REQUIRED IMPORTED_TARGET tesseract)
    #endif ()
    #message(STATUS "Tesseract found - ${Tesseract_VERSION}")



    if (NOT ${package}_FOUND)
        sc_ensure_package_source()
        if (UNIX AND EXISTS "/usr/bin/apt")
            message(STATUS "${package} not found, attempting apt installation...")
            execute_process(COMMAND sudo apt -y install ${apt_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()
        if (APPLE)
            message(STATUS "${package} not found, attempting brew installation...")
            execute_process(COMMAND brew install ${brew_name} RESULT_VARIABLE SC_PACKAGE_INSTALL_RESULT)
        endif ()

        # Re-check the package that was asked for.
        pkg_check_modules(${package} IMPORTED_TARGET ${package_lower})
        if (NOT ${package}_FOUND)
            message(FATAL_ERROR "Failed to install or configure ${package}"
                    " (install result=${SC_PACKAGE_INSTALL_RESULT})")
        endif ()
    endif ()

    message(STATUS "${package} found - ${${package}_VERSION}")
endmacro()

# find_or_fetch_package(<package> ...)
# Use an installed package when present; otherwise fetch it.
macro(find_or_fetch_package package)
    cmake_parse_arguments(SC_FETCH "" "GIT_REPOSITORY;GIT_TAG;VERSION;FORCE" "COMPONENTS;DECLARE_ARGS" ${ARGN})

    set(SC_FETCH_FIND_ARGS ${SC_FETCH_VERSION})
    if (SC_FETCH_COMPONENTS)
        list(APPEND SC_FETCH_FIND_ARGS COMPONENTS ${SC_FETCH_COMPONENTS})
    endif ()

    if (SC_FETCH_FORCE)
        set(${package}_FOUND FALSE)
    else ()
        message(STATUS "Detecting ${package}")
        sc_find_package_any_case(${package} ${SC_FETCH_FIND_ARGS})
    endif ()

    if (${package}_FOUND)
        message(STATUS "${package} found - ${${package}_VERSION}")
    else ()
        if (NOT SC_FETCH_GIT_REPOSITORY)
            message(FATAL_ERROR "find_or_fetch_package(${package}): not installed,"
                    " and no GIT_REPOSITORY given to fetch it from")
        endif ()
        if (NOT SC_FETCH_GIT_TAG)
            set(SC_FETCH_GIT_TAG main)
        endif ()
        message(STATUS "Fetching ${package} - ${SC_FETCH_GIT_REPOSITORY}@${SC_FETCH_GIT_TAG}")
        FetchContent_Declare(${package}
                GIT_REPOSITORY ${SC_FETCH_GIT_REPOSITORY}
                GIT_TAG ${SC_FETCH_GIT_TAG}
                GIT_SHALLOW TRUE
                EXCLUDE_FROM_ALL
                ${SC_FETCH_DECLARE_ARGS})
        FetchContent_MakeAvailable(${package})
    endif ()
endmacro()

# add_sc_object(<name> ...)
# Builds src/<name>.cpp into an object library for the module's consolidated libraries.
function(add_sc_object object)
    # sc-obj-, not sc-: sc-<module> is the final library.
    set(object_name "sc-obj-${object}")
    set(source_file "src/${object}.cpp")
    set(header_file "include/${object}.h")

    set(options)
    set(one_value_args)
    set(multi_value_args SOURCES INCLUDE_DIRS LINK_LIBRARIES PUBLIC_LINK_LIBRARIES)
    cmake_parse_arguments(SC_OBJECT "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})
    set(INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_library(${object_name} OBJECT ${source_file} ${header_file} ${SC_OBJECT_SOURCES})
    set_target_properties(${object_name} PROPERTIES EXCLUDE_FROM_ALL ON)
    target_include_directories(${object_name} PRIVATE ${INCLUDE} ${SC_OBJECT_INCLUDE_DIRS})

    if (SC_OBJECT_LINK_LIBRARIES)
        target_link_libraries(${object_name} PRIVATE ${SC_OBJECT_LINK_LIBRARIES})
    endif ()

    list(APPEND SOURCE_OBJECTS $<TARGET_OBJECTS:${object_name}>)
    set(SOURCE_OBJECTS "${SOURCE_OBJECTS}" PARENT_SCOPE)

    if (SC_OBJECT_PUBLIC_LINK_LIBRARIES)
        list(APPEND SOURCE_LINK_LIBRARIES ${SC_OBJECT_PUBLIC_LINK_LIBRARIES})
        set(SOURCE_LINK_LIBRARIES "${SOURCE_LINK_LIBRARIES}" PARENT_SCOPE)
    endif ()
endfunction()

# sc_module_name(<output>)
# Returns the module library/package name: sc-<SC_MODULE>.
function(sc_module_name output)
    if (NOT SC_MODULE)
        message(FATAL_ERROR "SC_MODULE is not set."
                " Set it before add_sc_libraries()/install_sc_module(), or pass NAME.")
    endif ()
    set(${output} "sc-${SC_MODULE}" PARENT_SCOPE)
endfunction()

# add_sc_libraries([NAME ...])
# Build the static and shared module libraries from SOURCE_OBJECTS.
function(add_sc_libraries)
    set(one_value_args NAME DESCRIPTION VERSION INCLUDE_DIR)
    cmake_parse_arguments(ARG "" "${one_value_args}" "" ${ARGN})

    set(name "${ARG_NAME}")
    if (NOT name)
        sc_module_name(name)
    endif ()
    if (NOT ARG_VERSION)
        set(ARG_VERSION "${SC_VERSION}")
    endif ()
    if (NOT ARG_INCLUDE_DIR)
        set(ARG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
    endif ()
    if (NOT ARG_DESCRIPTION)
        set(ARG_DESCRIPTION "simply-cpp ${name}")
    endif ()
    string(REGEX MATCH "^[0-9]+" version_major "${ARG_VERSION}")

    # Captured before the new targets are added, so they link the module's dependencies
    # and not each other.
    set(dependencies ${SOURCE_LIBRARIES} ${SOURCE_LINK_LIBRARIES})

    foreach (kind STATIC SHARED)
        set(target "${name}")
        set(what "consolidated static library")
        if (kind STREQUAL "SHARED")
            set(target "${name}-shared")
            set(what "common shared library")
        endif ()

        add_library(${target} ${kind} ${SOURCE_OBJECTS})
        add_library(sc::${target} ALIAS ${target})
        set_target_properties(${target} PROPERTIES
                VERSION ${ARG_VERSION}
                SOVERSION ${version_major}
                DESCRIPTION "${ARG_DESCRIPTION} ${what}")
        target_include_directories(${target} PUBLIC
                $<BUILD_INTERFACE:${ARG_INCLUDE_DIR}>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_link_libraries(${target} PUBLIC ${dependencies})
        list(APPEND SOURCE_LIBRARIES ${target})
        if (${kind} STREQUAL SHARED)
            # Export the shared library from the call that installs its real file.
            install(TARGETS ${target} EXPORT ${name}Targets
                    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime NAMELINK_COMPONENT development)
        endif ()
    endforeach ()

    set(SOURCE_LIBRARIES "${SOURCE_LIBRARIES}" PARENT_SCOPE)
endfunction()

# install_sc_module([NAME ...])
# Install headers and package config for consumers.
function(install_sc_module)
    set(one_value_args NAME VERSION CONFIG_TEMPLATE)
    set(multi_value_args PATH_VARS)
    cmake_parse_arguments(ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    set(name "${ARG_NAME}")
    if (NOT name)
        sc_module_name(name)
    endif ()
    if (NOT ARG_VERSION)
        set(ARG_VERSION "${SC_VERSION}")
    endif ()
    if (NOT ARG_CONFIG_TEMPLATE)
        set(ARG_CONFIG_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/${name}Config.cmake.in")
    endif ()
    if (NOT EXISTS "${ARG_CONFIG_TEMPLATE}")
        message(FATAL_ERROR "install_sc_module(${name}): no package config template at ${ARG_CONFIG_TEMPLATE}")
    endif ()

    set(package_destination "${CMAKE_INSTALL_LIBDIR}/cmake/${name}")

    # Install only the static archive here; the shared library is exported from its own install() call.
    set(sc_static_libraries "${SOURCE_LIBRARIES}")
    list(FILTER sc_static_libraries EXCLUDE REGEX "-shared$")
    install(TARGETS ${sc_static_libraries} EXPORT ${name}Targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT development)
    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ COMPONENT development DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN ".DS_Store" EXCLUDE)
    install(EXPORT ${name}Targets FILE ${name}Targets.cmake NAMESPACE sc:: DESTINATION ${package_destination} COMPONENT development)

    configure_package_config_file(${ARG_CONFIG_TEMPLATE} ${CMAKE_CURRENT_BINARY_DIR}/${name}Config.cmake
            INSTALL_DESTINATION ${package_destination}
            PATH_VARS ${ARG_PATH_VARS})
    write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/${name}ConfigVersion.cmake
            VERSION ${ARG_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${name}Config.cmake
            ${CMAKE_CURRENT_BINARY_DIR}/${name}ConfigVersion.cmake
            DESTINATION ${package_destination} COMPONENT development)
endfunction()

# package_sc_module([DEPENDS ...] [DEV_DEPENDS ...])
# Set Debian runtime/dev dependency metadata for the generated package.
function(package_sc_module)
    cmake_parse_arguments(ARG "" "" "DEPENDS;DEV_DEPENDS" ${ARGN})
    # cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    if (APPLE)
        set(CODENAME apple)
        set(CPACK_GENERATOR "TGZ")
    elseif (UNIX)
        execute_process(
                COMMAND lsb_release -sc
                OUTPUT_VARIABLE CODENAME
                OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        set(CPACK_GENERATOR "DEB")
    endif ()
    if (${SC_MODULE} STREQUAL "core")
        set(SC_PACKAGE_NAME simply-cpp)
    else ()
        set(SC_PACKAGE_NAME simply-cpp-${SC_MODULE})
    endif ()
    set(CPACK_COMPONENTS_ALL runtime development)
    set(CPACK_DEB_COMPONENT_INSTALL ON)
    set(CPACK_PACKAGE_NAME "${SC_PACKAGE_NAME}")
    set(CPACK_PACKAGE_VERSION "${SC_VERSION}~${CODENAME}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Simply C++ ${SC_MODULE} module")
    set(CPACK_PACKAGE_CONTACT "Roelof Rossouw")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_NAME "${SC_PACKAGE_NAME}")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_NAME "${SC_PACKAGE_NAME}-dev")
    # set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}-unknown-${CPACK_PACKAGE_VERSION}-${CODENAME}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_RUNTIME_FILE_NAME "${SC_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_DEVELOPMENT_FILE_NAME "${SC_PACKAGE_NAME}-dev-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}.deb")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Roelof Rossouw <roelof@roelof.co.za>")
    set(CPACK_DEBIAN_PACKAGE_DESCRIPTION "Simply C++ ${SC_MODULE} module for Ubuntu ${CODENAME}")
    set(CPACK_DEBIAN_RUNTIME_PACKAGE_SECTION "utils")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_LIBRARY_PACKAGE_SECTION "libs")
    if (ARG_DEPENDS)
        string(REPLACE ";" ", " ARG_DEPENDS_LIST "${ARG_DEPENDS}")
        set(CPACK_DEBIAN_RUNTIME_PACKAGE_DEPENDS "${ARG_DEPENDS_LIST}")
        # set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS "${ARG_DEPENDS_LIST}")
    endif ()
    set(development_depends "${SC_PACKAGE_NAME} (= ${CPACK_PACKAGE_VERSION})")
    if (ARG_DEV_DEPENDS)
        list(APPEND development_depends ${ARG_DEV_DEPENDS})
    endif ()
    string(REPLACE ";" ", " development_depends "${development_depends}")
    set(CPACK_DEBIAN_DEVELOPMENT_PACKAGE_DEPENDS "${development_depends}")
    include(CPack)
endfunction()

# add_sc_test(<name> ...)
# Build a CTest target from <name>.cpp.
function(add_sc_test name)
    # Prefix ARG to avoid clobbering the directory-level SC_TEST_LINK_LIBRARIES default.
    set(options)
    set(one_value_args TIMEOUT)
    set(multi_value_args LABELS LINK_LIBRARIES)
    cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if (NOT ARG_TIMEOUT)
        set(ARG_TIMEOUT 120)
    endif ()

    if (NOT ARG_LINK_LIBRARIES)
        set(ARG_LINK_LIBRARIES ${SC_TEST_LINK_LIBRARIES})
    endif ()
    if (NOT ARG_LINK_LIBRARIES)
        message(FATAL_ERROR "add_sc_test(${name}): nothing to link against."
                " Pass LINK_LIBRARIES, or set SC_TEST_LINK_LIBRARIES for the directory.")
    endif ()

    set(target "test-${name}")
    add_executable(${target} "${name}.cpp")
    target_link_libraries(${target} PRIVATE ${ARG_LINK_LIBRARIES})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SC_TEST_INCLUDE_DIR})

    add_test(NAME ${target} COMMAND ${target})
    set_tests_properties(${target} PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
            TIMEOUT ${ARG_TIMEOUT}
            LABELS "${ARG_LABELS}")
endfunction()
# add_sc_example(<name> [SOURCE <file>] [LINK_LIBRARIES ...])
# Build examples/<name>.cpp against the module's shared library and install it with the
# runtime package, so `apt install <package>` alone is enough to run it. Name examples
# sc-<module>-<what> since they land in bin/. A demo is for people trying the module, so it
# prints what it does rather than checking anything, and is not registered with CTest.
# README.md code blocks marked with <!-- sc-example: <file> --> are refreshed from the source.
function(add_sc_example name)
    set(one_value_args SOURCE TIMEOUT)
    set(multi_value_args LINK_LIBRARIES ARGS)
    cmake_parse_arguments(ARG "RUN_AS_TEST" "${one_value_args}" "${multi_value_args}" ${ARGN})
    if (ARG_RUN_AS_TEST OR ARG_TIMEOUT OR ARG_ARGS)
        message(WARNING "add_sc_example(${name}): demos are not tests, so RUN_AS_TEST, ARGS and TIMEOUT"
                " are ignored; drop them")
    endif ()

    sc_module_name(module)
    if (NOT name MATCHES "^${module}-")
        message(WARNING "add_sc_example(${name}): examples install to bin/, name it ${module}-<what>")
    endif ()
    if (NOT ARG_SOURCE)
        set(ARG_SOURCE "examples/${name}.cpp")
    endif ()
    if (NOT ARG_LINK_LIBRARIES)
        set(ARG_LINK_LIBRARIES sc::${module}-shared)
    endif ()

    sc_sync_readme_later()
    if (NOT SC_BUILD_EXAMPLES)
        return()
    endif ()

    add_executable(${name} "${ARG_SOURCE}")
    target_link_libraries(${name} PRIVATE ${ARG_LINK_LIBRARIES})
    install(TARGETS ${name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime)
endfunction()

# Sync README.md once, after the whole CMakeLists.txt has been read.
function(sc_sync_readme_later)
    get_property(scheduled DIRECTORY PROPERTY SC_README_SYNC_SCHEDULED)
    if (NOT scheduled)
        set_property(DIRECTORY PROPERTY SC_README_SYNC_SCHEDULED TRUE)
        cmake_language(DEFER CALL sc_sync_readme)
    endif ()
endfunction()

# sc_sync_readme([<readme>])
# Replace each block between
#   <!-- sc-example: <file> -->  and  <!-- /sc-example -->
# with a cpp code block holding <file> (relative to the module root). When the file has
# `// [readme]` and `// [/readme]` lines, only the lines between them are used.
function(sc_sync_readme)
    set(readme "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    if (ARGC GREATER 0)
        set(readme "${ARGV0}")
    endif ()
    if (NOT EXISTS "${readme}")
        return()
    endif ()

    set(open_marker "<!-- sc-example:")
    set(close_marker "<!-- /sc-example -->")
    string(LENGTH "${close_marker}" close_length)
    set(fence "```")

    file(READ "${readme}" original)
    set(rest "${original}")
    set(result "")
    while (TRUE)
        string(FIND "${rest}" "${open_marker}" start)
        if (start EQUAL -1)
            string(APPEND result "${rest}")
            break()
        endif ()
        string(SUBSTRING "${rest}" 0 ${start} before)
        string(APPEND result "${before}")
        string(SUBSTRING "${rest}" ${start} -1 rest)

        string(FIND "${rest}" "-->" marker_end)
        string(FIND "${rest}" "${close_marker}" block_end)
        if (marker_end EQUAL -1 OR block_end EQUAL -1)
            message(WARNING "${readme}: unterminated ${open_marker} block, left as is")
            string(APPEND result "${rest}")
            break()
        endif ()
        math(EXPR marker_end "${marker_end} + 3")
        string(SUBSTRING "${rest}" 0 ${marker_end} marker)
        string(REGEX REPLACE "^${open_marker} *([^ ]+) *-->$" "\\1" source "${marker}")

        sc_readme_snippet("${CMAKE_CURRENT_SOURCE_DIR}/${source}" snippet)
        if (snippet STREQUAL "")
            message(WARNING "${readme}: ${source} not found or empty, block left as is")
            math(EXPR keep "${block_end} + ${close_length}")
            string(SUBSTRING "${rest}" 0 ${keep} unchanged)
            string(APPEND result "${unchanged}")
        else ()
            string(APPEND result "${marker}\n${fence}cpp\n${snippet}\n${fence}\n${close_marker}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif ()
        math(EXPR after "${block_end} + ${close_length}")
        string(SUBSTRING "${rest}" ${after} -1 rest)
    endwhile ()

    if (NOT result STREQUAL original)
        file(WRITE "${readme}" "${result}")
        message(STATUS "Updated examples in ${readme}")
    endif ()
endfunction()

# The README part of a source file, dedented, without trailing newlines.
function(sc_readme_snippet file output)
    set(${output} "" PARENT_SCOPE)
    if (NOT EXISTS "${file}")
        return()
    endif ()
    file(READ "${file}" text)

    # Keep list separators and brackets out of the regex/list handling below.
    string(ASCII 1 semicolon)
    string(ASCII 2 open_bracket)
    string(ASCII 3 close_bracket)
    string(REPLACE ";" "${semicolon}" text "${text}")
    string(REPLACE "[" "${open_bracket}" text "${text}")
    string(REPLACE "]" "${close_bracket}" text "${text}")
    string(REPLACE "\r\n" "\n" text "${text}")

    set(begin_marker "// ${open_bracket}readme${close_bracket}")
    set(end_marker "// ${open_bracket}/readme${close_bracket}")
    string(FIND "${text}" "${begin_marker}" begin)
    string(FIND "${text}" "${end_marker}" end)
    if (NOT begin EQUAL -1 AND end GREATER begin)
        string(SUBSTRING "${text}" ${begin} -1 text)
        string(FIND "${text}" "\n" first_line_end)
        string(FIND "${text}" "${end_marker}" end)
        math(EXPR length "${end} - ${first_line_end} - 1")
        math(EXPR first_line_end "${first_line_end} + 1")
        string(SUBSTRING "${text}" ${first_line_end} ${length} text)
    endif ()

    # Drop the indentation shared by every non-blank line.
    string(REGEX REPLACE "\n[ \t]+\n" "\n\n" text "\n${text}")
    string(REGEX MATCHALL "\n *[^ \n]" indents "${text}")
    set(indent -1)
    foreach (line_start ${indents})
        string(LENGTH "${line_start}" width)
        math(EXPR width "${width} - 2")
        if (indent EQUAL -1 OR width LESS indent)
            set(indent ${width})
        endif ()
    endforeach ()
    if (indent GREATER 0)
        string(REPEAT " " ${indent} prefix)
        string(REPLACE "\n${prefix}" "\n" text "${text}")
    endif ()
    string(REGEX REPLACE "^\n+" "" text "${text}")
    string(REGEX REPLACE "[ \t\n]+$" "" text "${text}")

    string(REPLACE "${semicolon}" ";" text "${text}")
    string(REPLACE "${open_bracket}" "[" text "${text}")
    string(REPLACE "${close_bracket}" "]" text "${text}")
    set(${output} "${text}" PARENT_SCOPE)
endfunction()

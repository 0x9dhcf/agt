cmake_minimum_required(VERSION 3.21)
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
project(agt VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Options (default ON when this is the top-level project)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(_agt_toplevel ON)
else()
  set(_agt_toplevel OFF)
endif()
option(AGT_BUILD_TESTS "Build tests" ${_agt_toplevel})
option(AGT_BUILD_EXAMPLES "Build examples" ${_agt_toplevel})

# --- CPM dependencies ---
include(cmake/CPM.cmake)

CPMAddPackage(
  NAME nlohmann_json
  VERSION 3.11.3
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
set(nlohmann_json_VERSION 3.11.3)
CPMAddPackage(
  NAME nlohmann_json_schema_validator
  VERSION 2.4.0
  URL https://github.com/pboettch/json-schema-validator/archive/refs/tags/2.4.0.tar.gz
  OPTIONS
    "JSON_VALIDATOR_INSTALL OFF"
    "JSON_VALIDATOR_BUILD_TESTS OFF"
    "JSON_VALIDATOR_BUILD_EXAMPLES OFF"
)

# libpqxx — modern C++ Postgres client. BSD-3-Clause.
CPMAddPackage(
  NAME libpqxx
  GITHUB_REPOSITORY jtv/libpqxx
  GIT_TAG 7.10.0
  OPTIONS
    "BUILD_DOC OFF"
    "BUILD_TEST OFF"
    "SKIP_BUILD_TEST ON"
)

# --- System dependencies ---
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)

# --- Generated version header ---
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/agt/version.hpp
  @ONLY
)

# --- Sources ---
file(GLOB AGT_SOURCES CONFIGURE_DEPENDS src/*.cpp)

# --- Warning flags ---
set(AGT_WARNINGS
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Wdouble-promotion -Wformat=2 -Wnull-dereference
)

# --- Debug sanitizers (apply to all targets in this directory tree) ---
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- Release LTO ---
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()

# --- Library targets ---
foreach(_type STATIC SHARED)
  if(_type STREQUAL "STATIC")
    set(_target agt_static)
  else()
    set(_target agt_shared)
  endif()

  add_library(${_target} ${_type} ${AGT_SOURCES})
  set_target_properties(${_target} PROPERTIES OUTPUT_NAME agt)

  if(_type STREQUAL "SHARED")
    set_target_properties(${_target} PROPERTIES
      VERSION   ${PROJECT_VERSION}
      SOVERSION ${PROJECT_VERSION_MAJOR}
    )
  endif()

  target_include_directories(${_target}
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_options(${_target} PRIVATE ${AGT_WARNINGS})

  target_link_libraries(${_target}
    PUBLIC  nlohmann_json::nlohmann_json
    PRIVATE nlohmann_json_schema_validator CURL::libcurl SQLite::SQLite3
            libpqxx::pqxx
  )

  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(${_target} PRIVATE -march=native)
  endif()
endforeach()

# --- Alias ---
add_library(agt::agt ALIAS agt_static)

# --- Install ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install nlohmann_json alongside (header-only, provides its own export)
if(TARGET nlohmann_json AND NOT nlohmann_json_FOUND)
  install(TARGETS nlohmann_json EXPORT agtTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  )
endif()

# Install json-schema-validator alongside (private dep, needed at link time)
if(TARGET nlohmann_json_schema_validator AND NOT nlohmann_json_schema_validator_FOUND)
  set_target_properties(nlohmann_json_schema_validator PROPERTIES PUBLIC_HEADER "")
  install(TARGETS nlohmann_json_schema_validator EXPORT agtTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )
endif()

install(TARGETS agt_static agt_shared
  EXPORT agtTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/agt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/agt/version.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/agt)

install(EXPORT agtTargets
  NAMESPACE agt::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agt
)

configure_package_config_file(
  cmake/agtConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/agtConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agt
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/agtConfigVersion.cmake"
  COMPATIBILITY SameMajorVersion
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/agtConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/agtConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/agt
)

# --- Subdirectories ---
if(AGT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(AGT_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

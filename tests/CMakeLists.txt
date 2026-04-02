CPMAddPackage(
  NAME doctest
  VERSION 2.4.11
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
)

# --- test_public ---
add_executable(test_public
  test_main.cpp
  test_session.cpp
  test_sqlite_session.cpp
  test_provider_utils.cpp
)
target_link_libraries(test_public PRIVATE agt_static doctest::doctest)
add_test(NAME test_public COMMAND test_public)

# --- test_internals ---
# Includes .cpp files directly; does NOT link agt.
# session.cpp is needed transitively via runner.cpp.
add_executable(test_internals
  test_main.cpp
  test_openai.cpp
  test_anthropic.cpp
  test_gemini.cpp
  test_mistral.cpp
  test_runner.cpp
  ${PROJECT_SOURCE_DIR}/src/session.cpp
)
target_include_directories(test_internals PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/include
)
target_link_libraries(test_internals PRIVATE
  doctest::doctest
  nlohmann_json::nlohmann_json
  nlohmann_json_schema_validator
  CURL::libcurl
  SQLite::SQLite3
)
add_test(NAME test_internals COMMAND test_internals)

# --- test_integration ---
add_executable(test_integration
  test_main.cpp
  test_integration.cpp
)
target_link_libraries(test_integration PRIVATE agt_static doctest::doctest)
add_test(NAME test_integration COMMAND test_integration)
set_tests_properties(test_integration PROPERTIES LABELS "integration")

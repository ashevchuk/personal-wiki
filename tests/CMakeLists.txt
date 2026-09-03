find_package(Catch2 3 CONFIG REQUIRED)

add_executable(unit_tests
  unit/PathGuardTest.cpp
)
target_link_libraries(unit_tests PRIVATE wikicore Catch2::Catch2WithMain)

add_test(NAME unit_tests COMMAND unit_tests)

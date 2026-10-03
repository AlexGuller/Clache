file(GLOB CLACHE_TEST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/golden/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/property/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cpp")

add_executable(clache_tests ${CLACHE_TEST_SOURCES})
target_include_directories(clache_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clache_tests PRIVATE clache::core GTest::gmock GTest::gtest_main)
if(TARGET clache::sim)
  target_link_libraries(clache_tests PRIVATE clache::sim)
endif()
clache_common_target_settings(clache_tests)

# Every public header must compile on its own: generate one translation unit
# per header that includes nothing else first.
file(GLOB CLACHE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/include/clache/*.h")
set(CLACHE_HEADER_CHECK_SOURCES "")
foreach(header IN LISTS CLACHE_PUBLIC_HEADERS)
  get_filename_component(header_name "${header}" NAME)
  get_filename_component(header_stem "${header}" NAME_WE)
  set(check_source "${CMAKE_CURRENT_BINARY_DIR}/header_check/${header_stem}.cpp")
  file(CONFIGURE OUTPUT "${check_source}"
    CONTENT "#include \"clache/${header_name}\"  // must be first: self-containment check\n")
  list(APPEND CLACHE_HEADER_CHECK_SOURCES "${check_source}")
endforeach()
add_library(clache_header_check OBJECT ${CLACHE_HEADER_CHECK_SOURCES})
target_link_libraries(clache_header_check PRIVATE clache::core)
clache_common_target_settings(clache_header_check)
add_dependencies(clache_tests clache_header_check)

include(GoogleTest)
gtest_discover_tests(clache_tests DISCOVERY_MODE PRE_TEST)

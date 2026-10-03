file(GLOB CLACHE_MICRO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/micro/*.cpp")

if(NOT CLACHE_MICRO_SOURCES)
  return()
endif()

add_executable(clache_micro ${CLACHE_MICRO_SOURCES})
target_include_directories(clache_micro PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(clache_micro PRIVATE clache::core benchmark::benchmark_main)
clache_common_target_settings(clache_micro)

# Every apps/<name>.cpp becomes an executable called <name>
# (clache_sim, clache_tracegen, clache_bench, ...). Adding an app never
# requires editing this file.

file(GLOB CLACHE_APP_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(app_source IN LISTS CLACHE_APP_SOURCES)
  get_filename_component(app_name "${app_source}" NAME_WE)
  add_executable(${app_name} "${app_source}")
  target_link_libraries(${app_name} PRIVATE clache::core)
  if(TARGET clache::sim)
    target_link_libraries(${app_name} PRIVATE clache::sim)
  endif()
  clache_common_target_settings(${app_name})
endforeach()

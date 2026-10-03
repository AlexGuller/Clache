# Sources are globbed per directory (CONFIGURE_DEPENDS re-globs on every build)
# so that parallel branches adding new .cpp files never touch this file.

file(GLOB CLACHE_CORE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/estimators/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/index/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/policy/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/storage/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/telemetry/*.cpp")

if(NOT CLACHE_WITH_REDIS)
  list(FILTER CLACHE_CORE_SOURCES EXCLUDE REGEX ".*/storage/redis_adapter\\.cpp$")
endif()

add_library(clache_core STATIC ${CLACHE_CORE_SOURCES})
target_include_directories(clache_core
  PUBLIC  "${PROJECT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(clache_core PRIVATE clache_xxhash)
target_compile_features(clache_core PUBLIC cxx_std_20)
clache_common_target_settings(clache_core)

# Policies self-register through static initialisers (CLACHE_REGISTER_POLICY).
# A plain static library would let the linker drop those unreferenced objects,
# so every consumer links the archive whole via the clache::core target.
add_library(clache_core_whole INTERFACE)
target_link_libraries(clache_core_whole INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,clache_core>")
add_library(clache::core ALIAS clache_core_whole)

# Simulator library (Phase 1, feat/simulator). Created only once sources exist.
file(GLOB CLACHE_SIM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sim/*.cpp")
if(CLACHE_SIM_SOURCES)
  add_library(clache_sim_lib STATIC ${CLACHE_SIM_SOURCES})
  target_include_directories(clache_sim_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(clache_sim_lib PUBLIC clache::core)
  clache_common_target_settings(clache_sim_lib)
  add_library(clache::sim ALIAS clache_sim_lib)
endif()

# Applies CLACHE_SANITIZE to every target in the build (including dependencies,
# so that instrumented and uninstrumented code are never mixed).

if(CLACHE_SANITIZE STREQUAL "")
  return()
endif()

set(_clache_valid_sanitizers "address" "undefined" "address,undefined" "thread")
if(NOT CLACHE_SANITIZE IN_LIST _clache_valid_sanitizers)
  message(FATAL_ERROR
    "CLACHE_SANITIZE='${CLACHE_SANITIZE}' is not one of: ${_clache_valid_sanitizers}")
endif()

message(STATUS "Clache: building with -fsanitize=${CLACHE_SANITIZE}")
add_compile_options(-fsanitize=${CLACHE_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
add_link_options(-fsanitize=${CLACHE_SANITIZE})

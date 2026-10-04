# clache_set_warnings(<target>)
# Applies the project warning set to a first-party target. Never applied to
# third-party dependencies.

function(clache_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(CLACHE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    return()
  endif()

  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wimplicit-fallthrough)
  if(CLACHE_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# clache_common_target_settings(<target>)
# Warnings plus project-wide compile definitions.
function(clache_common_target_settings target)
  clache_set_warnings(${target})
  target_compile_definitions(${target} PRIVATE
    CLACHE_BUILD_VERSION="${PROJECT_VERSION}"
    CLACHE_BUILD_GIT_SHA="${CLACHE_SOURCE_REVISION}"
    CLACHE_GIT_SHA="${CLACHE_SOURCE_REVISION}")
  # Invariant checks: on when requested explicitly, and always in Debug.
  target_compile_definitions(${target} PRIVATE
    $<$<OR:$<BOOL:${CLACHE_CHECK_INVARIANTS}>,$<CONFIG:Debug>>:CLACHE_CHECK_INVARIANTS=1>)
endfunction()

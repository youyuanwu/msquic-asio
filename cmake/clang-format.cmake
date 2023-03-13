file(GLOB_RECURSE ALL_SOURCE_FILES 
    examples/*.cpp
    examples/*.hpp
    include/*.cpp
    include/*.hpp
    src/*.cpp
    src/*.hpp
    tests/*.cpp
    tests/*.hpp
)

add_custom_target(
  clangformat
  COMMAND clang-format
  -i
  ${ALL_SOURCE_FILES}
)

add_custom_target(
  clangformat-check 
  COMMAND clang-format
  --dry-run --Werror
  ${ALL_SOURCE_FILES}
)
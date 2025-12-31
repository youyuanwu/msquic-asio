cmake_minimum_required(VERSION 3.16)

set(msquic_asio_MAIN_PROJECT OFF)
if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(msquic_asio_MAIN_PROJECT ON)
endif()

if(msquic_asio_MAIN_PROJECT AND NOT MSQUIC_ASIO_CI_FORMAT)
    # configure vcpkg
    # we use CmakePresets.json to point to vcpkg
    if ("$ENV{VCPKG_ROOT}" STREQUAL "")
    message(FATAL_ERROR "VCPKG_ROOT not found")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(msquic-asio)

include(cmake/clang-format.cmake)

find_package(msquic CONFIG REQUIRED)

# compile the example
add_executable(quicsample
  example/sample.c
  example/inline.c
)

# copy dll into the exe dir so that exe can run
if(WIN32)
target_link_libraries(quicsample
  PUBLIC msquic
)
else()
target_link_libraries(quicsample
  PUBLIC msquic
)
endif()

# Get cert hash Get-ChildItem Cert:\CurrentUser\My
# .\build\Debug\quicsample.exe -server -cert_hash:
# .\build\Debug\quicsample.exe -client -unsecure -target:localhost

# linux:
# LD_LIBRARY_PATH=./build/_deps/msquic_release-src/bin ./build/quicsample

set(Boost_USE_STATIC_LIBS ON) # use static boost
set(Boost_NO_WARN_NEW_VERSIONS ON)
find_package(Boost REQUIRED headers)
find_package(ut CONFIG REQUIRED)

# third party oneshot asio lib
message(STATUS "fetching ashtum_oneshot")
include(FetchContent)
FetchContent_Declare(ashtum_oneshot
    GIT_REPOSITORY https://github.com/youyuanwu/oneshot
    GIT_TAG bf93793ad0b480599d32209c39e47f2dbb5abbe5
)
FetchContent_GetProperties(ashtum_oneshot)
if(NOT ashtum_oneshot_POPULATED)
  FetchContent_Populate(ashtum_oneshot)
  # do not add to tree
endif()

# import oneshot lib
add_library(asio_oneshot INTERFACE)
target_include_directories(asio_oneshot INTERFACE ${ashtum_oneshot_SOURCE_DIR}/include)


# msquic-asio lib
file(GLOB_RECURSE MSQUIC_ASIO_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp)

if(WIN32)
  add_library(msquic-asio INTERFACE ${MSQUIC_ASIO_SOURCES})
  # good practice
  target_compile_options(msquic-asio
  INTERFACE /W4 /WX
  )
else()
  #linux does not like sources for interface lib
  add_library(msquic-asio INTERFACE)
endif()
set_property(TARGET msquic-asio PROPERTY CXX_STANDARD 20)

target_include_directories(msquic-asio INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# header only
target_compile_definitions(msquic-asio
    INTERFACE _WIN32_WINNT=0x0601
)

# currently winasio uses boost log for logging
target_link_libraries(msquic-asio
    INTERFACE Boost::disable_autolinking Boost::headers asio_oneshot
    INTERFACE msquic
)

# only support win32 tests
if(WIN32)
  enable_testing()
  add_subdirectory(tests)
endif(WIN32)
# coverage
find_program(OpenCppCoverage_exe
        NAMES OpenCppCoverage.exe
)
if(OpenCppCoverage_exe)
    message(STATUS "coverage tool found: ${OpenCppCoverage_exe}")
    # coverage tool only recognizes windows style path, backslash.
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" PWD_WIN_PATH)
    add_custom_target(coverage
        COMMAND ${OpenCppCoverage_exe} --quiet --export_type cobertura:cobertura.xml --cover_children 
        --sources "${PWD_WIN_PATH}\\src" --sources "${PWD_WIN_PATH}\\include" --modules "${PWD_WIN_PATH}"
        -- ctest -C Debug --test-dir build --repeat until-pass:3 --timeout 30
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
else()
    message(STATUS "coverage tool not found: ${OpenCppCoverage_exe}")
endif()
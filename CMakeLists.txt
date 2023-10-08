cmake_minimum_required(VERSION 3.16)

project(msquic-asio)

include(cmake/clang-format.cmake)

if(WIN32)
  set(release_url "https://github.com/microsoft/msquic/releases/download/v2.2.2/msquic_windows_x64_Release_schannel.zip")
else()
  # linux
  set(release_url "https://github.com/microsoft/msquic/releases/download/v2.2.2/msquic_linux_x64_Release_openssl.zip")
  find_package(OpenSSL REQUIRED)
  # openssl needs version 1.1. ubuntu 20.04 has it, 22.04 is too new.
endif(WIN32)

message(STATUS "fetching msquic_release " ${release_url})
include(FetchContent)
FetchContent_Declare(
  msquic_release
  DOWNLOAD_EXTRACT_TIMESTAMP true
  URL ${release_url}
)

FetchContent_GetProperties(msquic_release)
if(NOT msquic_release_POPULATED)
  FetchContent_Populate(msquic_release)
endif()

# import msquic as a cmake lib
add_library(msquic SHARED IMPORTED GLOBAL)

if(WIN32)
set(_msquic_dll ${msquic_release_SOURCE_DIR}/bin/msquic.dll)
set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION ${_msquic_dll}
    IMPORTED_IMPLIB ${msquic_release_SOURCE_DIR}/lib/msquic.lib
)
else()
# glob msquic lib
file(GLOB _msquic_dll 
  ${msquic_release_SOURCE_DIR}/bin/libmsquic.so.*
)
message(STATUS "Found msquic lib: ${_msquic_dll}")
set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION ${_msquic_dll}
)
endif(WIN32)

target_include_directories(msquic
  INTERFACE ${msquic_release_SOURCE_DIR}/include
)

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
add_custom_command(TARGET quicsample POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -E copy_if_different ${_msquic_dll} ${CMAKE_CURRENT_BINARY_DIR}/Debug
)
else()
target_link_libraries(quicsample
  PUBLIC msquic
  OpenSSL::Crypto
)
add_custom_command(TARGET quicsample POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -E copy_if_different ${_msquic_dll} ${CMAKE_CURRENT_BINARY_DIR}/libmsquic.so.2
)
endif()

# Get cert hash Get-ChildItem Cert:\CurrentUser\My
# .\build\Debug\quicsample.exe -server -cert_hash:
# .\build\Debug\quicsample.exe -client -unsecure -target:localhost

# linux:
# LD_LIBRARY_PATH=./build/_deps/msquic_release-src/bin ./build/quicsample

# msquic-asio lib
set(Boost_USE_STATIC_LIBS ON) # use static boost
find_package(Boost REQUIRED)

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

target_include_directories(msquic-asio INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# header only
target_compile_definitions(msquic-asio
    INTERFACE _WIN32_WINNT=0x0601
)

# currently winasio uses boost log for logging
target_link_libraries(msquic-asio
    INTERFACE Boost::disable_autolinking Boost::headers
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
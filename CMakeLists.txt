cmake_minimum_required(VERSION 3.16)

project(msquic-asio)

if(WIN32)
  set(release_url "https://github.com/microsoft/msquic/releases/download/v2.1.7/msquic_windows_x64_Release_schannel.zip")
else()
  # linux
  set(release_url "https://github.com/microsoft/msquic/releases/download/v2.1.7/msquic_linux_x64_Release_openssl.zip")
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

# download msquic cpp headers
set(_h_files
    msquic.hpp
    msquicp.h # needed by hpp header
)

foreach(_h_file ${_h_files})
    set(_h_file_out_path ${CMAKE_CURRENT_BINARY_DIR}/inc/${_h_file})
    if(NOT EXISTS ${_h_file_out_path})
        message(STATUS "downloading ${_h_file}")
        # notice the tag matches the nuget version
        file(DOWNLOAD
            https://raw.githubusercontent.com/microsoft/msquic/v2.1.7/src/inc/${_h_file}
            ${_h_file_out_path}
        )
    endif()
endforeach()

# import msquic as a cmake lib
add_library(msquic SHARED IMPORTED GLOBAL)

if(WIN32)
set(_msquic_dll ${msquic_release_SOURCE_DIR}/bin/msquic.dll)
set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION ${_msquic_dll}
    IMPORTED_IMPLIB ${msquic_release_SOURCE_DIR}/lib/msquic.lib
)
else()
set(_msquic_dll ${msquic_release_SOURCE_DIR}/bin/libmsquic.so.2.1.7)
set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION ${_msquic_dll}
)
endif(WIN32)

target_include_directories(msquic
  INTERFACE ${msquic_release_SOURCE_DIR}/include
  INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/inc
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
)

enable_testing()
add_subdirectory(tests)
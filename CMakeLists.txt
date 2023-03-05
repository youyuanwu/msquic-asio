cmake_minimum_required(VERSION 3.14)

project(msquic-asio)

# find nuget
find_program(nuget_exe
  NAMES nuget
  REQUIRED
)

# download msquic nuget
add_custom_target(nuget_install_msquic
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  COMMAND ${nuget_exe} install Microsoft.Native.Quic.MsQuic.Schannel -Version 2.1.7 -DirectDownload -OutputDirectory ./nuget
)

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

set(_msquic_dll ${CMAKE_CURRENT_BINARY_DIR}/nuget/Microsoft.Native.Quic.MsQuic.Schannel.2.1.7/build/native/bin/x64/msquic.dll)

set_target_properties(msquic PROPERTIES
    IMPORTED_LOCATION ${_msquic_dll}
    IMPORTED_IMPLIB ${CMAKE_CURRENT_BINARY_DIR}/nuget/Microsoft.Native.Quic.MsQuic.Schannel.2.1.7/build/native/lib/x64/msquic.lib
)

target_include_directories(msquic
  INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/nuget/Microsoft.Native.Quic.MsQuic.Schannel.2.1.7/build/native/include
  INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/inc
)

# compile the example
add_executable(quicsample
  example/sample.c
)

target_link_libraries(quicsample
  PUBLIC msquic
)

# copy dll into the exe dir so that exe can run
add_custom_command(TARGET quicsample POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -E copy_if_different ${_msquic_dll} ${CMAKE_CURRENT_BINARY_DIR}/Debug
)
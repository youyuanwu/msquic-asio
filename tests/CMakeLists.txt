# New-SelfSignedCertificate -DnsName $env:computername,localhost -FriendlyName MsQuic-Test -KeyUsageProperty Sign -KeyUsage DigitalSignature -CertStoreLocation cert:\CurrentUser\My -HashAlgorithm SHA256 -Provider "Microsoft Software Key Storage Provider" -KeyExportPolicy Exportable


# get the test certificate
execute_process(COMMAND powershell.exe -Command "Get-ChildItem Cert:\\CurrentUser\\My | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Select-Object -ExpandProperty Thumbprint -First 1"
  OUTPUT_VARIABLE TEST_CERT_HASH
)

if(TEST_CERT_HASH STREQUAL "")
  message(Status "Generating TEST_CERT_HASH")
  execute_process(
    COMMAND powershell.exe -Command "New-SelfSignedCertificate -DnsName $env:computername,localhost -FriendlyName MsQuic-Test -KeyUsageProperty Sign -KeyUsage DigitalSignature -CertStoreLocation cert:\\CurrentUser\\My -HashAlgorithm SHA256 -Provider \"Microsoft Software Key Storage Provider\" -KeyExportPolicy Exportable"
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(COMMAND powershell.exe -Command "Get-ChildItem Cert:\\CurrentUser\\My | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Select-Object -ExpandProperty Thumbprint -First 1"
    OUTPUT_VARIABLE TEST_CERT_HASH
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()
  
message(STATUS "Using TEST_CERT_HASH ${TEST_CERT_HASH}")

# remove: Get-ChildItem Cert:\CurrentUser\My\ | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Remove-Item


set(Boost_USE_STATIC_LIBS ON) # use static boost
find_package(Boost REQUIRED COMPONENTS unit_test_framework )

# pthread
find_package(Threads REQUIRED)

file(GLOB SOURCES
*_test.cpp
*.hpp
)

set(test_name msquic_test)

add_executable(${test_name} ${SOURCES})
target_include_directories(${test_name} 
  PRIVATE .
)

target_compile_definitions(${test_name} 
  PRIVATE
  TEST_CERT_HASH=${TEST_CERT_HASH}
)


target_link_libraries(${test_name} PRIVATE Threads::Threads Boost::unit_test_framework Boost::disable_autolinking msquic-asio)
set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 20)

add_custom_command(TARGET ${test_name} POST_BUILD
    COMMAND ${CMAKE_COMMAND} 
        -E copy_if_different ${_msquic_dll} ${CMAKE_CURRENT_BINARY_DIR}/$<$<CONFIG:Debug>:Debug>$<$<CONFIG:Release>:Release>
)

add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
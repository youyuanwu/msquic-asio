# New-SelfSignedCertificate -DnsName $env:computername,localhost -FriendlyName MsQuic-Test -KeyUsageProperty Sign -KeyUsage DigitalSignature -CertStoreLocation cert:\CurrentUser\My -HashAlgorithm SHA256 -Provider "Microsoft Software Key Storage Provider" -KeyExportPolicy Exportable
find_program(pwsh_exe
  NAMES pwsh.exe
  REQUIRED
)

# get the test certificate
execute_process(COMMAND pwsh.exe -Command "Get-ChildItem Cert:\\CurrentUser\\My | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Select-Object -ExpandProperty Thumbprint -First 1"
  OUTPUT_VARIABLE TEST_CERT_HASH
)

if(TEST_CERT_HASH STREQUAL "")
  message(Status "Generating TEST_CERT_HASH")
  execute_process(
    COMMAND pwsh.exe -Command "New-SelfSignedCertificate -DnsName $env:computername,localhost -FriendlyName MsQuic-Test -KeyUsageProperty Sign -KeyUsage DigitalSignature -CertStoreLocation cert:\\CurrentUser\\My -HashAlgorithm SHA256 -Provider \"Microsoft Software Key Storage Provider\" -KeyExportPolicy Exportable"
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(COMMAND pwsh.exe -Command "Get-ChildItem Cert:\\CurrentUser\\My | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Select-Object -ExpandProperty Thumbprint -First 1"
    OUTPUT_VARIABLE TEST_CERT_HASH
    COMMAND_ERROR_IS_FATAL ANY
  )
endif()
  
message(STATUS "Using TEST_CERT_HASH ${TEST_CERT_HASH}")

# remove: Get-ChildItem Cert:\CurrentUser\My\ | Where-Object -Property FriendlyName -EQ -Value MsQuic-Test | Remove-Item

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


target_link_libraries(${test_name} PRIVATE Threads::Threads Boost::ut msquic-asio)
set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 20)

add_test(NAME ${test_name} 
  COMMAND ${test_name} --log_level=all
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
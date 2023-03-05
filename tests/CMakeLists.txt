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


target_link_libraries(${test_name} PRIVATE Threads::Threads Boost::unit_test_framework Boost::disable_autolinking msquic-asio)
set_property(TARGET ${test_name} PROPERTY CXX_STANDARD 20) # for semaphore
add_test(NAME ${test_name} COMMAND ${test_name} WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
cmake_minimum_required(VERSION 3.16)
project(TCPMonitor)

set(CMAKE_CXX_STANDARD 17)

add_executable(gateway gateway.cpp)
target_link_libraries(gateway opendnp3 pthread)

add_executable(master master.cpp)
target_link_libraries(master opendnp3 pthread)

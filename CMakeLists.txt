cmake_minimum_required(VERSION 3.1.0)

project(SberTest C)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(SberTest main.c)
target_link_libraries(SberTest PRIVATE Threads::Threads rt)

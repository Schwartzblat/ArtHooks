
cmake_minimum_required(VERSION 3.22.1)

project("selftest")

# Demo-only. The library itself is built by the :arthooks module; this exists purely so the
# self-test has a real JNI method to hook.
add_library(${CMAKE_PROJECT_NAME} SHARED
        selftest.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        log)

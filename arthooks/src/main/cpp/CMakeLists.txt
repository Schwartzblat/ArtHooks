
cmake_minimum_required(VERSION 3.22.1)

project("arthooks")


add_library(${CMAKE_PROJECT_NAME} SHARED
        arthooks.cpp art_method.cpp art_symbols.cpp class_init.cpp deoptimize.cpp
        hook_registry.cpp trampoline.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
        android
        log)

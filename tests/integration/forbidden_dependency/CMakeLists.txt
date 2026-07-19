cmake_minimum_required(VERSION 3.16)

project(UGDRForbiddenDependency LANGUAGES CXX)

add_library(ugdr_worker STATIC
    src/worker.cpp
)

add_library(ugdr_test_support STATIC
    src/test_support.cpp
)

add_library(ugdr_api STATIC
    src/api.cpp
)
target_link_libraries(ugdr_api
    PRIVATE
        ugdr_worker
        ugdr_test_support
)

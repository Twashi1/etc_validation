cmake_minimum_required(VERSION 3.10)

project(etcvalidation C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DynamoRIO_DIR ${CMAKE_SOURCE_DIR}/ext/dynamorio/exports/cmake)
message(${DynamoRIO_DIR})

find_package(DynamoRIO REQUIRED)

if (NOT DynamoRIO_FOUND)
  message(FATAL_ERROR "DynamoRIO package required to build")
endif(NOT DynamoRIO_FOUND)

set(ETC_HEADERS)
set(ETC_SOURCE "client/tracer.cpp")

add_library(etcvalidation SHARED ${ETC_HEADERS} ${ETC_SOURCE})

configure_DynamoRIO_client(etcvalidation)
use_DynamoRIO_extension(etcvalidation drmgr)

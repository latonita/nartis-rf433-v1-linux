cmake_minimum_required(VERSION 3.16)
project(nartis_cmt2300a C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(NARTIS_BUILD_TOOLS "Build command-line tools" ON)
option(NARTIS_BUILD_TESTS "Build unit tests" ON)

find_package(OpenSSL REQUIRED)

add_library(nartis
  src/cmt2300a/cmt2300a.c
  src/cc1101/cc1101.c
  src/radio/radio_cmt2300a.c
  src/radio/radio_cc1101.c
  src/dlms/dlms.c
  src/dlms/obis.c
  src/platform/backend_sysfs.c
  src/platform/log.c
  src/rf/rf_crypto.c
  src/rf/rf_frame.c
  src/session/nartis_meter.c
  src/session/session_store.c
)

target_include_directories(nartis
  PUBLIC include
  PRIVATE src
)
target_link_libraries(nartis PUBLIC OpenSSL::Crypto)

if(NARTIS_BUILD_TOOLS)
  add_executable(nartisctl tools/nartisctl.c)
  target_link_libraries(nartisctl PRIVATE nartis)

  add_executable(nartis_session_gen tools/nartis_session_gen.c)
  target_include_directories(nartis_session_gen PRIVATE src)
  target_link_libraries(nartis_session_gen PRIVATE nartis)
endif()

if(NARTIS_BUILD_TESTS)
  enable_testing()
  add_executable(test_protocol tests/test_protocol.c)
  target_include_directories(test_protocol PRIVATE src)
  target_link_libraries(test_protocol PRIVATE nartis)
  add_test(NAME protocol COMMAND test_protocol)
endif()

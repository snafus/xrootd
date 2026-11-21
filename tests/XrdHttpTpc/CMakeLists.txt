# The shared state of a pull in blocks names libcurl's result codes.
find_package(CURL REQUIRED)

add_executable(xrdhttptpc-unit-tests
  XrdHttpTpcTests.cc
  XrdHttpTpcPullOptionsTests.cc
  XrdHttpTpcBlockPullTests.cc)

add_library(XrdHttpTpcUtils
  ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcUtils.cc
  ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcPullOptions.cc
  ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcBlockPull.cc
)

target_link_libraries(XrdHttpTpcUtils
        PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto
        CURL::libcurl
        ${CMAKE_THREAD_LIBS_INIT})

target_link_libraries(xrdhttptpc-unit-tests
        XrdHttpTpcUtils
        GTest::gtest
        GTest::gtest_main)

gtest_discover_tests(xrdhttptpc-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

if(TARGET XrdServer)
  add_executable(xrdhttptpc-stream-unit-tests
          XrdHttpTpcStreamTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpc/XrdHttpTpcStream.cc)

  target_link_libraries(xrdhttptpc-stream-unit-tests
          XrdServer
          XrdUtils
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpc-stream-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)
endif()

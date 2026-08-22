find_package(CURL REQUIRED)

add_executable(xrdhttptpcr-unit-tests XrdHttpTpcRTests.cc)

add_library(XrdHttpTpcRUtils
  ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcUtils.cc
)

target_link_libraries(XrdHttpTpcRUtils
        PUBLIC
        OpenSSL::SSL
        OpenSSL::Crypto)

target_link_libraries(xrdhttptpcr-unit-tests
        XrdHttpTpcRUtils
        GTest::gtest
        GTest::gtest_main)

gtest_discover_tests(xrdhttptpcr-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

if(TARGET XrdServer)
  add_executable(xrdhttptpcr-stream-unit-tests
          XrdHttpTpcRStreamTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc)

  target_link_libraries(xrdhttptpcr-stream-unit-tests
          XrdServer
          XrdUtils
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpcr-stream-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

  # T-U2 State hygiene: needs the real State implementation plus its Stream and
  # header-parsing dependencies (XrdHttpUtils provides parseReprDigest).
  add_executable(xrdhttptpcr-state-unit-tests
          XrdHttpTpcRStateTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRState.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc)

  target_link_libraries(xrdhttptpcr-state-unit-tests
          XrdServer
          XrdUtils
          XrdHttpUtils
          CURL::libcurl
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpcr-state-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)
endif()

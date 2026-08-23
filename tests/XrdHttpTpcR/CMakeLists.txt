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

# T-U6 slab pool, T-U3 config, T-U9 scheduler FSM: pure TPCR code, no server
# libs needed.
add_executable(xrdhttptpcr-pool-unit-tests
        XrdHttpTpcRSlabPoolTests.cc
        XrdHttpTpcRConfigTests.cc
        XrdHttpTpcRSchedulerTests.cc
        ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSlabPool.cc
        ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRConfig.cc
        ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSchedulerCore.cc)

target_link_libraries(xrdhttptpcr-pool-unit-tests
        GTest::gtest
        GTest::gtest_main
        ${CMAKE_THREAD_LIBS_INIT})

gtest_discover_tests(xrdhttptpcr-pool-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

if(TARGET XrdServer)
  add_executable(xrdhttptpcr-stream-unit-tests
          XrdHttpTpcRStreamTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSlabPool.cc)

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
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSlabPool.cc)

  target_link_libraries(xrdhttptpcr-state-unit-tests
          XrdServer
          XrdUtils
          XrdHttpUtils
          CURL::libcurl
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpcr-state-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

  # T-U11 journal record: needs XrdOucCRC (XrdUtils) and the Stream/SlabPool
  # objects the journal translation unit references.
  add_executable(xrdhttptpcr-journal-unit-tests
          XrdHttpTpcRJournalTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRJournal.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSlabPool.cc)

  target_link_libraries(xrdhttptpcr-journal-unit-tests
          XrdServer
          XrdUtils
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpcr-journal-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

  # T-U12 digests: zlib serves as the independent reference implementation.
  find_package(ZLIB REQUIRED)
  add_executable(xrdhttptpcr-digest-unit-tests
          XrdHttpTpcRDigestTests.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRDigest.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRJournal.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRStream.cc
          ${PROJECT_SOURCE_DIR}/src/XrdHttpTpcR/XrdHttpTpcRSlabPool.cc)

  target_link_libraries(xrdhttptpcr-digest-unit-tests
          XrdServer
          XrdUtils
          ZLIB::ZLIB
          GTest::gtest
          GTest::gtest_main)

  gtest_discover_tests(xrdhttptpcr-digest-unit-tests PROPERTIES DISCOVERY_TIMEOUT 10)

  # (tpcr-journal-dump is built in src/XrdHttpTpcR/ -- shipped with the
  # handler, not gated on ENABLE_TESTS; the harnesses use it from bin/.)

  # Integration: real xrootd + TPCR handler against the fault-injectable
  # mock source (T-I1, T-I2, early T-I3/T-I4).  Skips (127) when python3,
  # curl, or the built artifacts are unavailable.
  add_test(NAME XrdHttpTpcR::integration
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/tpcr_test.sh
                   ${CMAKE_BINARY_DIR})
  set_tests_properties(XrdHttpTpcR::integration PROPERTIES
                       SKIP_RETURN_CODE 127
                       TIMEOUT 600
                       LABELS "integration")

  # WP-6 backend semantics matrix (M2 gate) and WP-7 crash matrix (T-I5).
  add_test(NAME XrdHttpTpcR::backend_matrix
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/backend_matrix.sh
                   ${CMAKE_BINARY_DIR})
  set_tests_properties(XrdHttpTpcR::backend_matrix PROPERTIES
                       SKIP_RETURN_CODE 127 TIMEOUT 300 LABELS "integration")
  add_test(NAME XrdHttpTpcR::crash_matrix
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/crash_matrix.sh
                   ${CMAKE_BINARY_DIR})
  set_tests_properties(XrdHttpTpcR::crash_matrix PROPERTIES
                       SKIP_RETURN_CODE 127 TIMEOUT 600 LABELS "integration")
  # WP-8 resume decision matrix (T-I6, T-I13 leg).
  add_test(NAME XrdHttpTpcR::resume_matrix
           COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/integration/resume_matrix.sh
                   ${CMAKE_BINARY_DIR})
  set_tests_properties(XrdHttpTpcR::resume_matrix PROPERTIES
                       SKIP_RETURN_CODE 127 TIMEOUT 900 LABELS "integration")
endif()

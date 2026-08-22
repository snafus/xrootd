unset(BUILD_TPCR CACHE)

if(NOT ENABLE_HTTP)
  return()
endif()

if(FORCE_ENABLED)
  find_package(CURL REQUIRED)
else()
  find_package(CURL)
endif()

if(CURL_FOUND)
  set(BUILD_TPCR TRUE CACHE INTERNAL "")
else()
  return()
endif()

set(XrdHttpTPCR XrdHttpTPCR-${PLUGIN_VERSION})

add_library(${XrdHttpTPCR} MODULE
  XrdHttpTpcRConfigure.cc
  XrdHttpTpcRMultistream.cc
  XrdHttpTpcPMarkManager.cc  XrdHttpTpcPMarkManager.hh
  XrdHttpTpcRState.cc        XrdHttpTpcRState.hh
  XrdHttpTpcRStream.cc       XrdHttpTpcRStream.hh
  XrdHttpTpcRTPC.cc          XrdHttpTpcRTPC.hh
  XrdHttpTpcUtils.cc         XrdHttpTpcUtils.hh
)

target_link_libraries(${XrdHttpTPCR}
  PRIVATE
    XrdServer
    XrdUtils
    XrdHttpUtils
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    ${CMAKE_THREAD_LIBS_INIT}
    ${CMAKE_DL_LIBS}
)

if(NOT APPLE)
  target_link_options(${XrdHttpTPCR} PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/export-lib-symbols")
endif()

install(TARGETS ${XrdHttpTPCR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

#undef NDEBUG

#include "XrdHttpTpc/XrdHttpTpcUtils.hh"
#include "XrdHttpTpc/XrdHttpTpcTPC.hh"

#include <exception>
#include <gtest/gtest.h>

#include <map>
#include <string>

using namespace testing;

class XrdHttpTpcTests : public Test {};

TEST(XrdHttpTpcTests, prepareOpenURLTest) {
  std::string resource = "/eos/test/file.txt";
  std::map<std::string,std::string> empty {};
  {
    // Nothing to set in the openURL
    std::map<std::string, std::string> headers {{"Test","Test"}};
    XrdHttpTpcUtils::PrepareOpenURLParams params{resource,headers, empty,empty};
    auto openURL = XrdHttpTpcUtils::prepareOpenURL(params);

    ASSERT_EQ(resource + "?" + TPC::TPCHandler::OSS_TASK_OPAQUE.data(),openURL);
  }

  {
    // If authz= was put in the opaque of the resource (and therefore put in the xrd-http-query header),
    // Then the authorization header should have been set and no "authz" should be found in the opaque of
    // the open URL
    std::map<std::string, std::string> headers {{"xrd-http-query","authz=test&scitag.flow=144"}};
    XrdHttpTpcUtils::PrepareOpenURLParams params{resource,headers, empty,empty};
    auto openURL = XrdHttpTpcUtils::prepareOpenURL(params);

    ASSERT_NE(resource,openURL);
    ASSERT_TRUE(headers.find("Authorization") != headers.end());
    ASSERT_TRUE(openURL.find("&authz") == std::string::npos);
    ASSERT_TRUE(openURL.find("&scitag.flow") != std::string::npos);
    ASSERT_TRUE(openURL.find("cks.type") == std::string::npos);
    ASSERT_TRUE(openURL.find(std::string("?") + TPC::TPCHandler::OSS_TASK_OPAQUE.data()) != std::string::npos);
  }

  {
    // If authz= was put in the opaque of the resource (and therefore put in the xrd-http-query header),
    // and if the the authorization header is provided, we should not override the provided authorization header
    std::map<std::string, std::string> headers {{"xrd-http-query","authz=test&scitag.flow=144"},{"Authorization","abcd"}};
    XrdHttpTpcUtils::PrepareOpenURLParams params{resource,headers, empty,empty};
    auto openURL = XrdHttpTpcUtils::prepareOpenURL(params);

    ASSERT_NE(resource,openURL);
    ASSERT_TRUE(openURL.find("&authz") == std::string::npos);
    ASSERT_TRUE(openURL.find("&scitag.flow") != std::string::npos);
    ASSERT_EQ("abcd",headers["Authorization"]);
  }

  {
    // Some hdr2cgi has been configured, we should find them in the opaque of the openURL
    std::map<std::string, std::string> headers {{"xrd-http-query","authz=test&test1=test2"},{"Scitag","144"},{"lowercase_header","test1"} };
    std::map<std::string, std::string> hdr2cgi {{"SciTag","scitag.flow"},{"LOWERCASE_HEADER","lowercase"}};
    std::map<std::string, std::string> reprDigest {{"adler32","adler32val"},{"sha256","sha256val"}};
    XrdHttpTpcUtils::PrepareOpenURLParams params{resource,headers,hdr2cgi,reprDigest};
    auto openURL = XrdHttpTpcUtils::prepareOpenURL(params);

    ASSERT_TRUE(openURL.find("&test1=test2") != std::string::npos);
    ASSERT_TRUE(openURL.find("&scitag.flow=144") != std::string::npos);
    ASSERT_TRUE(openURL.find("&lowercase=test1") != std::string::npos);
    ASSERT_TRUE(openURL.find("&cks.type=adler32&cks.value=adler32val") != std::string::npos);
    ASSERT_TRUE(openURL.find(std::string("?") + TPC::TPCHandler::OSS_TASK_OPAQUE.data()) != std::string::npos);
  }
}

TEST(XrdHttpTpcTests, parseContentRangeTest) {
  long long first = -1, last = -1;

  ASSERT_TRUE(XrdHttpTpcUtils::parseContentRange("bytes 0-99/1000", first, last));
  ASSERT_EQ(0, first);
  ASSERT_EQ(99, last);

  // As libcurl hands it over: the white space after the colon and the end of line
  ASSERT_TRUE(XrdHttpTpcUtils::parseContentRange(" bytes 16777216-33554431/50343993\r\n", first, last));
  ASSERT_EQ(16777216, first);
  ASSERT_EQ(33554431, last);

  // The total size may be unknown, the unit is case-insensitive
  ASSERT_TRUE(XrdHttpTpcUtils::parseContentRange("Bytes 5-5/*", first, last));
  ASSERT_EQ(5, first);
  ASSERT_EQ(5, last);

  // Offsets beyond 32 bits
  ASSERT_TRUE(XrdHttpTpcUtils::parseContentRange("bytes 107374182400-107390959615/214748364800", first, last));
  ASSERT_EQ(107374182400LL, first);
  ASSERT_EQ(107390959615LL, last);

  // Anything else is refused and leaves the output untouched
  first = -1; last = -1;
  for (const char * invalid : {"", "   ", "bytes", "bytes ", "0-99/1000", "items 0-99/1000",
                               "bytes */1000", "bytes 0-99", "bytes 0-99/", "bytes 0-/1000",
                               "bytes -99/1000", "bytes 99-0/1000", "bytes 0-1000/1000",
                               "bytes 0-99/1000 trailing", "bytes 0-99/10x", "bytes 0x10-99/1000",
                               "bytes 0-99999999999999999999/*"}) {
    ASSERT_FALSE(XrdHttpTpcUtils::parseContentRange(invalid, first, last)) << "'" << invalid << "'";
    ASSERT_EQ(-1, first);
    ASSERT_EQ(-1, last);
  }
}

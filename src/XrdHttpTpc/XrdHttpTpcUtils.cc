//------------------------------------------------------------------------------
// This file is part of XrdHttpTpcTPC
//
// Copyright (c) 2023 by European Organization for Nuclear Research (CERN)
// Author: Cedric Caffy <ccaffy@cern.ch>
// File Date: Oct 2023
//------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------
#include "XrdHttpTpcUtils.hh"
#include "XrdOuc/XrdOucTUtils.hh"
#include "XrdHttpTpc/XrdHttpTpcTPC.hh"

#include <sstream>
#include <strings.h>

std::string XrdHttpTpcUtils::prepareOpenURL(PrepareOpenURLParams & params) {
  auto iter = XrdOucTUtils::caseInsensitiveFind(params.reqHeaders,"xrd-http-query");
  std::stringstream opaque;

  // https://github.com/xrootd/xrootd/issues/2427 we tell the oss layer that this transfer is a HTTP TPC one
  opaque << "?" << TPC::TPCHandler::OSS_TASK_OPAQUE;

  if (iter != params.reqHeaders.end() && !iter->second.empty()) {
    std::string token;
    std::istringstream requestStream(iter->second);
    auto has_authz_header = XrdOucTUtils::caseInsensitiveFind(params.reqHeaders,"authorization") != params.reqHeaders.end();
    while (std::getline(requestStream, token, '&')) {
      if (token.empty()) {
        continue;
      } else if (!strncmp(token.c_str(), "authz=", 6)) {
        if (!has_authz_header) {
          params.reqHeaders["Authorization"] = token.substr(6);
          has_authz_header = true;
        }
      } else {
        opaque << "&" << token;
      }
    }
  }

  // Append CGI coming from the tpc.header2cgi parameter
  for(auto & hdr2cgi : params.hdr2cgimap) {
    auto it = std::find_if(params.reqHeaders.begin(),params.reqHeaders.end(),[&hdr2cgi](const auto & elt){
      return !strcasecmp(elt.first.c_str(),hdr2cgi.first.c_str());
    });
    if(it != params.reqHeaders.end()) {
      opaque << "&" << hdr2cgi.second << "=" << it->second;
    }
  }

  // Append CGI coming from repr-digest header
  if(!params.reprDigest.empty()) {
    // We take the first alphabetically ordered Repr-Digest
    const auto & [digestName,digestValue] = *params.reprDigest.begin();
    opaque << "&cks.type=" << digestName << "&cks.value=" << digestValue;
  }


  return params.reqResource + opaque.str();
}

namespace {
// Reads a non-negative decimal number at pos and moves pos past it.
bool readNumber(const std::string & str, size_t & pos, long long & number) {
  const size_t begin = pos;
  number = 0;
  // 18 digits always fit in a long long
  while (pos < str.size() && pos - begin < 18 && str[pos] >= '0' && str[pos] <= '9') {
    number = number * 10 + (str[pos++] - '0');
  }
  return pos != begin && (pos == str.size() || str[pos] < '0' || str[pos] > '9');
}
}

bool XrdHttpTpcUtils::parseContentRange(const std::string & value, long long & first, long long & last, long long * totalLength) {
  static const char blanks[] = " \t\r\n";
  const size_t begin = value.find_first_not_of(blanks);
  if (begin == std::string::npos) return false;
  const std::string range = value.substr(begin, value.find_last_not_of(blanks) - begin + 1);

  static const std::string unit = "bytes ";
  if (range.size() <= unit.size() || strncasecmp(range.c_str(), unit.c_str(), unit.size())) return false;

  size_t pos = unit.size();
  long long firstByte, lastByte, total = -1;
  if (!readNumber(range, pos, firstByte)) return false;
  if (pos == range.size() || range[pos++] != '-') return false;
  if (!readNumber(range, pos, lastByte)) return false;
  if (pos == range.size() || range[pos++] != '/') return false;
  if (range.compare(pos, std::string::npos, "*") != 0) {
    if (!readNumber(range, pos, total) || pos != range.size()) return false;
    if (lastByte >= total) return false;
  }
  if (lastByte < firstByte) return false;

  first = firstByte;
  last = lastByte;
  if (totalLength) *totalLength = total;
  return true;
}

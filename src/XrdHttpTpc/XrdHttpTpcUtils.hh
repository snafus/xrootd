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

#ifndef XROOTD_XRDTPCUTILS_HH
#define XROOTD_XRDTPCUTILS_HH

#include <string>
#include "XrdHttp/XrdHttpExtHandler.hh"

namespace TPC {
  class TPCHandler;
}


class XrdHttpTpcUtils {
public:
  struct PrepareOpenURLParams {
    PrepareOpenURLParams(const std::string & reqResourceParm,
                         std::map<std::string,std::string> & reqHeadersParm,
                         const std::map<std::string,std::string> & hdr2cgimapParm,
                         const std::map<std::string,std::string> & reprDigestParm):
      reqResource(reqResourceParm),reqHeaders(reqHeadersParm),hdr2cgimap(hdr2cgimapParm),reprDigest(reprDigestParm){}

    const std::string & reqResource;
    std::map<std::string,std::string> & reqHeaders;
    const std::map<std::string,std::string> & hdr2cgimap;
    const std::map<std::string,std::string> & reprDigest;
  };

  /**
   * Prepares the file XRootD open URL from the request resource, the xrd-http-query header of the HTTP request and the hdr2cgi map passed in parameter
   *
   * We first append oss.task=httptpc opaque info. It is therefore guaranteed that after
   * this function is called, at least one opaque info will be part of the full URL.
   * We need to utilize the full URL (including the query string), not just the
   * resource name.  The query portion is hidden in the `xrd-http-query` header;
   * we take this out and combine it with the resource name.
   * We also append the value of the headers configured in tpc.header2cgi to the resource full URL

   * One special key is `authz`; this is always stripped out and copied to the Authorization
   * header (which will later be used for XrdSecEntity).  The latter copy is only done if
   * the Authorization header is not already present.
   * @param reqResource the HTTP request resource
   * @param reqHeaders HTTP request headers that will be modified to contain what is in the authz opaque query if it was provided
   * @param hdr2cgimap the map containing header keys --> XRootD cgi mapping
   * @return the XRootD open URL that will contain at least one opaque parameter (oss.task)
   */
  static std::string prepareOpenURL(PrepareOpenURLParams & params);

  /**
   * Parses the value of a Content-Range response header of the form
   * "bytes FIRST-LAST/TOTAL", where TOTAL may be "*".
   * @param value the value of the header (surrounding white space is ignored)
   * @param first set to the offset of the first byte of the range
   * @param last set to the offset of the last byte of the range
   * @param totalLength if given, set to TOTAL, or to -1 when it is "*"
   * @return true if the value is a valid byte range, false otherwise (first,
   * last and the total are then left untouched)
   */
  static bool parseContentRange(const std::string & value, long long & first, long long & last,
                                long long * totalLength = nullptr);
};


#endif //XROOTD_XRDTPCUTILS_HH

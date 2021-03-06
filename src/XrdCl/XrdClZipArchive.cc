/*
 * XrdZipArchiveReader.cc
 *
 *  Created on: 10 Nov 2020
 *      Author: simonm
 */

#include "XrdCl/XrdClFileOperations.hh"
#include "XrdCl/XrdClZipArchive.hh"
#include "XrdZip/XrdZipZIP64EOCDL.hh"

#include <sys/stat.h>

namespace XrdCl
{

  using namespace XrdZip;

  ZipArchive::ZipArchive() : archsize( 0 ),
                       cdexists( false ),
                       updated( false ),
                       cdoff( 0 ),
                       openstage( None ),
                       flags( OpenFlags::None )
  {
  }

  ZipArchive::~ZipArchive()
  {
  }

  XRootDStatus ZipArchive::OpenArchive( const std::string      &url,
                                     OpenFlags::Flags flags,
                                     ResponseHandler *handler,
                                     uint16_t                timeout )
  {
    Fwd<uint32_t> rdsize;
    Fwd<uint64_t> rdoff;
    Fwd<void*>    rdbuff;
    uint32_t      maxrdsz = EOCD::maxCommentLength + EOCD::eocdBaseSize +
                            ZIP64_EOCDL::zip64EocdlSize;

    Pipeline open_archive = // open the archive
                            XrdCl::Open( archive, url, flags ) >>
                              [=]( XRootDStatus &status, StatInfo &info )
                              {
                                 if( !status.IsOK() )
                                   return handler->HandleResponse( make_status( status ), nullptr );
                                 archsize = info.GetSize();
                                 // if it is an empty file (possibly a new file) there's nothing more to do
                                 if( archsize == 0 )
                                 {
                                   cdexists = false;
                                   openstage = Done;
                                   handler->HandleResponse( make_status(), nullptr );
                                   Pipeline::Stop();
                                 }

                                 rdsize = ( archsize <= maxrdsz ? archsize : maxrdsz );
                                 rdoff  = archsize - *rdsize;
                                 buffer.reset( new char[*rdsize] );
                                 rdbuff = buffer.get();
                                 openstage = HaveEocdBlk;
                               }
                            // read the Central Directory (in several stages if necessary)
                          | XrdCl::Read( archive, rdoff, rdsize, rdbuff ) >>
                              [=]( XRootDStatus &status, ChunkInfo &chunk )
                              {
                                // if the pipeline was interrupted just return
                                if( interrupted( status ) ) return;
                                // check the status is OK
                                if( !status.IsOK() )
                                  return handler->HandleResponse( make_status( status ), nullptr );

                                const char *buff = reinterpret_cast<char*>( chunk.buffer );
                                while( true )
                                {
                                  switch( openstage )
                                  {
                                    case HaveEocdBlk:
                                    {
                                      // Parse the EOCD record
                                      const char *eocdBlock = EOCD::Find( buff, chunk.length );
                                      if( !eocdBlock )
                                      {
                                        XRootDStatus error( stError, errDataError, 0,
                                                            "End-of-central-directory signature not found." );
                                        handler->HandleResponse( make_status( error ), nullptr );;
                                        Pipeline::Stop( error );
                                      }
                                      eocd.reset( new EOCD( eocdBlock ) );

                                      // Do we have the whole archive?
                                      if( chunk.length == archsize )
                                      {
                                        // If we managed to download the whole archive we don't need to
                                        // worry about zip64, it is so small that standard EOCD will do
                                        cdoff = eocd->cdOffset;
                                        buff = buff + cdoff;
                                        openstage = HaveCdRecords;
                                        continue;
                                      }

                                      // Let's see if it is ZIP64 (if yes, the EOCD will be preceded with ZIP64 EOCD locator)
                                      const char *zip64EocdlBlock = eocdBlock - ZIP64_EOCDL::zip64EocdlSize;
                                      // make sure there is enough data to assume there's a ZIP64 EOCD locator
                                      if( zip64EocdlBlock > buffer.get() )
                                      {
                                        uint32_t signature = to<uint32_t>( zip64EocdlBlock );
                                        if( signature == ZIP64_EOCDL::zip64EocdlSign )
                                        {
                                          buff = zip64EocdlBlock;
                                          openstage = HaveZip64EocdlBlk;
                                          continue;
                                        }
                                      }

                                      // It's not ZIP64, we already know where the CD records are
                                      // we need to read more data
                                      cdoff  = eocd->cdOffset;
                                      rdoff  = eocd->cdOffset;
                                      rdsize = eocd->cdSize;
                                      buffer.reset( new char[*rdsize] );
                                      rdbuff = buffer.get();
                                      openstage = HaveCdRecords;
                                      Pipeline::Repeat();
                                    }

                                    case HaveZip64EocdlBlk:
                                    {
                                      std::unique_ptr<ZIP64_EOCDL> eocdl( new ZIP64_EOCDL( buff ) );
                                      if( chunk.offset > eocdl->zip64EocdOffset )
                                      {
                                        // we need to read more data
                                        rdsize = archsize - eocdl->zip64EocdOffset;
                                        rdoff  = eocdl->zip64EocdOffset;
                                        buffer.reset( new char[*rdsize] );
                                        rdbuff = buffer.get();
                                        openstage = HaveZip64EocdBlk;
                                        Pipeline::Repeat();
                                      }

                                      buff = buffer.get() + ( eocdl->zip64EocdOffset - chunk.offset );
                                      openstage = HaveZip64EocdBlk;
                                      continue;
                                    }

                                    case HaveZip64EocdBlk:
                                    {
                                      uint32_t signature = to<uint32_t>( buff );
                                      if( signature != ZIP64_EOCD::zip64EocdSign )
                                      {
                                        XRootDStatus error( stError, errDataError, 0,
                                                            "ZIP64 End-of-central-directory signature not found." );
                                        handler->HandleResponse( make_status( error ), nullptr );
                                        Pipeline::Stop( error );
                                      }
                                      zip64eocd.reset( new ZIP64_EOCD( buff ) );

                                      // now we can read the CD records
                                      cdoff  = zip64eocd->cdOffset;
                                      rdoff  = zip64eocd->cdOffset;
                                      rdsize = zip64eocd->cdSize;
                                      buffer.reset( new char[*rdsize] );
                                      rdbuff = buffer.get();
                                      openstage = HaveCdRecords;
                                      Pipeline::Repeat();
                                    }

                                    case HaveCdRecords:
                                    {
                                      try
                                      {
                                        std::tie( cdvec, cdmap ) = CDFH::Parse( buff, eocd->cdSize, eocd->nbCdRec );
                                      }
                                      catch( const bad_data &ex )
                                      {
                                        XRootDStatus error( stError, errDataError, 0,
                                                                   "ZIP Central Directory corrupted." );
                                        handler->HandleResponse( make_status( error ), nullptr );
                                        Pipeline::Stop( error );
                                      }
                                      openstage = Done;
                                      handler->HandleResponse( make_status( status ), nullptr );
                                      if( chunk.length != archsize ) buffer.reset();
                                      break;
                                    }

                                    default:
                                    {
                                      Pipeline::Stop( XRootDStatus( stError, errInvalidOp ) );
                                    }
                                  }

                                  break;
                                }
                              };


    Async( std::move( open_archive ), timeout );
    return XRootDStatus();
  }

  XRootDStatus ZipArchive::OpenFile( const std::string &fn,
                                     OpenFlags::Flags   flags,
                                     uint64_t           size,
                                     uint32_t           crc32,
                                     ResponseHandler   *handler,
                                     uint16_t           timeout )
  {
    if( !openfn.empty() || openstage != Done )
      return XRootDStatus( stError, errInvalidOp );

    this->flags = flags;
    auto  itr   = cdmap.find( fn );
    if( itr == cdmap.end() )
    {
      if( flags | OpenFlags::New )
      {
        openfn = fn;
        lfh.reset( new LFH( fn, crc32, size, time( 0 ) ) );

        uint64_t wrtoff = cdoff;
        uint32_t wrtlen = lfh->lfhSize;
        std::shared_ptr<buffer_t> wrtbuf( new buffer_t() );
        wrtbuf->reserve( wrtlen );
        lfh->Serialize( *wrtbuf );
        mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
        if( cdexists )
        {
          // TODO if this is an append: checkpoint the EOCD&co
          cdexists = false;
        }

        Pipeline p = XrdCl::Write( archive, wrtoff, lfh->lfhSize, wrtbuf->data() ) >>
                       [=]( XRootDStatus &st ) mutable
                       {
                         wrtbuf.reset();
                         if( st.IsOK() )
                         {
                           archsize += wrtlen;
                           cdoff    += wrtlen;
                           cdvec.emplace_back( new CDFH( lfh.get(), mode, wrtoff ) );
                           cdmap[fn] = cdvec.size() - 1;
                         }
                         if( handler )
                           handler->HandleResponse( make_status( st ), nullptr );
                       };
        Async( std::move( p ), timeout );
        return XRootDStatus();
      }
      return XRootDStatus( stError, errNotFound );
    }
    openfn = fn;
    if( handler ) Schedule( handler, make_status() );
    return XRootDStatus();
  }

  XRootDStatus ZipArchive::CloseArchive( ResponseHandler *handler,
                                      uint16_t         timeout )
  {
    if( updated )
    {
      uint64_t wrtoff  = cdoff;
      uint32_t wrtsize = 0;
      uint32_t cdsize  = CDFH::CalcSize( cdvec );
      // first create the EOCD record
      eocd.reset( new EOCD( cdoff, cdvec.size(), cdsize ) );
      wrtsize += eocd->eocdSize ;
      wrtsize += eocd->cdSize;
      // then create zip64eocd & zip64eocdl if necessary
      std::unique_ptr<ZIP64_EOCD>  zip64eocd;
      std::unique_ptr<ZIP64_EOCDL> zip64eocdl;
      if( eocd->useZip64 )
      {
        zip64eocd.reset( new ZIP64_EOCD( cdoff, cdvec.size(), cdsize ) );
        wrtsize += zip64eocd->zip64EocdTotalSize;
        zip64eocdl.reset( new ZIP64_EOCDL( *eocd, *zip64eocd ) );
        wrtsize += ZIP64_EOCDL::zip64EocdlSize;
      }

      // the shared pointer will be copied by the lambda (note [=])
      std::shared_ptr<buffer_t> wrtbuff( new buffer_t() );
      wrtbuff->reserve( wrtsize );
      // Now serialize all records into a buffer
      CDFH::Serialize( cdvec, *wrtbuff );
      if( zip64eocd )
        zip64eocd->Serialize( *wrtbuff );
      if( zip64eocdl )
        zip64eocdl->Serialize( *wrtbuff );
      eocd->Serialize( *wrtbuff );

      Pipeline p = XrdCl::Write( archive, wrtoff, wrtsize, wrtbuff->data() ) // TODO if this fails the status wont be passed to user handler
                 | Close( archive ) >>
                     [=]( XRootDStatus &st ) mutable
                     {
                       wrtbuff.reset();
                       if( st.IsOK() ) Clear();
                       else openstage = Error;
                       if( handler ) handler->HandleResponse( make_status( st ), nullptr );
                     };
      Async( std::move( p ), timeout );
      return XRootDStatus();
    }

    Pipeline p = Close( archive ) >>
                          [=]( XRootDStatus &st )
                          {
                            if( st.IsOK() ) Clear();
                            else openstage = Error;
                            if( handler ) handler->HandleResponse( make_status( st ), nullptr );
                          };
    Async( std::move( p ), timeout );
    return XRootDStatus();
  }

  XRootDStatus ZipArchive::Read( uint64_t         relativeOffset,
                              uint32_t         size,
                              void            *usrbuff,
                              ResponseHandler *usrHandler,
                              uint16_t         timeout )
  {
    if( openstage != Done || openfn.empty() )
      return XRootDStatus( stError, errInvalidOp,
                           errInvalidOp, "Archive not opened." );

    auto cditr = cdmap.find( openfn );
    if( cditr == cdmap.end() )
      return XRootDStatus( stError, errNotFound,
                           errNotFound, "File not found." );
    CDFH *cdfh = cdvec[cditr->second].get();

    // check if the file is compressed, for now we only support uncompressed and inflate/deflate compression
    if( cdfh->compressionMethod != 0 && cdfh->compressionMethod != Z_DEFLATED )
      return XRootDStatus( stError, errNotSupported,
                           0, "The compression algorithm is not supported!" );

    // Now the problem is that at the beginning of our
    // file there is the Local-file-header, which size
    // is not known because of the variable size 'extra'
    // field, so we need to know the offset of the next
    // record and shift it by the file size.
    // The next record is either the next LFH (next file)
    // or the start of the Central-directory.
    uint64_t cdOffset = zip64eocd ? zip64eocd->cdOffset : eocd->cdOffset;
    uint64_t nextRecordOffset = ( cditr->second + 1 < cdvec.size() ) ?
                                CDFH::GetOffset( *cdvec[cditr->second + 1] ) : cdOffset;
    uint64_t filesize  = cdfh->compressedSize;
    uint64_t fileoff  = nextRecordOffset - filesize;
    uint64_t offset   = fileoff + relativeOffset;
    uint64_t sizeTillEnd = relativeOffset > cdfh->uncompressedSize ?
                           0 : cdfh->uncompressedSize - relativeOffset;
    if( size > sizeTillEnd ) size = sizeTillEnd;

    // if it is a compressed file use ZIP cache to read from the file
    if( cdfh->compressionMethod == Z_DEFLATED )
    {
      // check if respective ZIP cache exists
      bool empty = inflcache.find( openfn ) == inflcache.end();
      // if the entry does not exist, it will be created using
      // default constructor
      InflCache &cache = inflcache[openfn];
      // if we have the whole ZIP archive we can populate the cache
      // straight away
      if( empty && buffer)
      {
        XRootDStatus st = cache.Input( buffer.get() + offset, filesize - fileoff, relativeOffset );
        if( !st.IsOK() ) return st;
      }

      XRootDStatus st = cache.Output( usrbuff, size, relativeOffset );

      // read from cache
      if( !empty || buffer )
      {
        uint32_t bytesRead = 0;
        st = cache.Read( bytesRead );
        // propagate errors to the end-user
        if( !st.IsOK() ) return st;
        // we have all the data ...
        if( st.code == suDone )
        {
          if( usrHandler )
          {
            XRootDStatus *st = make_status();
            ChunkInfo    *ch = new ChunkInfo( relativeOffset, size, usrbuff );
            Schedule( usrHandler, st, ch );
          }
          return XRootDStatus();
        }
      }

      // the raw offset of the next chunk within the file
      uint64_t rawOffset = cache.NextChunkOffset();
      // if this is the first time we are setting an input chunk
      // use the user-specified offset
      if( !rawOffset )
        rawOffset = relativeOffset;
      // size of the next chunk of raw (compressed) data
      uint32_t chunkSize = size;
      // make sure we are not reading passed the end of the file
      if( rawOffset + chunkSize > filesize )
        chunkSize = filesize - rawOffset;
      // allocate the buffer for the compressed data
      buffer.reset( new char[chunkSize] );
      Pipeline p = XrdCl::Read( archive, fileoff + rawOffset, chunkSize, buffer.get() ) >>
                     [=, &cache]( XRootDStatus &st, ChunkInfo &ch )
                     {
                       if( !st.IsOK() )
                       {
                         if( usrHandler ) usrHandler->HandleResponse( make_status( st ), nullptr );
                         return;
                       }
                       st = cache.Input( ch.buffer, ch.length, rawOffset );
                       if( !st.IsOK() )
                       {
                         if( usrHandler ) usrHandler->HandleResponse( make_status( st ), nullptr );
                         buffer.reset();
                         Pipeline::Stop( st );
                       }

                       // at this point we can be sure that all the needed data are in the cache
                       // (we requested as much data as the user asked for so in the worst case
                       // we have exactly as much data as the user needs, most likely we have
                       // more because the data are compressed)
                       uint32_t bytesRead = 0;
                       st = cache.Read( bytesRead );
                       if( !st.IsOK() )
                       {
                         if( usrHandler ) usrHandler->HandleResponse( make_status( st ), nullptr );
                         buffer.reset();
                         Pipeline::Stop( st );
                       }

                       // call the user handler
                       if( usrHandler)
                       {
                         ChunkInfo *rsp = new ChunkInfo( relativeOffset, size, usrbuff );
                         usrHandler->HandleResponse( make_status(), PkgRsp( rsp ) );
                       }
                       buffer.reset();
                     };
      Async( std::move( p ), timeout );
      return XRootDStatus();
    }

    // check if we have the whole file in our local buffer
    if( buffer || size == 0 )
    {
      if( size ) memcpy( usrbuff, buffer.get() + offset, size );

      if( usrHandler )
      {
        XRootDStatus *st = make_status();
        ChunkInfo    *ch = new ChunkInfo( relativeOffset, size, usrbuff );
        Schedule( usrHandler, st, ch );
      }
      return XRootDStatus();
    }

    Pipeline p = XrdCl::Read( archive, offset, size, usrbuff ) >>
                   [=]( XRootDStatus &st, ChunkInfo &chunk )
                   {
                     if( usrHandler )
                     {
                       XRootDStatus *status = make_status( st );
                       ChunkInfo    *rsp = nullptr;
                       if( st.IsOK() )
                         rsp = new ChunkInfo( relativeOffset, chunk.length, chunk.buffer );
                       usrHandler->HandleResponse( status, PkgRsp( rsp ) );
                     }
                   };
    Async( std::move( p ), timeout );
    return XRootDStatus();
  }

  XRootDStatus ZipArchive::List( DirectoryList *&list )
  {
    if( openstage != Done )
      return XRootDStatus( stError, errInvalidOp,
                                  errInvalidOp, "Archive not opened." );

    std::string value;
    archive.GetProperty( "LastURL", value );
    URL url( value );

    StatInfo *infoptr = 0;
    XRootDStatus st = archive.Stat( false, infoptr );
    std::unique_ptr<StatInfo> info( infoptr );

    list = new DirectoryList();
    list->SetParentName( url.GetPath() );

    auto itr = cdvec.begin();
    for( ; itr != cdvec.end() ; ++itr )
    {
      CDFH *cdfh = itr->get();
      StatInfo *entry_info = make_stat( *info, cdfh->uncompressedSize );
      DirectoryList::ListEntry *entry =
          new DirectoryList::ListEntry( url.GetHostId(), cdfh->filename, entry_info );
      list->Add( entry );
    }

    return XRootDStatus();
  }

  XRootDStatus ZipArchive::Write( uint32_t         size,
                               const void      *buffer,
                               ResponseHandler *handler,
                               uint16_t         timeout )
  {
    if( openstage != Done || openfn.empty() )
      return XRootDStatus( stError, errInvalidOp,
                           errInvalidOp, "Archive not opened." );

    uint64_t wrtoff = cdoff; // we only support appending
    Pipeline p = XrdCl::Write( archive, wrtoff, size, buffer ) >>
                   [=]( XRootDStatus &st )
                   {
                     if( st.IsOK() )
                     {
                       cdoff    += size;
                       archsize += size;
                       updated   = true;
                     }
                     if( handler )
                       handler->HandleResponse( make_status( st ), nullptr );
                   };
    Async( std::move( p ), timeout );
    return XRootDStatus();
  }

} /* namespace XrdZip */

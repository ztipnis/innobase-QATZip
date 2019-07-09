/***********************************************************************

Copyright (c) 1995, 2019, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2009, Percona Inc.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

***********************************************************************/

/** @file os/file.cc
 The interface to the operating system file i/o primitives

 Created 10/21/1995 Heikki Tuuri
 *******************************************************/

/** NOTE: The functions in this file should only use functions from
other files in library. The code in this file is used to make a library for
external tools. */
#ifdef assert
#undef assert
#endif
#define assert(bval) if(!bval) ib::fatal() << "assertion error caught";
#define NONE NONE_QZ
#include <qatzip.h>
#undef NONE


#include "db0err.h"
#include "fil0fil.h"
#include "mach0data.h"
#include "os0file.h"
#include "univ.i"

#include <lz4.h>
#include <zlib.h>




/**
@param[in]      type            The compression type
@return the string representation */
const char *Compression::to_string(Type type) {
  switch (type) {
    case NONE:
      return ("None");
    case ZLIB:
      return ("Zlib");
    case QZIP:
      return ("QATZip");
    case QZIP_DCO:
      return ("QATZip compression w/ Zlib decompression");
    case LZ4:
      return ("LZ4");
  }

  ut_ad(0);

  return ("<UNKNOWN>");
}

/**
@param[in]      meta		Page Meta data
@return the string representation */
std::string Compression::to_string(const Compression::meta_t &meta) {
  std::ostringstream stream;

  stream << "version: " << int(meta.m_version) << " "
         << "algorithm: " << meta.m_algorithm << " "
         << "(" << to_string(meta.m_algorithm) << ") "
         << "orginal_type: " << meta.m_original_type << " "
         << "original_size: " << meta.m_original_size << " "
         << "compressed_size: " << meta.m_compressed_size;

  return (stream.str());
}

/** @return true if it is a compressed page */
bool Compression::is_compressed_page(const byte *page) {
  return (mach_read_from_2(page + FIL_PAGE_TYPE) == FIL_PAGE_COMPRESSED);
}

/** Deserizlise the page header compression meta-data
@param[in]	page		Pointer to the page header
@param[out]	control		Deserialised data */
void Compression::deserialize_header(const byte *page,
                                     Compression::meta_t *control) {
  ut_ad(is_compressed_page(page));

  control->m_version =
      static_cast<uint8_t>(mach_read_from_1(page + FIL_PAGE_VERSION));

  control->m_original_type =
      static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_ORIGINAL_TYPE_V1));

  control->m_compressed_size =
      static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_COMPRESS_SIZE_V1));

  control->m_original_size =
      static_cast<uint16_t>(mach_read_from_2(page + FIL_PAGE_ORIGINAL_SIZE_V1));

  control->m_algorithm =
      static_cast<Type>(mach_read_from_1(page + FIL_PAGE_ALGORITHM_V1));
}

/** Decompress the page data contents. Page type must be FIL_PAGE_COMPRESSED, if
not then the source contents are left unchanged and DB_SUCCESS is returned.
@param[in]	dblwr_recover	true of double write recovery in progress
@param[in,out]	src		Data read from disk, decompressed data will be
                                copied to this page
@param[in,out]	dst		Scratch area to use for decompression
@param[in]	dst_len		Size of the scratch area in bytes
@return DB_SUCCESS or error code */
dberr_t Compression::deserialize(bool dblwr_recover, byte *src, byte *dst,
                                 ulint dst_len) {
  if (!is_compressed_page(src)) {
    /* There is nothing we can do. */
    return (DB_SUCCESS);
  }

  meta_t header;

  deserialize_header(src, &header);

  byte *ptr = src + FIL_PAGE_DATA;

  if (header.m_version != 1 ||
      header.m_original_size < UNIV_PAGE_SIZE_MIN - (FIL_PAGE_DATA + 8) ||
      header.m_original_size > UNIV_PAGE_SIZE_MAX - FIL_PAGE_DATA ||
      dst_len < header.m_original_size + FIL_PAGE_DATA) {
    /* The last check could potentially return DB_OVERFLOW,
    the caller should be able to retry with a larger buffer. */

    return (DB_CORRUPTION);
  }

  // FIXME: We should use TLS for this and reduce the malloc/free
  bool allocated;

  /* The caller doesn't know what to expect */
  if (dst == NULL) {
    /* Add a safety margin of an additional 50% */
    ulint n_bytes = header.m_original_size + (header.m_original_size / 2);

    dst = reinterpret_cast<byte *>(ut_malloc_nokey(n_bytes));

    if (dst == NULL) {
      return (DB_OUT_OF_MEMORY);
    }

    allocated = true;
  } else {
    allocated = false;
  }

  int ret;
  Compression compression;
  ulint len = header.m_original_size;

  compression.m_type = static_cast<Compression::Type>(header.m_algorithm);

  switch (compression.m_type) {
    case Compression::QZIP_DCO:
    case Compression::ZLIB: {
      uLongf zlen = header.m_original_size;

      if (uncompress(dst, &zlen, ptr, header.m_compressed_size) != Z_OK) {
        if (allocated) {
          ut_free(dst);
        }

        return (DB_IO_DECOMPRESS_FAIL);
      }

      len = static_cast<ulint>(zlen);

      break;
    }

    case Compression::QZIP:{
      unsigned int qzlen = static_cast<unsigned int>(header.m_original_size);
      unsigned int qzclen = static_cast<unsigned int>(header.m_compressed_size);

      QzSession_T session;
      QzSession_T *sess = &session;

      static QzSessionParams_T parameters;
      QzSessionParams_T *params = &parameters;

      QzStatus_T status;

      if(qzGetStatus (sess, &status ) != QZ_OK){
        //could not get status of card
        ib::error() << "Could not get QAT Status";
        return (DB_IO_DECOMPRESS_FAIL);
      }
      if(status.qat_hw_count <= 0){
        //no qzip card
        ib::error() << "QuickAssist Card Not Found";
      }else{
        ib::info() << "Using QuickAssist Card";
      }
      if(status.qat_service_stated == 0){
        //qzInit not started
        int rc = qzInit(sess, 1);
        if(rc != QZ_OK && rc != QZ_DUPLICATE){
          ib::error() << "Could not initialize QAT Process";
          return (DB_IO_DECOMPRESS_FAIL);
        }
      }

      if(status.qat_instance_attach == 0){
        //qzSession not attached
        if(qzGetDefaults(params) != QZ_OK){
          ib::error() << "Could not get QAT default parameters";
        }
        params->direction = QZ_DIR_BOTH;
        int rc = qzSetupSession (sess,params);
        if (rc != QZ_OK && rc != QZ_DUPLICATE){
          qzTeardownSession(sess);
          qzClose(sess);
          ib::error() << "Error setting up QZip Session";
        }
      }

      if(qzDecompress(sess, ptr, &qzclen, dst, &qzlen) != QZ_OK){
        if(allocated){
          ut_free(dst);
        }
        return (DB_IO_DECOMPRESS_FAIL);
      }
      qzTeardownSession(sess);
      len = static_cast<ulint>(qzlen);
      
      break;
    }

    case Compression::LZ4:

      if (dblwr_recover) {
        ret = LZ4_decompress_safe(
            reinterpret_cast<char *>(ptr), reinterpret_cast<char *>(dst),
            header.m_compressed_size, header.m_original_size);

      } else {
        /* This can potentially read beyond the input
        buffer if the data is malformed. According to
        the LZ4 documentation it is a little faster
        than the above function. When recovering from
        the double write buffer we can afford to us the
        slower function above. */

        ret = LZ4_decompress_fast(reinterpret_cast<char *>(ptr),
                                  reinterpret_cast<char *>(dst),
                                  header.m_original_size);
      }

      if (ret < 0) {
        if (allocated) {
          ut_free(dst);
        }

        return (DB_IO_DECOMPRESS_FAIL);
      }

      break;

    default:
#ifdef UNIV_NO_ERR_MSGS
      ib::error()
#else
      ib::error(ER_IB_MSG_741)
#endif /* UNIV_NO_ERR_MSGS */
          << "Compression algorithm support missing: "
          << Compression::to_string(compression.m_type);

      if (allocated) {
        ut_free(dst);
      }

      return (DB_UNSUPPORTED);
  }

  /* Leave the header alone */
  memmove(src + FIL_PAGE_DATA, dst, len);

  mach_write_to_2(src + FIL_PAGE_TYPE, header.m_original_type);

  ut_ad(dblwr_recover || memcmp(src + FIL_PAGE_LSN + 4,
                                src + (header.m_original_size + FIL_PAGE_DATA) -
                                    FIL_PAGE_END_LSN_OLD_CHKSUM + 4,
                                4) == 0);

  if (allocated) {
    ut_free(dst);
  }

  return (DB_SUCCESS);
}

/** Decompress the page data contents. Page type must be FIL_PAGE_COMPRESSED, if
not then the source contents are left unchanged and DB_SUCCESS is returned.
@param[in]	dblwr_recover	true of double write recovery in progress
@param[in,out]	src		Data read from disk, decompressed data will be
                                copied to this page
@param[in,out]	dst		Scratch area to use for decompression
@param[in]	dst_len		Size of the scratch area in bytes
@return DB_SUCCESS or error code */
dberr_t os_file_decompress_page(bool dblwr_recover, byte *src, byte *dst,
                                ulint dst_len) {
  return (Compression::deserialize(dblwr_recover, src, dst, dst_len));
}

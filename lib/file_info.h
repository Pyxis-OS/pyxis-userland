#ifndef LIB_FILE_INFO_H
#define LIB_FILE_INFO_H

#include <abi/file_info.h>
#include <stdbool.h>

static bool file_info_valid(const struct file_info_reply *info)
{
  return !(info->valid & ~FILE_INFO_VALID_BITS) &&
      (!(info->valid & FILE_INFO_OBJECT_VALID) || (info->valid & FILE_INFO_DOMAIN_VALID)) &&
      (!(info->valid & FILE_INFO_MTIME_VALID) || info->modified_nanoseconds < UINT64_C(1000000000));
}

#endif

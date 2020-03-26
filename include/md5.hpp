//
#pragma once

#include <array>
#include <boost/uuid/detail/md5.hpp>
#include <cstdio>
#include <fstream>
#include <string>

namespace MD5
{
using Hash = boost::uuids::detail::md5;

// 1ファイル分のMD5を計算
std::string
calc(std::string path)
{
  std::ifstream infile(path, std::ios::binary);
  Hash          hash;
  while (infile.eof() == false)
  {
    std::array<char, 8192> buff;
    infile.read(buff.data(), buff.size());
    auto nb = infile.gcount();
    hash.process_bytes(buff.data(), nb);
  }
  Hash::digest_type digest;
  hash.get_digest(digest);
  char md5string[64];
  std::snprintf(md5string,
                sizeof(md5string),
                "%08x%08x%08x%08x",
                digest[0],
                digest[1],
                digest[2],
                digest[3]);
  return md5string;
}
} // namespace MD5

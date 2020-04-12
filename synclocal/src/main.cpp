//
// ローカルディレクトリ版
//
#include <algorithm>
#include <array>
#include <atomic>
#include <boost/filesystem.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/uuid/detail/md5.hpp>
#include <condition_variable>
#include <cstdio>
#include <cxxopts.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <leveldb/db.h>
#include <list>
#include <md5.hpp>
#include <memory>
#include <queue>
#include <string>
#include <thread>

namespace
{
namespace fs = boost::filesystem;

//
//
//
struct FileInfo
{
  fs::path    src_path_;
  fs::path    dst_path_;
  std::string hash_;
  bool        update_;
};
using FileInfoPtr = std::shared_ptr<FileInfo>;
std::queue<FileInfoPtr> fileList;
std::mutex              flLock;
std::condition_variable flCond;
std::atomic_bool        flFinish;

std::unique_ptr<leveldb::DB> db;
leveldb::Options             dbopts;

// 実際のコピースレッド
void
copyThread()
{
  while (flFinish == false)
  {
    FileInfoPtr finfo;
    {
      std::unique_lock<std::mutex> l(flLock);
      flCond.wait(l, []() { return !(fileList.empty() && flFinish == false); });
      if (fileList.empty())
      {
        if (flFinish)
          break;
      }
      else
      {
        finfo = fileList.front();
        fileList.pop();
      }
    }
    if (finfo)
    {
      auto dpath = finfo->dst_path_;
      auto dir   = dpath.parent_path();
      if (fs::exists(dir) == false)
      {
        boost::system::error_code err;
        fs::create_directories(dir, err);
        // std::cout << "create directory:" << dir << std::endl;
      }
      std::cout << "[Update]: " << dpath << std::endl;
      fs::copy_file(finfo->src_path_,
                    finfo->dst_path_,
                    fs::copy_option::overwrite_if_exists);
      db->Put(leveldb::WriteOptions(), dpath.generic_string(), finfo->hash_);
    }
  }
}

// ファイルリスト作成
void
copyFiles(fs::path path, fs::path dstpath)
{
  fs::path abspath;
  if (path.filename() == ".")
    abspath = path.parent_path();
  else
    abspath = path;
  abspath      = fs::absolute(abspath).lexically_normal();
  auto pathstr = abspath.generic_string();
  auto pathlen = pathstr.length();

  auto dir =
      boost::make_iterator_range(fs::recursive_directory_iterator(abspath), {});
  for (const auto& e : dir)
  {
    if (!fs::is_directory(e))
    {
      auto srcabs = e.path();
      auto srcstr = srcabs.generic_string();
      auto hash   = MD5::calc(srcstr);

      std::string old_hash;
      auto        s      = db->Get(leveldb::ReadOptions(), srcstr, &old_hash);
      bool        update = false;
      if (!s.ok() || old_hash != hash)
      {
        // new file or update
        db->Put(leveldb::WriteOptions(), srcstr, hash);
        update = true;
      }

      auto dstabs = srcstr;
      auto fsp    = dstabs.find(pathstr);
      if (fsp == 0)
      {
        // ディレクトリをコピー先に差し替える
        auto     rel = dstabs.substr(pathlen);
        fs::path n_path{dstpath};
        dstabs = (n_path / rel).generic_string();
      }
      if (update == false)
      {
        // 元ファイルが更新されていない場合は先のファイルが存在するか調べる
        update = !fs::exists(dstabs);
      }
      if (update)
      {
        auto finfo       = std::make_shared<FileInfo>();
        finfo->src_path_ = srcabs;
        finfo->dst_path_ = dstabs;
        finfo->hash_     = hash;
        finfo->update_   = update;
        {
          std::lock_guard<std::mutex> l(flLock);
          fileList.emplace(finfo);
        }
        flCond.notify_one();
      }
    }
  }
}

} // namespace

//
//
//
int
main(int argc, char** argv)
{
  const fs::path   app(argv[0]);
  cxxopts::Options options(app.filename().generic_string(),
                           "directory synchronize utility");

  options.add_options()("h,help", "Print usage")(
      "f,filedb",
      "path to the files database",
      cxxopts::value<std::string>()->default_value("./.syncfiles.db"))(
      "j,job", "number of jobs", cxxopts::value<int>()->default_value("-1"))(
      "s,src",
      "source files path",
      cxxopts::value<std::string>()->default_value("."))(
      "d,dst",
      "destination files path",
      cxxopts::value<std::string>()->default_value("."))(
      "p,pattern",
      "matching pattern for copy files",
      cxxopts::value<std::string>()->default_value(""));

  options.parse_positional({"src", "dst", "args"});

  std::list<std::thread> thList;

  int ret = 0;
  try
  {
    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
      std::cout << options.help() << std::endl;
      return 0;
    }
    if (result.count("src") == 0 || result.count("dst") == 0)
    {
      std::cout << "need directories: <src> <dst>" << std::endl;
      std::cout << options.help() << std::endl;
      return 1;
    }
    auto srcpath = fs::path(result["src"].as<std::string>()).lexically_normal();
    auto dstpath = fs::path(result["dst"].as<std::string>()).lexically_normal();
    if (srcpath == dstpath)
    {
      std::cout << "same directory" << std::endl;
      return 0;
    }

    // copy thread
    int  njobs   = result["job"].as<int>();
    int  maxjobs = njobs <= 0 ? std::thread::hardware_concurrency() / 2 : njobs;
    auto nb_thread = std::max(1, maxjobs);
    for (int i = 0; i < nb_thread; i++)
    {
      thList.emplace_back(std::thread{copyThread});
    }

    // source db open
    dbopts.create_if_missing = true;
    leveldb::DB* tdb;
    auto         dbpath = result["filedb"].as<std::string>();
    auto         status = leveldb::DB::Open(dbopts, dbpath, &tdb);
    if (!status.ok())
    {
      std::cerr << status.ToString() << std::endl;
    }
    else
    {
      auto ndb = std::unique_ptr<leveldb::DB>{tdb};
      db.swap(ndb);
      copyFiles(srcpath, dstpath);
    }
  }
  catch (std::exception& e)
  {
    //
    std::cout << options.help() << std::endl;
    std::cerr << e.what() << std::endl;
    ret = 1;
  }
  //
  flFinish = true;
  flCond.notify_all();
  for (auto& th : thList)
  {
    th.join();
  }
  return ret;
}

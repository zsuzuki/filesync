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
struct Queue
{
  virtual ~Queue()       = default;
  virtual void execute() = 0;
};
using QueuePtr = std::shared_ptr<Queue>;
std::queue<QueuePtr>    workList;
std::mutex              qLock;
std::condition_variable qCond;
std::atomic_bool        qFinish{false};
std::atomic_int         qCount{0};

bool useTimeStamp = false;
bool checkOnly    = false;
bool verboseMode  = false;

//
struct FileInfo : public Queue
{
  fs::path    src_path_;
  fs::path    dst_path_;
  std::string hash_;
  bool        update_;

  ~FileInfo() = default;

  void copy();
  void execute() override { copy(); }
};

//
//
struct CheckInfo : public Queue
{
  fs::path    src_path_;
  fs::path    dst_dir_;
  std::string pathstr_;
  size_t      pathlen_;

  ~CheckInfo() = default;

  void check();
  void execute() override { check(); }
};

std::unique_ptr<leveldb::DB> db;
leveldb::Options             dbopts;

//
void
FileInfo::copy()
{
  auto dpath = dst_path_;
  auto dir   = dpath.parent_path();
  if (fs::exists(dir) == false)
  {
    boost::system::error_code err;
    fs::create_directories(dir, err);
    // std::cout << "create directory:" << dir << std::endl;
  }
  std::cout << "[Update]: " << dpath << std::endl;
  fs::remove(dst_path_);
  fs::copy_file(src_path_, dst_path_);
  db->Put(leveldb::WriteOptions(), dpath.generic_string(), hash_);
  --qCount;
}

//
void
CheckInfo::check()
{
  auto srcabs = src_path_;
  auto srcstr = srcabs.generic_string();

  std::string hash;
  uint64_t    hash_num = 0;
  if (useTimeStamp)
  {
    hash_num = fs::last_write_time(src_path_);
    hash     = std::to_string(hash_num);
  }
  else
  {
    hash = MD5::calc(srcstr);
  }
  auto check_hash = [&](auto old) {
    if (useTimeStamp)
      return hash_num > std::stoull(old);
    return hash != old;
  };

  std::string old_hash;
  auto        s      = db->Get(leveldb::ReadOptions(), srcstr, &old_hash);
  bool        update = false;
  if (!s.ok() || check_hash(old_hash))
  {
    // new file or update
    db->Put(leveldb::WriteOptions(), srcstr, hash);
    update = true;
    if (update && verboseMode)
      std::cout << "[db update]: " << srcstr << std::endl;
  }

  auto dstabs = srcstr;
  auto fsp    = dstabs.find(pathstr_);
  if (fsp == 0)
  {
    // ディレクトリをコピー先に差し替える
    auto     rel = dstabs.substr(pathlen_);
    fs::path n_path{dst_dir_};
    dstabs = (n_path / rel).generic_string();
  }
  if (update == false)
  {
    // 元ファイルが更新されていない場合は先のファイルが存在するか調べる
    update = !fs::exists(dstabs);
    if (update && verboseMode)
      std::cout << "[no exists]: " << dstabs << std::endl;
  }
  if (update && checkOnly == false)
  {
    auto finfo       = std::make_shared<FileInfo>();
    finfo->src_path_ = srcabs;
    finfo->dst_path_ = dstabs;
    finfo->hash_     = hash;
    finfo->update_   = update;
    {
      std::lock_guard<std::mutex> l(qLock);
      workList.push(finfo);
    }
    qCond.notify_one();
  }
  else
  {
    if (verboseMode)
      std::cout << "[no update]: " << srcstr << std::endl;
    --qCount;
  }
}

// ワーカースレッド
void
workThread()
{
  while (qFinish == false)
  {
    QueuePtr q;
    {
      std::unique_lock<std::mutex> l(qLock);
      qCond.wait(l, []() { return !(workList.empty() && qFinish == false); });
      if (workList.empty())
      {
        if (qFinish)
          break;
      }
      else
      {
        q = workList.front();
        workList.pop();
      }
    }
    if (q)
    {
      q->execute();
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
      qCount++;
      auto chinfo       = std::make_shared<CheckInfo>();
      chinfo->src_path_ = e.path();
      chinfo->dst_dir_  = dstpath;
      chinfo->pathstr_  = pathstr;
      chinfo->pathlen_  = pathlen;
      {
        std::lock_guard<std::mutex> l(qLock);
        workList.push(chinfo);
      }
      qCond.notify_one();
    }
  }
  //
  for (;;)
  {
    {
      std::lock_guard<std::mutex> l(qLock);
      if (workList.empty() && qCount == 0)
      {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(0));
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
      "t,time",
      "check time stamp",
      cxxopts::value<bool>()->default_value("false"))(
      "v,verbose",
      "verbose mode",
      cxxopts::value<bool>()->default_value("false"))(
      "c,check", "check only", cxxopts::value<bool>()->default_value("false"))(
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
      thList.emplace_back(std::thread{workThread});
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
      useTimeStamp = result["time"].as<bool>();
      checkOnly    = result["check"].as<bool>();
      verboseMode  = result["verbose"].as<bool>();
      if (verboseMode)
        std::cout << "number of job: " << nb_thread << std::endl;
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
  qFinish = true;
  qCond.notify_all();
  for (auto& th : thList)
  {
    th.join();
  }
  return ret;
}

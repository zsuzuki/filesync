#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/xpressive/xpressive.hpp>
#include <connection.hpp>
#include <cstdio>
#include <cxxopts.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <thread>

namespace
{
namespace asio    = boost::asio;
namespace process = boost::process;
namespace fs      = boost::filesystem;
using asio::ip::tcp;
using work_ptr = std::shared_ptr<asio::io_service::work>;

bool verboseMode = false;

// ファイル情報
struct FileInfo
{
  fs::path    full_path_;
  fs::path    rel_path_;
  std::string time_;
};
using FileList = std::list<FileInfo>;
FileList transFileList;

// ファイルリスト作成
FileList
makeFilelist(const fs::path path, std::string without)
{
  using sregex  = boost::xpressive::sregex;
  sregex rex    = sregex::compile(without);
  bool   no_rex = without.empty();

  FileList tflist;
  if (verboseMode)
  {
    std::cout << "Search Path: " << path << std::endl;
  }

  auto it   = fs::recursive_directory_iterator(path);
  auto dir  = boost::make_iterator_range(it, {});
  auto rstr = path.generic_string();
  auto rlen = rstr.length();
  for (const auto& e : dir)
  {
    if (!fs::is_directory(e))
    {
      using namespace boost::xpressive;
      auto   fname = e.path();
      smatch sm;
      if (no_rex || !regex_search(fname.generic_string(), sm, rex))
      {
        // 除外パターンに掛からなかったので通過
        auto pstr    = fname.generic_string();
        auto len     = pstr[rlen] == '/' ? rlen + 1 : rlen;
        auto relpath = pstr.substr(len);
        auto wtime   = fs::last_write_time(fname);
        auto wtstr   = std::to_string(wtime);
        //
        FileInfo fi;
        fi.full_path_ = fname;
        fi.rel_path_  = relpath;
        fi.time_      = wtstr;
        tflist.push_back(fi);
        if (verboseMode)
        {
          std::cout << "Append: " << fname << "(" << relpath << "): " << wtstr
                    << std::endl;
        }
      }
    }
  }
  return tflist;
}

//
//
//
class Server : public Network::ConnectionBase
{
  tcp::acceptor acceptor_;
  work_ptr      work_;
  fs::path      req_dir_;
  FileList      filelist_;

public:
  Server(asio::io_service& io_service)
      : Network::ConnectionBase(io_service),
        acceptor_(io_service, tcp::endpoint(tcp::v4(), 34000)),
        work_(std::make_shared<asio::io_service::work>(io_service))
  {
  }

  void start() { start_accept(); }

private:
  // 接続待機
  void start_accept()
  {
    acceptor_.async_accept(socket_, [&](auto& err) { on_accept(err); });
  }

  // 接続待機完了
  void on_accept(const boost::system::error_code& error)
  {
    if (error)
    {
      std::cout << "accept failed: " << error.message() << std::endl;
      return;
    }

    start_receive(
        [&](auto cmd, auto bufflist) { receive_loop(cmd, bufflist); });
  }

  //
  void receive_loop(const char* cmd, const Network::BufferList& bufflist)
  {
    std::string command = cmd;
    bool        finish  = false;
    if (command != "error" && bufflist.size() > 0)
    {
      if (command == "request")
      {
        if (bufflist[0] == "filelist")
        {
          fs::path    source_path{"."};
          std::string without_regex;
          if (bufflist.size() > 1)
          {
            source_path = bufflist[1];
            if (bufflist.size() > 2)
            {
              without_regex = bufflist[2];
            }
            if (verboseMode)
            {
              std::cout << "Source Path: " << source_path << std::endl;
              std::cout << "Without Regex: " << without_regex << std::endl;
            }
            // リストの更新
            req_dir_  = source_path.lexically_normal();
            filelist_ = makeFilelist(req_dir_, without_regex);
          }
          return_file_list();
        }
        // 次の指示を待つ
        start_receive(
            [&](auto cmd, auto bufflist) { receive_loop(cmd, bufflist); });
      }
      else if (command == "filereq")
      {
        // ファイルを送り返す
        fs::path fname = (req_dir_ / bufflist[0]).lexically_normal();
        std::cout << "request: " << fname << std::endl;
        sendFile(fname.generic_string(), [&](bool s) {
          //
          start_receive(
              [&](auto cmd, auto bufflist) { receive_loop(cmd, bufflist); });
        });
      }
      else if (command == "finish")
      {
        // 終了
        work_.reset();
      }
    }
  }

  //
  void return_file_list()
  {
    try
    {
      Network::BufferList send_fl;
      for (auto& f : filelist_)
      {
        send_fl.push_back(f.rel_path_.generic_string());
        send_fl.push_back(f.time_);
      }
      send("filelist", send_fl, [&](bool) {});
    }
    catch (std::exception& e)
    {
      send("finish", {e.what()}, [&](bool) {});
    }
  }
};

} // namespace

int
main(int argc, char** argv)
{
  const fs::path   app(argv[0]);
  cxxopts::Options options(app.filename().generic_string(),
                           "directory synchronize server");

  options.add_options()("h,help", "Print usage")(
      "v,verbose",
      "verbose mode",
      cxxopts::value<bool>()->default_value("false"));

  auto result = options.parse(argc, argv);
  if (result.count("help"))
  {
    std::cout << options.help() << std::endl;
    return 0;
  }

  verboseMode = result["verbose"].as<bool>();

  // サーバ起動
  for (;;)
  {
    if (verboseMode)
      std::cout << "Server launch(waiting...)" << std::endl;
    asio::io_service io_service;
    Server           server(io_service);
    server.start();
    io_service.run();
    if (verboseMode)
      std::cout << "transfer done." << std::endl;
  }

  return 0;
}

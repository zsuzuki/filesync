#include <algorithm>
#include <array>
#include <atomic>
#include <boost/algorithm/hex.hpp>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/uuid/detail/md5.hpp>
#include <connection.hpp>
#include <cstdio>
#include <cxxopts.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using asio::ip::tcp;
using JSON = nlohmann::json;

bool verboseMode = false;

//
//
//
struct FileInfo
{
  std::string file_name_;
  fs::path    real_path_;
  std::string old_hash_;
  std::string new_hash_;
  bool        exists_;

  bool operator==(const FileInfo& o) { return file_name_ == o.file_name_; }
  bool operator==(const std::string fn) { return file_name_ == fn; }
};

std::vector<FileInfo> fileList;

//
//
//
class Client : public Network::ConnectionBase
{
  using Super = Network::ConnectionBase;

  tcp::resolver    resolver_;
  std::string      server_name_;
  fs::path         output_dir_;
  std::atomic_bool is_connect_;
  std::atomic_bool is_finished_;

public:
  Client(asio::io_service& io_service)
      : Super(io_service), resolver_(io_service), is_connect_(false),
        is_finished_(false)
  {
  }

  void start(std::string sv, std::string dir)
  {
    server_name_ = sv;
    output_dir_  = dir;
    connect();
  }

  // ファイルリストリクエスト
  void requestFileList(const std::vector<std::string>& cmd)
  {
    Network::BufferList flist = {"filelist"};
    for (auto& f : cmd)
      flist.push_back(f);

    Super::send("request", flist, [&](bool s) {
      if (!s)
      {
        is_finished_ = false;
      }
    });
  }

  // メッセージ送信
  void send(Network::BufferList& bl)
  {
    Super::send("command", bl, [&](bool s) {
      if (!s)
      {
        is_finished_ = false;
      }
    });
  }

  bool isConnect() const { return is_connect_; }
  bool isFinished() const { return is_finished_; }

private:
  void connect()
  {
    tcp::resolver::query query(server_name_, "34000");
    resolver_.async_resolve(
        query, [&](auto& err, auto iter) { on_resolve(err, iter); });
  }
  //
  void on_resolve(const boost::system::error_code& error,
                  tcp::resolver::iterator          endpoint_iterator)
  {
    if (error)
    {
      std::cout << "resolve failed: " << error.message() << std::endl;
      return;
    }
    asio::async_connect(socket_, endpoint_iterator, [&](auto& err, auto i) {
      on_connect(err);
    });
  }
  //
  void on_connect(const boost::system::error_code& error)
  {
    if (error)
    {
      std::cout << "connect failed : " << error.message() << std::endl;
      return;
    }
    receive();
    is_connect_ = true;
  }
  //
  void receive()
  {
    start_receive([&](auto cmd, auto buff) {
      std::string command = cmd;
      if (command != "error" && buff.size() > 0)
      {
        if (command == "filelist")
        {
          for (size_t i = 0; i < buff.size(); i += 2)
          {
            auto fname   = buff[i];
            auto rpath   = (output_dir_ / fname).lexically_normal();
            auto timestr = i + 1 < buff.size() ? buff[i + 1] : "";
            auto wtime   = std::stoull(timestr);

            time_t uptime = 0;
            if (fs::exists(rpath))
            {
              // ファイルがあるなら更新時刻を取得
              uptime = fs::last_write_time(rpath);
            }

            if (wtime > uptime)
            {
              // サーバの方が新しい=更新
              FileInfo nf;
              nf.file_name_ = fname;
              nf.real_path_ = rpath;
              fileList.push_back(nf);
            }
          }
        }
      }
      //
      if (command == "error" || command == "finish")
      {
        // finish
        is_finished_ = true;
        std::cout << "Finished" << std::endl;
      }
      else
      {
        asio::post([&]() { copy_loop(0); });
      }
    });
  }

  //
  void copy_loop(int idx)
  {
    for (; idx < fileList.size(); idx++)
    {
      auto& fi = fileList[idx];
      Super::send("filereq", {fi.file_name_}, [&](bool) {
        start_receive(fi.real_path_.generic_string(), [&]() {
          if (fi.old_hash_.empty())
          {
            std::cout << "create: " << fi.real_path_ << " : " << fi.new_hash_
                      << std::endl;
          }
          else
          {
            std::cout << "update: " << fi.real_path_ << " : " << fi.old_hash_
                      << " -> " << fi.new_hash_ << std::endl;
          }
          asio::post([&]() { copy_loop(idx + 1); });
        });
      });
      break;
    }

    if (idx >= fileList.size())
    {
      // 全転送完了
      Super::send("finish", {"no error"}, [&](bool) {});
      is_finished_ = true;
    }
  }
};

} // namespace

//
int
main(int argc, char** argv)
{
  const fs::path   app(argv[0]);
  cxxopts::Options options(app.filename().generic_string(),
                           "directory synchronize client");

  options.add_options()("h,help", "Print usage")(
      "hostname",
      "Hostname",
      cxxopts::value<std::string>()->default_value("localhost"))(
      "o,output",
      "Output path",
      cxxopts::value<std::string>()->default_value("."))(
      "r,request",
      "request directory",
      cxxopts::value<std::string>()->default_value("."))(
      "w,without",
      "without pattern",
      cxxopts::value<std::string>()->default_value(""))(
      "v,verbose",
      "verbose mode",
      cxxopts::value<bool>()->default_value("false"));

  options.parse_positional("hostname");

  int ret = 0;
  try
  {
    auto result   = options.parse(argc, argv);
    auto hostname = result["hostname"].as<std::string>();
    if (result.count("help"))
    {
      std::cout << options.help() << std::endl;
      return 0;
    }

    verboseMode = result["verbose"].as<bool>();

    asio::io_service io_service;
    Client           client(io_service);
    auto             output_dir = result["output"].as<std::string>();
    auto             w  = std::make_shared<asio::io_service::work>(io_service);
    auto             th = std::thread([&]() { io_service.run(); });
    // 接続
    client.start(hostname, output_dir);
    while (client.isConnect() == false)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // 要求(まずはファイルリストから)
    Network::BufferList req;
    req.push_back(result["request"].as<std::string>());
    req.push_back(result["without"].as<std::string>());
    client.requestFileList(req);
    // 転送待ち
    while (client.isFinished() == false)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    w.reset();
    th.join();
  }
  catch (std::exception& e)
  {
    //
    std::cout << options.help() << std::endl;
    std::cerr << e.what() << std::endl;
    ret = 1;
  }
  return ret;
}

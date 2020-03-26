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

void
readFileList(std::string outdir)
{
  fs::path json_path{outdir};
  json_path /= "files.json";
  if (fs::exists(json_path))
  {
    std::ifstream jfile{json_path.generic_string()};
    JSON          json;
    jfile >> json;

    JSON fj = json["filelist"];
    for (auto& f : fj)
    {
      FileInfo fi;
      fs::path rp{outdir};
      fi.file_name_ = f["file"];
      fi.real_path_ = rp / fi.file_name_;
      fi.old_hash_  = f["hash"];
      fi.new_hash_  = fi.old_hash_;
      fi.exists_    = true;
      fileList.push_back(fi);
    }
  }
}

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
  void requestFileList(bool update)
  {
    Super::send("request", {"filelist", update ? "--" : ""}, [&](bool s) {
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
            auto fname = buff[i];
            auto rpath = (output_dir_ / fname).lexically_normal();
            auto hash  = i + 1 < buff.size() ? buff[i + 1] : "";
            auto it    = std::find(fileList.begin(), fileList.end(), fname);
            if (it != fileList.end())
            {
              // リストにある
              auto& fi      = *it;
              fi.new_hash_  = hash;
              fi.real_path_ = rpath;
              if (fs::exists(rpath) == false)
              {
                // ファイルが消されている
                fi.old_hash_ = "";
              }
            }
            else
            {
              // new file
              FileInfo nf;
              nf.file_name_ = fname;
              nf.real_path_ = rpath;
              nf.old_hash_  = "";
              nf.new_hash_  = hash;
              nf.exists_    = true;
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
      if (fi.new_hash_ != fi.old_hash_)
      {
        // copy
        std::cout << fi.real_path_ << ":" << fi.old_hash_ << "/" << fi.new_hash_
                  << std::endl;
        Super::send("filereq", {fi.file_name_}, [&](bool) {
          start_receive(fi.real_path_.generic_string(), [&]() {
            std::cout << "save to: " << fi.real_path_ << std::endl;
            asio::post([&]() { copy_loop(idx + 1); });
          });
        });
        break;
      }
      else
      {
        // through
        std::cout << "skip: " << fi.file_name_ << std::endl;
      }
    }

    if (idx >= fileList.size())
    {
      // 全転送完了
      save_json();
      Super::send("finish", {"no error"}, [&](bool) {});
      is_finished_ = true;
    }
  }

  //
  void save_json()
  {
    JSON json;
    JSON fl;
    for (auto& fi : fileList)
    {
      JSON fj;
      fj["file"] = fi.file_name_;
      fj["hash"] = fi.new_hash_;
      fl.push_back(fj);
    }
    json["filelist"] = fl;

    fs::path      json_path = (output_dir_ / "files.json").lexically_normal();
    std::ofstream jfile{json_path.generic_string()};
    jfile << std::setw(4) << json;
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
      "u,update",
      "Update files",
      cxxopts::value<bool>()->default_value("false"))(
      "hostname",
      "Hostname",
      cxxopts::value<std::string>()->default_value("localhost"))(
      "o,output",
      "Output path",
      cxxopts::value<std::string>()->default_value("."))(
      "d,debug",
      "Enable debugging",
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

    asio::io_service io_service;
    Client           client(io_service);
    auto             output_dir = result["output"].as<std::string>();
    auto             w  = std::make_shared<asio::io_service::work>(io_service);
    auto             th = std::thread([&]() { io_service.run(); });
    client.start(hostname, output_dir);
    while (client.isConnect() == false)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    readFileList(output_dir);
    client.requestFileList(result["update"].as<bool>());
    // client.send(buff_list);
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

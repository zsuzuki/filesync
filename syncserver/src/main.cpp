#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/range/iterator_range.hpp>
#include <connection.hpp>
#include <cstdio>
#include <cxxopts.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <md5.hpp>
#include <nlohmann/json.hpp>
#include <string>

namespace
{
namespace asio    = boost::asio;
namespace process = boost::process;
namespace fs      = boost::filesystem;
using asio::ip::tcp;
using work_ptr = std::shared_ptr<asio::io_service::work>;
using JSON     = nlohmann::json;

// ファイルリスト作成
JSON
make_filelist(std::string pathname)
{
  JSON           json;
  const fs::path path(pathname);
  const fs::path jpath = path / "files.json";

  nlohmann::json filelist;
  for (const auto& e :
       boost::make_iterator_range(fs::recursive_directory_iterator(path), {}))
  {
    if (!fs::is_directory(e))
    {
      auto& p  = e.path();
      auto  pn = p.parent_path() / p.filename();

      // files.json自身は収集しない
      if (pn != jpath)
      {
        auto pstr = pn.generic_string();
        auto fsp  = pstr.find(pathname);
        if (fsp == 0)
        {
          // 先頭からpathname分を削る
          auto     rel = pstr.substr(pathname.length());
          fs::path n_path(".");
          n_path = (n_path / rel).lexically_normal();
          pstr   = n_path.generic_string();
        }

        JSON item;
        auto hash    = MD5::calc(pn.generic_string());
        item["file"] = pstr;
        item["hash"] = hash;
        filelist.push_back(item);
      }
    }
  }
  json["filelist"] = filelist;

  std::ofstream ofile(jpath.generic_string());
  ofile << std::setw(4) << json << std::endl;
  std::cout << "output json[" << jpath.generic_string() << "]" << std::endl;

  return json;
}

//
//
//
class Server : public Network::ConnectionBase
{
  tcp::acceptor acceptor_;
  work_ptr      work_;
  JSON          filelist_;
  fs::path      sync_dir_;

public:
  Server(asio::io_service& io_service)
      : Network::ConnectionBase(io_service),
        acceptor_(io_service, tcp::endpoint(tcp::v4(), 34000)),
        work_(std::make_shared<asio::io_service::work>(io_service))
  {
  }

  void start(JSON& json, const fs::path& sdir)
  {
    filelist_ = json;
    sync_dir_ = sdir;
    start_accept();
  }

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
          if (bufflist.size() > 1 && bufflist[1] == "--")
          {
            // リストの更新
            filelist_ = make_filelist(sync_dir_.generic_string());
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
        fs::path fname = (sync_dir_ / bufflist[0]).lexically_normal();
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
      auto                fl = filelist_["filelist"];
      for (auto& f : fl)
      {
        send_fl.push_back(f["file"].get<std::string>());
        send_fl.push_back(f["hash"].get<std::string>());
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
      "p,path",
      "Search path",
      cxxopts::value<std::string>()->default_value("."))(
      "d,debug",
      "Enable debugging",
      cxxopts::value<bool>()->default_value("false"));

  auto result = options.parse(argc, argv);
  if (result.count("help"))
  {
    std::cout << options.help() << std::endl;
    return 0;
  }

  const fs::path target_path(result["path"].as<std::string>());
  if (fs::is_directory(target_path) == false)
  {
    std::cerr << "\"PATH\"<" << target_path.generic_string()
              << "> has to be a directory." << std::endl;
    return 1;
  }

  // ファイルリストのjson作成
  auto json = make_filelist(target_path.generic_string());

  // サーバ起動
  for (;;)
  {
    asio::io_service io_service;
    Server           server(io_service);
    server.start(json, target_path);
    io_service.run();
  }

  return 0;
}

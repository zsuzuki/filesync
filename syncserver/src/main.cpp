#include <algorithm>
#include <array>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/range/iterator_range.hpp>
#include <boost/xpressive/xpressive.hpp>
#include <connection.hpp>
#include <cpptoml.h>
#include <cstdio>
#include <cxxopts.hpp>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <list>
#include <md5.hpp>
#include <nlohmann/json.hpp>
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
using JSON     = nlohmann::json;
using CPPTOML  = std::shared_ptr<cpptoml::table>;

CPPTOML settings;

// ファイル更新時に実行するコマンドの情報
struct Command
{
  using sregex = boost::xpressive::sregex;

  std::string            pattern_;
  std::list<std::string> list_;
  sregex                 rex_;

  void build(std::string p, std::string l)
  {
    pattern_ = p;
    boost::split(list_, l, boost::is_space());
    rex_ = sregex::compile(pattern_);
  }

  bool check(std::string path)
  {
    using namespace boost::xpressive;
    smatch sm;
    return regex_search(path, sm, rex_);
  }
};
std::vector<Command> commandList;

// 実行したコマンド
struct Execute
{
  using child_ptr = std::shared_ptr<boost::process::child>;

  child_ptr        child_;
  std::future<int> future_;
  fs::path         fname_;
  JSON*            json_;

  void run(std::string cmd, fs::path fn, JSON* j)
  {
    fname_  = fn;
    child_  = std::make_shared<process::child>(cmd);
    future_ = std::async(std::launch::async, [&]() { return 0; });
  }
  void wait()
  {
    child_->wait();
    auto& item     = *json_;
    auto  new_hash = MD5::calc(fname_.generic_string());
    item["hash"]   = new_hash;
    std::cout << "update hash: " << fname_ << " -> " << new_hash << std::endl;
  }

  ~Execute() { wait(); }
};

// tomlからコマンド情報を作る
void
buildCommand(std::shared_ptr<cpptoml::table_array> table)
{
  if (!table)
    return;

  for (const auto& uf : *table)
  {
    auto pat  = uf->get_as<std::string>("pattern");
    auto func = uf->get_as<std::string>("command");
    if (pat && func)
    {
      Command cmd;
      cmd.build(*pat, *func);
      commandList.push_back(cmd);
    }
  }
}

// コマンド文字列を取得
std::string
getUpdateCommand(std::string path)
{
  using namespace boost::xpressive;
  for (auto& cmd : commandList)
  {
    if (cmd.check(path))
    {
      std::string cmdline;
      for (const auto& l : cmd.list_)
      {
        if (cmdline.empty() == false)
          cmdline += " ";
        if (l == "$in")
          cmdline += path;
        else
          cmdline += l;
      }
      return cmdline;
    }
  }
  return {};
}

//
std::optional<JSON*>
append(JSON& flist, std::string fname, std::string hash)
{
  for (auto& fj : flist)
  {
    auto n = fj["file"].get<std::string>();
    if (n == fname)
    {
      // 発見
      auto h     = fj["hash"].get<std::string>();
      fj["hash"] = hash;
      return h != hash ? std::optional<JSON*>{&fj} : std::nullopt;
    }
  }
  // 新規
  JSON item;
  item["file"] = fname;
  item["hash"] = hash;
  flist.push_back(item);
  return std::optional<JSON*>(&flist.back());
}

// ファイルリスト作成
JSON
make_filelist(std::string pathname, std::vector<std::string> fnlist)
{
  const fs::path path(pathname);
  const auto     jpath       = path / "files.json";
  const auto     jpath_fname = jpath.generic_string();

  JSON json, filelist;
  {
    // 既に存在するJSONを読む
    std::ifstream ifs(jpath_fname);
    if (ifs.fail() == false)
    {
      ifs >> json;
      filelist = json["filelist"];
    }
  }

  std::list<std::shared_ptr<Execute>> ex_list;
  for (const auto& e :
       boost::make_iterator_range(fs::recursive_directory_iterator(path), {}))
  {
    if (!fs::is_directory(e))
    {
      auto& p  = e.path();
      auto  pn = p.parent_path() / p.filename();

      // 調べるファイル名が指定されているならそれを確認
      bool check = fnlist.empty();
      if (check == false)
      {
        auto pfn = p.filename().generic_string();
        for (const auto& fn : fnlist)
        {
          if (fn == pfn)
          {
            check = true;
            break;
          }
        }
      }

      // files.json自身は収集しない
      if (check && pn != jpath)
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

        auto hash = MD5::calc(pn.generic_string());
        if (auto optjs = append(filelist, pstr, hash))
        {
          // ファイルが更新もしくは新規
          auto cmd = getUpdateCommand(pn.generic_string());
          if (cmd.empty() == false)
          {
            auto ex = std::make_shared<Execute>();
            ex_list.push_back(ex);
            ex->run(cmd, pn, *optjs);
          }
        }
      }
    }
  }
  json["filelist"] = filelist;
  ex_list.clear();

  std::ofstream ofile(jpath_fname);
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
          if (bufflist.size() > 1)
          {
            // リストの更新
            std::vector<std::string> l;
            if (bufflist[1] != "--")
            {
              auto st = bufflist.begin() + 1;
              auto ed = bufflist.end();
              l       = std::vector<std::string>(st, ed);
            }
            filelist_ = make_filelist(sync_dir_.generic_string(), l);
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

  // 設定
  settings = cpptoml::parse_file("settings.toml");
  buildCommand(settings->get_table_array("update"));

  // ファイルリストのjson作成
  std::vector<std::string> e;
  auto json = make_filelist(target_path.generic_string(), e);

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

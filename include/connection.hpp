//
#pragma once

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <functional>
#include <iostream>
#include <lz4.h>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace Network
{
namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using asio::ip::tcp;

// 送受信データ
using Buffer     = std::vector<char>;
using BufferList = std::vector<std::string>;
// コールバック
using SendCallback     = std::function<void(bool)>;
using RecvCallback     = std::function<void(const char*, const BufferList&)>;
using RecvFileCallback = std::function<void()>;

// 送受信ヘッダ

// 接続
class ConnectionBase
{
protected:
  static constexpr size_t BLOCK_SIZE = 8 * 1024;

  struct Header
  {
    size_t length_;
    size_t count_;
    char   command_[128];
  };
  struct TransHeader
  {
    size_t size_;
    size_t compSize_;
    bool   eof_;
    char   p[128 - 17];
  };
  struct TransBuffer
  {
    TransHeader header_;
    char        body_[BLOCK_SIZE];
  };
  struct SendInfoBase
  {
    Header       header_;
    SendCallback callback_;
    virtual ~SendInfoBase() = default;
  };
  struct SendInfo : public SendInfoBase
  {
    Buffer body_;
  };
  struct SendFileInfo : public SendInfoBase
  {
    std::ifstream infile_;
    TransBuffer   buffer_;
    size_t        trans_;
    LZ4_stream_t  lz4_stream_;
  };
  using SendInfoPtr = std::shared_ptr<SendInfoBase>;
  using SendQueue   = std::queue<SendInfoPtr>;

  struct ReadFileInfo
  {
    std::string        filename_;
    std::ofstream      ofs_;
    RecvFileCallback   callback_;
    LZ4_streamDecode_t lz4_stream_;
    ReadFileInfo(std::string fn, RecvFileCallback cb)
        : filename_(fn), ofs_(fn, std::ios::binary), callback_(cb)
    {
    }
  };
  using ReadFileInfoPtr = std::shared_ptr<ReadFileInfo>;

  asio::io_service& io_service_;
  tcp::socket       socket_;
  Header            read_header_;
  Buffer            read_buffer_;
  RecvCallback      read_callback_;
  SendQueue         send_que_;
  std::mutex        que_lock_;
  ReadFileInfoPtr   read_file_info_;

public:
  ConnectionBase(asio::io_service& io_service)
      : io_service_(io_service), socket_(io_service)
  {
  }

  /// 通常のメッセージ送信
  void send(const char* cmd, BufferList buff_list, SendCallback cb)
  {
    auto  info      = std::make_shared<SendInfo>();
    auto& header    = info->header_;
    auto& buffer    = info->body_;
    info->callback_ = cb;

    int total_size = 0;
    for (auto& b : buff_list)
    {
      total_size += b.size() + 1;
    }
    buffer.resize(total_size);
    int ofs = 0;
    for (auto& b : buff_list)
    {
      int n = b.size() + 1;
      strncpy(&buffer[ofs], b.c_str(), n);
      ofs += n;
    }
    strncpy(header.command_, cmd, sizeof(header.command_));
    header.length_ = buffer.size();
    header.count_  = buff_list.size();

    req_send(info);
  }
  /// ファイル送信
  void sendFile(std::string fname, SendCallback cb)
  {
    auto  info      = std::make_shared<SendFileInfo>();
    auto& header    = info->header_;
    info->callback_ = cb;
    info->infile_.open(fname, std::ios::binary);
    info->trans_ = fs::file_size(fname);
    LZ4_initStream(&info->lz4_stream_, sizeof(info->lz4_stream_));

    strncpy(header.command_, "filecopy", sizeof(header.command_));
    header.length_ = info->trans_;
    header.count_  = 1;

    req_send(info);
  }

  /// メッセージ受信
  void start_receive(RecvCallback cb)
  {
    read_callback_ = cb;
    boost::asio::async_read(
        socket_,
        asio::buffer(&read_header_, sizeof(read_header_)),
        [&](auto& err, auto bytes) { on_header_receive(err, bytes); });
  }
  /// ファイル受信
  void start_receive(std::string fname, RecvFileCallback cb)
  {
    fs::path fullpath{fname};
    if (fs::exists(fullpath))
    {
      fs::remove(fullpath);
    }
    else
    {
      fs::create_directories(fullpath.parent_path());
    }
    read_file_info_ = std::make_shared<ReadFileInfo>(fname, cb);
    LZ4_setStreamDecode(&read_file_info_->lz4_stream_, nullptr, 0);
    read_buffer_.resize(sizeof(TransBuffer));
    boost::asio::async_read(
        socket_,
        asio::buffer(&read_header_, sizeof(read_header_)),
        [&](auto& err, auto bytes) { on_file_receive(err, bytes); });
  }

private:
  void req_send(SendInfoPtr info)
  {
    bool launch;
    {
      std::lock_guard<std::mutex> l(que_lock_);
      launch = send_que_.empty();
      send_que_.push(info);
    }
    if (launch)
    {
      io_service_.post([this]() { send_loop(); });
    }
  }

  // メッセージ受信
  void on_header_receive(const boost::system::error_code& error, size_t bytes)
  {
    if (error && error != boost::asio::error::eof)
    {
      std::cout << "receive header failed: " << error.message() << std::endl;
      read_callback_("error", {});
    }
    else
    {
      read_buffer_.resize(read_header_.length_);
      boost::asio::async_read(
          socket_, asio::buffer(read_buffer_), [&](auto& err, auto bytes) {
            on_receive(err, bytes);
          });
    }
  }
  void on_receive(const boost::system::error_code& error, size_t bytes)
  {
    if (error && error != boost::asio::error::eof)
    {
      std::cout << "receive failed: " << error.message() << std::endl;
      read_callback_("error", {});
    }
    else
    {
      int        ofs = 0;
      BufferList ret;
      for (int i = 0; i < read_header_.count_; i++)
      {
        ret.push_back(&read_buffer_[ofs]);
        ofs += strlen(&read_buffer_[ofs]) + 1;
      }
      read_callback_(read_header_.command_, ret);
    }
  }
  // ファイル受信
  void on_file_receive(const boost::system::error_code& error, size_t bytes)
  {
    if (error && error != boost::asio::error::eof)
    {
      std::cout << "receive header failed: " << error.message() << std::endl;
      read_file_info_->callback_();
    }
    else
    {
      // パケットヘッダ
      read_buffer_.resize(sizeof(TransHeader));
      asio::async_read(
          socket_, asio::buffer(read_buffer_), [&](auto& err, auto bytes) {
            const TransHeader* header =
                reinterpret_cast<const TransHeader*>(read_buffer_.data());
            std::array<char, BLOCK_SIZE> body;
            asio::read(socket_, asio::buffer(body.data(), header->compSize_));
            std::array<char, BLOCK_SIZE> buff;
            int                          decSize =
                LZ4_decompress_safe_continue(&read_file_info_->lz4_stream_,
                                             body.data(),
                                             buff.data(),
                                             header->compSize_,
                                             BLOCK_SIZE);
            // std::cout << "read body:" << header->compSize_ << "," << decSize
            //           << std::endl;
            auto& ofs = read_file_info_->ofs_;
            ofs.write(buff.data(), header->size_);
            if (header->eof_)
            {
              ofs.close();
              read_file_info_->callback_();
            }
            else
            {
              // continue
              on_file_receive(err, bytes);
            }
          });
    }
  }

  //
  void send_loop()
  {
    SendInfoPtr info;
    {
      std::lock_guard<std::mutex> l(que_lock_);
      if (!send_que_.empty())
      {
        info = send_que_.front();
      }
    }

    // ヘッダ転送(共通)
    auto& header = info->header_;
    asio::async_write(socket_,
                      asio::buffer(&header, sizeof(header)),
                      [this, info](auto& err, auto bytes) {
                        on_send_header(info, err, bytes);
                      });
  }
  //
  void on_send_header(SendInfoPtr info, const boost::system::error_code& error,
                      size_t bytes)
  {
    if (error)
    {
      std::cerr << "error[send header]: " << error.message() << std::endl;
      info->callback_(false);
    }
    else if (auto minfo = std::dynamic_pointer_cast<SendInfo>(info))
    {
      // メッセージ送信
      auto& buffer = minfo->body_;
      asio::async_write(
          socket_, asio::buffer(buffer), [this, info](auto& err, auto bytes) {
            on_send(info, err, bytes);
          });
    }
    else if (auto minfo = std::dynamic_pointer_cast<SendFileInfo>(info))
    {
      // ファイル送信
      auto& ifs    = minfo->infile_;
      auto& buff   = minfo->buffer_;
      auto& header = buff.header_;

      std::array<char, BLOCK_SIZE> temp_buffer;
      ifs.read(temp_buffer.data(), temp_buffer.size());
      auto readSize    = ifs.gcount();
      int  compSize    = LZ4_compress_fast_continue(&minfo->lz4_stream_,
                                                temp_buffer.data(),
                                                buff.body_,
                                                readSize,
                                                temp_buffer.size(),
                                                1);
      header.size_     = readSize;
      header.eof_      = ifs.eof();
      header.compSize_ = compSize;
      minfo->trans_ -= header.size_;
      size_t send_size = sizeof(header) + compSize;
      // std::cout << "send size: " << send_size << ",body=" << compSize << "/"
      //           << header.size_ << std::endl;
      asio::async_write(socket_,
                        asio::buffer(&buff, send_size),
                        [this, minfo](auto& err, auto bytes) {
                          auto& h = minfo->buffer_.header_;
                          if (h.eof_)
                          {
                            on_send(minfo, err, bytes);
                            std::cout << "file size: " << minfo->header_.length_
                                      << std::endl;
                          }
                          else
                          {
                            on_send_header(minfo, err, bytes);
                          }
                        });
    }
  }
  void on_send(SendInfoPtr info, const boost::system::error_code& error,
               size_t bytes)
  {
    if (error)
    {
      std::cerr << "error[send body]: " << error.message() << std::endl;
      info->callback_(false);
    }
    else
    {
      info->callback_(true);
    }
    {
      std::lock_guard<std::mutex> l(que_lock_);
      send_que_.pop();
      if (!send_que_.empty())
      {
        io_service_.post([this]() { send_loop(); });
      }
    }
  }
};

} // namespace Network

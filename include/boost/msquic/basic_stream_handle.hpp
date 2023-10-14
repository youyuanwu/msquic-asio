#include "boost/msquic/basic_quic_handle.hpp"
#include "boost/msquic/event.hpp"

#include <oneshot.hpp>

#include <memory>
#include <mutex>
#include <semaphore>
#include <string_view>

namespace boost {
namespace msquic {

// forward declare
template <typename Executor> class basic_stream_handle;

namespace details {

using receive_t =
    std::pair<boost::system::error_code, std::vector<std::u8string_view>>;

template <typename Executor> class AsioServerStreamCtx {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;

  enum class state {
    ok,
    peer_shutdown, // peer direction will no longer deliver data
    peer_abort,
    shutdown_complete
  };

  struct pending_actions {
    bool start;
    bool send;
    // who ever arrive first needs to init the channel
    // frontend arrive first wait for backend.
    bool receive_frontend;
    // backend arrive first wait for frontend.
    bool receive_backend;
    bool shutdown;
  };

  AsioServerStreamCtx() : state_(state::ok), mtx_(), pending_() {}

  void set_state(state s) { state_ = s; }

  state get_state() { return state_; }

  // recieved buffers.
  // TODO: make without alloc
  std::vector<std::u8string_view> buf_vec_;

  // allow only 1 callback at a time
  std::mutex mtx_;
  // allow 1 task at a time;
  // std::binary_semaphore bs_;

  pending_actions pending_;

  oneshot::sender<boost::system::error_code> start_complete_tx_;
  oneshot::receiver<boost::system::error_code> start_complete_rx_;

  oneshot::sender<boost::system::error_code> send_tx_;
  oneshot::receiver<boost::system::error_code> send_rx_;

  oneshot::sender<receive_t> receive_tx_;
  oneshot::receiver<receive_t> receive_rx_;

  oneshot::sender<boost::system::error_code> shutdown_tx_;
  oneshot::receiver<boost::system::error_code> shutdown_rx_;

private:
  state state_;
};

template <typename Executor>
_IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_STREAM_CALLBACK) QUIC_STATUS QUIC_API
    AsioServerStreamCallback(_In_ HQUIC Stream, _In_opt_ void *Context,
                             _Inout_ QUIC_STREAM_EVENT *Event) noexcept {
  UNREFERENCED_PARAMETER(Stream);

  typedef Executor executor_type;
  typedef AsioServerStreamCtx<executor_type>::state state_type;

  boost::system::error_code ec = {};

  details::AsioServerStreamCtx<executor_type> *cpContext =
      (details::AsioServerStreamCtx<executor_type> *)Context;

  // allow 1 callback at a time, and synchronize with frontend calls.
  std::scoped_lock lock(cpContext->mtx_);

  QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
  switch (Event->Type) {
  case QUIC_STREAM_EVENT_START_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_START_COMPLETE");
    assert(cpContext->pending_.start);
    ec.assign(Event->START_COMPLETE.Status,
              boost::asio::error::get_system_category());
    cpContext->pending_.start = false;
    cpContext->start_complete_tx_.send(ec);
    break;
  case QUIC_STREAM_EVENT_SEND_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_SEND_COMPLETE");
    assert(cpContext->pending_.send);
    cpContext->pending_.send = false;
    cpContext->send_tx_.send(ec);
    break;
  case QUIC_STREAM_EVENT_RECEIVE: {
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_RECEIVE: len " +
                        std::to_string(Event->RECEIVE.TotalBufferLength));
    //
    // Data was received from the peer on the stream.
    //

    // init channels if arrived first
    assert(!cpContext->pending_.receive_backend);
    if (cpContext->pending_.receive_frontend) {
      cpContext->pending_.receive_frontend = false;
    } else {
      // backend arrive first
      cpContext->pending_.receive_backend = true;
      auto [tx, rx] =
          oneshot::create<std::pair<boost::system::error_code,
                                    std::vector<std::u8string_view>>>();
      cpContext->receive_tx_ = std::move(tx);
      cpContext->receive_rx_ = std::move(rx);
    }

    // Buffer pointers are valid until we call resume.
    std::vector<std::u8string_view> buffs;
    for (size_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
      const QUIC_BUFFER *buf = Event->RECEIVE.Buffers + i;
      // printf("[%p] %.*s\n",buf->Buffer, buf->Length, buf->Buffer);
      // add to buf vec
      std::u8string_view temp(reinterpret_cast<char8_t const *>(buf->Buffer),
                              buf->Length);
      buffs.push_back(std::move(temp));
    }
    cpContext->receive_tx_.send(std::make_pair(ec, std::move(buffs)));
    // cpContext->set_stream_event(ec);
    // return pending so that msquic will retain buffers, and we handle it in
    // asio.
    // The buffer array is not retained by msquic, so we copied to vector above.
    Status = QUIC_STATUS_PENDING;
  } break;
  case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN");
    cpContext->set_state(state_type::peer_shutdown);
    // start and read case has complete event notification
    // only handle receive here.
    if (cpContext->pending_.receive_frontend) {
      // frontend is waiting. This means no more data to receive.
      // send 0 len and success
      cpContext->pending_.receive_frontend = false;
      cpContext->receive_tx_.send(
          std::make_pair(ec, std::vector<std::u8string_view>{}));
    }
    if (cpContext->pending_.send || cpContext->pending_.receive_backend) {
      // this direction send to peer might continue.
      // backend queued receive can proceed.
    }
    //  The peer gracefully shut down its send direction of the stream.
  } break;
  case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_PEER_SEND_ABORTED");
    cpContext->set_state(state_type::peer_abort);
    // start and read case has complete event notification
    // only handle receive here.
    if (cpContext->pending_.receive_frontend) {
      // frontend is waiting. This means no more data to receive.
      // send 0 len and error
      ec.assign(net::error::basic_errors::connection_aborted,
                net::error::get_system_category());
      cpContext->pending_.receive_frontend = false;
      cpContext->receive_tx_.send(
          std::make_pair(ec, std::vector<std::u8string_view>{}));
    }
    // this direction for send should have been shutdown? TODO.
    assert(!cpContext->pending_.send);
    break;
  case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE");
    //
    // Both directions of the stream have been shut down and MsQuic is done
    // with the stream. It can now be safely cleaned up.
    //
    cpContext->set_state(state_type::shutdown_complete);
    if (cpContext->pending_.receive_frontend) {
      // frontend is waiting. This means no more data to receive.
      // send 0 len and error. TODO: figureout ec
      ec.assign(net::error::basic_errors::broken_pipe,
                net::error::get_system_category());
      cpContext->pending_.receive_frontend = false;
      cpContext->receive_tx_.send(
          std::make_pair(ec, std::vector<std::u8string_view>{}));
    }
    if (cpContext->pending_.shutdown) {
      // front end must have inited the channel
      cpContext->pending_.shutdown = false;
      ec.clear();
      cpContext->shutdown_tx_.send(ec);
    }
    assert(!cpContext->pending_.send);
    assert(!cpContext->pending_.receive_frontend);
    assert(!cpContext->pending_.receive_backend);
    break;
  default:
    break;
  }
  return Status;
}
} // namespace details

template <typename Executor = net::any_io_executor>
class basic_stream_handle : public basic_quic_handle<Executor> {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  typedef details::AsioServerStreamCtx<executor_type>::state state_type;

  basic_stream_handle(const executor_type &ex, const QUIC_API_TABLE *api)
      : basic_quic_handle<executor_type>(ex, api),
        ctx_(std::make_unique<details::AsioServerStreamCtx<executor_type>>()) {}

  ~basic_stream_handle() { this->close(); }

  // after move the other stream handle should become empty just after
  // constructor. this stream owns other's handle.
  basic_stream_handle(basic_stream_handle &&other)
      : basic_quic_handle<executor_type>(std::move(other)),
        ctx_(std::move(other.ctx_)) {}

  void set_callback() {
    assert(this->is_open());
    this->api_->SetCallbackHandler(
        this->h_, (void *)details::AsioServerStreamCallback<executor_type>,
        ctx_.get());
  }

  // client open stream:
  void open(_In_ _Pre_defensive_ HQUIC Connection,
            _In_ QUIC_STREAM_OPEN_FLAGS Flags,
            _Out_ boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status = this->api_->StreamOpen(
        Connection, Flags, details::AsioServerStreamCallback<executor_type>,
        ctx_.get(), &this->h_);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
    }
  }

  // client start
  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_start(_In_ QUIC_STREAM_START_FLAGS Flags,
              BOOST_ASIO_MOVE_ARG(Token)
                  token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    boost::system::error_code ec = {};

    // backend callback is guaranteed to have valid tx.
    // start is intended to be called once only during stream's lifetime
    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->start_complete_tx_ = std::move(tx);
    ctx_->start_complete_rx_ = std::move(rx);

    assert(!ctx_->pending_.start);
    ctx_->pending_.start = true;

    QUIC_STATUS Status = this->api_->StreamStart(this->h_, Flags);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
      ctx_->pending_.start = false;
      ctx_->start_complete_tx_.send(ec);
    }
    return net::async_initiate<decltype(token),
                               void(boost::system::error_code)>(
        [this](auto handler) {
          ctx_->start_complete_rx_.async_wait(
              [h = std::move(handler),
               this](boost::system::error_code ec) mutable {
                // if oneshot has error retain it, else use msquic callback
                // error sent from backend
                if (!ec.failed()) {
                  ec = ctx_->start_complete_rx_.get();
                }
                // TODO: can clean up the channels here.
                std::move(h)(ec);
              });
        },
        token);
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code,
                                                 std::size_t))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code,
                                                 std::size_t))
  async_send(_In_reads_(BufferCount)
                 _Pre_defensive_ const QUIC_BUFFER *const Buffers,
             _In_ uint32_t BufferCount, _In_ QUIC_SEND_FLAGS Flags,
             _In_opt_ void *ClientSendContext,
             BOOST_ASIO_MOVE_ARG(Token)
                 token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {

    // channel is made per async send call.
    // when backend callback is excecuted the tx is always available.
    // There is no support for multiple send in parallel.
    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->send_tx_ = std::move(tx);
    ctx_->send_rx_ = std::move(rx);

    assert(!ctx_->pending_.send);
    ctx_->pending_.send = true;

    boost::system::error_code ec = {};
    QUIC_STATUS Status = this->api_->StreamSend(this->h_, Buffers, BufferCount,
                                                Flags, ClientSendContext);
    std::size_t len = 0;
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
      // event handler should be triggered
      // we launch the compose op but it completes immediately
      ctx_->pending_.send = false;
      ctx_->send_tx_.send(ec);
    } else {
      // count the byte len of all buffers.
      // all buffers are queued in to msquic, and considered success.
      for (std::size_t i = 0; i < BufferCount; i++) {
        const QUIC_BUFFER *const b = Buffers + i;
        len += b->Length;
      }
    }
    return net::async_initiate<decltype(token),
                               void(boost::system::error_code, std::size_t)>(
        [this, len](auto handler) {
          ctx_->send_rx_.async_wait([h = std::move(handler), this, len](
                                        boost::system::error_code ec) mutable {
            // if oneshot has error retain it, else use msquic callback error
            // sent from backend
            if (ec.failed()) {
              std::move(h)(ec, 0);
              return;
            }
            ec = ctx_->send_rx_.get();
            std::move(h)(ec, len);
          });
        },
        token);
  }

  // receive works because we always use partial return to callback
  // so that only 1 receive can be pending at a time.
  template <typename Token>
  auto async_receive(_Out_ LPVOID lpBuffer, _In_ std::size_t bufferLen,
                     Token &&token) {
    // since receive event can arrive without frontend action, it needs to be
    // synchronized.
    std::scoped_lock lock(ctx_->mtx_);

    // frontend flag should be unset in empty state.
    assert(!ctx_->pending_.receive_frontend);
    if (ctx_->pending_.receive_backend) {
      // backend arrives first, this means backend init channels already.
      // read off the already sent values.
      ctx_->pending_.receive_backend = false;
    } else {
      // frontend arrives first.

      auto [tx, rx] =
          oneshot::create<std::pair<boost::system::error_code,
                                    std::vector<std::u8string_view>>>();
      ctx_->receive_tx_ = std::move(tx);
      ctx_->receive_rx_ = std::move(rx);
      // handle stream state.
      if (ctx_->get_state() != state_type::ok) {
        boost::system::error_code ec;
        if (ctx_->get_state() == state_type::peer_shutdown) {
          // error is success and len is 0;
        } else if (ctx_->get_state() == state_type::peer_abort) {
          ec = net::error::make_error_code(
              net::error::basic_errors::connection_aborted);
        } else if (ctx_->get_state() == state_type::shutdown_complete) {
          ec = net::error::make_error_code(
              net::error::basic_errors::broken_pipe);
        }
        ctx_->receive_tx_.send(ec, std::vector<std::u8string_view>{});
      } else {
        // backend has work todo.
        ctx_->pending_.receive_frontend = true;
      }
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code, std::size_t)>(
        [this, lpBuffer, bufferLen](auto handler) {
          ctx_->receive_rx_.async_wait(
              [h = std::move(handler), this, lpBuffer,
               bufferLen](boost::system::error_code ec) mutable {
                // if oneshot has error retain it, else use msquic callback
                // error sent from backend
                if (ec.failed()) {
                  std::move(h)(ec, 0);
                  return;
                }
                // get buffers sent from backend
                auto [ec2, buffs] = ctx_->receive_rx_.get();
                // copy buffers into out buffer. make a temp variable to advance
                // during copy
                std::size_t remaining = bufferLen; // remaining size of outbuff
                std::size_t copied = 0;            // bytes copied already
                for (auto &buff : buffs) {
                  // printf("Copy buffer [%p] %d\n", b.Buffer, b.Length);
                  std::size_t to_copy = std::min(remaining, buff.size());
                  if (to_copy == 0) {
                    continue;
                  }
                  errno_t ec_cp =
                      memcpy_s(lpBuffer, remaining, buff.data(), to_copy);
                  assert(ec_cp == 0);
                  DBG_UNREFERENCED_LOCAL_VARIABLE(ec_cp);
                  remaining -= to_copy;
                  copied += to_copy;
                }
                assert(copied <= bufferLen);
                // inform msquic to receive next
                this->receive_complete(copied);
                std::move(h)(ec2, copied);
              });
        },
        token);
  }

  void receive_complete(std::size_t len) {
    this->api_->StreamReceiveComplete(this->h_, len);
  }

  template <typename Token>
  auto async_shutdown(_In_ QUIC_STREAM_SHUTDOWN_FLAGS Flags,
                      _In_ _Pre_defensive_ QUIC_UINT62
                          ErrorCode, // Application defined error code
                      Token &&token) {
    assert(this->is_open());
    std::scoped_lock lock(ctx_->mtx_);

    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->shutdown_tx_ = std::move(tx);
    ctx_->shutdown_rx_ = std::move(rx);

    boost::system::error_code ec;

    // shutdown is not triggered from front end so need to lock it.
    if (ctx_->get_state() == state_type::shutdown_complete) {
      // already shutdown
      ctx_->shutdown_tx_.send(ec);
    } else {
      QUIC_STATUS Status =
          this->api_->StreamShutdown(this->h_, Flags, ErrorCode);
      if (QUIC_FAILED(Status)) {
        ec.assign(Status, boost::asio::error::get_system_category());
        ctx_->shutdown_tx_.send(ec);
      } else {
        ctx_->pending_.shutdown = true;
      }
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code)>(
        [this](auto handler) {
          ctx_->shutdown_rx_.async_wait(
              [h = std::move(handler),
               this](boost::system::error_code ec) mutable {
                // if oneshot has error retain it, else use msquic callback
                // error sent from backend
                if (!ec.failed()) {
                  ec = ctx_->shutdown_rx_.get();
                }
                // TODO: can clean up the channels here.
                std::move(h)(ec);
              });
        },
        token);
  }

  void close() {
    if (this->is_open()) {
      this->api_->StreamClose(this->h_);
      this->h_ = nullptr;
    }
  }

  details::AsioServerStreamCtx<executor_type> *get_ctx() { return ctx_.get(); }

private:
  // use pointer to make easier move and ctx passing in msquic
  std::unique_ptr<details::AsioServerStreamCtx<executor_type>> ctx_;
};

} // namespace msquic
} // namespace boost
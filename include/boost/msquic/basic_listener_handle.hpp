#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/coroutine.hpp>
// #include <msquic.hpp>

#include "boost/msquic/basic_config_handle.hpp"
#include "boost/msquic/basic_connection_handle.hpp"
#include "boost/msquic/basic_quic_handle.hpp"

#include "oneshot.hpp"

namespace boost {
namespace msquic {

namespace net = boost::asio;

namespace details {

template <typename Executor> class AsioServerListenerCtx {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  struct pending_actions {
    bool accept;
    bool stop;
  };

  enum class state { ok, stopped };

  AsioServerListenerCtx(const executor_type &ex, const QUIC_API_TABLE *api,
                        basic_config_handle<executor_type> &config)
      : ex_(ex), api_(api), config_(config), accept_bs_(0), state_(state::ok),
        pending_() {
    assert(api != nullptr);
  }

  AsioServerListenerCtx(AsioServerListenerCtx &&other) =
      delete; // not supported for now

  executor_type get_executor() { return ex_; }

  executor_type ex_;

  basic_config_handle<executor_type> &config_;

  std::mutex mtx_;

  pending_actions pending_;

  state state_;

  // semaphore for accept.
  // the first accept does not need semaphore if start of listener is called
  // after accept is initiated. But the subsquent new connection needs this to
  // synchonize.
  std::binary_semaphore accept_bs_;

  oneshot::sender<std::pair<boost::system::error_code,
                            basic_connection_handle<executor_type>>>
      accept_tx_;
  oneshot::receiver<std::pair<boost::system::error_code,
                              basic_connection_handle<executor_type>>>
      accept_rx_;

  oneshot::sender<boost::system::error_code> stop_tx_;
  oneshot::receiver<boost::system::error_code> stop_rx_;

  const QUIC_API_TABLE *api_;
};

//
// The server's callback for listener events from MsQuic.
//
template <typename Executor = net::any_io_executor>
_IRQL_requires_max_(PASSIVE_LEVEL)
    _Function_class_(QUIC_LISTENER_CALLBACK) QUIC_STATUS QUIC_API
    AsioServerListenerCallback(_In_ HQUIC Listener, _In_opt_ void *Context,
                               _Inout_ QUIC_LISTENER_EVENT *Event) {
  typedef Executor executor_type;
  typedef details::AsioServerListenerCtx<executor_type>::state state_type;
  UNREFERENCED_PARAMETER(Listener);
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Event);
  assert(Context != nullptr);
  AsioServerListenerCtx<executor_type> *cpContext;
  cpContext = (AsioServerListenerCtx<executor_type> *)Context;

  // allow 1 callback at a time
  std::unique_lock lock(cpContext->mtx_);

  boost::system::error_code ec = {};

  DBG_UNREFERENCED_LOCAL_VARIABLE(cpContext);

  QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
  switch (Event->Type) {
  case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
    lock.unlock(); // let frontend proceed.
    // wait for front end.
    cpContext->accept_bs_.acquire();
    MSQUIC_ASIO_MESSAGE("QUIC_LISTENER_EVENT_NEW_CONNECTION");
    assert(cpContext->pending_.accept);
    cpContext->pending_.accept = false;
    //
    // A new connection is being attempted by a client. For the handshake to
    // proceed, the server must provide a configuration for QUIC to use. The
    // app MUST set the callback handler before returning.
    assert(Event->NEW_CONNECTION.Connection != nullptr);
    // cpContext->init_conn_holder(Event->NEW_CONNECTION.Connection);
    // cpContext->set_conn_event(ec);
    // make a new conn
    basic_connection_handle<executor_type> conn(cpContext->ex_,
                                                cpContext->api_);
    conn.assign(Event->NEW_CONNECTION.Connection);
    conn.set_callback();
    conn.set_config(cpContext->config_.native_handle());
    cpContext->accept_tx_.send(ec, std::move(conn));

  } break;
  case QUIC_LISTENER_EVENT_STOP_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_LISTENER_EVENT_STOP_COMPLETE");
    cpContext->state_ = state_type::stopped;
    ec.assign(net::error::basic_errors::shut_down,
              boost::asio::error::get_system_category());
    if (cpContext->pending_.accept) {
      cpContext->pending_.accept = false;
      basic_connection_handle<executor_type> dummy(cpContext->ex_,
                                                   cpContext->api_);
      cpContext->accept_tx_.send(ec, std::move(dummy));
    }
    if (cpContext->pending_.stop) {
      cpContext->pending_.stop = false;
      ec.clear();
      cpContext->stop_tx_.send(ec);
    }
    break;
  default:
    assert(false);
    break;
  }
  return Status;
}

} // namespace details

template <typename Executor = net::any_io_executor>
class basic_listener_handle : public basic_quic_handle<Executor> {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;
  typedef details::AsioServerListenerCtx<executor_type>::state state_type;

  basic_listener_handle(const executor_type &ex, const QUIC_API_TABLE *api,
                        basic_config_handle<executor_type> &config)
      : basic_quic_handle<executor_type>(ex, api),
        ctx_(std::make_unique<details::AsioServerListenerCtx<executor_type>>(
            ex, api, config)) {}

  ~basic_listener_handle() { this->close(); }

  basic_listener_handle(basic_listener_handle &&) = delete; // not yet supported

  void open(native_handle_type registration,
            boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status = this->api_->ListenerOpen(
        registration, details::AsioServerListenerCallback<executor_type>,
        (void *)ctx_.get(), &this->h_);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
    }
  }

  void start(const QUIC_BUFFER &Alpn, const QUIC_ADDR &Address,
             boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status =
        this->api_->ListenerStart(this->h_, &Alpn, 1 /*alpn count*/, &Address);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
    }
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(
      void(boost::system::error_code, basic_connection_handle<executor_type>))
                MoveAcceptToken BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(
                    executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
      MoveAcceptToken,
      void(boost::system::error_code, basic_connection_handle<executor_type>))
  async_accept(BOOST_ASIO_MOVE_ARG(MoveAcceptToken)
                   token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {

    std::scoped_lock lock(ctx_->mtx_);
    assert(!ctx_->pending_.accept);
    auto [tx, rx] =
        oneshot::create<std::pair<boost::system::error_code,
                                  basic_connection_handle<executor_type>>>();
    ctx_->accept_tx_ = std::move(tx);
    ctx_->accept_rx_ = std::move(rx);
    if (ctx_->state_ == state_type::stopped) {
      boost::system::error_code ec;
      ec.assign(net::error::connection_aborted,
                boost::asio::error::get_system_category());
      basic_connection_handle<executor_type> dummy(this->ex_, this->api_);
      ctx_->accept_tx_.send(ec, std::move(dummy));
    } else {
      ctx_->pending_.accept = true;
      // backend can go ahead.
      ctx_->accept_bs_.release();
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code,
                                    basic_connection_handle<executor_type>)>(
        [this](auto handler) {
          ctx_->accept_rx_.async_wait(
              [h = std::move(handler),
               this](boost::system::error_code ec) mutable {
                // if oneshot has error retain it, else use msquic callback
                // error sent from backend
                if (ec.failed()) {
                  basic_connection_handle<executor_type> dummy(this->ex_,
                                                               this->api_);
                  std::move(h)(ec, std::move(dummy));
                  return;
                }
                auto [ec2, conn] = std::move(ctx_->accept_rx_.get());
                std::move(h)(ec2, std::move(conn));
              });
        },
        token);
  }

  template <typename Token> auto async_stop(Token &&token) {
    std::unique_lock lock(ctx_->mtx_);
    assert(!ctx_->pending_.stop);

    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->stop_tx_ = std::move(tx);
    ctx_->stop_rx_ = std::move(rx);

    if (ctx_->state_ == state_type::stopped) {
      // already shutdown
      ctx_->stop_tx_.send(boost::system::error_code{});
    } else {
      // shutdown returns void
      ctx_->pending_.stop = true;
      // This will wait for stop callback to finish, so we need to release lock
      // here.
      lock.unlock();
      this->api_->ListenerStop(this->h_);
    }
    return net::async_initiate<decltype(token),
                               void(boost::system::error_code)>(
        [this](auto handler) {
          ctx_->stop_rx_.async_wait([h = std::move(handler), this](
                                        boost::system::error_code ec) mutable {
            // if oneshot has error retain it, else use msquic callback
            // error sent from backend
            if (!ec.failed()) {
              ec = ctx_->stop_rx_.get();
            }
            // TODO: can clean up the channels here.
            std::move(h)(ec);
          });
        },
        token);
  }

  void close() {
    if (this->is_open()) {
      this->api_->ListenerClose(this->h_);
      this->h_ = nullptr;
    }
  }

private:
  std::unique_ptr<details::AsioServerListenerCtx<executor_type>> ctx_;
};

} // namespace msquic
} // namespace boost
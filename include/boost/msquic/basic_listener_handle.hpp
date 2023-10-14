#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/compose.hpp>
#include <boost/asio/coroutine.hpp>
// #include <msquic.hpp>

#include "boost/msquic/basic_config_handle.hpp"
#include "boost/msquic/basic_connection_handle.hpp"
#include "boost/msquic/basic_quic_handle.hpp"

#include "boost/msquic/event.hpp"

namespace boost {
namespace msquic {

namespace net = boost::asio;

namespace details {

template <typename Executor> class AsioServerListenerCtx {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  AsioServerListenerCtx(const executor_type &ex, const QUIC_API_TABLE *api,
                        basic_config_handle<executor_type> &config)
      : ex_(ex), ch_(ex, api), ev_(ex), config_(config), accept_bs_(0) {
    assert(api != nullptr);
  }

  AsioServerListenerCtx(AsioServerListenerCtx &&other) =
      delete; // not supported for now

  executor_type get_executor() { return ex_; }

  basic_connection_handle<executor_type> &get_conn_holder() { return ch_; }

  // incomming connection handle provided in the callback
  void init_conn_holder(native_handle_type h) {
    // assign handle
    ch_.assign(h);
    // set callback
    // if we set callback here it can be immediately triggered???
    ch_.set_callback();
    // set config
    ch_.set_config(this->config_.native_handle());
  }

  void set_conn_event(boost::system::error_code ec) {
    ec_ = ec;
    ev_.set();
  }

  void reset_conn_event() {
    ec_.clear();
    ev_.reset();
  }

  executor_type ex_;
  // placeholder connection for accept. Intended for std::move to handlers.
  basic_connection_handle<executor_type> ch_;

  basic_config_handle<executor_type> &config_;

  event<executor_type> ev_;

  // error code saved from callback
  boost::system::error_code ec_;

  // semaphore for accept.
  // the first accept does not need semaphore if start of listener is called
  // after accept is initiated. But the subsquent new connection needs this to
  // synchonize.
  std::binary_semaphore accept_bs_;
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
  UNREFERENCED_PARAMETER(Listener);
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Event);
  assert(Context != nullptr);
  AsioServerListenerCtx<executor_type> *cpContext;
  cpContext = (AsioServerListenerCtx<executor_type> *)Context;
  boost::system::error_code ec = {};

  DBG_UNREFERENCED_LOCAL_VARIABLE(cpContext);

  QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
  switch (Event->Type) {
  case QUIC_LISTENER_EVENT_NEW_CONNECTION:
    // wait for front end.
    cpContext->accept_bs_.acquire();
    MSQUIC_ASIO_MESSAGE("QUIC_LISTENER_EVENT_NEW_CONNECTION");
    //
    // A new connection is being attempted by a client. For the handshake to
    // proceed, the server must provide a configuration for QUIC to use. The
    // app MUST set the callback handler before returning.
    assert(Event->NEW_CONNECTION.Connection != nullptr);
    cpContext->init_conn_holder(Event->NEW_CONNECTION.Connection);
    cpContext->set_conn_event(ec);

    break;
  case QUIC_LISTENER_EVENT_STOP_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_LISTENER_EVENT_STOP_COMPLETE");
    ec.assign(net::error::basic_errors::shut_down,
              boost::asio::error::get_system_category());
    cpContext->set_conn_event(ec);
    break;
  default:
    assert(false);
    break;
  }
  return Status;
}

// handler signature: void(ec, conn)
template <typename Executor>
class async_move_accept_op : boost::asio::coroutine {
public:
  async_move_accept_op(basic_connection_handle<Executor> *conn,
                       event<Executor> *ev, boost::system::error_code *ctx_ec)
      : conn_(conn), ev_(ev), ctx_ec_(ctx_ec) {}

  template <typename Self>
  void operator()(Self &self, boost::system::error_code ec = {}) {
    if (ec) {
      self.complete(ec, std::move(*conn_));
      return;
    }
    ev_->async_wait([self = std::move(self), c = ctx_ec_,
                     conn = conn_](boost::system::error_code ec) mutable {
      assert(!ec.failed());
      DBG_UNREFERENCED_LOCAL_VARIABLE(ec);
      // pass the ctx_ec and move connection to the handler
      self.complete(*c, std::move(*conn));
    });
  }

private:
  basic_connection_handle<Executor> *conn_;
  event<Executor> *ev_;
  // ec shared and set in the callback
  boost::system::error_code *ctx_ec_;
};

} // namespace details

template <typename Executor = net::any_io_executor>
class basic_listener_handle : public basic_quic_handle<Executor> {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  basic_listener_handle(const executor_type &ex, const QUIC_API_TABLE *api,
                        basic_config_handle<executor_type> &config)
      : basic_quic_handle<executor_type>(ex, api), ctx_(ex, api, config) {}

  ~basic_listener_handle() { this->close(); }

  basic_listener_handle(basic_listener_handle &&) = delete; // not yet supported

  void open(native_handle_type registration,
            boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status = this->api_->ListenerOpen(
        registration, details::AsioServerListenerCallback<executor_type>,
        (void *)&ctx_, &this->h_);
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

  // TODO: implement connection first and then this async accept

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(
      void(boost::system::error_code, basic_connection_handle<executor_type>))
                MoveAcceptToken BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(
                    executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(
      MoveAcceptToken,
      void(boost::system::error_code, basic_connection_handle<executor_type>))
  async_accept(BOOST_ASIO_MOVE_ARG(MoveAcceptToken)
                   token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {

    this->ctx_.reset_conn_event();

    auto temp = boost::asio::async_compose<
        MoveAcceptToken, void(boost::system::error_code,
                              basic_connection_handle<executor_type>)>(
        details::async_move_accept_op<executor_type>(
            &this->ctx_.ch_, &this->ctx_.ev_, &this->ctx_.ec_),
        token, this->get_executor());
    this->ctx_.accept_bs_.release();
    return std::move(temp);
  }

  void close() {
    if (this->is_open()) {
      this->api_->ListenerClose(this->h_);
      this->h_ = nullptr;
    }
  }

private:
  details::AsioServerListenerCtx<executor_type> ctx_;
};

} // namespace msquic
} // namespace boost
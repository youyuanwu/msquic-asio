#pragma once

#include "boost/msquic/basic_stream_handle.hpp"
#include "boost/msquic/event.hpp"
#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <mutex>
#include <semaphore>

namespace boost {
namespace msquic {
// connection accepts streams.

namespace details {
template <typename Executor> class AsioServerConnectionCtx {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  enum class state {
    idle,
    connect,
    accept,
    peer_shutdown,
    transport_shutdown,
    shutdown_complete,
    done,
    error
  };

  AsioServerConnectionCtx(const executor_type &ex, const QUIC_API_TABLE *api)
      : sh_(ex, api), ev_(ex), ec_(), state_(state::idle), mtx_(), bs_(0) {}

  AsioServerConnectionCtx(AsioServerConnectionCtx &&other) =
      delete; // not needed since we use ptr

  void set_conn_event(boost::system::error_code ec) {
    ec_ = ec;
    ev_.set();
  }

  void reset_conn_event() {
    ec_.clear();
    ev_.reset();
  }

  // init the stream holder. pass the stream handle.
  void init_stream_holder(native_handle_type h) {
    sh_.assign(h);
    sh_.set_callback();
  }

  state get_state() { return state_; }
  void set_state(state s) { state_ = s; }

  // stream holder
  basic_stream_handle<executor_type> sh_;
  event<executor_type> ev_;

  // error code saved from callback
  boost::system::error_code ec_;

  // allow one callback at a time.
  std::mutex mtx_;
  // every front end call releases a semaphore token.
  std::binary_semaphore bs_;

private:
  state state_;
};

template <typename Executor>
_IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_CONNECTION_CALLBACK) QUIC_STATUS QUIC_API
    AsioMsQuicConnectionCallback(_In_ HQUIC Connection, _In_opt_ void *Context,
                                 _Inout_ QUIC_CONNECTION_EVENT *Event) {
  typedef Executor executor_type;
  typedef AsioServerConnectionCtx<executor_type>::state state_type;

  assert(Context != nullptr);
  AsioServerConnectionCtx<executor_type> *cpContext =
      (AsioServerConnectionCtx<executor_type> *)Context;

  UNREFERENCED_PARAMETER(Connection);

  // allow 1 callback at a time
  std::scoped_lock lock(cpContext->mtx_);

  boost::system::error_code ec = {};

  switch (Event->Type) {
  case QUIC_CONNECTION_EVENT_CONNECTED:
    // wait semaphore to be 0
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_CONNECTED");
    //
    // The handshake has completed for the connection.
    //
    if (cpContext->get_state() != state_type::connect) {
      // connection is in wrong state
      // this is is a bug in this lib
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
    }
    cpContext->set_conn_event(ec);
    break;
  case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT");

    if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
        QUIC_STATUS_CONNECTION_IDLE) {
      // printf("[conn][%p] Successfully shut down on idle.\n", Connection);
      BOOST_TEST_MESSAGE(
          "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT idle");
    } else {
      // printf("[conn][%p] Shut down by transport, 0x%x\n", Connection,
      //        Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
      BOOST_TEST_MESSAGE(
          "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT error");
    }
    //
    // The connection has been shut down by the transport. Generally, this
    // is the expected way for the connection to shut down with this
    // protocol, since we let idle timeout kill the connection.
    //
    if (cpContext->get_state() == state_type::transport_shutdown) {
      // wait for front end
      cpContext->set_conn_event(ec);
    } else {
      // connection is in wrong state
      // this is is a bug in this lib
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_conn_event(ec);
    }
    break;
  case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER");
    //
    // The connection was explicitly shut down by the peer.
    //
    // printf("[conn][%p] Shut down by peer, 0x%llu\n", Connection,
    //        (unsigned long long)Event->SHUTDOWN_INITIATED_BY_PEER.ErrorCode);
    // final shutdown has not happend yet.

    if (cpContext->get_state() == state_type::peer_shutdown) {
      // wait for front end.
      // no error. since frontend is waiting for peer shutdown.
      cpContext->set_conn_event(ec);
    } else {
      // frontend is doing other operation.
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_conn_event(ec);
    }
    break;
  case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE");
    //
    // The connection has completed the shutdown process and is ready to be
    // safely cleaned up.
    //
    // printf("[conn][%p] All done\n", Connection);
    // MsQuic->ConnectionClose(Connection);
    if (cpContext->get_state() == state_type::shutdown_complete) {
      cpContext->set_conn_event(ec);
    } else {
      // frontend is doing other operation.
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_conn_event(ec);
    }
    break;
  case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED");
    //
    // The peer has started/created a new stream. The app MUST set the
    // callback handler before returning.
    //
    // printf("[strm][%p] Peer started ctx [%p]\n",
    //        Event->PEER_STREAM_STARTED.Stream, Context);
    // MsQuic->SetCallbackHandler(Event->PEER_STREAM_STARTED.Stream,
    // (void*)ServerStreamCallback, NULL);
    if (cpContext->get_state() != state_type::accept) {
      // bug in this lib
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
    } else {
      // prepare stream
      assert(Event->PEER_STREAM_STARTED.Stream != nullptr);
      cpContext->init_stream_holder(Event->PEER_STREAM_STARTED.Stream);
    }
    cpContext->set_conn_event(ec);
    break;
  case QUIC_CONNECTION_EVENT_RESUMED:
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_RESUMED");
    // not supported
    assert(false);
    //
    // The connection succeeded in doing a TLS resumption of a previous
    // connection's session.
    //
    // printf("[conn][%p] Connection resumed!\n", Connection);

    // front end should be in async_resume case
    break;
  case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
    // ignore for now
    BOOST_TEST_MESSAGE("QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED");
    break;
  default:
    break;
  }
  return QUIC_STATUS_SUCCESS;
}

// handler signature: void(ec, conn)
template <typename Executor>
class async_stream_move_accept_op : boost::asio::coroutine {
public:
  async_stream_move_accept_op(basic_stream_handle<Executor> *conn,
                              event<Executor> *ev,
                              boost::system::error_code *ctx_ec)
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
  basic_stream_handle<Executor> *conn_;
  event<Executor> *ev_;
  // ec shared and set in the callback
  boost::system::error_code *ctx_ec_;
};

} // namespace details

// for each callback event case, make a async_xxx version
// if client send triggers different callback event or shutdown, pass it in ec
// argument

// async_connect
// async_accept(stream)

template <typename Executor = net::any_io_executor>
class basic_connection_handle : public basic_quic_handle<Executor> {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;
  typedef details::AsioServerConnectionCtx<executor_type>::state state_type;

  basic_connection_handle(const executor_type &ex, const QUIC_API_TABLE *api)
      : basic_quic_handle<executor_type>(ex, api),
        ctx_(std::make_unique<details::AsioServerConnectionCtx<executor_type>>(
            ex, api)) {}

  // after move the other connection handle should become empty just after
  // constructor. this connection owns other's handle.
  basic_connection_handle(basic_connection_handle &&other)
      : basic_quic_handle<executor_type>(std::move(other)),
        ctx_(std::move(other.ctx_)) {
    // the moved from conn needs to have a new ctx
    // because it is a conn place holder, and needs to be reused.
    other.ctx_ =
        std::make_unique<details::AsioServerConnectionCtx<executor_type>>(
            this->ex_, this->api_);
  }

  ~basic_connection_handle() { this->close(); }

  // used by server
  // needs to assgn conn handle first
  void set_callback() {
    assert(this->is_open());
    this->api_->SetCallbackHandler(
        this->h_, (void *)details::AsioMsQuicConnectionCallback<executor_type>,
        (void *)ctx_.get());
  }

  void set_config(native_handle_type config) {
    assert(this->is_open());
    this->api_->ConnectionSetConfiguration(this->h_, config);
  }

  void send_resumption_ticket(boost::system::error_code &_Out_ ec) {
    assert(this->api_ != nullptr);
    assert(this->h_ != nullptr);
    // todo: add params
    QUIC_STATUS Status = this->api_->ConnectionSendResumptionTicket(
        this->h_, QUIC_SEND_RESUMPTION_FLAG_NONE, 0, NULL);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
    }
  }

  void close() {
    if (this->is_open()) {
      this->api_->ConnectionClose(this->h_);
      this->h_ = nullptr;
    }
  }

  // client open connection
  void open(_In_ native_handle_type Registration,
            _Out_ boost::system::error_code &ec) {
    assert(!this->is_open());
    QUIC_STATUS Status = this->api_->ConnectionOpen(
        Registration, details::AsioMsQuicConnectionCallback<executor_type>,
        (void *)ctx_.get(), &this->h_);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
    }
  }

  // client start connection
  void start(_In_ _Pre_defensive_ HQUIC Configuration,
             _In_ QUIC_ADDRESS_FAMILY Family,
             _In_reads_or_z_opt_(QUIC_MAX_SNI_LENGTH) const char *ServerName,
             _In_ uint16_t ServerPort, // Host byte order
             _Out_ boost::system::error_code &ec) {
    assert(this->is_open());
    QUIC_STATUS Status = this->api_->ConnectionStart(
        this->native_handle(), Configuration, Family, ServerName, ServerPort);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
    }
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_connect(BOOST_ASIO_MOVE_ARG(Token)
                    token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    // backend is blocked until semaphore is decremented.
    // wait for connect to happen in callback
    ctx_->set_state(state_type::connect);
    ctx_->reset_conn_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    // add a task so that backend can pickup.
    ctx_->bs_.release();
    return std::move(temp);
  }

  // don't use this if peer does not send shutdown, it will stuck msquic backend
  // thread.
  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_wait_peer_shutdown(BOOST_ASIO_MOVE_ARG(
      Token) token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    // wait for peer shutdown to happen in callback
    ctx_->set_state(state_type::peer_shutdown);
    ctx_->reset_conn_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_wait_transport_shutdown(BOOST_ASIO_MOVE_ARG(
      Token) token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    // wait for peer shutdown to happen in callback
    ctx_->set_state(state_type::transport_shutdown);
    ctx_->reset_conn_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_wait_shutdown_complete(BOOST_ASIO_MOVE_ARG(
      Token) token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    ctx_->set_state(state_type::shutdown_complete);
    ctx_->reset_conn_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(
      void(boost::system::error_code, basic_stream_handle<executor_type>))
                MoveAcceptToken BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(
                    executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(MoveAcceptToken,
                                     void(boost::system::error_code,
                                          basic_stream_handle<executor_type>))
  async_accept(BOOST_ASIO_MOVE_ARG(MoveAcceptToken)
                   token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    // backend is waiting on semaphore
    this->ctx_->set_state(state_type::accept);
    this->ctx_->reset_conn_event();

    auto temp =
        boost::asio::async_compose<MoveAcceptToken,
                                   void(boost::system::error_code,
                                        basic_stream_handle<executor_type>)>(
            details::async_stream_move_accept_op<executor_type>(
                &this->ctx_->sh_, &this->ctx_->ev_, &this->ctx_->ec_),
            token, this->get_executor());
    // release a task to unblock backend
    this->ctx_->bs_.release();
    return std::move(temp);
  }

private:
  // use ptr to facilitate the ctx passed to msquic callback.
  // otherwise need to set callback ctx multiple times during move.
  std::unique_ptr<details::AsioServerConnectionCtx<executor_type>> ctx_;
};

} // namespace msquic
} // namespace boost
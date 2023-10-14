#pragma once

#include "boost/msquic/basic_stream_handle.hpp"
#include <boost/asio/any_io_executor.hpp>
#include <oneshot.hpp>

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
    ok,
    peer_shutdown,
    transport_shutdown,
    shutdown_complete,
  };

  // actions from frontend
  struct pending_actions {
    bool client_start; // connect start triggered from front end
    bool server_connect;
    bool accept;
    bool shutdown;
  };

  AsioServerConnectionCtx(const executor_type &ex, const QUIC_API_TABLE *api)
      : state_(state::ok), mtx_(), accept_bs_(0), pending_(), ex_(ex),
        api_(api) {}

  AsioServerConnectionCtx(AsioServerConnectionCtx &&other) =
      delete; // not needed since we use ptr

  state get_state() { return state_; }
  void set_state(state s) { state_ = s; }

  // allow one callback at a time.
  std::mutex mtx_;

  pending_actions pending_;

  // client -> async_start
  // server -> async_connect
  oneshot::sender<boost::system::error_code> conn_tx_;
  oneshot::receiver<boost::system::error_code> conn_rx_;

  // accept can only do one at a time. front end needs to arrive first.
  std::binary_semaphore accept_bs_;
  oneshot::sender<
      std::pair<boost::system::error_code, basic_stream_handle<executor_type>>>
      accept_tx_;
  oneshot::receiver<
      std::pair<boost::system::error_code, basic_stream_handle<executor_type>>>
      accept_rx_;

  oneshot::sender<boost::system::error_code> shutdown_tx_;
  oneshot::receiver<boost::system::error_code> shutdown_rx_;

  executor_type ex_;
  const QUIC_API_TABLE *api_;

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
  std::unique_lock lock(cpContext->mtx_);

  boost::system::error_code ec = {};

  switch (Event->Type) {
  case QUIC_CONNECTION_EVENT_CONNECTED:
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_CONNECTED");
    //
    // The handshake has completed for the connection.
    //
    // only client mode xor server mode
    assert(cpContext->pending_.client_start ||
           cpContext->pending_.server_connect);
    assert(!(cpContext->pending_.client_start &&
             cpContext->pending_.server_connect));
    if (cpContext->pending_.client_start) {
      cpContext->pending_.client_start = false;
      cpContext->conn_tx_.send(ec);
    }
    if (cpContext->pending_.server_connect) {
      cpContext->pending_.server_connect = false;
      cpContext->conn_tx_.send(ec);
    }
    break;
  // TODO: handle QUIC_CONNECTION_EVENT_CLOSED
  case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
    MSQUIC_ASIO_MESSAGE(
        "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT");
    if (Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status ==
        QUIC_STATUS_CONNECTION_IDLE) {
      // printf("[conn][%p] Successfully shut down on idle.\n", Connection);
      MSQUIC_ASIO_MESSAGE(
          "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT idle");
    } else {
      // printf("[conn][%p] Shut down by transport, 0x%x\n", Connection,
      //        Event->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
      MSQUIC_ASIO_MESSAGE(
          "QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT error");
    }
    //
    // The connection has been shut down by the transport. Generally, this
    // is the expected way for the connection to shut down with this
    // protocol, since we let idle timeout kill the connection.
    //
    cpContext->set_state(state_type::transport_shutdown);
    ec.assign(net::error::basic_errors::connection_aborted,
              boost::asio::error::get_system_category());

    // cleanup pending actions
    // client start should be handled differently before because it is init by
    // frontend.
    assert(!cpContext->pending_.client_start);
    if (cpContext->pending_.server_connect) {
      cpContext->pending_.server_connect = false;
      cpContext->conn_tx_.send(ec);
    }
    if (cpContext->pending_.accept) {
      cpContext->pending_.accept = false;
      basic_stream_handle<executor_type> dummy(cpContext->ex_, cpContext->api_);
      cpContext->accept_tx_.send(ec, std::move(dummy));
    }
    break;
  case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER");
    //
    // The connection was explicitly shut down by the peer.
    //
    cpContext->set_state(state_type::peer_shutdown);
    ec.assign(net::error::basic_errors::connection_aborted,
              boost::asio::error::get_system_category());
    assert(!cpContext->pending_.client_start);
    if (cpContext->pending_.server_connect) {
      cpContext->pending_.server_connect = false;
      cpContext->conn_tx_.send(ec);
    }
    if (cpContext->pending_.accept) {
      cpContext->pending_.accept = false;
      basic_stream_handle<executor_type> dummy(cpContext->ex_, cpContext->api_);
      cpContext->accept_tx_.send(ec, std::move(dummy));
    }
    break;
  case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE");
    //
    // The connection has completed the shutdown process and is ready to be
    // safely cleaned up.
    //
    cpContext->set_state(state_type::shutdown_complete);
    ec.assign(net::error::basic_errors::connection_aborted,
              boost::asio::error::get_system_category());
    assert(!cpContext->pending_.client_start);
    if (cpContext->pending_.server_connect) {
      cpContext->pending_.server_connect = false;
      cpContext->conn_tx_.send(ec);
    }
    if (cpContext->pending_.accept) {
      cpContext->pending_.accept = false;
      basic_stream_handle<executor_type> dummy(cpContext->ex_, cpContext->api_);
      cpContext->accept_tx_.send(ec, std::move(dummy));
    }
    if (cpContext->pending_.shutdown) {
      // front end must have inited the channel
      cpContext->pending_.shutdown = false;
      ec.clear();
      cpContext->shutdown_tx_.send(ec);
    }
    break;
  case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED");
    //
    // The peer has started/created a new stream. The app MUST set the
    // callback handler before returning.
    //

    // prepare stream
    assert(Event->PEER_STREAM_STARTED.Stream != nullptr);
    // wait until sh is empty. !!! TODO: there is dead lock for mtx and bs.
    lock.unlock(); // let frontend proceed.
    cpContext->accept_bs_.acquire();
    assert(cpContext->pending_.accept);
    cpContext->pending_.accept = false;

    // build the new stream
    basic_stream_handle<executor_type> stream(cpContext->ex_, cpContext->api_);
    stream.assign(Event->PEER_STREAM_STARTED.Stream);
    stream.set_callback();
    // send stream
    cpContext->accept_tx_.send(ec, std::move(stream));
  } break;
  case QUIC_CONNECTION_EVENT_RESUMED:
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_RESUMED");
    // not supported
    assert(false);
    //
    // The connection succeeded in doing a TLS resumption of a previous
    // connection's session.
    //

    // front end should be in async_resume case
    break;
  case QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED:
    // ignore for now
    MSQUIC_ASIO_MESSAGE("QUIC_CONNECTION_EVENT_RESUMPTION_TICKET_RECEIVED");
    break;
  default:
    break;
  }
  return QUIC_STATUS_SUCCESS;
}

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

  template <typename Token>
  auto async_shutdown(_In_ QUIC_CONNECTION_SHUTDOWN_FLAGS Flags,
                      _In_ _Pre_defensive_ QUIC_UINT62 ErrorCode,
                      Token &&token) {
    assert(this->is_open());
    // backend and frontend can call at the same time
    std::scoped_lock lock(ctx_->mtx_);

    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->shutdown_tx_ = std::move(tx);
    ctx_->shutdown_rx_ = std::move(rx);

    boost::system::error_code ec;

    if (ctx_->get_state() == state_type::shutdown_complete) {
      // already shutdown
      ctx_->shutdown_tx_.send(ec);
    } else {
      // shutdown returns void
      this->api_->ConnectionShutdown(this->h_, Flags, ErrorCode);
      ctx_->pending_.shutdown = true;
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
  template <typename Token>
  auto async_start(_In_ _Pre_defensive_ HQUIC Configuration,
                   _In_ QUIC_ADDRESS_FAMILY Family,
                   _In_reads_or_z_opt_(QUIC_MAX_SNI_LENGTH)
                       const char *ServerName,
                   _In_ uint16_t ServerPort, // Host byte order
                   Token &&token) {
    assert(this->is_open());

    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->conn_tx_ = std::move(tx);
    ctx_->conn_rx_ = std::move(rx);

    assert(!ctx_->pending_.client_start);
    ctx_->pending_.client_start = true;

    boost::system::error_code ec = {};

    QUIC_STATUS Status = this->api_->ConnectionStart(
        this->native_handle(), Configuration, Family, ServerName, ServerPort);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
      ctx_->pending_.client_start = false;
      ctx_->conn_tx_.send(ec);
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code)>(
        [this](auto handler) {
          ctx_->conn_rx_.async_wait([h = std::move(handler), this](
                                        boost::system::error_code ec) mutable {
            // if oneshot has error retain it, else use msquic callback
            // error sent from backend
            if (!ec.failed()) {
              ec = ctx_->conn_rx_.get();
            }
            // TODO: can clean up the channels here.
            std::move(h)(ec);
          });
        },
        token);
  }

  // used by server.
  // handler type void(ec)
  template <typename Token> auto async_connect(Token &&token) {
    // for server, the conn event is auto triggered by backend without front end
    // action
    assert(this->is_open());
    std::scoped_lock lock(ctx_->mtx_);

    auto [tx, rx] = oneshot::create<boost::system::error_code>();
    ctx_->conn_tx_ = std::move(tx);
    ctx_->conn_rx_ = std::move(rx);

    assert(!ctx_->pending_.server_connect);
    if (ctx_->get_state() == state_type::shutdown_complete) {
      boost::system::error_code ec;
      ec.assign(net::error::connection_aborted,
                boost::asio::error::get_system_category());
      ctx_->conn_tx_.send(ec);
    } else {
      ctx_->pending_.server_connect = true;
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code)>(
        [this](auto handler) {
          ctx_->conn_rx_.async_wait([h = std::move(handler), this](
                                        boost::system::error_code ec) mutable {
            // if oneshot has error retain it, else use msquic callback
            // error sent from backend
            if (!ec.failed()) {
              ec = ctx_->conn_rx_.get();
            }
            // TODO: can clean up the channels here.
            std::move(h)(ec);
          });
        },
        token);
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
    assert(this->is_open());

    // TODO: check conn state
    std::scoped_lock lock(ctx_->mtx_);
    assert(!ctx_->pending_.accept);
    auto [tx, rx] =
        oneshot::create<std::pair<boost::system::error_code,
                                  basic_stream_handle<executor_type>>>();
    ctx_->accept_tx_ = std::move(tx);
    ctx_->accept_rx_ = std::move(rx);

    if (ctx_->get_state() == state_type::shutdown_complete) {
      boost::system::error_code ec;
      ec.assign(net::error::connection_aborted,
                boost::asio::error::get_system_category());
      basic_stream_handle<executor_type> dummy(this->ex_, this->api_);
      ctx_->accept_tx_.send(ec, std::move(dummy));
    } else {
      ctx_->pending_.accept = true;
      // backend can go ahead.
      ctx_->accept_bs_.release();
    }

    return net::async_initiate<decltype(token),
                               void(boost::system::error_code,
                                    basic_stream_handle<executor_type>)>(
        [this](auto handler) {
          ctx_->accept_rx_.async_wait(
              [h = std::move(handler),
               this](boost::system::error_code ec) mutable {
                // if oneshot has error retain it, else use msquic callback
                // error sent from backend
                if (ec.failed()) {
                  basic_stream_handle<executor_type> dummy(this->ex_,
                                                           this->api_);
                  std::move(h)(ec, std::move(dummy));
                  return;
                }
                auto [ec2, stream] = std::move(ctx_->accept_rx_.get());
                std::move(h)(ec2, std::move(stream));
              });
        },
        token);
  }

private:
  // use ptr to facilitate the ctx passed to msquic callback.
  // otherwise need to set callback ctx multiple times during move.
  std::unique_ptr<details::AsioServerConnectionCtx<executor_type>> ctx_;
};

} // namespace msquic
} // namespace boost
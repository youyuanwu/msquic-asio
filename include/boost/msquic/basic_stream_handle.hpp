#include "boost/msquic/basic_quic_handle.hpp"
#include "boost/msquic/event.hpp"

#include <memory>
#include <mutex>
#include <semaphore>

namespace boost {
namespace msquic {

// forward declare
template <typename Executor> class basic_stream_handle;

namespace details {

template <typename Executor> class AsioServerStreamCtx {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;

  enum class state {
    idle,
    start,
    send,
    receive,
    peer_shutdown,
    peer_abort,
    shutdown_complete,
    done,
    error
  };

  AsioServerStreamCtx(const executor_type &ex)
      : ev_(ex), ec_(), len_(0), state_(state::idle), mtx_(), bs_(0) {}

  void set_stream_event(boost::system::error_code ec) {
    ec_ = ec;
    ev_.set();
  }

  void reset_stream_event() {
    ec_.clear();
    ev_.reset();
  }

  void set_len(std::size_t len) { len_ = len; }

  void set_state(state s) { state_ = s; }

  state get_state() { return state_; }

  event<executor_type> ev_;

  // error code saved from callback
  boost::system::error_code ec_;

  // len saved from callback.
  // or send total len.
  std::size_t len_;

  // recieved buffers.
  // TODO: make without alloc
  std::vector<QUIC_BUFFER> buf_vec_;

  // allow only 1 callback at a time
  std::mutex mtx_;
  // allow 1 task at a time;
  std::binary_semaphore bs_;

private:
  state state_;
};

template <typename Executor>
_IRQL_requires_max_(DISPATCH_LEVEL)
    _Function_class_(QUIC_STREAM_CALLBACK) QUIC_STATUS QUIC_API
    AsioServerStreamCallback(_In_ HQUIC Stream, _In_opt_ void *Context,
                             _Inout_ QUIC_STREAM_EVENT *Event) {
  UNREFERENCED_PARAMETER(Stream);

  typedef Executor executor_type;
  typedef AsioServerStreamCtx<executor_type>::state state_type;

  boost::system::error_code ec = {};

  details::AsioServerStreamCtx<executor_type> *cpContext =
      (details::AsioServerStreamCtx<executor_type> *)Context;

  // allow 1 callback at a time
  std::scoped_lock lock(cpContext->mtx_);

  QUIC_STATUS Status = QUIC_STATUS_SUCCESS;
  switch (Event->Type) {
  case QUIC_STREAM_EVENT_START_COMPLETE:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_START_COMPLETE");
    // printf("[strm][%p] Start complete\n", Stream);
    //  TODO: this event is not delivered in success case.???
    // cpContext->start_bs_.acquire();
    assert(cpContext->get_state() == state_type::start);
    cpContext->set_stream_event(ec);
    break;
  case QUIC_STREAM_EVENT_SEND_COMPLETE:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_SEND_COMPLETE");
    if (cpContext->get_state() != state_type::send) {
      // stream is in wrong state
      // this is is a bug in this lib
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
    }
    // printf("[strm][%p] Data sent\n", Stream);
    cpContext->set_stream_event(ec);
    break;
  case QUIC_STREAM_EVENT_RECEIVE:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_RECEIVE: len " +
                       std::to_string(Event->RECEIVE.TotalBufferLength));
    assert(cpContext->get_state() == state_type::receive);
    //
    // Data was received from the peer on the stream.
    //
    // Buffer pointers are valid until we call resume.
    // printf("[strm][%p] Data received\n", Stream);
    for (size_t i = 0; i < Event->RECEIVE.BufferCount; i++) {
      const QUIC_BUFFER *buf = Event->RECEIVE.Buffers + i;
      // printf("[%p] %.*s\n",buf->Buffer, buf->Length, buf->Buffer);
      // add to buf vec
      QUIC_BUFFER temp = {};
      temp.Buffer = buf->Buffer;
      temp.Length = buf->Length;
      cpContext->buf_vec_.push_back(std::move(temp));
    }
    // cpContext->BufferCount_ = Event->RECEIVE.BufferCount;
    // cpContext->Buffers_ = Event->RECEIVE.Buffers;
    cpContext->set_stream_event(ec);
    // return pending so that msquic will retail buffers, and we handle it in
    // asio.
    // The buffer array is not retained by msquic, so we copied to vector above.
    // TODO: maybe buf->Buffer pointer is not freed but the buf is freed.
    Status = QUIC_STATUS_PENDING;
    break;
  case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN");
    if (cpContext->get_state() != state_type::peer_shutdown) {
      // state not match
      cpContext->set_state(state_type::error);
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_stream_event(ec);
    }
    cpContext->set_stream_event(ec);
    //  The peer gracefully shut down its send direction of the stream.
    //
    //  printf("[strm][%p] Peer shut down\n", Stream);
    //  ServerSend(Stream);
    break;
  case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_PEER_SEND_ABORTED");
    if (cpContext->get_state() == state_type::peer_abort) {
      cpContext->set_stream_event(ec);
    } else {
      // state not match
      cpContext->set_state(state_type::error);
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_stream_event(ec);
    }
    // The peer aborted its send direction of the stream.
    //
    // printf("[strm][%p] Peer aborted\n", Stream);
    // MsQuic->StreamShutdown(Stream, QUIC_STREAM_SHUTDOWN_FLAG_ABORT, 0);
    break;
  case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
    cpContext->bs_.acquire();
    BOOST_TEST_MESSAGE("QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE");
    if (cpContext->get_state() == state_type::shutdown_complete) {
      cpContext->set_state(state_type::done);
      cpContext->set_stream_event(ec);
    } else {
      // state not match
      cpContext->set_state(state_type::error);
      ec.assign(net::error::basic_errors::fault,
                boost::asio::error::get_system_category());
      cpContext->set_stream_event(ec);
    }
    //
    // Both directions of the stream have been shut down and MsQuic is done
    // with the stream. It can now be safely cleaned up.
    //
    // printf("[strm][%p] All done\n", Stream);
    // MsQuic->StreamClose(Stream);
    break;
  default:
    break;
  }
  return Status;
}

// handler signature void(ec, size_t)
template <typename Executor> class async_len_op : boost::asio::coroutine {
public:
  async_len_op(event<Executor> *ev, boost::system::error_code *ctx_ec,
               std::size_t *len)
      : ev_(ev), ctx_ec_(ctx_ec), len_(len) {}

  template <typename Self>
  void operator()(Self &self, boost::system::error_code ec = {}) {
    if (ec) {
      self.complete(ec, 0);
      return;
    }
    ev_->async_wait([self = std::move(self), c = ctx_ec_,
                     l = len_](boost::system::error_code ec) mutable {
      assert(!ec.failed());
      DBG_UNREFERENCED_LOCAL_VARIABLE(ec);
      // pass the ctx_ec error from callback
      self.complete(*c, *l);
    });
  }

private:
  event<Executor> *ev_;
  // ec shared and set in the callback
  boost::system::error_code *ctx_ec_;
  std::size_t *len_;
};

// handler signature void(ec)
// async wait an action.
template <typename Executor> class async_wait_op : boost::asio::coroutine {
public:
  async_wait_op(event<Executor> *ev, boost::system::error_code *ctx_ec)
      : ev_(ev), ctx_ec_(ctx_ec) {}

  template <typename Self>
  void operator()(Self &self, boost::system::error_code ec = {}) {
    if (ec) {
      self.complete(ec);
      return;
    }
    ev_->async_wait([self = std::move(self),
                     c = ctx_ec_](boost::system::error_code ec) mutable {
      assert(!ec.failed());
      DBG_UNREFERENCED_LOCAL_VARIABLE(ec);
      // pass the ctx_ec error from callback
      self.complete(*c);
    });
  }

private:
  event<Executor> *ev_;
  // ec shared and set in the callback
  boost::system::error_code *ctx_ec_;
};

// handler signature void(ec, size_t)
// async wait an action.
template <typename Executor> class async_receive_op : boost::asio::coroutine {
public:
  async_receive_op(basic_stream_handle<Executor> *h,
                   // params from front end
                   LPVOID outbuff, std::size_t outbuff_size)
      : h_(h), outbuff_(outbuff), outbuff_size_(outbuff_size) {}

  template <typename Self>
  void operator()(Self &self, boost::system::error_code ec = {}) {
    if (ec) {
      self.complete(ec, 0);
      return;
    }
    h_->get_ctx()->ev_.async_wait(
        [self = std::move(self), h = h_, outbuffptr = outbuff_,
         outbuff_size = outbuff_size_](boost::system::error_code ec) mutable {
          assert(!ec.failed());
          assert(outbuffptr != nullptr);
          DBG_UNREFERENCED_LOCAL_VARIABLE(ec);
          // if has error in ctx, abort
          if (h->get_ctx()->ec_) {
            self.complete(h->get_ctx()->ec_, 0);
            return;
          }
          std::vector<QUIC_BUFFER> &buffs = h->get_ctx()->buf_vec_;
          // copy buffers into out buffer. make a temp variable to advance
          // during copy
          LPVOID outbuff = outbuffptr;
          std::size_t remaining = outbuff_size; // remaining size of outbuff
          std::size_t copied = 0;               // bytes copied already
          for (std::size_t i = 0; i < buffs.size(); i++) {
            const QUIC_BUFFER &b = buffs[i];
            // printf("Copy buffer [%p] %d\n", b.Buffer, b.Length);
            std::size_t to_copy =
                std::min(remaining, static_cast<std::size_t>(b.Length));
            if (to_copy == 0) {
              continue;
            }
            errno_t ec_cp = memcpy_s(outbuff, remaining, b.Buffer, to_copy);
            assert(ec_cp == 0);
            DBG_UNREFERENCED_LOCAL_VARIABLE(ec_cp);
            remaining -= to_copy;
            copied += to_copy;
          }
          // clear buffs
          buffs.clear();
          assert(copied <= outbuff_size);
          // copy completes, send signal to msquic
          BOOST_TEST_MESSAGE("stream callback copied buffer: len: " +
                             std::to_string(copied));
          h->receive_complete(copied);
          self.complete(h->get_ctx()->ec_, copied);
        });
  }

private:
  // access ctx and methods
  basic_stream_handle<Executor> *h_;
  // out parameters
  LPVOID outbuff_;
  std::size_t outbuff_size_;
};

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
        ctx_(
            std::make_unique<details::AsioServerStreamCtx<executor_type>>(ex)) {
  }

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

    ctx_->set_state(state_type::start);
    ctx_->reset_stream_event();

    QUIC_STATUS Status = this->api_->StreamStart(this->h_, Flags);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
      ctx_->set_stream_event(ec);
    }
    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
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
    this->ctx_->set_state(state_type::send);
    // This does not need semaphore because send complete always comes after
    // initiate.
    boost::system::error_code ec = {};
    QUIC_STATUS Status = this->api_->StreamSend(this->h_, Buffers, BufferCount,
                                                Flags, ClientSendContext);
    if (QUIC_FAILED(Status)) {
      // todo : make the right ec
      ec.assign(net::error::basic_errors::network_down,
                boost::asio::error::get_system_category());
      // event handler should be triggered
      // we launch the compose op but it completes immediately
      ctx_->set_len(0);
      ctx_->set_stream_event(ec);
    } else {
      ctx_->set_state(state_type::send);
      // count the byte len of all buffers.
      // all buffers are queued in to msquic, and considered success.
      std::size_t len = 0;
      for (std::size_t i = 0; i < BufferCount; i++) {
        const QUIC_BUFFER *const b = Buffers + i;
        len += b->Length;
      }
      this->ctx_->set_len(len);
      this->ctx_->reset_stream_event();
    }
    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code,
                                               std::size_t)>(
            details::async_len_op<executor_type>(
                &this->ctx_->ev_, &this->ctx_->ec_, &this->ctx_->len_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  template <typename Token>
  auto async_recieve(_Out_ LPVOID lpBuffer, _In_ std::size_t bufferLen,
                     Token &&token) {
    this->ctx_->set_state(state_type::receive);
    this->ctx_->reset_stream_event();
    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code,
                                               std::size_t)>(
            details::async_receive_op<executor_type>(this, lpBuffer, bufferLen),
            token, this->get_executor());
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
    ctx_->reset_stream_event();

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
  async_wait_peer_abort(BOOST_ASIO_MOVE_ARG(
      Token) token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    // wait for peer shutdown to happen in callback
    ctx_->set_state(state_type::peer_abort);
    ctx_->reset_stream_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  void receive_complete(std::size_t len) {
    this->api_->StreamReceiveComplete(this->h_, len);
  }

  template <BOOST_ASIO_COMPLETION_TOKEN_FOR(void(boost::system::error_code))
                Token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN_TYPE(executor_type)>
  BOOST_ASIO_INITFN_AUTO_RESULT_TYPE(Token, void(boost::system::error_code))
  async_wait_shutdown_complete(BOOST_ASIO_MOVE_ARG(
      Token) token BOOST_ASIO_DEFAULT_COMPLETION_TOKEN(executor_type)) {
    ctx_->set_state(state_type::shutdown_complete);
    ctx_->reset_stream_event();

    auto temp =
        boost::asio::async_compose<Token, void(boost::system::error_code)>(
            details::async_wait_op<executor_type>(&this->ctx_->ev_,
                                                  &this->ctx_->ec_),
            token, this->get_executor());
    ctx_->bs_.release();
    return std::move(temp);
  }

  void shutdown(_In_ QUIC_STREAM_SHUTDOWN_FLAGS Flags,
                _In_ _Pre_defensive_ QUIC_UINT62
                    ErrorCode, // Application defined error code
                _Out_ boost::system::error_code &ec) {
    if (this->is_open()) {
      QUIC_STATUS Status =
          this->api_->StreamShutdown(this->h_, Flags, ErrorCode);
      if (QUIC_FAILED(Status)) {
        ec.assign(Status, boost::asio::error::get_system_category());
      }
    }
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
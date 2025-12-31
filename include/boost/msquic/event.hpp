#pragma once

// make sure IDE tools picks up right macro
#include <boost/asio/detail/config.hpp>

#ifdef WIN32
#include <boost/asio/windows/basic_object_handle.hpp>
#else
#include "boost/msquic/posix_descriptor.hpp"
#include <sys/eventfd.h>
#endif

#include <boost/assert.hpp>

// impl crossplat event for asio

namespace boost {
namespace msquic {

namespace net = boost::asio;

template <typename Executor> class event {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;

  explicit event(const executor_type &ex) : h_(ex) {
#ifdef WIN32
    HANDLE ev = CreateEvent(NULL,  // default security attributes
                            TRUE,  // manual-reset event
                            FALSE, // initial state is nonsignaled
                            NULL   // object name
    );
    assert(ev != nullptr);
    this->h_.assign(ev);
#else
    // when creation, event is not set.
    int efd = eventfd(0, 0);
    assert(efd != -1);
    this->h_.assign(efd);
#endif
  }

  event(event &&other) : h_(std::move(other.h_)) {}

  ~event() {}

  void set() {
#ifdef WIN32
    HANDLE h = this->h_.native_handle();
    assert(h != NULL);
    assert(h != INVALID_HANDLE_VALUE);
    [[maybe_unused]] bool ok = SetEvent(h);
    if (!ok) {
      auto ec = boost::system::error_code(
          ::GetLastError(), boost::asio::error::get_system_category());
      MSQUIC_ASIO_MESSAGE(std::string("set event failed: ") + ec.message());
      BOOST_ASSERT(false);
    }

#else
    // write the data_num to event
    int efd = this->h_.native_handle();
    uint64_t u = this->data_num;
    [[maybe_unused]] ssize_t s = write(efd, &u, sizeof(uint64_t));
    assert(sizeof(uint64_t) == s);
#endif
  }

  void reset() {
#ifdef WIN32
    HANDLE h = this->h_.native_handle();
    assert(h != NULL);
    assert(h != INVALID_HANDLE_VALUE);
    [[maybe_unused]] bool ok = ResetEvent(h);
    assert(ok);
#else
    // read will reset the event number to 0;
    int efd = this->h_.native_handle();
    uint64_t u = {};
    [[maybe_unused]] ssize_t s = read(efd, &u, sizeof(uint64_t));
    assert(sizeof(uint64_t) == s);
    assert(u == this->data_num);
#endif
  }

  template <typename WaitHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(WaitHandler, void(boost::system::error_code))
  async_wait(BOOST_ASIO_MOVE_ARG(WaitHandler) handler) {
    return h_.async_wait(std::move(handler));
  }

  // close the descriptor/handle
  void close() {
    if (h_.is_open()) {
      h_.close();
    }
  }

private:
#ifdef WIN32
  net::windows::basic_object_handle<executor_type> h_;
#else
  posix_descriptor<executor_type> h_;
  const uint64_t data_num = 0xf;
#endif
};

} // namespace msquic
} // namespace boost
#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <msquic.h>

#ifndef MSQUIC_ASIO_MESSAGE
#define MSQUIC_ASIO_MESSAGE(x)
#endif

// We roll our own cpp wrapper because the msquic provided one is not move
// semantic friendly. The cpp wrapper should look like asio::windows wrappers.

namespace boost {
namespace msquic {

namespace net = boost::asio;

template <typename Executor = net::any_io_executor> class basic_quic_handle {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  basic_quic_handle(const executor_type &ex, const QUIC_API_TABLE *api)
      : ex_(ex), api_(api), h_(nullptr) {
    assert(api != nullptr);
  }

  basic_quic_handle(basic_quic_handle &&other)
      : api_(other.api_), h_(other.h_), ex_(other.ex_) {
    // only move the handle to the other
    // api ptr stays.
    // the ownership of handle is transfered to this
    other.release();
  }

  executor_type get_executor() { return ex_; }

  native_handle_type native_handle() { return h_; }

  const QUIC_API_TABLE *get_api() { return api_; }

  void assign(const native_handle_type &handle) { this->h_ = handle; }

  // release handle ownership
  native_handle_type release() {
    native_handle_type copy = this->h_;
    this->h_ = nullptr;
    return copy;
  }

  bool is_open() const {
    // TODO: this might not be sufficient
    return this->h_ != nullptr;
  }

  void cancel() {
    // TODO:
  }

protected:
  /// Protected destructor to prevent deletion through this type.
  /**
   * This function destroys the handle, cancelling any outstanding asynchronous
   * wait operations associated with the handle as if by calling @c cancel.
   */
  ~basic_quic_handle() {}

  // close the handle
  // different handle needs to be closed differently
  // prevent close from this type
  // void close(){
  // }

  executor_type ex_;
  native_handle_type h_;

  const QUIC_API_TABLE *api_;
};

} // namespace msquic
} // namespace boost
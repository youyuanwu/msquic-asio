#pragma once

#ifdef WIN32
#error "posix code included in windows"
#endif
#include <boost/asio/posix/basic_descriptor.hpp>

// make basic descriptor destructable.

namespace boost {
namespace msquic {

namespace net = boost::asio;

// user needs to close before destruction
template <typename Executor>
class posix_descriptor : public net::posix::basic_descriptor<Executor> {
public:
  /// The type of the executor associated with the object.
  typedef Executor executor_type;
  typedef net::posix::basic_descriptor<executor_type> parent_type;

  explicit posix_descriptor(const executor_type &ex)
      : net::posix::basic_descriptor<executor_type>(ex) {}

  template <typename WaitHandler>
  BOOST_ASIO_INITFN_RESULT_TYPE(WaitHandler, void(boost::system::error_code))
  async_wait(BOOST_ASIO_MOVE_ARG(WaitHandler) token) {
    // wait fd to be ready to read.
    // This means the event number should be non-zero.
    // We set eventfd to be non-zero in our event set call.
    // So this wait is to wait for if the event to be set.
    return parent_type::async_wait(
        net::posix::descriptor_base::wait_type::wait_read, std::move(token));
  }

  ~posix_descriptor() {}
};

} // namespace msquic
} // namespace boost
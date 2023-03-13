#pragma once

#include "boost/msquic/basic_quic_handle.hpp"

namespace boost {
namespace msquic {

template <typename Executor = net::any_io_executor>
class basic_registration_handle : public basic_quic_handle<Executor> {
public:
  typedef Executor executor_type;

  basic_registration_handle(const executor_type &ex, const QUIC_API_TABLE *api)
      : basic_quic_handle<executor_type>(ex, api) {}

  ~basic_registration_handle() { this->close(); }

  void open(const QUIC_REGISTRATION_CONFIG &RegConfig,
            boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status = this->api_->RegistrationOpen(&RegConfig, &this->h_);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
    }
  }

  void close() {
    if (this->is_open()) {
      this->api_->RegistrationClose(this->h_);
      this->h_ = nullptr;
    }
  }
};

} // namespace msquic
} // namespace boost
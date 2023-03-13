#pragma once

#include "boost/msquic/basic_quic_handle.hpp"
#include <boost/algorithm/hex.hpp>

namespace boost {
namespace msquic {

template <typename Executor = net::any_io_executor>
class basic_config_handle : public basic_quic_handle<Executor> {
public:
  typedef Executor executor_type;
  typedef HQUIC native_handle_type;

  basic_config_handle(const executor_type &ex, const QUIC_API_TABLE *api)
      : basic_quic_handle<executor_type>(ex, api) {}

  ~basic_config_handle() { this->close(); }

  void open(native_handle_type Registration, const QUIC_SETTINGS &Settings,
            const QUIC_BUFFER &Alpn, boost::system::error_code &_Out_ ec) {
    QUIC_STATUS Status = this->api_->ConfigurationOpen(
        Registration, &Alpn, 1, &Settings, sizeof(Settings), NULL /*ctx TODO*/,
        &this->h_);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
    }
  }

  void load_cred2(_In_ QUIC_CREDENTIAL_CONFIG &Config,
                  boost::system::error_code &_Out_ ec) {
    assert(this->is_open());
    QUIC_STATUS Status =
        this->api_->ConfigurationLoadCredential(this->h_, &Config);
    if (QUIC_FAILED(Status)) {
      ec.assign(Status, boost::asio::error::get_system_category());
    }
  }

  // void load_cred(boost::system::error_code &_Out_ ec) {
  //   assert(this->is_open());
  //   QUIC_CREDENTIAL_CONFIG Config = {};
  //   QUIC_CERTIFICATE_HASH certHash = {};

  //   // hard code for now
  //   const char *Cert = "0EAB72BCF9CCA779AF10B9548888396D5A36DACB";
  //   std::string hash = boost::algorithm::unhex(std::string(Cert));
  //   assert(hash.size() == sizeof(certHash.ShaHash));
  //   //
  //   // Load the server's certificate from the default certificate store,
  //   // using the provided certificate hash.
  //   //
  //   Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
  //   std::copy(hash.begin(), hash.end(), certHash.ShaHash);
  //   Config.CertificateHash = &certHash;

  //   QUIC_STATUS Status =
  //       this->api_->ConfigurationLoadCredential(this->h_, &Config);
  //   if (QUIC_FAILED(Status)) {
  //     ec.assign(Status, boost::asio::error::get_system_category());
  //   }
  // }

  void close() {
    if (this->is_open()) {
      this->api_->ConfigurationClose(this->h_);
      this->h_ = nullptr;
    }
  }
};

} // namespace msquic
} // namespace boost
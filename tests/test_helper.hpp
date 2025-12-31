#pragma once

#include "boost/msquic/basic_config_handle.hpp"
#include "boost/msquic/basic_registration_handle.hpp"

#ifndef TEST_CERT_HASH
#error "test cert not found"
#endif

#define QUOTE(x) #x
#define STR(x) QUOTE(x)

namespace net = boost::asio;
namespace quic = boost::msquic;

const QUIC_BUFFER &get_test_alpn() {
  static const QUIC_BUFFER Alpn = {sizeof("sample") - 1, (uint8_t *)"sample"};
  return Alpn;
}

template <typename Executor>
void set_registration_handle(quic::basic_registration_handle<Executor> &reg) {
  boost::system::error_code ec = {};
  const QUIC_REGISTRATION_CONFIG RegConfig = {
      "quicsample", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
  reg.open(RegConfig, ec);
  MSQUIC_ASIO_TEST_REQUIRE_EQUAL(ec.failed(), false);
  MSQUIC_ASIO_TEST_REQUIRE_EQUAL(reg.is_open(), true);
}

template <typename Executor>
void config_test_client_handle(quic::basic_config_handle<Executor> &config,
                               HQUIC reg) {
  boost::system::error_code ec = {};

  // open client config
  QUIC_SETTINGS Settings = {0};
  //
  // Configures the client's idle timeout.
  //
  Settings.IdleTimeoutMs = 1000;
  Settings.IsSet.IdleTimeoutMs = TRUE;

  //
  // Configures a default client configuration, optionally disabling
  // server certificate validation.
  //
  QUIC_CREDENTIAL_CONFIG CredConfig = {};
  CredConfig.Type = QUIC_CREDENTIAL_TYPE_NONE;
  CredConfig.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
  // use insecure
  CredConfig.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

  config.open(reg, Settings, get_test_alpn(), ec);
  MSQUIC_ASIO_TEST_REQUIRE_EQUAL(ec.failed(), false);
  // fix me:
  config.load_cred2(CredConfig, ec);
  MSQUIC_ASIO_TEST_REQUIRE_EQUAL(ec.failed(), false);
}

template <typename Executor>
void config_test_server_handle(quic::basic_config_handle<Executor> &config,
                               HQUIC reg) {
  boost::system::error_code ec = {};
  {
    // config
    QUIC_SETTINGS Settings = {0};
    //
    // Configures the server's idle timeout.
    //
    Settings.IdleTimeoutMs = 1000;
    Settings.IsSet.IdleTimeoutMs = TRUE;
    //
    // Configures the server's resumption level to allow for resumption and
    // 0-RTT.
    //
    Settings.ServerResumptionLevel = QUIC_SERVER_RESUME_AND_ZERORTT;
    Settings.IsSet.ServerResumptionLevel = TRUE;
    //
    // Configures the server's settings to allow for the peer to open a single
    // bidirectional stream. By default connections are not configured to allow
    // any streams from the peer.
    //
    Settings.PeerBidiStreamCount = 1;
    Settings.IsSet.PeerBidiStreamCount = TRUE;

    config.open(reg, Settings, get_test_alpn(), ec);
    MSQUIC_ASIO_TEST_REQUIRE_EQUAL(ec.failed(), false);
    QUIC_CREDENTIAL_CONFIG Config = {};
    QUIC_CERTIFICATE_HASH certHash = {};

    // hard code for now
    const char *Cert = STR(TEST_CERT_HASH);
    std::string hash = boost::algorithm::unhex(std::string(Cert));
    assert(hash.size() == sizeof(certHash.ShaHash));
    //
    // Load the server's certificate from the default certificate store,
    // using the provided certificate hash.
    //
    Config.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_HASH;
    std::copy(hash.begin(), hash.end(), certHash.ShaHash);
    Config.CertificateHash = &certHash;

    config.load_cred2(Config, ec);
    MSQUIC_ASIO_TEST_REQUIRE_EQUAL(ec.failed(), false);
  }
}
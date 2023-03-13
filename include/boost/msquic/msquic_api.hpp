#pragma once

#include "msquic.h"

// msquic api table

namespace boost {
namespace msquic {

// auto open and close msquic api table
// on app should use this class once
class api {
public:
  api() {
    [[maybe_unused]] QUIC_STATUS status = MsQuicOpen2(&MsQuic_);
    assert(QUIC_SUCCEEDED(status));
  }

  const QUIC_API_TABLE *get() { return MsQuic_; }

  ~api() {
    if (MsQuic_ != nullptr) {
      MsQuicClose(MsQuic_);
    }
  }

private:
  const QUIC_API_TABLE *MsQuic_;
};

// class registration{
// public:
//   registration(const QUIC_REGISTRATION_CONFIG & RegConfig, const
//   QUIC_API_TABLE* api) {
//       MsQuic->RegistrationOpen(&RegConfig, &Registration));
//   }

//   ~registration(){
//   }
// private:
//   const QUIC_API_TABLE* api_;
// };

} // namespace msquic
} // namespace boost
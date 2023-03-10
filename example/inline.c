/*++

    Copyright (c) Microsoft Corporation.
    Licensed under the MIT License.

Abstract:

    External definition of C99 inline functions.
    See "Clang" / "Language Compatibility" / "C99 inline functions"
    ( https://clang.llvm.org/compatibility.html#inline .)
    It seems that C99 standard requires that every inline function defined
    in a header have a corresponding non-inline definition in a C source file.
    Observed behavior is that Clang is enforcing this, but not MSVC.
    Until an alternative solution is found, this file is required for Clang.

--*/

// #include "platform_internal.h"
// #ifdef QUIC_CLOG
// #include "inline.c.clog.h"
// #endif
#include "msquic.h"

QUIC_ADDRESS_FAMILY
QuicAddrGetFamily(
    _In_ const QUIC_ADDR* const Addr
    );

void
QuicAddrSetFamily(
    _In_ QUIC_ADDR* Addr,
    _In_ QUIC_ADDRESS_FAMILY Family
    );

uint16_t
QuicAddrGetPort(
    _In_ const QUIC_ADDR* const Addr
    );

void
QuicAddrSetPort(
    _Out_ QUIC_ADDR* Addr,
    _In_ uint16_t Port
    );
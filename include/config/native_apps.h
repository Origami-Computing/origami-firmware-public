#pragma once
// Native game ABI epoch. Bump when host layouts/contracts become incompatible.
#define KAMI_NATIVE_ABI_VERSION 1
namespace config { namespace native_apps {
// Development publisher public key; private key is never part of this repository.
static constexpr char publicKey[] = R"KEY(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAsgOYb0GNATrC4JtztDNO
P0O4l/KxD7JR2DdCE/+Ib8eHpOxb3PhNfuHNOX+fVMJd0tMo5rUdFOOcYHvYfgIL
2/rRDnpxAS7kIIxmb+Q55sXaG586dcjbymvtpH6Cokm3Tqi06TZ4kymlrgX1JLE5
+iUK0yq7Shplhi5rG0CpHwCOl4ubljUX6vfFfNTxBCds64v1RLen8asNyMjccK42
JiN0XE3tE24vF5WnrJbaOOcGC0ju73PZnLFZlS9IlzsrVsUDtyB8Yku7xrWRiTJ8
5bgO0+3bMLSgAx+R3pPYBFZPgGqyPiFB1xmNbiwl12AXfdIPT7cXh1j0aM5J4KMj
lwIDAQAB
-----END PUBLIC KEY-----
)KEY";
} }

#pragma once
struct sslclient_context;
namespace core { namespace tls {
// Application-task SDK adapter: remember endpoint identity while the descriptor
// exists. The owner pointer is an identity key only, never dereferenced later.
void rememberSocket(sslclient_context* client);
} }

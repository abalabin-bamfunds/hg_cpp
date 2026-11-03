# Client system CA trust

Relocatable wheels cannot rely on OpenSSL CA paths from the build machine. Linux
clients resolve CA paths at runtime and honor `SSL_CERT_FILE` and `SSL_CERT_DIR`.
WSS uses native Windows and macOS trust through the pinned Boost.Certify headers so
platform trust and distrust rules remain effective. HTTP uses libcurl's selected TLS
backend and native CA support.

See [Client TLS trust](../../user/extensions/web.md#client-tls-trust) for the public
trust behavior and `ca_file` contract.

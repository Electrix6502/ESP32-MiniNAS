# MiniNAS integration and local fixes

Upstream: https://github.com/sahlberg/libsmb2

Base commit: `5199123de81d66e80f0d6c996449ea6501db6fcb`.
All additions/modifications are LGPL-2.1-or-later; original notices are retained.

1. `library.json`: compile the portable core C sources as a PlatformIO Arduino
   library using the ESP configuration, without Kerberos or external transports.
2. `include/esp/config.h`: unused macro, TCP header, existing readv/writev/getlogin_r
   portability fallbacks needed by the pinned ESP-IDF 4.4 / Arduino 2.0.17 build.
3. `lib/compat.c`: include config.h so these fallback implementations are built.
4. `include/smb2/mininas-server.h`, `lib/mininas-server.c`: small bridge for
   request TreeId, MessageId-preserving replies, NTSTATUS errors, connection
   close and the ESP-only gethostname fallback.
5. `lib/smb2-cmd-query-info.c`: passthrough also bypasses known info encoders;
   the adapter supplies bounded on-wire file/filesystem records.
6. `lib/libsmb2.c`, QUERY_DIRECTORY: save the request name before invoking a
   handler which may queue a reply and release the request PDU. Prevents reading
   req->name after the request was freed.
7. `lib/libsmb2.c`, SESSION_SETUP: dispatch the actual command if it differs.
   Prevents a repeated LOGOFF from being interpreted as a SESSION_SETUP body.
8. `lib/libsmb2.c`, accept loop: an invalid/closed incoming TCP connection must
   not stop the entire listener.
9. `lib/smb2-cmd-negotiate.c`: encryption-context DataLength excludes padding;
   the old length broke SMB 3.1.1 negotiation with Impacket.
10. `lib/unicode.c`: byte-oriented UTF-16LE reads support unaligned NTLM strings.

The real file adapter is `src/SmbServer.cpp`. Unsupported features return an
explicit unsupported status. Guest access has no password database.

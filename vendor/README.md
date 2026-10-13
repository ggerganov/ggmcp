# Vendored single-header libraries

ggmcp has no package manager and no build-time downloads: the two third-party
libraries it uses each ship as a single header and are checked in here.

| Library | Version | File | License |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | [`nlohmann/json.hpp`](nlohmann/json.hpp) | MIT ([`nlohmann/LICENSE`](nlohmann/LICENSE)) |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.1 | [`httplib.h`](httplib.h) | MIT ([`LICENSE.httplib`](LICENSE.httplib)) |

## Who includes what

* `include/ggmcp/json.hpp` includes `<nlohmann/json.hpp>` and aliases it as
  `ggmcp::Value`, so the whole project - and its tests - sees one JSON type.
  `src/json.cpp` is the only file with JSON behaviour of its own: a
  nesting-depth guard in front of nlohmann's parser, `find()` returning a
  pointer instead of an iterator, and `dump()` for serialization.
* `src/http_server.cpp` includes `<httplib.h>` and nothing else does. The
  server is built behind a pimpl so that `include/ggmcp/http_server.hpp`,
  which the rest of ggmcp includes, stays free of a 340 KB header.

Both are on the include path via CMake:

```cmake
target_include_directories(ggmcp_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
```

cpp-httplib is used **without TLS**: `CPPHTTPLIB_OPENSSL_SUPPORT` is not
defined, so there is no HTTPS here - bind to loopback or put a proxy in front.

## Where ggmcp differs from the libraries' defaults

Three deliberate deviations, all in ggmcp's glue rather than in these headers:

* **Depth cap.** `ggmcp::parse()` refuses a document nested deeper than 128
  levels *before* handing it to nlohmann. nlohmann parses iteratively but
  serializes recursively, so the cap protects the writer rather than the
  reader, and refusing early means no tree is built for a hostile message.
* **`ggmcp::dump()` instead of `Value::dump()`.** nlohmann's default is to
  throw `type_error.316` on invalid UTF-8. A tool reading a binary file can
  return bytes that are not part of a character, and a path or header can
  carry a stray one, so ggmcp replaces invalid bytes with U+FFFD: the answer
  still goes out, as valid JSON.
* **Socket options.** cpp-httplib sets `SO_REUSEPORT` on the listening socket,
  which lets a second process bind the very same address and split the traffic
  with the first. `HttpServer::listen()` overrides the socket options with
  `SO_REUSEADDR` alone, so an address already in use stays an error the
  operator sees instead of a silently shared port.

## Updating

```bash
curl -fsSL -o nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/vX.Y.Z/single_include/nlohmann/json.hpp
curl -fsSL -o nlohmann/LICENSE \
  https://raw.githubusercontent.com/nlohmann/json/vX.Y.Z/LICENSE.MIT

curl -fsSL -o httplib.h \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/vX.Y.Z/httplib.h
curl -fsSL -o LICENSE.httplib \
  https://raw.githubusercontent.com/yhirose/cpp-httplib/vX.Y.Z/LICENSE
```

Update the versions in the table above, then run `make test`: the in-process
suite pins the JSON behaviour the project relies on, and the round-trip test
drives the HTTP server over a real socket.

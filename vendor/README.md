# Vendored backends

Header only copies, kept verbatim so the build needs no network and no package
manager. Each one is reached through an interface (`gg::json::IBackend`,
`gg::http::IServer`), so replacing a backend means writing one `.cpp` and adding a
name to `src/backends.cpp`.

| Upstream | Version | Used for | Files |
| --- | --- | --- | --- |
| [nlohmann/json](https://github.com/nlohmann/json) | `v3.12.0` | the JSON backend | `nlohmann_json/include/nlohmann/json.hpp`, `json_fwd.hpp` (from `single_include/`), `LICENSE.MIT` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | `v0.29.0` | the HTTP backend | `cpp-httplib/include/httplib.h`, `LICENSE` |

Both are MIT licensed; the license files are kept next to the sources.

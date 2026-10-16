# vendor

Third party headers, vendored so that a build needs no network.

| Path | Project | Version | License |
| --- | --- | --- | --- |
| `nlohmann/json.hpp` | [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | MIT (`nlohmann/LICENSE`) |
| `httplib.h` | [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.3 | MIT (`LICENSE.httplib`) |

Both are used as headers only. `nlohmann/json.hpp` is the JSON backend, `httplib.h`
is the HTTP backend, and both report their version in the start up banner.

Nothing here is edited. To update, drop in the new single header and bump the table.

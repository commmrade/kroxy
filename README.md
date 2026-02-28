### Kroxy

**Kroxy** is an educational reverse proxy for Linux written in modern C++ using the Boost.Asio and Boost.Beast libraries. It supports both HTTP and raw TCP stream proxying, multiple load‑balancing strategies, per‑upstream TLS, configurable timeouts, and flexible access logging.

> **Important:** Kroxy is an **educational project** and is **not production‑ready**. It is designed for learning purposes and experimentation, not for use in security‑ or reliability‑critical environments.

---

### Features

- **Reverse proxy for multiple protocols**
  - HTTP reverse proxy using Boost.Beast.
  - Generic TCP stream proxy (e.g. for custom protocols).
- **Configurable via JSON**
  - Single JSON file passed via `--config`.
  - Two modes: `http` and `stream`.
  - Per‑upstream settings and load‑balancing strategies.
  - Per‑connection timeouts (DNS resolve, connect, read, write).
- **Load balancing**
  - Round‑robin, first, least‑connections.
  - Host‑based (HTTP only).
  - SNI‑based routing.
- **TLS support**
  - Optional TLS termination on the server side (client → kroxy).
  - Optional TLS for upstream connections (kroxy → backend).
- **Logging**
  - Access log to an arbitrary file (including `/dev/stdout`).
  - Customizable log format with variables like `$client_addr`, `$request_uri`, `$status`, etc.
- **Multi‑process worker model**
  - Optional `--multiprocess` mode spawns multiple worker processes.
  - Master process monitors workers and respawns them on abnormal exit.

---

### High‑level architecture

- **Configuration layer (`config.hpp` / `config.cpp`)**
  - Parses a JSON config file on startup using JsonCpp.
  - Exposes a global `Config` singleton:
    - Chooses between `HttpConfig` and `StreamConfig` via `std::variant`.
    - Provides access to common settings (port, timeouts, logging, TLS).
    - Holds a map of named upstream server groups.
  - Builds `Upstream` objects for each defined server group and configures per‑upstream TLS.

- **Server and session layer (`server.*`, `session.*`, `httpsession.*`, `streamsession.*`)**
  - `Server` owns a `boost::asio::ip::tcp::acceptor` and shared TLS context.
  - On each incoming connection:
    - Creates either `HttpSession` or `StreamSession` depending on the configuration.
    - Optionally performs a server‑side TLS handshake (if `tls_enabled` is true).
    - Hands control to the session’s `run()` method.
  - `Session` is a polymorphic base providing:
    - A client `Stream` wrapper (optionally TLS).
    - A lazily‑created upstream `Stream` wrapper (optionally TLS).
    - Common error handling and timeout management via `boost::asio::steady_timer`.
    - Access to the configured `Upstream` group for backend selection.
  - `HttpSession`:
    - Uses Boost.Beast HTTP parsers/serializers.
    - Implements a request/response pipeline with header and body phases.
    - Applies configured HTTP headers and logs per‑request context (URI, method, status, user agent, byte counts).
  - `StreamSession`:
    - Implements bidirectional copying between client and service sockets (raw TCP).
    - Tracks bytes sent and timing for logging.

- **Upstream and load‑balancing layer (`upstream.*`)**
  - Abstract class `Upstream` stores:
    - A list of `Host { std::string host; unsigned short port; }`.
    - `UpstreamOptions` (proxy TLS options).
    - A shared TLS context for client connections, when enabled.
  - Concrete strategies:
    - `RoundRobinUpstream`
    - `FirstUpstream`
    - `LeastConnectionUpstream`
    - `HostBasedUpstream` (HTTP‑aware)
    - `SNIBasedUpstream` (SNI‑aware)
  - `Session` consults the selected `Upstream` to pick a backend host before resolving and connecting.

- **Process model (`worker.*`)**
  - `Master` manages a vector of `Worker` records (each is a child process).
  - `Worker::spawn` uses `fork()` plus Boost.Asio’s `notify_fork` hooks to create child processes that:
    - Own their own `io_context` loop.
    - Run the `Server` accept loop and handle connections.
  - Master:
    - Listens to `SIGCHLD` to detect worker termination.
    - Decides whether to respawn based on exit status or signal.
    - Handles termination signals (`SIGINT`, `SIGTERM`) by gracefully shutting down all workers.

- **Logging (`logger.*`)**
  - `Logger` writes timestamped log lines to an open file descriptor.
  - `format_log` from the config is processed to replace variables with per‑session values.
  - HTTP and stream sessions maintain small `LogContext` structs that accumulate information over the lifetime of a request/connection and flush logs at appropriate points.

---

### Building

Kroxy targets Linux and relies on:

- A modern C++ compiler (supporting at least C++20).
- Boost libraries (Asio, Beast, System, etc.).
- JsonCpp.
- `spdlog`.
- `argparse` for C++.

The exact build system configuration depends on your local environment (e.g. CMake setup and library locations). Typical steps:

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Refer to your local toolchain and CMake configuration for precise flags and dependency paths.

---

### Running kroxy

After building, you can run kroxy using a JSON configuration file:

```bash
./kroxy --config path/to/config.json
```

Options:

- **`-c, --config <path>`** (required): path to the JSON configuration file.
- **`-m, --multiprocess`** (flag, optional): enables multi‑process mode with the number of workers taken from `workers_num` in the configuration.

Example using the provided sample configs:

```bash
./kroxy --config conf/http.example.config.json
./kroxy --config conf/stream.example.config.json
```

See `CONFIGURATION.md` for a complete reference of the configuration format.

---

### Limitations and non‑production status

Kroxy is intentionally minimal and focuses on illustrating core reverse‑proxy concepts:

- It does not implement a full HTTP feature set (caching, HTTP/2/3, advanced header manipulation, health‑checks, etc.).
- Error handling, resilience, and security hardening are basic compared to mature proxies.
- Configuration is loaded only once at startup; there is no hot‑reload.
- The codebase assumes a Unix‑like environment and uses Linux‑specific APIs (e.g. `fork`, POSIX signals, low‑level file descriptors).

For production use cases, you should study battle‑tested proxies (such as nginx, Envoy, HAProxy) and treat kroxy as a **learning tool** and experimental playground.

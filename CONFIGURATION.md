### Kroxy JSON Configuration

This document describes the JSON configuration format used by **kroxy**, the educational reverse proxy written in C++ for Linux. It covers both `http` and `stream` server modes, load‑balancing options, TLS settings, timeouts, and logging.

The configuration is parsed by the code in `config.hpp` / `config.cpp` and loaded once at startup via the `Config::instance(path)` singleton.

---

### Top‑level structure

A configuration file must contain exactly one of the following top‑level objects:

- **`http`**: enables HTTP proxy mode.
- **`stream`**: enables raw TCP stream proxy mode.

Example:

```json
{
  "http": {
    ...
  }
}
```

or

```json
{
  "stream": {
    ...
  }
}
```

If neither `http` nor `stream` is present, kroxy will fail to start.

---

### Common server configuration fields

Both `http` and `stream` objects share a common set of fields (mapped to `CommonConfig` in the code):

- **`port`** (integer, optional): TCP port to listen on.
  - Default: `8080` (`DEFAULT_PORT`).
- **`workers_num`** (integer, optional): number of worker processes when `--multiprocess` is used.
  - Default: `1` (`DEFAULT_WORKERS_NUM`).
  - Must be greater than 0.
- **`proxy_to`** (string, required): name of the upstream server group to use by default.
  - Must match a key in the `servers` block, otherwise startup fails.

#### Timeouts (all in milliseconds)

These timeouts control how long kroxy waits for operations before aborting the session:

- **`send_timeout`**: timeout for sending data to the client.
- **`connect_timeout`**: timeout for establishing a connection to upstream.
- **`resolve_timeout`**: timeout for DNS resolution of upstream hostnames.
- **`proxy_read_timeout`**: timeout for reading from the upstream service.
- **`proxy_send_timeout`**: timeout for writing to the upstream service.

If omitted, each timeout falls back to the internal default (`DEFAULT_*_TIMEOUT`, currently 60 seconds).

---

### TLS settings (server side)

These options control TLS termination between clients and kroxy (used by `Server` and the `Stream` wrapper around the client socket):

- **`tls_enabled`** (boolean, optional):
  - If `true`, kroxy accepts TLS connections from clients.
  - If `false` or omitted, client connections are plain TCP.
- **`tls_cert_path`** (string, required if `tls_enabled` is `true`):
  - Path to the server certificate chain file.
- **`tls_key_path`** (string, required if `tls_enabled` is `true`):
  - Path to the server private key (PEM).
- **`tls_verify_client`** (boolean, optional):
  - If `true`, enables client certificate verification (`SSL_VERIFY_PEER` with default CA paths).
  - If `false` or omitted, client certs are not verified.

If `tls_enabled` is set but `tls_cert_path` or `tls_key_path` is missing or empty, kroxy will throw an error and exit.

---

### Logging configuration

Logging is controlled by two fields on the server object:

- **`file_log`** (string, optional):
  - Filesystem path to a log file (e.g. `"/dev/stdout"`).
  - If present, kroxy creates a `Logger` writing timestamped log lines to this path.
- **`format_log`** (string, optional but recommended):
  - Template string for access logs.
  - May include variables prefixed with `$`, which are expanded per request/connection.

#### Available log variables

The set of supported variables depends on server type but they all share the same `$name` syntax. Internally, they are represented by `LogFormat::Variable`:

- **Common (HTTP + stream)**:
  - **`$client_addr`**: client IP address.
  - **`$bytes_sent_upstream`**: bytes sent from kroxy to upstream.
  - **`$bytes_sent_downstream`**: bytes sent from kroxy to client.
  - **`$processing_time`**: total request/session processing time.

- **HTTP‑specific**:
  - **`$request_uri`**: full request URI.
  - **`$request_method`**: HTTP method (e.g. `GET`, `POST`).
  - **`$status`**: HTTP status code of the response.
  - **`$http_user_agent`**: value of the `User-Agent` request header.

Variables are discovered by scanning `format_log` for `$` and collecting all `[$A-Za-z_]` sequences. Unknown variables cause configuration parsing to fail.

#### Logging examples

HTTP example (from `conf/http.example.config.json`):

```json
"file_log": "/dev/stdout",
"format_log": "URI: $request_uri, method $request_method, status: $status, user agent: $http_user_agent, $bytes_sent_upstream b. $bytes_sent_downstream b."
```

Stream example (from `conf/stream.example.config.json`):

```json
"file_log": "/dev/stdout",
"format_log": "Client address is $client_addr, $bytes_sent_upstream $bytes_sent_downstream bytes have been sent, $processing_time"
```

---

### Upstream servers (`servers` block)

Each server type (`http` or `stream`) defines a **`servers`** object that describes upstream backends and load‑balancing behavior:

```json
"servers": {
  "name1": { ... },
  "name2": { ... }
}
```

Each key inside `servers` (e.g. `"google"`, `"httpbin"`) defines an upstream group and corresponds to a `std::shared_ptr<Upstream>` in code. The `proxy_to` field selects one of these groups as the default target.

#### Common upstream fields

For each upstream group:

- **`hosts`** (array, required):
  - List of host/port pairs:
  - Example:
    ```json
    "hosts": [
      { "host": "example.com", "port": 443 },
      { "host": "example.org", "port": 443 }
    ]
    ```
- **`balancing_algo`** (string, optional):
  - Chooses a load‑balancing strategy (see below).
  - If omitted or empty, defaults to `"round_robin"`.

#### Per‑upstream TLS options (client side)

These control TLS between kroxy and the upstream service. They are mapped to `UpstreamOptions` and used to configure a client‑side `Stream`:

- **`proxy_tls_enabled`** (boolean, optional):
  - If `true`, connections to this upstream group use TLS.
  - If `false` or omitted, connections are plain TCP.
- **`proxy_tls_verify`** (boolean, optional, only meaningful when `proxy_tls_enabled` is `true`):
  - If `true`, verify the upstream certificate.
  - If `false`, do not verify the certificate.
  - If present while `proxy_tls_enabled` is `false`, kroxy logs a warning and ignores it.
- **`proxy_tls_cert_path`** (string, optional, only meaningful when `proxy_tls_enabled` is `true`):
  - Path to a client certificate for mutual TLS, if used.
  - Ignored with a warning if `proxy_tls_enabled` is `false`.
- **`proxy_tls_key_path`** (string, optional, only meaningful when `proxy_tls_enabled` is `true`):
  - Private key path for the client certificate.
  - Ignored with a warning if `proxy_tls_enabled` is `false`.

---

### Load‑balancing algorithms

The `balancing_algo` field selects which `Upstream` implementation to use:

- **`"round_robin"`** (default):
  - Implemented by `RoundRobinUpstream`.
  - Cycles through `hosts` in order.
- **`"first"`**:
  - Implemented by `FirstUpstream`.
  - Always chooses the first host.
- **`"least_conn"`**:
  - Implemented by `LeastConnectionUpstream`.
  - Routes traffic to the host with the fewest active connections (tracked internally).
- **`"host"`**:
  - Implemented by `HostBasedUpstream`.
  - Uses HTTP `Host` header and client IP for deterministic routing.
  - **Only valid for `http` servers.** Using `"host"` with `stream` causes an error at config parse time.
- **`"sni"`**:
  - Implemented by `SNIBasedUpstream`.
  - Uses TLS SNI and other data from `BalancerData` to choose the host.

If an unknown `balancing_algo` is provided, configuration parsing fails with `"Unknown balancing algorithm"`.

---

### HTTP‑specific fields

These fields only apply when the top‑level object is `http` and are mapped to `HttpConfig`:

- **`headers`** (object, optional):
  - Static headers to add/override on outbound requests to the upstream service.
  - Values may contain simple macros like `$host` or `$addr` that kroxy expands at runtime based on the selected upstream host.
  - Example:
    ```json
    "headers": {
      "Host": "$host"
    }
    ```

- **`client_header_timeout`** (integer, optional):
  - Maximum time to wait for the full set of request headers from the client.
  - Default: `DEFAULT_CLIENT_HEADER_TIMEOUT` (60 seconds).

- **`client_body_timeout`** (integer, optional):
  - Maximum time to wait for the full request body.
  - Default: `DEFAULT_CLIENT_BODY_TIMEOUT` (60 seconds).

Kroxy uses these timeouts when scheduling timers in `HttpSession` for reading headers and body from the client. On timeout, it generates an HTTP timeout response and closes the session.

---

### Stream‑specific fields

These fields only apply when the top‑level object is `stream` and are mapped to `StreamConfig`:

- **`read_timeout`** (integer, optional):
  - Timeout for reading data from the client in stream mode.
  - Default: `DEFAULT_READ_TIMEOUT` (60 seconds).

Other timeout and logging fields mirror the HTTP configuration.

---

### Configuration lifecycle in code

- `Config::instance(path)` is called once at startup in `main.cpp` with the `-c/--config` argument.
- The configuration file is parsed into a `Config` object via `parse_config(path)`:
  - Validates presence of `http` or `stream`.
  - Builds either `HttpConfig` or `StreamConfig` inside the `server_config` variant.
  - Parses `servers`, upstream options, load‑balancing algorithms, TLS settings, and logging configuration.
  - Verifies that `proxy_to` refers to an existing server block.
- `Config::instance()` is then reused across the codebase:
  - `Server` uses it to determine the listening port and whether the listener is TLS‑enabled, and to choose between `HttpSession` and `StreamSession`.
  - `Session` and its subclasses use it to:
    - Obtain upstream groups (`get_upstream()`).
    - Read timeout values for client and upstream operations.
    - Configure logging through `Logger`.

Because `Config` is a process‑wide singleton, the configuration is **static for the lifetime of the process**: it is not reloaded at runtime. Any configuration change requires restarting the kroxy process.

---

### Example configurations

See the `conf/` directory for working examples:

- **`http.example.config.json`**: demonstrates HTTP mode, multiple upstreams, different load‑balancing algorithms, and detailed logging.
- **`stream.example.config.json`**: demonstrates TCP stream mode with simple logging and basic timeouts.

There are also smaller JSON configs under `tests/` that cover specific scenarios (timeouts, chunked HTTP, etc.).


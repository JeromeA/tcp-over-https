# ssh_over_https

Tools to tunnel arbitrary TCP connections such as SSH through HTTPS requests using the SCGI protocol.

## Building

A simple Makefile builds the backend and frontend along with their tests. The frontend depends on libcurl.

```
make            # builds tunnel_backend_server and tunnel_frontend_server
make test       # builds and runs tests
make clean      # removes binaries
```

## Protocol

To tunnel data, the frontend sends it as the body of an HTTP POST to an SCGI endpoint. The HTTP server (any SCGI-capable server such as lighttpd or Apache) forwards the request to `tunnel_backend_server`, which appends the bytes to a persistent TCP connection on localhost. After writing the request body, the backend drains any immediately available bytes from that connection and returns them as the HTTP response with `Content-Type: application/octet-stream`. Subsequent POSTs continue the conversation over the same target socket; if the target closes, the backend reconnects on the next request. `tunnel_frontend_server` uses libcurl to make these HTTP(S) requests and exposes a local TCP port.

## Back end

`tunnel_backend_server` is a minimal SCGI application. Each HTTP request appends bytes to a persistent connection on localhost, then returns any pending bytes from that connection as the HTTP response. The server automatically reconnects to the target when it closes, and request/response sizes are capped by constants in the source.

### Example config for lighttpd

```
server.modules += ( "mod_scgi" )

scgi.server = (
  "/tunnel" => (
    "main" => (
      "host"         => "127.0.0.1",
      "port"         => 9001,
      "check-local"  => "disable"
    )
  )
)
```

## Front end

`tunnel_frontend_server` exposes a local TCP port and uses libcurl to exchange bytes with the backend. Data read from the local connection is sent in HTTP POST requests; response bytes are written back to the local socket. The client polls the backend when idle using an exponential backoff.

### Example to run it

```
./tunnel_frontend_server 2222 https://example.com/tunnel
# then connect to the local port, e.g.
ssh -p 2222 user@localhost
```

This forwards the SSH session through the remote HTTPS endpoint.

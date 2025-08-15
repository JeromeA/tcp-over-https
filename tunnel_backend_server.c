// See README.md for detailed protocol and usage information.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_HDRS 65536          // max bytes for SCGI headers netstring
#define MAX_BODY 10485760       // 10 MiB cap for request body (safety)
#define MAX_RESP 10485760       // 10 MiB cap for per-request readback

static volatile sig_atomic_t keep_running = 1;
static void on_sigint(int sig){ (void)sig; keep_running = 0; }

static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

static ssize_t read_n(int fd, void *buf, size_t n) {
    size_t off = 0; char *p = (char*)buf;
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0) return off; // EOF
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            return -1;
        }
        off += (size_t)r;
    }
    return (ssize_t)off;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t off = 0; const char *p = (const char*)buf;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(1000); continue; }
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

// === SCGI parsing ===
static int read_netstring(int fd, char **out, size_t *outlen) {
    char lenbuf[32]; size_t l = 0;
    while (l < sizeof(lenbuf)-1) {
        char c; ssize_t r = read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == ':') break;
        if (c < '0' || c > '9') return -1;
        lenbuf[l++] = c;
    }
    if (l == 0 || l >= sizeof(lenbuf)-1) return -1;
    lenbuf[l] = 0;
    long n = strtol(lenbuf, NULL, 10);
    if (n < 0 || n > (long)MAX_HDRS) return -1;
    char *payload = (char*)malloc((size_t)n + 1);
    if (!payload) return -1;
    if (read_n(fd, payload, (size_t)n) != n) { free(payload); return -1; }
    payload[n] = 0;
    char comma;
    if (read_n(fd, &comma, 1) != 1 || comma != ',') { free(payload); return -1; }
    *out = payload; *outlen = (size_t)n;
    return 0;
}

static const char* kv_get(const char *hdrs, size_t len, const char *key) {
    size_t klen = strlen(key);
    size_t i = 0;
    while (i < len) {
        const char *k = hdrs + i; size_t ks = strnlen(k, len - i); i += ks + 1;
        if (i >= len) break;
        const char *v = hdrs + i; size_t vs = strnlen(v, len - i); i += vs + 1;
        if (ks == klen && memcmp(k, key, klen) == 0) return v;
    }
    return NULL;
}

// === Persistent target connection ===
static int target_fd = -1;          // persistent across requests
static uint16_t target_port_g = 0;

static void close_target(void){ if (target_fd >= 0) { close(target_fd); target_fd = -1; } }

static int connect_local(uint16_t port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    set_nonblock(s);
    return s;
}

static int ensure_target(void){
    if (target_fd >= 0) return 0;
    target_fd = connect_local(target_port_g);
    return (target_fd >= 0) ? 0 : -1;
}

static int forward_body_to_target(const char *body, size_t body_len){
    if (ensure_target() < 0) return -1;
    ssize_t w = write_all(target_fd, body, body_len);
    if (w < 0) {
        // Try one reconnect (simple robustness)
        close_target();
        if (ensure_target() < 0) return -1;
        w = write_all(target_fd, body, body_len);
        if (w < 0) { close_target(); return -1; }
    }
    return 0;
}

static ssize_t drain_target(char *dst, size_t cap){
    if (ensure_target() < 0) return -1;
    size_t off = 0;
    for (;;) {
        if (off == cap) break;
        ssize_t r = recv(target_fd, dst + off, cap - off, MSG_DONTWAIT);
        if (r > 0) { off += (size_t)r; continue; }
        if (r == 0) { // target closed
            close_target();
            break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // no more right now
        if (errno == EINTR) continue;
        // other error: treat as closed for simplicity
        close_target();
        break;
    }
    return (ssize_t)off; // may be 0
}

// === Handle a single SCGI request over client_fd ===
static int handle_scgi_request(int client_fd) {
    // 1) Read netstring headers
    char *hdrs = NULL; size_t hdrs_len = 0;
    if (read_netstring(client_fd, &hdrs, &hdrs_len) < 0) {
        const char *msg = "Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\ninvalid SCGI netstring\n";
        write_all(client_fd, msg, strlen(msg));
        return -1;
    }
    // 2) Validate
    const char *scgi = kv_get(hdrs, hdrs_len, "SCGI");
    const char *clen = kv_get(hdrs, hdrs_len, "CONTENT_LENGTH");
    if (!scgi || strcmp(scgi, "1") != 0 || !clen) {
        const char *msg = "Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nmissing SCGI or CONTENT_LENGTH\n";
        write_all(client_fd, msg, strlen(msg));
        free(hdrs);
        return -1;
    }
    long body_len = strtol(clen, NULL, 10);
    if (body_len < 0 || body_len > (long)MAX_BODY) {
        const char *msg = "Status: 413 Payload Too Large\r\nContent-Type: text/plain\r\n\r\nbody too large\n";
        write_all(client_fd, msg, strlen(msg));
        free(hdrs);
        return -1;
    }

    // 3) Read request body
    char *body = NULL;
    if (body_len > 0) {
        body = (char*)malloc((size_t)body_len);
        if (!body) { free(hdrs); return -1; }
        if (read_n(client_fd, body, (size_t)body_len) != body_len) {
            const char *msg = "Status: 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nshort body\n";
            write_all(client_fd, msg, strlen(msg));
            free(hdrs); free(body);
            return -1;
        }
    }

    // 4) Forward body to persistent target
    if (body_len > 0) {
        if (forward_body_to_target(body, (size_t)body_len) < 0) {
            const char *msg = "Status: 502 Bad Gateway\r\nContent-Type: text/plain\r\n\r\nwrite to target failed\n";
            write_all(client_fd, msg, strlen(msg));
            free(hdrs); if (body) free(body);
            return -1;
        }
    } else {
        // Even with zero-length body, we still drain target below.
    }

    // 5) Non-blocking drain of any bytes currently available from target
    char *resp = (char*)malloc(MAX_RESP);
    if (!resp) { free(hdrs); if (body) free(body); return -1; }
    ssize_t got = drain_target(resp, MAX_RESP);
    if (got < 0) {
        const char *msg = "Status: 502 Bad Gateway\r\nContent-Type: text/plain\r\n\r\nread from target failed\r";
        write_all(client_fd, msg, strlen(msg));
        free(hdrs); if (body) free(body); free(resp);
        return -1;
    }

    // 6) Reply to lighttpd
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "Status: 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %zu\r\n\r\n",
                        (size_t)got);
    if (hlen < 0 || hlen >= (int)sizeof(header))
        hlen = (int)strlen("Status: 200 OK\r\n\r\n");
    write_all(client_fd, header, (size_t)hlen);
    if (got > 0) write_all(client_fd, resp, (size_t)got);

    free(hdrs); if (body) free(body); free(resp);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <scgi_listen_port> <target_local_port>\n", argv[0]);
        return 1;
    }
    int scgi_port = atoi(argv[1]);
    int target_port = atoi(argv[2]);
    if (scgi_port <= 0 || scgi_port > 65535 || target_port <= 0 || target_port > 65535) {
        fprintf(stderr, "invalid port\n");
        return 1;
    }
    target_port_g = (uint16_t)target_port;

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)scgi_port);

    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(srv); return 1; }
    if (listen(srv, 64) < 0) { perror("listen"); close(srv); return 1; }
    fprintf(stderr, "SCGI tunnel listening on 127.0.0.1:%d → localhost:%d (persistent target)\n", scgi_port, target_port);

    while (keep_running) {
        struct sockaddr_in cli; socklen_t cl = sizeof(cli);
        int fd = accept(srv, (struct sockaddr*)&cli, &cl);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept"); break;
        }
        handle_scgi_request(fd);
        close(fd);
    }

    close_target();
    close(srv);
    return 0;
}

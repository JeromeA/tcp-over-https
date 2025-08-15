#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>

#define BUF_SIZE 65536

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t off = 0; const unsigned char *p = buf;
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return (ssize_t)off;
}

struct mem_buf {
    unsigned char *data;
    size_t len;
};

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct mem_buf *mb = userdata;
    unsigned char *nbuf = realloc(mb->data, mb->len + total);
    if (!nbuf) return 0;
    memcpy(nbuf + mb->len, ptr, total);
    mb->data = nbuf;
    mb->len += total;
    return total;
}

static int http_exchange(CURL *curl, const char *url,
                         const unsigned char *body, size_t body_len,
                         unsigned char **out, size_t *out_len) {
    struct mem_buf mb = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_len > 0 ? (const char*)body : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mb);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    hdrs = curl_slist_append(hdrs, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    if (res != CURLE_OK) { free(mb.data); return -1; }
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) { free(mb.data); return -1; }
    *out = mb.data;
    *out_len = mb.len;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <listen_port> <url>\n", argv[0]);
        return 1;
    }
    int listen_port = atoi(argv[1]);
    const char *url = argv[2];

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)listen_port);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(srv); return 1; }
    if (listen(srv, 1) < 0) { perror("listen"); close(srv); return 1; }
    fprintf(stderr, "frontend listening on 127.0.0.1:%d -> %s\n", listen_port, url);
    int conn = accept(srv, NULL, NULL);
    if (conn < 0) { perror("accept"); close(srv); return 1; }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL *curl = curl_easy_init();
    if (!curl) { fprintf(stderr, "curl init failed\n"); close(conn); close(srv); return 1; }

    double delay = 0.1;
    const double max_delay = 10.0;
    unsigned char buf[BUF_SIZE];

    while (1) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(conn, &rfds);
        struct timeval tv;
        tv.tv_sec = (int)delay;
        tv.tv_usec = (int)((delay - tv.tv_sec) * 1000000.0);
        int r = select(conn + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        size_t send_len = 0;
        if (r > 0 && FD_ISSET(conn, &rfds)) {
            ssize_t rd = read(conn, buf, sizeof(buf));
            if (rd < 0) {
                if (errno == EINTR) continue;
                perror("read");
                break;
            }
            if (rd == 0) {
                // peer closed
                break;
            }
            send_len = (size_t)rd;
        }

        unsigned char *resp = NULL; size_t resp_len = 0;
        if (http_exchange(curl, url, send_len ? buf : NULL, send_len, &resp, &resp_len) != 0) {
            fprintf(stderr, "http exchange failed\n");
            free(resp);
            break;
        }
        if (resp_len > 0) {
            if (write_all(conn, resp, resp_len) < 0) {
                perror("write");
                free(resp);
                break;
            }
            free(resp);
            delay = 0.1;
        } else {
            free(resp);
            if (send_len == 0) {
                delay *= 2;
                if (delay > max_delay) delay = max_delay;
            } else {
                delay = 0.1;
            }
        }
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    close(conn);
    close(srv);
    return 0;
}


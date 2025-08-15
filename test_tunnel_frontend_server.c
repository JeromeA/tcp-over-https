#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned char req1[1024];
static size_t req1_len = 0;
static unsigned char req2[1024];
static size_t req2_len = 0;

static int read_full(int fd, unsigned char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static void *http_server_thread(void *arg) {
    int port = *(int *)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("http server socket"); exit(1); }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("http bind"); exit(1); }
    if (listen(srv, 1) < 0) { perror("http listen"); exit(1); }
    for (int i = 0; i < 2; i++) {
        int conn = accept(srv, NULL, NULL);
        if (conn < 0) { perror("http accept"); exit(1); }
        char hdr[1024]; size_t h = 0;
        while (h < sizeof(hdr) - 1) {
            ssize_t r = read(conn, hdr + h, 1);
            if (r <= 0) { perror("read hdr"); exit(1); }
            h += (size_t)r;
            if (h >= 4 && memcmp(hdr + h - 4, "\r\n\r\n", 4) == 0) break;
        }
        hdr[h] = 0;
        const char *cl = strstr(hdr, "Content-Length:");
        int len = cl ? atoi(cl + strlen("Content-Length:")) : 0;
        unsigned char buf[1024];
        if (len > 0) {
            if (read_full(conn, buf, (size_t)len) < 0) { perror("read body"); exit(1); }
        }
        if (i == 0) { memcpy(req1, buf, (size_t)len); req1_len = (size_t)len; }
        else { memcpy(req2, buf, (size_t)len); req2_len = (size_t)len; }
        const char *body = (i == 0) ? "world" : "again";
        int blen = strlen(body);
        char resp[256];
        int l = snprintf(resp, sizeof(resp),
                         "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n",
                         blen);
        if (write(conn, resp, l) != l) { perror("write resp hdr"); exit(1); }
        if (blen > 0 && write(conn, body, blen) != blen) { perror("write resp body"); exit(1); }
        close(conn);
    }
    close(srv);
    return NULL;
}

int main() {
    int base = 30000 + (getpid() % 10000);
    int front_port = base;
    int http_port = base + 1;

    pthread_t tid;
    if (pthread_create(&tid, NULL, http_server_thread, &http_port) != 0) {
        perror("pthread_create");
        return 1;
    }

    pid_t child = fork();
    if (child == 0) {
        char port_s[16]; sprintf(port_s, "%d", front_port);
        char url[64]; sprintf(url, "http://127.0.0.1:%d", http_port);
        execl("./tunnel_frontend_server", "./tunnel_frontend_server", port_s, url, NULL);
        perror("execl");
        _exit(1);
    }

    sleep(1); // allow frontend to start

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("client socket"); return 1; }
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)front_port);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect frontend");
        return 1;
    }

    const unsigned char msg[] = "hello";
    if (write(fd, msg, sizeof(msg) - 1) != (ssize_t)(sizeof(msg) - 1)) {
        perror("write msg");
    }

    unsigned char buf[16];
    if (read_full(fd, buf, 5) == 0 && memcmp(buf, "world", 5) == 0) {
        printf("[test] first response ok\n");
    } else {
        fprintf(stderr, "[test] first response mismatch\n");
    }

    fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
    struct timeval tv; tv.tv_sec = 5; tv.tv_usec = 0;
    int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (sel > 0 && read_full(fd, buf, 5) == 0 && memcmp(buf, "again", 5) == 0) {
        printf("[test] poll response ok\n");
    } else {
        fprintf(stderr, "[test] poll response mismatch\n");
    }

    if (req1_len == sizeof(msg) - 1 && memcmp(req1, msg, sizeof(msg) - 1) == 0) {
        printf("[test] server received first body\n");
    } else {
        fprintf(stderr, "[test] server body mismatch on first\n");
    }
    if (req2_len == 0) {
        printf("[test] second request had empty body\n");
    } else {
        fprintf(stderr, "[test] second request body not empty\n");
    }

    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    close(fd);
    pthread_join(tid, NULL);
    return 0;
}

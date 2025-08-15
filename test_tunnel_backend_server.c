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

static int data_conn_fd = -1;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned char data_buf[1024];
static size_t data_len = 0;
static volatile int done_flag = 0;

static void hexdump(const char *prefix, const unsigned char *buf, size_t len) {
    printf("%s", prefix);
    for (size_t i = 0; i < len; i++) printf("%02x ", buf[i]);
    printf("\n");
}

static void *data_server_thread(void *arg) {
    int port = *(int *)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("data server socket"); exit(1); }
    int one = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("data server bind"); exit(1); }
    if (listen(srv, 1) < 0) { perror("data server listen"); exit(1); }
    printf("[data] listening on 127.0.0.1:%d\n", port);
    int conn = accept(srv, NULL, NULL);
    if (conn < 0) { perror("data server accept"); exit(1); }
    printf("[data] accepted connection\n");
    data_conn_fd = conn;
    while (!done_flag) {
        unsigned char buf[256];
        ssize_t r = read(conn, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("[data] read");
            break;
        }
        if (r == 0) {
            printf("[data] connection closed\n");
            break;
        }
        hexdump("[data] received: ", buf, (size_t)r);
        pthread_mutex_lock(&data_mutex);
        if (data_len + (size_t)r < sizeof(data_buf)) {
            memcpy(data_buf + data_len, buf, (size_t)r);
            data_len += (size_t)r;
        }
        pthread_mutex_unlock(&data_mutex);
    }
    close(conn);
    close(srv);
    return NULL;
}

static int read_full(int fd, unsigned char *buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, buf + off, n - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return 0;
}

static int send_scgi(int port, const unsigned char *body, size_t body_len,
                     unsigned char **resp, size_t *resp_len) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1; }
    char hdr[256];
    int pos = 0;
    pos += sprintf(hdr + pos, "CONTENT_LENGTH");
    hdr[pos++] = 0;
    pos += sprintf(hdr + pos, "%zu", body_len);
    hdr[pos++] = 0;
    pos += sprintf(hdr + pos, "SCGI");
    hdr[pos++] = 0;
    pos += sprintf(hdr + pos, "1");
    hdr[pos++] = 0;
    int hdr_len = pos;
    char pre[32];
    int pre_len = sprintf(pre, "%d:", hdr_len);
    if (write(fd, pre, pre_len) != pre_len ||
        write(fd, hdr, hdr_len) != hdr_len ||
        write(fd, ",", 1) != 1) {
        perror("write headers"); close(fd); return -1; }
    if (body_len > 0 && write(fd, body, body_len) != (ssize_t)body_len) {
        perror("write body"); close(fd); return -1; }
    printf("[client] sent %zu bytes\n", body_len);
    char resp_hdr[1024]; size_t h = 0;
    while (h < sizeof(resp_hdr) - 1) {
        ssize_t r = read(fd, resp_hdr + h, 1);
        if (r <= 0) { perror("read resp hdr"); close(fd); return -1; }
        h += (size_t)r;
        if (h >= 4 && memcmp(resp_hdr + h - 4, "\r\n\r\n", 4) == 0) break;
    }
    resp_hdr[h] = 0;
    printf("[client] response headers:\n%s", resp_hdr);
    const char *cl = strstr(resp_hdr, "Content-Length:");
    if (!cl) { fprintf(stderr, "missing Content-Length\n"); close(fd); return -1; }
    int len = atoi(cl + strlen("Content-Length:"));
    *resp_len = (size_t)len;
    *resp = malloc(len);
    if (len > 0) {
        if (read_full(fd, *resp, len) < 0) { perror("read body"); close(fd); return -1; }
        hexdump("[client] response body: ", *resp, (size_t)len);
    } else {
        printf("[client] response body empty\n");
    }
    close(fd);
    return 0;
}

int main() {
    int base = 30000 + (getpid() % 10000);
    int data_port = base;
    int scgi_port = base + 1;
    pthread_t tid;
    if (pthread_create(&tid, NULL, data_server_thread, &data_port) != 0) {
        perror("pthread_create");
        return 1;
    }
    pid_t child = fork();
    if (child == 0) {
        char port1[16], port2[16];
        sprintf(port1, "%d", scgi_port);
        sprintf(port2, "%d", data_port);
        execl("./tunnel_backend_server", "./tunnel_backend_server", port1, port2, NULL);
        perror("execl");
        _exit(1);
    }
    printf("[main] started tunnel_backend_server pid=%d\n", child);
    sleep(1); // allow server to start

    size_t last = 0;
    unsigned char *resp = NULL; size_t resp_len = 0;
    const unsigned char body1[] = "hello";
    if (send_scgi(scgi_port, body1, sizeof(body1) - 1, &resp, &resp_len) != 0) goto cleanup;
    free(resp);
    pthread_mutex_lock(&data_mutex);
    if (data_len - last == sizeof(body1) - 1 &&
        memcmp(data_buf + last, body1, sizeof(body1) - 1) == 0) {
        printf("[main] data server received first body correctly\n");
    } else {
        fprintf(stderr, "[main] data server mismatch on first body\n");
    }
    last = data_len;
    pthread_mutex_unlock(&data_mutex);

    const unsigned char reply[] = "back";
    printf("[main] data server sending reply bytes\n");
    write(data_conn_fd, reply, sizeof(reply) - 1);
    usleep(100000);

    const unsigned char body2[] = "world";
    if (send_scgi(scgi_port, body2, sizeof(body2) - 1, &resp, &resp_len) != 0) goto cleanup;
    if (resp_len == sizeof(reply) - 1 &&
        memcmp(resp, reply, sizeof(reply) - 1) == 0) {
        printf("[main] second response matches expected bytes from data server\n");
    } else {
        fprintf(stderr, "[main] second response mismatch\n");
    }
    free(resp);
    pthread_mutex_lock(&data_mutex);
    if (data_len - last == sizeof(body2) - 1 &&
        memcmp(data_buf + last, body2, sizeof(body2) - 1) == 0) {
        printf("[main] data server received second body correctly\n");
    } else {
        fprintf(stderr, "[main] data server mismatch on second body\n");
    }
    pthread_mutex_unlock(&data_mutex);

cleanup:
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    done_flag = 1;
    if (data_conn_fd >= 0) {
        shutdown(data_conn_fd, SHUT_RDWR);
        close(data_conn_fd);
    }
    pthread_join(tid, NULL);
    printf("[main] test complete\n");
    return 0;
}


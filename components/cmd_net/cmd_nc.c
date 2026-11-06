#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "argtable3/argtable3.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "shell.h"
#include "net.h"
#include "cmd_net_priv.h"

#define NC_CONNECT_TIMEOUT_S 10
#define NC_TICK_MS 100

static const char *TAG = "cmd_nc";

static struct {
    struct arg_lit *udp;
    struct arg_lit *listen;
    struct arg_lit *zero;
    struct arg_lit *crlf;
    struct arg_lit *verbose;
    struct arg_int *wait;
    struct arg_str *host;
    struct arg_str *port;
    struct arg_end *end;
} s_nc_args;

typedef struct {
    bool udp;
    bool listen;
    bool crlf;
    bool verbose;
    int wait_s;
    int sock;
    struct sockaddr_storage peer;
    socklen_t peer_len;
} nc_t;

typedef enum { INPUT_NONE, INPUT_CHAR, INPUT_ABORT } input_t;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static input_t read_input(int *c)
{
    unsigned char ch;
    if (read(fileno(stdin), &ch, 1) != 1) {
        return INPUT_ABORT;
    }
    *c = ch;
    return ch == SHELL_CTRL_C ? INPUT_ABORT : INPUT_CHAR;
}

static int wait_sock(int sock, bool for_write, int timeout_ms)
{
    int in = fileno(stdin);
    int64_t deadline = now_ms() + timeout_ms;
    for (;;) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(in, &rfds);
        FD_SET(sock, for_write ? &wfds : &rfds);
        struct timeval tv = { .tv_usec = NC_TICK_MS * 1000 };
        int r = select((sock > in ? sock : in) + 1, &rfds, &wfds, NULL, &tv);
        if (r < 0) {
            return -1;
        }
        int c;
        if (FD_ISSET(in, &rfds) && read_input(&c) == INPUT_ABORT) {
            return -1;
        }
        if (FD_ISSET(sock, for_write ? &wfds : &rfds)) {
            return 1;
        }
        if (timeout_ms > 0 && now_ms() > deadline) {
            return 0;
        }
    }
}

static bool send_all(nc_t *nc, const char *buf, size_t len)
{
    if (nc->udp && nc->listen) {
        if (!nc->peer_len) {
            fprintf(stderr, "nc: no peer yet, dropped\n");
            return true;
        }
        return sendto(nc->sock, buf, len, 0, (struct sockaddr *)&nc->peer, nc->peer_len) >= 0;
    }
    while (len) {
        ssize_t n = send(nc->sock, buf, len, 0);
        if (n < 0) {
            if (errno == EAGAIN && wait_sock(nc->sock, true, 5000) == 1) {
                continue;
            }
            return false;
        }
        buf += n;
        len -= n;
    }
    return true;
}

static int open_client(nc_t *nc, const char *host, const char *port)
{
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = nc->udp ? SOCK_DGRAM : SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        fprintf(stderr, "nc: cannot resolve %s\n", host);
        return -1;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s < 0) {
        freeaddrinfo(res);
        fprintf(stderr, "nc: socket: %s\n", strerror(errno));
        return -1;
    }
    set_nonblocking(s);
    int r = connect(s, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno == EINPROGRESS) {
        int timeout_s = nc->wait_s ? nc->wait_s : NC_CONNECT_TIMEOUT_S;
        int ready = wait_sock(s, true, timeout_s * 1000);
        int err = 0;
        socklen_t len = sizeof(err);
        if (ready == 1) {
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len);
        }
        r = ready == 1 && err == 0 ? 0 : -1;
        errno = ready == 0 ? ETIMEDOUT : ready < 0 ? EINTR : err;
    }
    if (r < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    return s;
}

static int open_listener(nc_t *nc, int port)
{
    int s = socket(AF_INET, nc->udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "nc: socket: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || (!nc->udp && listen(s, 1) < 0)) {
        fprintf(stderr, "nc: bind %d: %s\n", port, strerror(errno));
        close(s);
        return -1;
    }
    set_nonblocking(s);
    if (nc->verbose) {
        fprintf(stderr, "listening on port %d %s\n", port, nc->udp ? "udp" : "tcp");
    }
    if (nc->udp) {
        return s;
    }

    if (wait_sock(s, false, 0) != 1) {
        close(s);
        return -1;
    }
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int c = accept(s, (struct sockaddr *)&peer, &len);
    close(s);
    if (c < 0) {
        fprintf(stderr, "nc: accept: %s\n", strerror(errno));
        return -1;
    }
    set_nonblocking(c);
    if (nc->verbose) {
        fprintf(stderr, "connection from %s:%d\n", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
    }
    return c;
}

static bool on_socket_readable(nc_t *nc)
{
    char buf[512];
    struct sockaddr_storage from;
    socklen_t from_len = sizeof(from);
    ssize_t n = nc->udp && nc->listen
                ? recvfrom(nc->sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len)
                : recv(nc->sock, buf, sizeof(buf), 0);
    if (n < 0) {
        return errno == EAGAIN;
    }
    if (nc->udp && nc->listen) {
        nc->peer = from;
        nc->peer_len = from_len;
    }
    if (n == 0 && !nc->udp) {
        return false;
    }
    fwrite(buf, 1, n, stdout);
    fflush(stdout);
    return true;
}

static void session(nc_t *nc)
{
    int in = fileno(stdin);
    char line[256];
    size_t len = 0;
    int prev = 0;
    int64_t idle_deadline = now_ms() + nc->wait_s * 1000;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(in, &rfds);
        FD_SET(nc->sock, &rfds);
        struct timeval tv = { .tv_usec = NC_TICK_MS * 1000 };
        if (select((nc->sock > in ? nc->sock : in) + 1, &rfds, NULL, NULL, &tv) < 0) {
            fprintf(stderr, "nc: select: %s\n", strerror(errno));
            return;
        }
        if (FD_ISSET(nc->sock, &rfds)) {
            if (!on_socket_readable(nc)) {
                return;
            }
            idle_deadline = now_ms() + nc->wait_s * 1000;
        }
        int c;
        if (FD_ISSET(in, &rfds)) {
            input_t r = read_input(&c);
            if (r == INPUT_ABORT || (r == INPUT_CHAR && c == SHELL_CTRL_D)) {
                return;
            }
            if (r == INPUT_CHAR) {
                if (c == '\r' || (c == '\n' && prev != '\r')) {
                    printf("\n");
                    if (nc->crlf) {
                        line[len++] = '\r';
                    }
                    line[len++] = '\n';
                    if (!send_all(nc, line, len)) {
                        fprintf(stderr, "nc: send: %s\n", strerror(errno));
                        return;
                    }
                    len = 0;
                } else if ((c == 0x7f || c == '\b') && len) {
                    len--;
                    printf("\b \b");
                } else if ((isprint(c) || c == '\t') && len < sizeof(line) - 2) {
                    line[len++] = c;
                    putchar(c);
                }
                prev = c;
                fflush(stdout);
                idle_deadline = now_ms() + nc->wait_s * 1000;
            }
        }
        if (nc->wait_s && now_ms() > idle_deadline) {
            return;
        }
    }
}

static bool parse_port(const char *s, int *port)
{
    char *end;
    long p = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || p < 1 || p > 65535) {
        fprintf(stderr, "nc: invalid port '%s'\n", s);
        return false;
    }
    *port = p;
    return true;
}

static int cmd_nc(int argc, char **argv)
{
    if (!shell_args_ok(argc, argv, &s_nc_args)) {
        return 1;
    }
    nc_t nc = {
        .udp = s_nc_args.udp->count,
        .listen = s_nc_args.listen->count,
        .crlf = s_nc_args.crlf->count,
        .verbose = s_nc_args.verbose->count,
        .wait_s = s_nc_args.wait->count ? s_nc_args.wait->ival[0] : 0,
    };
    bool zero = s_nc_args.zero->count;
    bool have_host = s_nc_args.host->count;
    bool have_port = s_nc_args.port->count;
    const char *host = s_nc_args.host->sval[0];
    const char *port_str = nc.listen && !have_port ? host : s_nc_args.port->sval[0];

    if (nc.wait_s < 0) {
        fprintf(stderr, "nc: -w must be >= 0\n");
        return 1;
    }
    if (nc.listen ? !have_host || have_port : !have_port) {
        fprintf(stderr, "usage: nc [-uCv] [-w sec] <host> <port>\n       nc -l [-uCv] [-w sec] <port>\n       nc -z [-v] [-w sec] <host> <port>\n");
        return 1;
    }
    if (zero && (nc.udp || nc.listen)) {
        fprintf(stderr, "nc: -z works with tcp client mode only\n");
        return 1;
    }
    int port;
    if (!parse_port(port_str, &port)) {
        return 1;
    }
    if (!net_has_ip()) {
        fprintf(stderr, "nc: network is down, see 'wifi connect'\n");
        return 1;
    }

    if (nc.listen) {
        nc.sock = open_listener(&nc, port);
    } else {
        nc.sock = open_client(&nc, host, port_str);
        bool ok = nc.sock >= 0;
        if (zero) {
            printf("%s %s: %s\n", host, port_str, ok ? "open" : "closed");
        } else if (!ok) {
            fprintf(stderr, "nc: connect to %s %s: %s\n", host, port_str, strerror(errno));
        } else if (nc.verbose) {
            fprintf(stderr, "connected to %s %s\n", host, port_str);
        }
        if (zero && ok) {
            close(nc.sock);
            return 0;
        }
    }
    if (nc.sock < 0) {
        return 1;
    }
    session(&nc);
    close(nc.sock);
    if (nc.verbose) {
        fprintf(stderr, "\nclosed\n");
    }
    return 0;
}

esp_err_t cmd_nc_register(void)
{
    s_nc_args.udp = arg_lit0("u", "udp", "use UDP instead of TCP");
    s_nc_args.listen = arg_lit0("l", "listen", "listen for one incoming connection");
    s_nc_args.zero = arg_lit0("z", NULL, "only check that the TCP port is open");
    s_nc_args.crlf = arg_lit0("C", "crlf", "send CRLF line endings");
    s_nc_args.verbose = arg_lit0("v", "verbose", "print connection info");
    s_nc_args.wait = arg_int0("w", "wait", "<sec>", "connect and idle timeout");
    s_nc_args.host = arg_str0(NULL, NULL, "<host>", "remote host; omit with -l");
    s_nc_args.port = arg_str0(NULL, NULL, "<port>", "remote port, or local port with -l");
    s_nc_args.end = arg_end(4);

    const esp_console_cmd_t cmd = {
        .command = "nc",
        .help = "Netcat: TCP/UDP client or listener; type lines, Ctrl+C or Ctrl+D to quit",
        .func = cmd_nc,
        .argtable = &s_nc_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "register nc");
    return ESP_OK;
}

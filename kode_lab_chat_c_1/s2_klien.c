/* s2_klien.c — klien chat: memantau keyboard dan socket sekaligus dengan poll().
 * Kompilasi: gcc -Wall -Wextra -O2 -o s2_klien s2_klien.c
 * Interaktif: ./s2_klien 5000
 * Skrip     : printf 'halo semua\n' | ./s2_klien 5000 budi 5
 *             (argumen ke-2 = nama otomatis, ke-3 = batas waktu hidup dalam detik)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static double detik(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1.0e9;
}

static int kirim_penuh(int fd, const char *buf, size_t n)
{
    while (n > 0) {
        ssize_t w = send(fd, buf, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        buf += w; n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    int         port  = (argc > 1) ? atoi(argv[1]) : 5000;
    const char *nama  = (argc > 2) ? argv[2] : NULL;
    double      batas = (argc > 3) ? atof(argv[3]) : 0.0;
    double      t0    = detik();

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    if (nama) {
        char baris[64];
        int m = snprintf(baris, sizeof baris, "%s\n", nama);
        if (kirim_penuh(s, baris, (size_t)m) < 0) { perror("send nama"); return 1; }
    }

    int stdin_hidup = 1;
    for (;;) {
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = s; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        if (stdin_hidup) { pfd[n].fd = 0; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }

        int rc = poll(pfd, (nfds_t)n, 200);
        if (rc < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        if (pfd[0].revents & POLLIN) {
            char buf[1024];
            ssize_t r = recv(s, buf, sizeof buf, 0);
            if (r == 0) { fprintf(stderr, "[klien] server menutup koneksi\n"); break; }
            if (r < 0)  { if (errno == EINTR) continue; perror("recv"); break; }
            fwrite(buf, 1, (size_t)r, stdout);
            fflush(stdout);
        } else if (pfd[0].revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "[klien] koneksi putus\n");
            break;
        }

        if (stdin_hidup && n > 1 && (pfd[1].revents & POLLIN)) {
            char baris[1024];
            if (!fgets(baris, sizeof baris, stdin)) {
                stdin_hidup = 0;            /* EOF: berhenti membaca, koneksi tetap hidup */
                continue;
            }
            if (kirim_penuh(s, baris, strlen(baris)) < 0) { perror("send"); break; }
        }

        if (batas > 0.0 && detik() - t0 >= batas) break;
        if (!stdin_hidup && batas == 0.0) { /* tanpa batas waktu, tunggu server saja */ }
    }
    close(s);
    return 0;
}

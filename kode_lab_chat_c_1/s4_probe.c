/* s4_probe.c — mengukur responsivitas server: kirim /stat, catat waktu sampai balasan datang.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s4_probe s4_probe.c
 * Jalankan  : ./s4_probe 5000 probe 8      (8 kali pengukuran, jeda 1 detik)
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

static double ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int         port = (argc > 1) ? atoi(argv[1]) : 5000;
    const char *nama = (argc > 2) ? argv[2] : "probe";
    int         ulang = (argc > 3) ? atoi(argv[3]) : 8;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    char baris[64];
    int m = snprintf(baris, sizeof baris, "%s\n", nama);
    send(s, baris, (size_t)m, 0);

    char buang[65536];
    double maks = 0.0;
    for (int i = 0; i < ulang; i++) {
        /* Kosongkan dulu semua pesan siaran yang menumpuk. */
        for (;;) {
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, 0) <= 0) break;
            if (recv(s, buang, sizeof buang, 0) <= 0) break;
        }

        double t0 = ms();
        const char *perintah = "/stat\n";
        if (send(s, perintah, strlen(perintah), 0) < 0) { perror("send"); break; }

        struct pollfd p = { s, POLLIN, 0 };
        int rc = poll(&p, 1, 5000);
        double lat = ms() - t0;
        if (rc <= 0) {
            printf("[probe] ukur#%d : TIDAK ADA BALASAN dalam 5000 ms\n", i + 1);
            maks = 5000.0;
        } else {
            ssize_t n = recv(s, buang, sizeof buang - 1, 0);
            if (n <= 0) { printf("[probe] koneksi ditutup server\n"); break; }
            if (lat > maks) maks = lat;
            printf("[probe] ukur#%d : balasan dalam %.1f ms\n", i + 1, lat);
        }
        fflush(stdout);
        usleep(1000 * 1000);
    }
    printf("[probe] latensi maksimum = %.1f ms\n", maks);
    close(s);
    return 0;
}

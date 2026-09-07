/* s1_klien.c — klien uji untuk Tahap 1 (blocking, kirim lalu tunggu balasan).
 * Kompilasi: gcc -Wall -Wextra -O2 -o s1_klien s1_klien.c
 * Jalankan  : printf 'halo\n' | ./s1_klien 5000 A
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static double waktu_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 5000;
    const char *label = (argc > 2) ? argv[2] : "?";
    double t0 = waktu_ms();

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }
    printf("[%s +%6.1f ms] connect() sukses\n", label, waktu_ms() - t0);
    fflush(stdout);

    char baris[512];
    while (fgets(baris, sizeof baris, stdin)) {
        size_t len = strlen(baris);
        if (send(s, baris, len, 0) < 0) { perror("send"); break; }
        printf("[%s +%6.1f ms] kirim   : %.*s\n", label, waktu_ms() - t0,
               (int)(len ? len - 1 : 0), baris);
        fflush(stdout);

        char buf[512];
        ssize_t n = recv(s, buf, sizeof buf - 1, 0);
        if (n <= 0) { printf("[%s] koneksi ditutup server\n", label); break; }
        buf[n] = '\0';
        printf("[%s +%6.1f ms] balasan : %s", label, waktu_ms() - t0, buf);
        fflush(stdout);
    }
    close(s);
    printf("[%s +%6.1f ms] selesai\n", label, waktu_ms() - t0);
    return 0;
}

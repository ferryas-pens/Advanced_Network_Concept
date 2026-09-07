/* s1_server_iteratif.c — Tahap 1: server ECHO satu klien pada satu waktu.
 * Tujuan: menunjukkan MENGAPA server iteratif gagal melayani banyak pengguna.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s1_server s1_server_iteratif.c
 * Jalankan  : ./s1_server 5000
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static int buat_listener(const char *ip, int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); exit(1); }

    int yes = 1;
    if (setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        perror("setsockopt"); exit(1);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "alamat tidak valid: %s\n", ip); exit(1);
    }

    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); exit(1); }
    if (listen(ls, 16) < 0) { perror("listen"); exit(1); }
    return ls;
}

int main(int argc, char **argv)
{
    int port = (argc > 1) ? atoi(argv[1]) : 5000;
    int ls = buat_listener("127.0.0.1", port);
    printf("[server] listen di 127.0.0.1:%d (iteratif)\n", port);
    fflush(stdout);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cs = accept(ls, (struct sockaddr *)&cli, &clen);
        if (cs < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof ipstr);
        printf("[server] MASUK  %s:%u\n", ipstr, ntohs(cli.sin_port));
        fflush(stdout);

        /* Selama loop ini berjalan, accept() TIDAK dipanggil lagi. */
        char buf[512];
        ssize_t n;
        while ((n = recv(cs, buf, sizeof buf, 0)) > 0) {
            printf("[server] terima %zd byte dari %s:%u\n", n, ipstr, ntohs(cli.sin_port));
            fflush(stdout);
            ssize_t w = send(cs, buf, (size_t)n, 0);   /* echo balik */
            if (w < 0) { perror("send"); break; }
        }
        if (n < 0) perror("recv");

        printf("[server] KELUAR %s:%u\n", ipstr, ntohs(cli.sin_port));
        fflush(stdout);
        close(cs);
    }
    close(ls);
    return 0;
}

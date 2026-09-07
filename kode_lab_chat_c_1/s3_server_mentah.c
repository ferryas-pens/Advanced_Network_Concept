/* s3_server_mentah.c — Tahap 3: membuktikan TCP adalah byte stream, bukan message stream.
 * Server ini tidak merakit baris. Ia hanya melaporkan setiap panggilan recv().
 * Kompilasi: gcc -Wall -Wextra -O2 -o s3_server s3_server_mentah.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define BUF 4096

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int port = (argc > 1) ? atoi(argv[1]) : 5000;

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    listen(ls, 8);
    printf("[mentah] listen 127.0.0.1:%d, buffer recv = %d byte\n", port, BUF);
    fflush(stdout);

    int cs = accept(ls, NULL, NULL);
    if (cs < 0) { perror("accept"); return 1; }

    char buf[BUF];
    ssize_t n;
    int ke = 0;
    long total = 0;
    while ((n = recv(cs, buf, sizeof buf, 0)) > 0) {
        ke++;
        total += n;
        /* Tampilkan 24 byte pertama, ganti karakter kendali dengan titik. */
        char cuplik[25];
        int m = (n < 24) ? (int)n : 24;
        for (int i = 0; i < m; i++)
            cuplik[i] = (buf[i] >= 32 && buf[i] < 127) ? buf[i] : '.';
        cuplik[m] = '\0';
        printf("[mentah] recv#%d = %zd byte | awal: \"%s\"\n", ke, n, cuplik);
        fflush(stdout);
    }
    printf("[mentah] selesai: %d panggilan recv, total %ld byte\n", ke, total);
    fflush(stdout);
    close(cs); close(ls);
    return 0;
}

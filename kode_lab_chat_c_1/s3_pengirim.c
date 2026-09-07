/* s3_pengirim.c — Tahap 3: mengirim 3 pesan pendek beruntun, lalu 1 pesan 200 KB.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s3_pengirim s3_pengirim.c
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

#define BESAR 200000

static int kirim_penuh(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, 0);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int port = (argc > 1) ? atoi(argv[1]) : 5000;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    /* Bagian 1: tiga send() beruntun tanpa jeda. */
    const char *p1 = "pesan-satu\n";
    const char *p2 = "pesan-dua\n";
    const char *p3 = "pesan-tiga\n";
    kirim_penuh(s, p1, strlen(p1));
    kirim_penuh(s, p2, strlen(p2));
    kirim_penuh(s, p3, strlen(p3));
    printf("[kirim] 3 send() beruntun, total %zu byte\n",
           strlen(p1) + strlen(p2) + strlen(p3));
    fflush(stdout);

    sleep(1);

    /* Bagian 2: satu send() besar. */
    char *besar = malloc(BESAR + 2);
    memset(besar, 'X', BESAR);
    besar[BESAR] = '\n';
    besar[BESAR + 1] = '\0';
    kirim_penuh(s, besar, BESAR + 1);
    printf("[kirim] 1 send() besar, %d byte + newline\n", BESAR);
    fflush(stdout);
    free(besar);

    sleep(1);
    close(s);
    return 0;
}

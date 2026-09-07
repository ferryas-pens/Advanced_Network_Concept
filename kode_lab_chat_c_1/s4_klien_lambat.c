/* s4_klien_lambat.c — klien "slow reader": mendaftar lalu berhenti membaca sama sekali.
 * Dipakai untuk menguji apakah server bisa dibekukan oleh satu klien.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s4_lambat s4_klien_lambat.c
 * Jalankan  : ./s4_lambat 5000 lambat 20
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int         port  = (argc > 1) ? atoi(argv[1]) : 5000;
    const char *nama  = (argc > 2) ? argv[2] : "lambat";
    int         tidur = (argc > 3) ? atoi(argv[3]) : 20;

    int s = socket(AF_INET, SOCK_STREAM, 0);

    /* Receive buffer sengaja dikecilkan supaya cepat penuh. */
    int rcv = 512;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    char baris[64];
    int m = snprintf(baris, sizeof baris, "%s\n", nama);
    if (send(s, baris, (size_t)m, 0) < 0) { perror("send"); return 1; }
    printf("[lambat] terdaftar sebagai %s, lalu diam %d detik tanpa recv()\n", nama, tidur);
    fflush(stdout);

    sleep(tidur);                 /* tidak pernah memanggil recv() */
    close(s);
    printf("[lambat] selesai\n");
    return 0;
}

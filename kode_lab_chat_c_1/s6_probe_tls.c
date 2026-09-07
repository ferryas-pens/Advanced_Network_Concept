/* s6_probe_tls.c — versi TLS dari s4_probe. Mengukur responsivitas server TLS.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s6_probe s6_probe_tls.c -lssl -lcrypto
 * Jalankan  : ./s6_probe 5443 pki/ca.crt probe 8
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
#include <openssl/ssl.h>
#include <openssl/err.h>

static double ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int         port  = (argc > 1) ? atoi(argv[1]) : 5443;
    const char *ca    = (argc > 2) ? argv[2] : "pki/ca.crt";
    const char *nama  = (argc > 3) ? argv[3] : "probe";
    int         ulang = (argc > 4) ? atoi(argv[4]) : 8;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1) {
        fprintf(stderr, "gagal memuat CA\n"); return 1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, s);
    SSL_set_tlsext_host_name(ssl, "localhost");
    SSL_set1_host(ssl, "localhost");
    if (SSL_connect(ssl) != 1) { fprintf(stderr, "handshake gagal\n"); return 1; }

    char baris[64];
    int m = snprintf(baris, sizeof baris, "%s\n", nama);
    size_t w = 0;
    SSL_write_ex(ssl, baris, (size_t)m, &w);

    char buang[65536];
    double maks = 0.0;
    for (int i = 0; i < ulang; i++) {
        for (;;) {                                  /* kosongkan siaran menumpuk */
            struct pollfd p = { s, POLLIN, 0 };
            if (SSL_pending(ssl) == 0 && poll(&p, 1, 0) <= 0) break;
            size_t r = 0;
            if (SSL_read_ex(ssl, buang, sizeof buang, &r) != 1) break;
        }

        double t0 = ms();
        const char *perintah = "/stat\n";
        if (SSL_write_ex(ssl, perintah, strlen(perintah), &w) != 1) { fprintf(stderr, "write gagal\n"); break; }

        struct pollfd p = { s, POLLIN, 0 };
        int rc = poll(&p, 1, 5000);
        double lat = ms() - t0;
        if (rc <= 0) {
            printf("[probe] ukur#%d : TIDAK ADA BALASAN dalam 5000 ms\n", i + 1);
            maks = 5000.0;
        } else {
            size_t r = 0;
            if (SSL_read_ex(ssl, buang, sizeof buang - 1, &r) != 1) { printf("[probe] sesi berakhir\n"); break; }
            if (lat > maks) maks = lat;
            printf("[probe] ukur#%d : balasan dalam %.1f ms\n", i + 1, lat);
        }
        fflush(stdout);
        usleep(1000 * 1000);
    }
    printf("[probe] latensi maksimum = %.1f ms\n", maks);
    SSL_shutdown(ssl); SSL_free(ssl); close(s); SSL_CTX_free(ctx);
    return 0;
}

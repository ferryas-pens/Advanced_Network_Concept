/* s6_klien_tls.c — klien chat TLS dengan verifikasi sertifikat.
 *
 * Tiga hal yang membedakannya dari klien Tahap 2:
 *   1. CA lab dimuat, dan verifikasi peer diaktifkan.
 *   2. Nama host diperiksa terhadap SAN sertifikat lewat SSL_set1_host().
 *   3. SNI dikirim supaya server dapat memilih sertifikat yang tepat.
 *
 * Verifikasi sertifikat adalah bagian yang paling sering dimatikan orang
 * ketika "programnya tidak jalan". Mematikannya membuat TLS hanya melindungi
 * dari penyadap pasif, dan sama sekali tidak melindungi dari man in the
 * middle. Enkripsi tanpa verifikasi identitas bukan keamanan.
 *
 * Kompilasi: gcc -Wall -Wextra -O2 -o s6_klien s6_klien_tls.c -lssl -lcrypto
 * Interaktif: ./s6_klien 5443 pki/ca.crt
 * Skrip     : printf 'halo\n' | ./s6_klien 5443 pki/ca.crt budi 5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define NAMA_HOST "localhost"

static FILE *keylog_fp = NULL;

static void keylog_cb(const SSL *ssl, const char *baris)
{
    (void)ssl;
    if (keylog_fp) { fprintf(keylog_fp, "%s\n", baris); fflush(keylog_fp); }
}

static double detik(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1.0e9;
}

static void cetak_error_ssl(const char *konteks)
{
    unsigned long e;
    fprintf(stderr, "[klien] gagal pada %s\n", konteks);
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof buf);
        fprintf(stderr, "[klien]   %s\n", buf);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    int         port  = (argc > 1) ? atoi(argv[1]) : 5443;
    const char *ca    = (argc > 2) ? argv[2] : "pki/ca.crt";
    const char *nama  = (argc > 3) ? argv[3] : NULL;
    double      batas = (argc > 4) ? atof(argv[4]) : 0.0;
    double      t0    = detik();

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { cetak_error_ssl("SSL_CTX_new"); return 1; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    if (SSL_CTX_load_verify_locations(ctx, ca, NULL) != 1) {
        cetak_error_ssl("load_verify_locations"); return 1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    const char *klog = getenv("SSLKEYLOGFILE");
    if (klog && *klog) {
        keylog_fp = fopen(klog, "a");
        if (keylog_fp) SSL_CTX_set_keylog_callback(ctx, keylog_cb);
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("connect"); return 1; }

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, s);
    SSL_set_tlsext_host_name(ssl, NAMA_HOST);        /* SNI */
    if (SSL_set1_host(ssl, NAMA_HOST) != 1) {        /* verifikasi nama host */
        cetak_error_ssl("SSL_set1_host"); return 1;
    }

    if (SSL_connect(ssl) != 1) {
        long v = SSL_get_verify_result(ssl);
        fprintf(stderr, "[klien] handshake TLS GAGAL: %s\n", X509_verify_cert_error_string(v));
        cetak_error_ssl("SSL_connect");
        return 1;
    }
    fprintf(stderr, "[klien] TLS %s, cipher %s, verifikasi sertifikat OK\n",
            SSL_get_version(ssl), SSL_get_cipher(ssl));

    /* Socket dibuat non-blocking setelah handshake, supaya loop di bawah
     * dapat memantau keyboard dan socket sekaligus tanpa tertahan. */
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);

    if (nama) {
        char baris[64];
        int m = snprintf(baris, sizeof baris, "%s\n", nama);
        size_t tertulis = 0;
        while (tertulis < (size_t)m) {
            size_t w = 0;
            if (SSL_write_ex(ssl, baris + tertulis, (size_t)m - tertulis, &w) == 1) tertulis += w;
            else if (SSL_get_error(ssl, 0) == SSL_ERROR_WANT_WRITE) continue;
            else { cetak_error_ssl("SSL_write_ex nama"); return 1; }
        }
    }

    int stdin_hidup = 1;
    for (;;) {
        struct pollfd pfd[2];
        int n = 0;
        pfd[n].fd = s; pfd[n].events = POLLIN; pfd[n].revents = 0; n++;
        if (stdin_hidup) { pfd[n].fd = 0; pfd[n].events = POLLIN; pfd[n].revents = 0; n++; }

        int rc = poll(pfd, (nfds_t)n, 200);
        if (rc < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP)) {
            /* Loop sampai SSL kehabisan data, bukan sekali baca. */
            int putus = 0;
            for (;;) {
                char buf[1024];
                size_t r = 0;
                int ok = SSL_read_ex(ssl, buf, sizeof buf, &r);
                if (ok == 1) { fwrite(buf, 1, r, stdout); fflush(stdout); continue; }
                int e = SSL_get_error(ssl, ok);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) break;
                if (e == SSL_ERROR_ZERO_RETURN) { fprintf(stderr, "[klien] server menutup sesi TLS\n"); putus = 1; break; }
                fprintf(stderr, "[klien] koneksi berakhir (kode %d)\n", e);
                putus = 1; break;
            }
            if (putus) break;
        }

        if (stdin_hidup && n > 1 && (pfd[1].revents & POLLIN)) {
            char baris[1024];
            if (!fgets(baris, sizeof baris, stdin)) { stdin_hidup = 0; continue; }
            size_t panjang = strlen(baris), tertulis = 0;
            while (tertulis < panjang) {
                size_t w = 0;
                if (SSL_write_ex(ssl, baris + tertulis, panjang - tertulis, &w) == 1) { tertulis += w; continue; }
                int e = SSL_get_error(ssl, 0);
                if (e == SSL_ERROR_WANT_WRITE || e == SSL_ERROR_WANT_READ) continue;
                cetak_error_ssl("SSL_write_ex"); tertulis = panjang; break;
            }
        }

        if (batas > 0.0 && detik() - t0 >= batas) break;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(s);
    SSL_CTX_free(ctx);
    if (keylog_fp) fclose(keylog_fp);
    return 0;
}

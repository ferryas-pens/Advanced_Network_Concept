/* s6_server_tls.c — Tahap 6: server chat Tahap 4 ditambah TLS (OpenSSL).
 *
 * Struktur event loop, output buffer, rate limit, dan timeout diwarisi dari
 * Tahap 4. Yang berubah hanya lapisan transport. Perbedaan pentingnya:
 *
 *   1. recv()/send() diganti SSL_read_ex()/SSL_write_ex().
 *   2. Kesiapan socket menurut poll() TIDAK lagi sama dengan kesiapan data
 *      aplikasi. SSL bisa menyimpan plaintext di buffernya sendiri, jadi
 *      pembacaan harus diulang sampai SSL_ERROR_WANT_READ.
 *   3. SSL_read() dapat meminta socket siap TULIS, dan SSL_write() dapat
 *      meminta socket siap BACA. Renegosiasi dan key update membuat arah
 *      kebutuhan tidak selalu sama dengan arah operasinya.
 *   4. Handshake adalah state tersendiri sebelum klien boleh mengirim apa pun.
 *
 * Kompilasi: gcc -Wall -Wextra -O2 -o s6_server s6_server_tls.c -lssl -lcrypto
 * Jalankan  : ./s6_server 5443 pki/server.crt pki/server.key
 *
 * Untuk praktikum Wireshark, jalankan dengan variabel lingkungan
 * SSLKEYLOGFILE. Lihat catatan keamanan di fungsi pasang_keylog().
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
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

#define MAKS_KLIEN            64
#define BUF_MASUK             2048
#define MAKS_NAMA             17
#define MAKS_OUT              (64 * 1024)
#define BATAS_DAFTAR_DETIK    10
#define BATAS_HANDSHAKE_DETIK 10
#define BATAS_IDLE_DETIK      300
#define MAKS_BARIS_PER_DETIK  10
#define BATAS_FLUSH_DETIK     5
#define BUF_KIRIM             (BUF_MASUK + MAKS_NAMA + 64)

typedef struct {
    int    fd;
    SSL   *ssl;
    int    hs_selesai;           /* handshake TLS sudah tuntas */
    int    hs_mau_tulis;         /* handshake menunggu socket siap tulis */
    int    baca_butuh_tulis;     /* SSL_read meminta POLLOUT */
    int    tulis_butuh_baca;     /* SSL_write meminta POLLIN */

    char   nama[MAKS_NAMA];
    int    terdaftar;
    char   asal[INET_ADDRSTRLEN + 8];

    char   in[BUF_MASUK];
    size_t in_len;

    char  *out;
    size_t out_len, out_cap;

    time_t t_masuk, t_aktif;
    time_t jatah_detik;
    int    jatah_terpakai;
    int    akan_tutup, tutup_paksa;
    time_t t_tutup;
} Klien;

static Klien klien[MAKS_KLIEN];
static int   n_klien = 0;

/* ---------- utilitas ---------- */

static const char *stempel(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static void catat(const char *fmt, ...)
{
    va_list ap;
    fprintf(stdout, "%s ", stempel());
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static void catat_ssl(const char *konteks)
{
    unsigned long e;
    while ((e = ERR_get_error()) != 0) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof buf);
        catat("TLSERR %s: %s", konteks, buf);
    }
}

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { perror("fcntl"); exit(1); }
}

/* ---------- penyiapan TLS ---------- */

static FILE *keylog_fp = NULL;

/* Callback ini menuliskan rahasia sesi TLS ke berkas, dalam format NSS key
 * log. Wireshark memakainya untuk mendekripsi capture.
 *
 * BAHAYA: berkas ini membuka SELURUH isi percakapan bagi siapa pun yang
 * memilikinya, sekarang maupun nanti terhadap capture lama. Aktifkan hanya
 * di lab, hanya pada sertifikat lab, dan hapus setelah praktikum selesai.
 * Jangan pernah mengaktifkannya pada layanan yang membawa data sungguhan. */
static void keylog_cb(const SSL *ssl, const char *baris)
{
    (void)ssl;
    if (keylog_fp) { fprintf(keylog_fp, "%s\n", baris); fflush(keylog_fp); }
}

static void pasang_keylog(SSL_CTX *ctx)
{
    const char *path = getenv("SSLKEYLOGFILE");
    if (!path || !*path) return;
    keylog_fp = fopen(path, "a");
    if (!keylog_fp) { perror("SSLKEYLOGFILE"); return; }
    SSL_CTX_set_keylog_callback(ctx, keylog_cb);
    catat("KEYLOG aktif ke %s -- HANYA UNTUK LAB", path);
}

static SSL_CTX *buat_ctx(const char *berkas_crt, const char *berkas_key)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { catat_ssl("SSL_CTX_new"); exit(1); }

    /* Menolak protokol lama. TLS 1.0 dan 1.1 sudah tidak layak dipakai. */
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)) {
        catat_ssl("set_min_proto_version"); exit(1);
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, berkas_crt) != 1) {
        catat_ssl("use_certificate_chain_file"); exit(1);
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, berkas_key, SSL_FILETYPE_PEM) != 1) {
        catat_ssl("use_PrivateKey_file"); exit(1);
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        catat_ssl("check_private_key"); exit(1);
    }

    /* Dua mode ini wajib untuk pola output buffer yang dipakai server ini.
     * PARTIAL_WRITE mengizinkan SSL_write menuliskan sebagian.
     * ACCEPT_MOVING_WRITE_BUFFER diperlukan karena isi buffer digeser
     * dengan memmove setelah sebagian terkirim. */
    SSL_CTX_set_mode(ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
                          SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    pasang_keylog(ctx);
    return ctx;
}

static int buat_listener(const char *ip, int port)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); exit(1); }
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "alamat tidak valid: %s\n", ip); exit(1);
    }
    if (bind(ls, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); exit(1); }
    if (listen(ls, 32) < 0) { perror("listen"); exit(1); }
    set_nonblocking(ls);
    return ls;
}

/* ---------- antrean keluar ---------- */

static int antri(int i, const char *teks)
{
    Klien *k = &klien[i];
    size_t n = strlen(teks);
    if (k->out_len + n > MAKS_OUT) return 0;

    if (k->out_len + n > k->out_cap) {
        size_t cap = k->out_cap ? k->out_cap : 1024;
        while (cap < k->out_len + n) cap *= 2;
        if (cap > MAKS_OUT) cap = MAKS_OUT;
        char *baru = realloc(k->out, cap);
        if (!baru) return 0;
        k->out = baru; k->out_cap = cap;
    }
    memcpy(k->out + k->out_len, teks, n);
    k->out_len += n;
    return 1;
}

static void tutup_klien(int i, const char *alasan)
{
    catat("KELUAR %s nama=%s alasan=%s sisa_antrean=%zu",
          klien[i].asal, klien[i].terdaftar ? klien[i].nama : "-", alasan, klien[i].out_len);

    if (klien[i].ssl) {
        if (klien[i].hs_selesai && !klien[i].tutup_paksa)
            SSL_shutdown(klien[i].ssl);      /* kirim close_notify sebisanya */
        SSL_free(klien[i].ssl);
    }
    close(klien[i].fd);
    free(klien[i].out);

    int tadinya = klien[i].terdaftar;
    char nama[MAKS_NAMA];
    snprintf(nama, sizeof nama, "%s", klien[i].nama);

    klien[i] = klien[n_klien - 1];
    n_klien--;

    if (tadinya) {
        char baris[BUF_KIRIM];
        snprintf(baris, sizeof baris, "*** %s meninggalkan ruang chat\n", nama);
        for (int j = 0; j < n_klien; j++)
            if (klien[j].terdaftar && !klien[j].akan_tutup) antri(j, baris);
    }
}

static void siar(int kecuali, const char *teks)
{
    for (int j = 0; j < n_klien; j++) {
        if (j == kecuali || !klien[j].terdaftar || klien[j].akan_tutup) continue;
        if (!antri(j, teks)) {
            catat("PUTUS  %s nama=%s alasan=antrean_penuh(%d byte)",
                  klien[j].asal, klien[j].nama, MAKS_OUT);
            klien[j].akan_tutup = 1;
            klien[j].tutup_paksa = 1;
            klien[j].t_tutup = time(NULL);
        }
    }
}

/* ---------- protokol aplikasi (identik dengan Tahap 4) ---------- */

static int nama_valid(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > MAKS_NAMA - 1) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isalnum(c) && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int nama_dipakai(const char *s)
{
    for (int i = 0; i < n_klien; i++)
        if (klien[i].terdaftar && strcmp(klien[i].nama, s) == 0) return 1;
    return 0;
}

static int lolos_rate_limit(int i)
{
    time_t sekarang = time(NULL);
    if (klien[i].jatah_detik != sekarang) {
        klien[i].jatah_detik = sekarang;
        klien[i].jatah_terpakai = 0;
    }
    return ++klien[i].jatah_terpakai <= MAKS_BARIS_PER_DETIK;
}

static int proses_baris(int i, char *baris)
{
    char keluar[BUF_KIRIM];
    klien[i].t_aktif = time(NULL);

    if (!klien[i].terdaftar) {
        if (!nama_valid(baris))  { antri(i, "!!! nama tidak valid (1-16: huruf, angka, _ atau -)\n"); return 0; }
        if (nama_dipakai(baris)) { antri(i, "!!! nama sudah dipakai\n"); return 0; }

        size_t ln = strlen(baris);
        if (ln > MAKS_NAMA - 1) ln = MAKS_NAMA - 1;
        memcpy(klien[i].nama, baris, ln);
        klien[i].nama[ln] = '\0';
        klien[i].terdaftar = 1;
        catat("DAFTAR %s nama=%s", klien[i].asal, klien[i].nama);

        snprintf(keluar, sizeof keluar, "*** selamat datang, %s\n", klien[i].nama);
        antri(i, keluar);
        snprintf(keluar, sizeof keluar, "*** %s bergabung ke ruang chat\n", klien[i].nama);
        siar(i, keluar);
        return 1;
    }

    if (!lolos_rate_limit(i)) {
        catat("PUTUS  %s nama=%s alasan=rate_limit(>%d baris/detik)",
              klien[i].asal, klien[i].nama, MAKS_BARIS_PER_DETIK);
        antri(i, "!!! terlalu banyak pesan, koneksi diputus\n");
        return 0;
    }

    if (baris[0] == '\0') return 1;
    if (strcmp(baris, "/quit") == 0) return 0;

    if (strcmp(baris, "/who") == 0) {
        antri(i, "*** pengguna online:\n");
        for (int j = 0; j < n_klien; j++) {
            if (!klien[j].terdaftar) continue;
            snprintf(keluar, sizeof keluar, "***   %s%s\n", klien[j].nama, j == i ? " (Anda)" : "");
            antri(i, keluar);
        }
        return 1;
    }

    if (strcmp(baris, "/stat") == 0) {
        snprintf(keluar, sizeof keluar, "*** klien=%d antrean_anda=%zu byte\n", n_klien, klien[i].out_len);
        antri(i, keluar);
        return 1;
    }

    /* Perintah tambahan Tahap 6: menampilkan parameter TLS sesi ini. */
    if (strcmp(baris, "/tls") == 0) {
        snprintf(keluar, sizeof keluar, "*** TLS %s cipher %s\n",
                 SSL_get_version(klien[i].ssl), SSL_get_cipher(klien[i].ssl));
        antri(i, keluar);
        return 1;
    }

    snprintf(keluar, sizeof keluar, "[%s] %s\n", klien[i].nama, baris);
    siar(i, keluar);
    return 1;
}

static int rakit_baris(int i)
{
    for (;;) {
        char *nl = memchr(klien[i].in, '\n', klien[i].in_len);
        if (!nl) break;
        *nl = '\0';
        size_t panjang = (size_t)(nl - klien[i].in);
        if (panjang > 0 && klien[i].in[panjang - 1] == '\r') klien[i].in[panjang - 1] = '\0';

        int lanjut = proses_baris(i, klien[i].in);
        size_t dipakai = panjang + 1;
        memmove(klien[i].in, klien[i].in + dipakai, klien[i].in_len - dipakai);
        klien[i].in_len -= dipakai;
        if (!lanjut) return 0;
    }
    if (klien[i].in_len == BUF_MASUK) {
        antri(i, "!!! baris terlalu panjang\n");
        return 0;
    }
    return 1;
}

/* ---------- lapisan TLS ---------- */

/* 1 = lanjut, 0 = tutup koneksi. */
static int maju_handshake(int i)
{
    Klien *k = &klien[i];
    int rc = SSL_accept(k->ssl);
    if (rc == 1) {
        k->hs_selesai = 1;
        k->hs_mau_tulis = 0;
        catat("TLS-OK %s versi=%s cipher=%s",
              k->asal, SSL_get_version(k->ssl), SSL_get_cipher(k->ssl));
        antri(i, "*** ketik nama Anda lalu ENTER: ");
        return 1;
    }
    int e = SSL_get_error(k->ssl, rc);
    if (e == SSL_ERROR_WANT_READ)  { k->hs_mau_tulis = 0; return 1; }
    if (e == SSL_ERROR_WANT_WRITE) { k->hs_mau_tulis = 1; return 1; }
    catat("TLS-GAGAL %s kode=%d", k->asal, e);
    catat_ssl("SSL_accept");
    return 0;
}

/* Membaca sampai SSL kehabisan data. Ini WAJIB berupa loop.
 * Satu record TLS bisa berisi banyak baris aplikasi. Bila hanya membaca
 * sekali per notifikasi poll(), sisa plaintext akan tertahan di dalam SSL
 * dan poll() tidak akan memberi tahu lagi, karena socket memang sudah kosong. */
static int tls_baca(int i)
{
    Klien *k = &klien[i];
    for (;;) {
        size_t ruang = BUF_MASUK - k->in_len;
        if (ruang == 0) break;

        size_t n = 0;
        int rc = SSL_read_ex(k->ssl, k->in + k->in_len, ruang, &n);
        if (rc == 1) {
            k->in_len += n;
            k->baca_butuh_tulis = 0;
            if (!rakit_baris(i)) {
                k->akan_tutup = 1;
                k->t_tutup = time(NULL);
                return 1;                      /* pesan error tetap dikirim dulu */
            }
            continue;
        }
        int e = SSL_get_error(k->ssl, rc);
        if (e == SSL_ERROR_WANT_READ)  { k->baca_butuh_tulis = 0; break; }
        if (e == SSL_ERROR_WANT_WRITE) { k->baca_butuh_tulis = 1; break; }
        if (e == SSL_ERROR_ZERO_RETURN) return 0;   /* close_notify dari klien */
        if (e == SSL_ERROR_SYSCALL && errno == 0) return 0;  /* putus tanpa close_notify */
        catat_ssl("SSL_read_ex");
        return 0;
    }
    return 1;
}

static int tls_tulis(int i)
{
    Klien *k = &klien[i];
    while (k->out_len > 0) {
        size_t n = 0;
        int rc = SSL_write_ex(k->ssl, k->out, k->out_len, &n);
        if (rc == 1) {
            memmove(k->out, k->out + n, k->out_len - n);
            k->out_len -= n;
            k->tulis_butuh_baca = 0;
            continue;
        }
        int e = SSL_get_error(k->ssl, rc);
        if (e == SSL_ERROR_WANT_WRITE) { k->tulis_butuh_baca = 0; return 1; }
        if (e == SSL_ERROR_WANT_READ)  { k->tulis_butuh_baca = 1; return 1; }
        catat_ssl("SSL_write_ex");
        return 0;
    }
    return 1;
}

/* ---------- program utama ---------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);

    int port = (argc > 1) ? atoi(argv[1]) : 5443;
    const char *crt = (argc > 2) ? argv[2] : "pki/server.crt";
    const char *key = (argc > 3) ? argv[3] : "pki/server.key";

    SSL_CTX *ctx = buat_ctx(crt, key);
    int ls = buat_listener("127.0.0.1", port);
    catat("LISTEN 127.0.0.1:%d TLS aktif crt=%s maks_klien=%d", port, crt, MAKS_KLIEN);

    for (;;) {
        struct pollfd pfd[MAKS_KLIEN + 1];
        pfd[0].fd = ls; pfd[0].events = POLLIN; pfd[0].revents = 0;

        for (int i = 0; i < n_klien; i++) {
            Klien *k = &klien[i];
            pfd[i + 1].fd = k->fd;
            pfd[i + 1].revents = 0;
            if (!k->hs_selesai) {
                pfd[i + 1].events = k->hs_mau_tulis ? POLLOUT : POLLIN;
            } else {
                short ev = 0;
                if (!k->akan_tutup) ev |= POLLIN;
                if (k->tulis_butuh_baca) ev |= POLLIN;
                if (k->out_len > 0 || k->baca_butuh_tulis) ev |= POLLOUT;
                pfd[i + 1].events = ev;
            }
        }

        int siap = poll(pfd, (nfds_t)(n_klien + 1), 1000);
        if (siap < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        for (int i = n_klien - 1; i >= 0; i--) {
            short re = pfd[i + 1].revents;
            if (re & (POLLERR | POLLNVAL)) { tutup_klien(i, "socket error"); continue; }
            if (re == 0) continue;

            if (!klien[i].hs_selesai) {
                if (!maju_handshake(i)) { tutup_klien(i, "handshake gagal"); continue; }
                if (!klien[i].hs_selesai) continue;
            }

            /* Kedua arah dicoba setiap kali ada kesiapan. Ini sedikit boros
             * panggilan sistem, tetapi menghilangkan seluruh kelas bug
             * pemetaan arah WANT_READ dan WANT_WRITE. */
            if (!tls_baca(i))  { tutup_klien(i, "baca TLS berakhir"); continue; }
            if (!tls_tulis(i)) { tutup_klien(i, "tulis TLS gagal"); continue; }
        }

        time_t sekarang = time(NULL);
        for (int i = n_klien - 1; i >= 0; i--) {
            Klien *k = &klien[i];
            if (k->akan_tutup) {
                int selesai = (k->out_len == 0);
                int paksa   = k->tutup_paksa;
                int kadaluarsa = (sekarang - k->t_tutup > BATAS_FLUSH_DETIK);
                if (selesai || paksa || kadaluarsa) {
                    tutup_klien(i, paksa ? "penutupan paksa" :
                                   selesai ? "penutupan terjadwal" : "flush timeout");
                    continue;
                }
            }
            if (!k->hs_selesai && sekarang - k->t_masuk > BATAS_HANDSHAKE_DETIK) {
                tutup_klien(i, "timeout handshake"); continue;
            }
            if (k->hs_selesai && !k->terdaftar && sekarang - k->t_masuk > BATAS_DAFTAR_DETIK) {
                tutup_klien(i, "timeout registrasi"); continue;
            }
            if (k->terdaftar && sekarang - k->t_aktif > BATAS_IDLE_DETIK) {
                tutup_klien(i, "idle timeout"); continue;
            }
        }

        if (pfd[0].revents & POLLIN) {
            for (;;) {
                struct sockaddr_in cli;
                socklen_t clen = sizeof cli;
                int cs = accept(ls, (struct sockaddr *)&cli, &clen);
                if (cs < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    perror("accept"); break;
                }
                char ipstr[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof ipstr);

                if (n_klien >= MAKS_KLIEN) {
                    close(cs);
                    catat("TOLAK  %s:%u alasan=penuh", ipstr, ntohs(cli.sin_port));
                    continue;
                }
                set_nonblocking(cs);

                SSL *ssl = SSL_new(ctx);
                if (!ssl) { catat_ssl("SSL_new"); close(cs); continue; }
                SSL_set_fd(ssl, cs);
                SSL_set_accept_state(ssl);

                Klien *k = &klien[n_klien];
                memset(k, 0, sizeof *k);
                k->fd = cs;
                k->ssl = ssl;
                k->t_masuk = k->t_aktif = time(NULL);
                snprintf(k->asal, sizeof k->asal, "%s:%u", ipstr, ntohs(cli.sin_port));
                n_klien++;

                catat("MASUK  %s total=%d (menunggu handshake TLS)", k->asal, n_klien);
                if (!maju_handshake(n_klien - 1))
                    tutup_klien(n_klien - 1, "handshake gagal");
            }
        }
    }
    close(ls);
    SSL_CTX_free(ctx);
    if (keylog_fp) fclose(keylog_fp);
    return 0;
}

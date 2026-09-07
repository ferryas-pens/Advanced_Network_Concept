/* s2_server_poll.c — Tahap 2: server chat multi-pengguna dengan poll().
 * Satu proses, satu thread, banyak koneksi.
 * Protokol: teks baris, dipisah '\n'. Baris pertama = nama pengguna.
 * Kompilasi: gcc -Wall -Wextra -O2 -o s2_server s2_server_poll.c
 * Jalankan  : ./s2_server 5000
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAKS_KLIEN   32
#define BUF_MASUK    2048
#define MAKS_NAMA    17          /* 16 karakter + NUL */
#define BUF_KIRIM    (BUF_MASUK + MAKS_NAMA + 64)

typedef struct {
    int    fd;
    char   nama[MAKS_NAMA];
    int    terdaftar;            /* 0 = belum mengirim nama */
    char   in[BUF_MASUK];        /* buffer perakit baris */
    size_t in_len;
    char   asal[INET_ADDRSTRLEN + 8];
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
    if (listen(ls, 16) < 0) { perror("listen"); exit(1); }
    return ls;
}

/* Tahap 2 memakai send() blocking. Kelemahannya dibahas di Tahap 4. */
static void kirim_ke(int i, const char *teks)
{
    size_t sisa = strlen(teks);
    const char *p = teks;
    while (sisa > 0) {
        ssize_t n = send(klien[i].fd, p, sisa, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;                     /* klien akan dibersihkan pada iterasi berikut */
        }
        p += n; sisa -= (size_t)n;
    }
}

static void siar(int kecuali, const char *teks)
{
    for (int i = 0; i < n_klien; i++)
        if (i != kecuali && klien[i].terdaftar)
            kirim_ke(i, teks);
}

static void tutup_klien(int i, const char *alasan)
{
    catat("KELUAR %s nama=%s alasan=%s",
          klien[i].asal, klien[i].terdaftar ? klien[i].nama : "-", alasan);
    close(klien[i].fd);

    if (klien[i].terdaftar) {
        char baris[BUF_KIRIM];
        snprintf(baris, sizeof baris, "*** %s meninggalkan ruang chat\n", klien[i].nama);
        klien[i].terdaftar = 0;
        siar(i, baris);
    }
    klien[i] = klien[n_klien - 1];       /* padatkan array */
    n_klien--;
}

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

/* Mengembalikan 0 bila klien harus ditutup. */
static int proses_baris(int i, char *baris)
{
    char keluar[BUF_KIRIM];

    if (!klien[i].terdaftar) {
        if (!nama_valid(baris)) {
            kirim_ke(i, "!!! nama tidak valid (1-16 karakter: huruf, angka, _ atau -)\n");
            return 0;
        }
        if (nama_dipakai(baris)) {
            kirim_ke(i, "!!! nama sudah dipakai\n");
            return 0;
        }
        snprintf(klien[i].nama, sizeof klien[i].nama, "%s", baris);
        klien[i].terdaftar = 1;
        catat("DAFTAR %s nama=%s", klien[i].asal, klien[i].nama);

        snprintf(keluar, sizeof keluar, "*** selamat datang, %s\n", klien[i].nama);
        kirim_ke(i, keluar);
        snprintf(keluar, sizeof keluar, "*** %s bergabung ke ruang chat\n", klien[i].nama);
        siar(i, keluar);
        return 1;
    }

    if (baris[0] == '\0') return 1;                 /* baris kosong diabaikan */

    if (strcmp(baris, "/quit") == 0) return 0;

    if (strcmp(baris, "/who") == 0) {
        kirim_ke(i, "*** pengguna online:\n");
        for (int j = 0; j < n_klien; j++) {
            if (!klien[j].terdaftar) continue;
            snprintf(keluar, sizeof keluar, "***   %s%s\n",
                     klien[j].nama, j == i ? " (Anda)" : "");
            kirim_ke(i, keluar);
        }
        return 1;
    }

    catat("PESAN  nama=%s panjang=%zu", klien[i].nama, strlen(baris));
    snprintf(keluar, sizeof keluar, "[%s] %s\n", klien[i].nama, baris);
    siar(i, keluar);
    return 1;
}

/* Ambil sebanyak mungkin baris utuh dari buffer. 0 = klien harus ditutup. */
static int rakit_baris(int i)
{
    for (;;) {
        char *nl = memchr(klien[i].in, '\n', klien[i].in_len);
        if (!nl) break;

        *nl = '\0';
        size_t panjang = (size_t)(nl - klien[i].in);
        if (panjang > 0 && klien[i].in[panjang - 1] == '\r')
            klien[i].in[panjang - 1] = '\0';        /* klien telnet mengirim CRLF */

        int lanjut = proses_baris(i, klien[i].in);

        size_t dipakai = panjang + 1;
        memmove(klien[i].in, klien[i].in + dipakai, klien[i].in_len - dipakai);
        klien[i].in_len -= dipakai;

        if (!lanjut) return 0;
    }
    if (klien[i].in_len == BUF_MASUK) {             /* penuh tanpa '\n' */
        kirim_ke(i, "!!! baris terlalu panjang\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);                       /* tanpa ini server mati saat menulis ke socket tertutup */

    int port = (argc > 1) ? atoi(argv[1]) : 5000;
    int ls = buat_listener("127.0.0.1", port);
    catat("LISTEN 127.0.0.1:%d maks_klien=%d", port, MAKS_KLIEN);

    for (;;) {
        struct pollfd pfd[MAKS_KLIEN + 1];
        pfd[0].fd = ls; pfd[0].events = POLLIN; pfd[0].revents = 0;
        for (int i = 0; i < n_klien; i++) {
            pfd[i + 1].fd = klien[i].fd;
            pfd[i + 1].events = POLLIN;
            pfd[i + 1].revents = 0;
        }

        int siap = poll(pfd, (nfds_t)(n_klien + 1), -1);
        if (siap < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        /* Klien diproses dari belakang supaya penghapusan tidak merusak indeks. */
        for (int i = n_klien - 1; i >= 0; i--) {
            short re = pfd[i + 1].revents;
            if (re == 0) continue;

            if (re & (POLLERR | POLLNVAL)) { tutup_klien(i, "socket error"); continue; }

            if (re & POLLIN) {
                size_t ruang = BUF_MASUK - klien[i].in_len;
                ssize_t n = recv(klien[i].fd, klien[i].in + klien[i].in_len, ruang, 0);
                if (n == 0)      { tutup_klien(i, "ditutup klien"); continue; }
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN) continue;
                    tutup_klien(i, strerror(errno)); continue;
                }
                klien[i].in_len += (size_t)n;
                if (!rakit_baris(i)) { tutup_klien(i, "protokol/permintaan keluar"); continue; }
            } else if (re & POLLHUP) {
                tutup_klien(i, "hangup"); continue;
            }
        }

        if (pfd[0].revents & POLLIN) {
            struct sockaddr_in cli;
            socklen_t clen = sizeof cli;
            int cs = accept(ls, (struct sockaddr *)&cli, &clen);
            if (cs < 0) { perror("accept"); continue; }

            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof ipstr);

            if (n_klien >= MAKS_KLIEN) {
                const char *tolak = "!!! server penuh\n";
                send(cs, tolak, strlen(tolak), 0);
                close(cs);
                catat("TOLAK  %s:%u alasan=penuh", ipstr, ntohs(cli.sin_port));
                continue;
            }

            Klien *k = &klien[n_klien];
            memset(k, 0, sizeof *k);
            k->fd = cs;
            snprintf(k->asal, sizeof k->asal, "%s:%u", ipstr, ntohs(cli.sin_port));
            n_klien++;

            catat("MASUK  %s total=%d", k->asal, n_klien);
            kirim_ke(n_klien - 1, "*** ketik nama Anda lalu ENTER: ");
        }
    }
    close(ls);
    return 0;
}

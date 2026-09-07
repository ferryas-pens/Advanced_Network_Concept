/* s4_server_tangguh.c — Tahap 4: server chat multi-pengguna yang tahan klien nakal.
 *
 * Perbedaan terhadap Tahap 2:
 *   1. Semua socket non-blocking. Tidak ada satu pun panggilan yang bisa membekukan server.
 *   2. Setiap klien punya output buffer sendiri + POLLOUT (mengatasi slow reader).
 *   3. Batas ukuran output buffer. Klien yang tidak membaca akan diputus, bukan membekukan server.
 *   4. Timeout registrasi dan timeout idle (mengatasi koneksi menggantung).
 *   5. Rate limit per klien (mengatasi flooding).
 *   6. Log ber-timestamp UTC untuk keperluan audit.
 *
 * Kompilasi: gcc -Wall -Wextra -O2 -o s4_server s4_server_tangguh.c
 * Jalankan  : ./s4_server 5000
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

#define MAKS_KLIEN            64
#define BUF_MASUK             2048          /* batas panjang satu baris */
#define MAKS_NAMA             17
#define MAKS_OUT              (64 * 1024)   /* batas antrean kirim per klien */
#define BATAS_DAFTAR_DETIK    10            /* waktu mengirim nama */
#define BATAS_IDLE_DETIK      300
#define MAKS_BARIS_PER_DETIK  10
#define BATAS_FLUSH_DETIK     5             /* batas menunggu antrean kosong saat menutup */
#define BUF_KIRIM             (BUF_MASUK + MAKS_NAMA + 64)

typedef struct {
    int    fd;
    char   nama[MAKS_NAMA];
    int    terdaftar;
    char   asal[INET_ADDRSTRLEN + 8];

    char   in[BUF_MASUK];
    size_t in_len;

    char  *out;                  /* buffer dinamis, tumbuh sesuai kebutuhan */
    size_t out_len, out_cap;

    time_t t_masuk, t_aktif;
    time_t jatah_detik;
    int    jatah_terpakai;
    int    akan_tutup;           /* tutup setelah antrean kosong */
    int    tutup_paksa;          /* tutup sekarang, antrean diabaikan */
    time_t t_tutup;              /* kapan penutupan dijadwalkan */
} Klien;

static Klien klien[MAKS_KLIEN];
static int   n_klien = 0;

/* Knob praktikum: bila > 0, SO_SNDBUF socket klien dikecilkan.
 * Gunanya membuat efek backpressure terlihat dalam hitungan detik, bukan menit.
 * Di produksi biarkan 0 (kernel yang mengatur). */
static int opt_sndbuf = 0;

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

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { perror("fcntl"); exit(1); }
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

/* ---------- manajemen output ---------- */

/* Menambahkan teks ke antrean kirim. Mengembalikan 0 bila antrean melewati batas. */
static int antri(int i, const char *teks)
{
    Klien *k = &klien[i];
    size_t n = strlen(teks);

    if (k->out_len + n > MAKS_OUT) return 0;        /* klien terlalu lambat membaca */

    if (k->out_len + n > k->out_cap) {
        size_t cap = k->out_cap ? k->out_cap : 1024;
        while (cap < k->out_len + n) cap *= 2;
        if (cap > MAKS_OUT) cap = MAKS_OUT;
        char *baru = realloc(k->out, cap);
        if (!baru) return 0;
        k->out = baru;
        k->out_cap = cap;
    }
    memcpy(k->out + k->out_len, teks, n);
    k->out_len += n;
    return 1;
}

/* Mencoba mengosongkan antrean. Mengembalikan 0 bila koneksi harus ditutup. */
static int coba_kirim(int i)
{
    Klien *k = &klien[i];
    while (k->out_len > 0) {
        ssize_t w = send(k->fd, k->out, k->out_len, 0);
        if (w > 0) {
            memmove(k->out, k->out + w, k->out_len - (size_t)w);
            k->out_len -= (size_t)w;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;  /* nanti lagi */
        return 0;
    }
    return 1;
}

static void tutup_klien(int i, const char *alasan)
{
    catat("KELUAR %s nama=%s alasan=%s sisa_antrean=%zu",
          klien[i].asal, klien[i].terdaftar ? klien[i].nama : "-", alasan, klien[i].out_len);
    close(klien[i].fd);
    free(klien[i].out);

    int tadinya_terdaftar = klien[i].terdaftar;
    char nama[MAKS_NAMA];
    snprintf(nama, sizeof nama, "%s", klien[i].nama);

    klien[i] = klien[n_klien - 1];
    n_klien--;

    if (tadinya_terdaftar) {
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
            /* Klien ini tidak membaca. Menunggu antreannya kosong sama saja
             * dengan tidak pernah menutupnya, jadi penutupan dipaksakan. */
            klien[j].akan_tutup = 1;
            klien[j].tutup_paksa = 1;
            klien[j].t_tutup = time(NULL);
        }
    }
}

/* ---------- protokol ---------- */

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

/* Token bucket sederhana: MAKS_BARIS_PER_DETIK baris tiap detik. */
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

        /* baris menunjuk ke dalam klien[i].in, jadi ini penyalinan di dalam
         * objek yang sama. memcpy dengan panjang eksplisit lebih jelas
         * maksudnya daripada snprintf, dan tidak memicu peringatan -Wrestrict. */
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

/* ---------- program utama ---------- */

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int port = (argc > 1) ? atoi(argv[1]) : 5000;
    if (argc > 2) opt_sndbuf = atoi(argv[2]);        /* argumen opsional untuk praktikum */
    int ls = buat_listener("127.0.0.1", port);
    catat("LISTEN 127.0.0.1:%d maks_klien=%d maks_antrean=%d rate=%d/detik sndbuf=%d",
          port, MAKS_KLIEN, MAKS_OUT, MAKS_BARIS_PER_DETIK, opt_sndbuf);

    for (;;) {
        struct pollfd pfd[MAKS_KLIEN + 1];
        pfd[0].fd = ls; pfd[0].events = POLLIN; pfd[0].revents = 0;
        for (int i = 0; i < n_klien; i++) {
            pfd[i + 1].fd = klien[i].fd;
            pfd[i + 1].events = 0;
            if (!klien[i].akan_tutup)   pfd[i + 1].events |= POLLIN;
            if (klien[i].out_len > 0)   pfd[i + 1].events |= POLLOUT;
            pfd[i + 1].revents = 0;
        }

        int siap = poll(pfd, (nfds_t)(n_klien + 1), 1000);
        if (siap < 0) { if (errno == EINTR) continue; perror("poll"); break; }

        for (int i = n_klien - 1; i >= 0; i--) {
            short re = pfd[i + 1].revents;

            if (re & (POLLERR | POLLNVAL)) { tutup_klien(i, "socket error"); continue; }

            if (re & POLLOUT) {
                if (!coba_kirim(i)) { tutup_klien(i, "gagal kirim"); continue; }
            }

            if (re & POLLIN) {
                size_t ruang = BUF_MASUK - klien[i].in_len;
                ssize_t n = recv(klien[i].fd, klien[i].in + klien[i].in_len, ruang, 0);
                if (n == 0) { tutup_klien(i, "ditutup klien"); continue; }
                if (n < 0) {
                    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    tutup_klien(i, strerror(errno)); continue;
                }
                klien[i].in_len += (size_t)n;
                if (!rakit_baris(i)) {          /* pesan error tetap dikirim dulu */
                    klien[i].akan_tutup = 1;
                    klien[i].t_tutup = time(NULL);
                }
                if (!coba_kirim(i)) { tutup_klien(i, "gagal kirim"); continue; }
            } else if (re & POLLHUP) {
                tutup_klien(i, "hangup"); continue;
            }
        }

        /* Sapuan periodik: timeout dan penutupan tertunda. */
        time_t sekarang = time(NULL);
        for (int i = n_klien - 1; i >= 0; i--) {
            if (klien[i].akan_tutup) {
                int selesai = (klien[i].out_len == 0);
                int paksa   = klien[i].tutup_paksa;
                int kadaluarsa = (sekarang - klien[i].t_tutup > BATAS_FLUSH_DETIK);
                if (selesai || paksa || kadaluarsa) {
                    tutup_klien(i, paksa ? "penutupan paksa" :
                                   selesai ? "penutupan terjadwal" : "flush timeout");
                    continue;
                }
            }
            if (!klien[i].terdaftar && sekarang - klien[i].t_masuk > BATAS_DAFTAR_DETIK) {
                antri(i, "!!! timeout registrasi\n");
                coba_kirim(i);
                tutup_klien(i, "timeout registrasi"); continue;
            }
            if (klien[i].terdaftar && sekarang - klien[i].t_aktif > BATAS_IDLE_DETIK) {
                tutup_klien(i, "idle timeout"); continue;
            }
        }

        if (pfd[0].revents & POLLIN) {
            for (;;) {                                  /* listener non-blocking: kuras antrean */
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
                    const char *tolak = "!!! server penuh\n";
                    send(cs, tolak, strlen(tolak), MSG_NOSIGNAL);
                    close(cs);
                    catat("TOLAK  %s:%u alasan=penuh", ipstr, ntohs(cli.sin_port));
                    continue;
                }
                set_nonblocking(cs);
                if (opt_sndbuf > 0)
                    setsockopt(cs, SOL_SOCKET, SO_SNDBUF, &opt_sndbuf, sizeof opt_sndbuf);

                Klien *k = &klien[n_klien];
                memset(k, 0, sizeof *k);
                k->fd = cs;
                k->t_masuk = k->t_aktif = time(NULL);
                snprintf(k->asal, sizeof k->asal, "%s:%u", ipstr, ntohs(cli.sin_port));
                n_klien++;

                catat("MASUK  %s total=%d", k->asal, n_klien);
                antri(n_klien - 1, "*** ketik nama Anda lalu ENTER: ");
                coba_kirim(n_klien - 1);
            }
        }
    }
    close(ls);
    return 0;
}

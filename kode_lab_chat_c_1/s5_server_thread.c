/* s5_server_thread.c — Tahap 5: satu thread per klien (pthread).
 *
 * Struktur data bersama dilindungi satu mutex. Kompilasi dengan -DTANPA_KUNCI
 * untuk melihat apa yang terjadi bila proteksi itu dihilangkan (uji dengan
 * ThreadSanitizer, bukan dengan mata).
 *
 * Kompilasi: gcc -Wall -Wextra -O2 -pthread -o s5_server s5_server_thread.c
 * Race demo : gcc -Wall -Wextra -g -O1 -fsanitize=thread -pthread -DTANPA_KUNCI \
 *                 -o s5_race s5_server_thread.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define MAKS_KLIEN 32
#define BUF_MASUK  2048
#define MAKS_NAMA  17
#define BUF_KIRIM  (BUF_MASUK + MAKS_NAMA + 64)

typedef struct {
    int  fd;
    int  aktif;
    char nama[MAKS_NAMA];
    char asal[INET_ADDRSTRLEN + 8];
} Klien;

static Klien          daftar[MAKS_KLIEN];
static pthread_mutex_t kunci = PTHREAD_MUTEX_INITIALIZER;

#ifdef TANPA_KUNCI
#define AMBIL_KUNCI()   ((void)0)
#define LEPAS_KUNCI()   ((void)0)
#else
#define AMBIL_KUNCI()   pthread_mutex_lock(&kunci)
#define LEPAS_KUNCI()   pthread_mutex_unlock(&kunci)
#endif

static const char *stempel(void)
{
    static __thread char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

static void catat(const char *fmt, ...)
{
    va_list ap;
    char pesan[512];
    va_start(ap, fmt);
    vsnprintf(pesan, sizeof pesan, fmt, ap);
    va_end(ap);
    fprintf(stdout, "%s %s\n", stempel(), pesan);   /* fprintf sudah thread-safe */
    fflush(stdout);
}

static int kirim_penuh(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* Siaran dilakukan sambil memegang kunci. Konsekuensinya dibahas di tutorial:
 * satu klien yang lambat membaca menahan kunci untuk semua thread lain. */
static void siar(int slot_pengirim, const char *teks)
{
    AMBIL_KUNCI();
    for (int i = 0; i < MAKS_KLIEN; i++)
        if (daftar[i].aktif && i != slot_pengirim)
            kirim_penuh(daftar[i].fd, teks, strlen(teks));
    LEPAS_KUNCI();
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

static void *layani(void *arg)
{
    int slot = (int)(intptr_t)arg;
    int fd   = daftar[slot].fd;

    char in[BUF_MASUK];
    size_t in_len = 0;
    int terdaftar = 0;
    char keluar[BUF_KIRIM];

    { const char *sapa = "*** ketik nama Anda lalu ENTER: ";
      kirim_penuh(fd, sapa, strlen(sapa)); }

    for (;;) {
        ssize_t n = recv(fd, in + in_len, sizeof in - in_len, 0);
        if (n <= 0) break;
        in_len += (size_t)n;

        for (;;) {
            char *nl = memchr(in, '\n', in_len);
            if (!nl) break;
            *nl = '\0';
            size_t panjang = (size_t)(nl - in);
            if (panjang > 0 && in[panjang - 1] == '\r') in[panjang - 1] = '\0';

            if (!terdaftar) {
                if (!nama_valid(in)) { { const char *e = "!!! nama tidak valid\n"; kirim_penuh(fd, e, strlen(e)); } goto tutup; }
                AMBIL_KUNCI();
                snprintf(daftar[slot].nama, MAKS_NAMA, "%s", in);
                LEPAS_KUNCI();
                terdaftar = 1;
                catat("DAFTAR %s nama=%s slot=%d", daftar[slot].asal, in, slot);
                snprintf(keluar, sizeof keluar, "*** %s bergabung ke ruang chat\n", in);
                siar(slot, keluar);
            } else if (strcmp(in, "/quit") == 0) {
                goto tutup;
            } else if (strcmp(in, "/stat") == 0) {
                /* Membaca state bersama, jadi tetap butuh kunci yang sama
                 * dengan siar(). Inilah yang membuat perintah ini ikut
                 * tertahan saat satu klien lambat memblokir siaran. */
                int jumlah = 0;
                AMBIL_KUNCI();
                for (int i = 0; i < MAKS_KLIEN; i++) if (daftar[i].aktif) jumlah++;
                LEPAS_KUNCI();
                snprintf(keluar, sizeof keluar, "*** klien=%d\n", jumlah);
                kirim_penuh(fd, keluar, strlen(keluar));
            } else if (in[0] != '\0') {
                snprintf(keluar, sizeof keluar, "[%s] %s\n", daftar[slot].nama, in);
                siar(slot, keluar);
            }

            size_t dipakai = panjang + 1;
            memmove(in, in + dipakai, in_len - dipakai);
            in_len -= dipakai;
        }
        if (in_len == sizeof in) { { const char *e = "!!! baris terlalu panjang\n"; kirim_penuh(fd, e, strlen(e)); } break; }
    }

tutup:
    if (terdaftar) {
        snprintf(keluar, sizeof keluar, "*** %s meninggalkan ruang chat\n", daftar[slot].nama);
        siar(slot, keluar);
    }
    catat("KELUAR %s slot=%d", daftar[slot].asal, slot);
    close(fd);
    AMBIL_KUNCI();
    daftar[slot].aktif = 0;
    LEPAS_KUNCI();
    return NULL;
}

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
    listen(ls, 16);
    catat("LISTEN 127.0.0.1:%d model=thread-per-klien", port);

    for (;;) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof cli;
        int cs = accept(ls, (struct sockaddr *)&cli, &clen);
        if (cs < 0) { if (errno == EINTR) continue; perror("accept"); break; }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof ipstr);

        /* Seluruh pengisian slot dilakukan di dalam kunci. Bila aktif=1
         * ditulis lebih dulu dan fd diisi belakangan, thread lain bisa
         * menyiarkan ke slot yang belum terisi. ThreadSanitizer menemukan
         * bug ini pada versi pertama kode ini. */
        int slot = -1;
        AMBIL_KUNCI();
        for (int i = 0; i < MAKS_KLIEN; i++) {
            if (!daftar[i].aktif) {
                slot = i;
                daftar[i].fd = cs;
                daftar[i].nama[0] = '\0';
                snprintf(daftar[i].asal, sizeof daftar[i].asal, "%s:%u",
                         ipstr, ntohs(cli.sin_port));
                daftar[i].aktif = 1;
                break;
            }
        }
        LEPAS_KUNCI();

        if (slot < 0) { close(cs); catat("TOLAK alasan=penuh"); continue; }
        catat("MASUK  %s slot=%d", daftar[slot].asal, slot);

        pthread_t th;
        if (pthread_create(&th, NULL, layani, (void *)(intptr_t)slot) != 0) {
            perror("pthread_create");
            close(cs);
            AMBIL_KUNCI(); daftar[slot].aktif = 0; LEPAS_KUNCI();
            continue;
        }
        pthread_detach(th);
    }
    close(ls);
    return 0;
}

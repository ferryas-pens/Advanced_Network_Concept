# Makefile praktikum LAB-CHAT
CFLAGS = -Wall -Wextra -O2
SAN    = -Wall -Wextra -g -O1

all: s1_server s1_klien s2_server s2_klien s3_server s3_pengirim s4_server s4_lambat s4_probe s5_server s6_server s6_klien s6_probe

s1_server: s1_server_iteratif.c ; $(CC) $(CFLAGS) -o $@ $<
s1_klien:  s1_klien.c           ; $(CC) $(CFLAGS) -o $@ $<
s2_server: s2_server_poll.c     ; $(CC) $(CFLAGS) -o $@ $<
s2_klien:  s2_klien.c           ; $(CC) $(CFLAGS) -o $@ $<
s3_server: s3_server_mentah.c   ; $(CC) $(CFLAGS) -o $@ $<
s3_pengirim: s3_pengirim.c      ; $(CC) $(CFLAGS) -o $@ $<
s4_server: s4_server_tangguh.c  ; $(CC) $(CFLAGS) -o $@ $<
s4_lambat: s4_klien_lambat.c    ; $(CC) $(CFLAGS) -o $@ $<
s4_probe:  s4_probe.c           ; $(CC) $(CFLAGS) -o $@ $<
s5_server: s5_server_thread.c   ; $(CC) $(CFLAGS) -pthread -o $@ $<
s6_server: s6_server_tls.c      ; $(CC) $(CFLAGS) -o $@ $< -lssl -lcrypto
s6_klien:  s6_klien_tls.c       ; $(CC) $(CFLAGS) -o $@ $< -lssl -lcrypto
s6_probe:  s6_probe_tls.c       ; $(CC) $(CFLAGS) -o $@ $< -lssl -lcrypto

# sertifikat lab (hanya untuk mesin sendiri)
pki: ; sh buat_sertifikat.sh pki 30

# target verifikasi
asan: s4_server_tangguh.c ; $(CC) $(SAN) -fsanitize=address,undefined -o s4_asan $<
asan-tls: s6_server_tls.c ; $(CC) $(SAN) -fsanitize=address,undefined -o s6_asan $< -lssl -lcrypto
tsan: s5_server_thread.c
	$(CC) $(SAN) -fsanitize=thread -pthread -o s5_kontrol $<
	$(CC) $(SAN) -fsanitize=thread -pthread -DTANPA_KUNCI -o s5_race $<

clean: ; rm -f s1_server s1_klien s2_server s2_klien s3_server s3_pengirim s4_server s4_lambat s4_probe s5_server s6_server s6_klien s6_probe s4_asan s6_asan s5_kontrol s5_race
.PHONY: all asan asan-tls tsan pki clean

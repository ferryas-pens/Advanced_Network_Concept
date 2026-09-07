# Contoh capture untuk Bagian 13

Tiga berkas ini adalah hasil perekaman nyata pada 2026-08-30, supaya Anda dapat
mencoba analisisnya tanpa harus merekam sendiri lebih dulu.

| Berkas | Isi |
|---|---|
| `capture_plain.pcap` | Percakapan pada server Tahap 4, port 5090, tanpa enkripsi |
| `capture_tls.pcap` | Percakapan yang sama pada server Tahap 6, port 5445, TLS 1.3 |
| `kunci_sesi.log` | Rahasia sesi TLS untuk `capture_tls.pcap`, format key log NSS |

Kalimat penanda pada kedua percakapan: `rahasia-rapat-anggaran-2026`

## Perintah untuk memulai

```bash
# 1. plaintext: pesan terbaca langsung
strings capture_plain.pcap | grep rahasia
tshark -r capture_plain.pcap -q -z follow,tcp,ascii,0

# 2. TLS tanpa kunci: pesan tidak ada, tetapi SNI dan ukuran record terbaca
strings capture_tls.pcap | grep -c rahasia
tshark -r capture_tls.pcap -d tcp.port==5445,tls -Y 'tls.handshake.extensions_server_name' \
       -T fields -e tls.handshake.extensions_server_name
tshark -r capture_tls.pcap -d tcp.port==5445,tls -Y 'tls.record.opaque_type==23' \
       -T fields -e frame.number -e tls.record.length

# 3. TLS dengan kunci: percakapan pulih seluruhnya
tshark -r capture_tls.pcap -d tcp.port==5445,tls \
       -o tls.keylog_file:$PWD/kunci_sesi.log -q -z follow,tls,ascii,0
```

## Peringatan

`kunci_sesi.log` membuka seluruh isi `capture_tls.pcap`. Kunci ini sengaja
dibagikan karena sesi tersebut hanya berisi kalimat latihan, dan sertifikatnya
sertifikat lab sekali pakai. Perlakukan berkas key log pada sistem sungguhan
sebagai rahasia tingkat tertinggi, dan perlakukan keberadaannya di sebuah host
sebagai temuan yang perlu ditindaklanjuti.

#!/bin/sh
# buat_sertifikat.sh — membuat CA lab dan sertifikat server untuk praktikum TLS.
#
# PERINGATAN: seluruh kunci dan sertifikat di sini HANYA untuk lab di mesin
# sendiri. CA ini tidak boleh dipasang ke trust store sistem, browser, atau
# mesin lain. Kunci privat tidak diberi passphrase supaya praktikum ringkas,
# dan itu justru alasan tambahan untuk tidak memakainya di luar lab.
set -e

DIR=${1:-pki}
HARI=${2:-30}
mkdir -p "$DIR"
cd "$DIR"

echo "[1/4] membuat kunci dan sertifikat CA lab"
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$HARI" \
    -keyout ca.key -out ca.crt \
    -subj "/C=ID/O=Lab Kamsiber PENS/CN=CA Lab Chat" 2>/dev/null

echo "[2/4] membuat kunci dan CSR server"
openssl req -newkey rsa:2048 -nodes -sha256 \
    -keyout server.key -out server.csr \
    -subj "/C=ID/O=Lab Kamsiber PENS/CN=localhost" 2>/dev/null

echo "[3/4] menandatangani sertifikat server dengan SAN"
cat > san.cnf <<'EOF'
subjectAltName = DNS:localhost, IP:127.0.0.1
basicConstraints = critical, CA:FALSE
keyUsage = critical, digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
EOF
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days "$HARI" -sha256 -extfile san.cnf 2>/dev/null

chmod 600 ca.key server.key

echo "[4/4] verifikasi rantai"
openssl verify -CAfile ca.crt server.crt
openssl x509 -in server.crt -noout -subject -issuer -dates \
    -ext subjectAltName | sed 's/^/    /'

echo
echo "Selesai. Berkas ada di direktori: $DIR"
echo "  ca.crt      dipakai KLIEN untuk memverifikasi server"
echo "  server.crt  dipakai SERVER sebagai identitasnya"
echo "  server.key  kunci privat server, jangan dibagikan"

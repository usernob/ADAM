# 10. Deployment & Infrastruktur

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Seluruh sistem operasional berjalan di **satu VPS**:

* **Node Gateway** — HTTP API, SSE, antrean, dispatch MQTT, dan SQLite.
* **Mosquitto** — broker MQTT lokal untuk gateway dan robot.
* **Caddy** — reverse proxy untuk HTTP/SSE dan WebSocket MQTT.
* **Cloudflare Tunnel** — jalur masuk HTTPS/WSS tanpa membuka port publik di VPS.
* **Robot A.D.A.M** — klien MQTT WSS dan pengunduh `GET /graph.bin` saat boot.

Tidak ada Supabase, Vercel, EMQX Cloud, database terkelola, atau browser yang terhubung langsung ke MQTT.

## 10.1 Topologi

```text
Internet
   |
   | HTTPS / WSS :443
   v
[ Cloudflare Edge ]
   |
   | Cloudflare Tunnel (koneksi keluar dari VPS)
   v
[ cloudflared ]
   |
   | http://127.0.0.1:8080
   v
[ Caddy ]
   ├── /mqtt*  -> 127.0.0.1:8083  (Mosquitto WebSocket, robot only)
   └── /*      -> 127.0.0.1:3000  (Node Gateway)
                        |
                        ├── SQLite: data/adam.db
                        ├── SSE /events
                        └── MQTT client -> 127.0.0.1:1883
                                              |
                                              v
                                         [ Mosquitto ]
                                              ^
                                              | WSS /mqtt :443
                                              |
                                         [ Robot ]
```

VPS berada di belakang NAT. Tidak ada port 80, 443, 1883, 8083, atau 8883 yang perlu dibuka ke internet. Cloudflare Tunnel membuka koneksi **keluar** ke Cloudflare, lalu Cloudflare meneruskan trafik masuk.

Jalur WSS `/mqtt` hanya dipakai robot. Browser tidak pernah membuka koneksi MQTT dan tidak boleh menerima kredensial MQTT; browser hanya memakai HTTP/SSE ke Node Gateway. Jalur `/mqtt` tetap publik agar robot bisa connect dari jaringan luar, jadi lindungi dengan TLS, password kuat, ACL, dan rate limit di edge.

## 10.2 Komponen & Port

| Komponen | Bind | Port | Akses |
| :--- | :--- | :---: | :--- |
| Node Gateway | `127.0.0.1` | `3000` | Hanya lewat Caddy |
| Caddy | `127.0.0.1` | `8080` | Hanya lewat Cloudflare Tunnel |
| Mosquitto MQTT | `127.0.0.1` | `1883` | Gateway lokal |
| Mosquitto WebSocket | `127.0.0.1` | `8083` | Caddy / Cloudflare Tunnel |
| cloudflared | keluar | `443` | Cloudflare |
| SSH | sesuai kebijakan | `22` | Admin |

Jangan bind layanan internal ke `0.0.0.0` kecuali benar-benar diperlukan.

## 10.3 Cloudflare Tunnel & Caddy

Cloudflare Tunnel menjalankan `cloudflared` di VPS dan mendaftarkan hostname publik, misalnya `adam.example.com`. Konfigurasi ingress minimal:

```yaml
tunnel: adam
credentials-file: /etc/cloudflared/adam.json

ingress:
  - hostname: adam.example.com
    service: http://127.0.0.1:8080
  - service: http_status:404
```

Caddy menerima trafik dari tunnel dan memisahkan WebSocket MQTT dari aplikasi:

```caddyfile
http://127.0.0.1:8080 {
	handle /mqtt* {
		reverse_proxy 127.0.0.1:8083
	}
	handle {
		reverse_proxy 127.0.0.1:3000
	}
}
```

* Cloudflare menangani TLS di edge; koneksi `cloudflared` → Caddy → Mosquitto berada di localhost.
* WebSocket MQTT memakai path `/mqtt`.
* SSE tidak boleh di-buffer. Pastikan Caddy/Cloudflare tidak meng-cache `/events`; gateway mengirim heartbeat setiap 15 detik.
* Cloudflare Access **opsional** untuk melindungi `/admin*`, `/api/admin/*`, dan `/save`. Jangan memasang Access di `/mqtt`, `/api/call`, `/api/graph`, `/graph.bin`, atau `/events`, karena robot dan user publik membutuhkannya.
* Jika memakai Cloudflare Access untuk admin, tetap pertahankan login admin di gateway; Access adalah lapisan tambahan, bukan pengganti session.

## 10.4 Mosquitto

Konfigurasi dasar `/etc/mosquitto/conf.d/adam.conf`:

```conf
per_listener_settings false
allow_anonymous false
password_file /etc/mosquitto/passwd
acl_file /etc/mosquitto/acl

listener 1883 127.0.0.1
protocol mqtt

listener 8083 127.0.0.1
protocol websockets

persistence true
persistence_location /var/lib/mosquitto/
```

Buat user gateway dan robot:

```bash
mosquitto_passwd -c /etc/mosquitto/passwd gateway
mosquitto_passwd /etc/mosquitto/passwd robot
```

ACL `/etc/mosquitto/acl`:

```conf
user gateway
topic write srv/adam/#
topic read adam/srv/#

user robot
topic write adam/srv/#
topic read srv/adam/#
```

Aturan:

* Tidak ada anonymous.
* Gateway hanya boleh menulis perintah dan membaca telemetri.
* Robot hanya boleh menulis telemetri dan membaca perintah.
* Retain hanya untuk state telemetri (`current_pos`, `status`, `graph_version`, `battery`, `trash`, `online`), bukan perintah.
* `status` retained adalah last-known untuk web. Gateway tetap mewajibkan status fresh maksimal 10 detik sebelum dispatch; robot mem-publish status setiap 3 detik.
* Clean session `true` di robot; subscription dipasang ulang setiap connect.
* Gunakan QoS 1 untuk seluruh perintah dan telemetri.

## 10.5 Koneksi Robot

Konfigurasi target robot:

```text
Broker URI : wss://adam.example.com/mqtt
Port       : 443
TLS        : ya (Cloudflare edge)
Username   : robot
Password   : <secret>
Clean session : true
Keepalive  : 30 detik
LWT        : adam/srv/online = 0x00, retain
```

Sebelum disconnect normal, robot wajib mem-publish `adam/srv/online = 0x00` sendiri; broker tidak mem-publish LWT pada clean disconnect.

Kredensial MQTT disimpan di NVS robot, bukan di source code. Robot juga menyimpan URL gateway untuk `GET /graph.bin`.

### Fallback MQTT langsung

Jika WSS tidak memungkinkan, gunakan MQTT over TLS langsung:

1. Petakan port publik `8883` di provider/NAT ke VPS `8883`.
2. Tambahkan listener TLS di Mosquitto:

```conf
listener 8883 0.0.0.0
protocol mqtt
cafile   /etc/mosquitto/certs/chain.pem
certfile /etc/mosquitto/certs/fullchain.pem
keyfile  /etc/mosquitto/certs/privkey.pem
```

3. Terbitkan sertifikat dengan **DNS-01** (bukan HTTP-01), karena port 80 tidak terbuka.
4. Ubah URI robot menjadi `mqtts://vps.example.com:8883`.

Fallback ini tidak dipakai secara default: WSS 443 lebih ramah NAT dan firewall. Jika fallback diaktifkan, port 8883 menjadi satu-satunya pengecualian pada firewall; tetap batasi akses dan gunakan TLS. Jangan mengaktifkan dua jalur masuk tanpa alasan operasional.

## 10.6 SQLite

Semua state persisten berada di `data/adam.db`:

* `admins` — akun admin dan password hash.
* `sessions` — hash token session, waktu kedaluwarsa.
* `requests` — request_id, target, status, `req_id` wire, timestamps, error.
* `queue`/view antrean — request `queued`/`dispatched`/`serving`.
* `graph_versions` — riwayat immutable setiap versi: body matriks 676 byte, daftar node/edge, timestamp.
* `graph` — pointer ke versi graf aktif dan timestamp deployment.
* `settings` — flag maintenance dan konfigurasi gateway yang harus bertahan saat restart.
* `robot_state`/cache — versi graf terakhir yang dilaporkan robot, status freshness, dan timestamp telemetry.
* `logs` — audit dan kejadian sistem.

Versi graf pertama adalah `1`; `0` dicadangkan untuk “graf belum siap”. Setiap save/rollback menambah baris baru di `graph_versions`; baris lama tidak diubah.

Aktifkan WAL:

```sql
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
PRAGMA busy_timeout = 5000;
```

Aturan:

* Hanya **satu proses gateway** yang boleh menulis. Jangan jalankan mode cluster.
* File database dan direktori `data/` hanya dapat diakses user service.
* Backup harian dengan perintah backup SQLite, bukan copy file mentah saat database aktif:

```bash
mkdir -p /var/backups/adam
sqlite3 /opt/adam/data/adam.db \
  ".backup '/var/backups/adam/adam-$(date +%F).db'"
```

* Simpan 7–14 backup terakhir, dan uji restore secara berkala.
* Jangan commit `data/`, `.env`, atau file secret ke Git.

## 10.7 Admin & Autentikasi

Akun admin hanya dibuat lewat CLI di server:

```bash
node scripts/admin.mjs create admin
node scripts/admin.mjs reset-password admin
node scripts/admin.mjs list
```

* Tidak ada endpoint registrasi.
* Tidak ada OAuth, reset password via email, atau TOTP.
* Password disimpan sebagai hash Argon2id.
* Session memakai cookie `HttpOnly`, `Secure`, `SameSite=Lax`; token disimpan sebagai hash di SQLite.
* Login admin dibatasi rate limit dan dicatat di audit log.
* Jika memakai Cloudflare Access, akun Access tidak otomatis menjadi admin gateway.

## 10.8 Proses & Service

Gunakan satu instance gateway. Contoh systemd:

```ini
[Unit]
Description=ADAM Gateway
After=network-online.target mosquitto.service
Wants=network-online.target

[Service]
Type=simple
User=adam
Group=adam
WorkingDirectory=/opt/adam
EnvironmentFile=/opt/adam/.env
ExecStart=/usr/bin/node server/index.js
Restart=always
RestartSec=3
NoNewPrivileges=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
```

Alternatif PM2: gunakan `instances: 1`. **Jangan** memakai `pm2 start ... -i max` atau cluster mode, karena antrean dan koneksi MQTT harus dimiliki satu proses.

Service lain:

```bash
systemctl enable --now mosquitto
systemctl enable --now caddy
systemctl enable --now cloudflared
systemctl enable --now adam-gateway
```

## 10.9 Keamanan

* Firewall default-deny; hanya SSH dari IP admin.
* Tidak ada port aplikasi/MQTT yang terbuka langsung ke internet.
* MQTT tanpa anonymous, ACL ketat, password kuat, TLS di edge.
* `allow_anonymous false` dan `acl_file` wajib.
* `.env` berisi `SESSION_SECRET`, kredensial MQTT, dan konfigurasi sensitif; permission `0600`, owner user service.
* Rate limit `POST /api/call` (default 1 permintaan/10 detik per IP/`client_id`) dan batas antrean (default 20 `queued`).
* Validasi ketat input graf di `POST /save`: label `A`–`Z` unik dengan `A` wajib ada, boleh tidak berurutan, maksimal 26 node; bobot `1..63`; tidak ada self-loop/duplicate pair; maksimal satu edge per arah mata angin per node; graf connected dari `A`; reverse edge dibuat otomatis.
* Jangan mencatat password, token session, atau kredensial MQTT di log.
* Perbarui Mosquitto, Node, Caddy, dan cloudflared secara berkala.
* Batasi akses SSH dan gunakan key, bukan password.

## 10.10 Observability & Operasional

* `GET /healthz` memeriksa gateway, SQLite, koneksi MQTT, status online robot, status freshness, maintenance, dan kecocokan versi graf.
* Log gateway ke journald/PM2; log Mosquitto ke file dengan rotasi.
* Pantau disk, memori, dan uptime.
* Alert sederhana jika robot offline, MQTT disconnect, SQLite gagal dibuka, status tidak fresh lebih dari 10 detik, maintenance aktif terlalu lama, atau `graph_version` robot tidak cocok dengan graf aktif.
* Sertakan `request_id`/`req_id` di log untuk melacak satu panggilan dari HTTP sampai `ack`/`error`.

## 10.11 Deployment Graf

Graf hanya diaktifkan saat robot boot. Tidak ada refresh graf saat robot sedang bekerja.

1. Aktifkan maintenance lewat `POST /api/admin/maintenance` dengan `{ "enabled": true }`. Panggilan baru ditolak `503 maintenance` dan dispatch antrean dihentikan.
2. Tunggu request aktif selesai, lalu kosongkan antrean lewat admin. `POST /save` ditolak `409` selama masih ada request aktif atau antrean.
3. Simpan graf baru lewat `POST /save` dalam bentuk JSON `{ nodes, edges }`. Gateway memvalidasi, menulis versi baru ke riwayat, menaikkan nomor versi, dan menahan dispatch.
4. Jika ada perubahan marka/lintasan fisik, lakukan **sekarang**, saat robot berhenti. Software tidak dapat memverifikasi kesesuaian graf dengan lantai.
5. Pastikan antrean masih kosong dan maintenance tetap aktif. Matikan robot, pindahkan ke Home `A` menghadap Utara, lalu nyalakan kembali.
6. Robot mengunduh `GET /graph.bin` secara atomic (header versi dan body 676 byte dari snapshot yang sama). Robot memvalidasi HTTP 200, `X-Graph-Version` bukan `0`, dan body 676 byte; maksimal 3 percobaan dengan jeda 2 detik. Setelah berhasil, robot mem-publish `adam/srv/graph_version`.
7. Gateway membandingkan versi. Setelah versi cocok dan status fresh `IDLE`, nonaktifkan maintenance. Dispatch baru dilanjutkan.

Jika langkah 6 gagal setelah 3 percobaan, robot mem-publish `graph_version = 0`, `status = ERROR`, dan tidak retry otomatis sampai reboot. Gateway tetap menahan dispatch. Jangan mengirim graf lewat MQTT atau memaksa robot menerima job dengan graf lama.

Rollback:

1. Lihat riwayat lewat `GET /api/admin/graph/versions`.
2. Panggil `POST /api/admin/graph/rollback` dengan `{ "version": <lama> }`. Gateway membuat **versi baru** dari snapshot lama; nomor versi tidak diturunkan.
3. Ulangi prosedur maintenance dan reboot di atas. Jangan mengubah baris versi secara manual di SQLite.

## 10.12 Batasan

Satu VPS adalah **single point of failure**. Untuk R&D ini disengaja agar antrean, SQLite, dan MQTT tetap sederhana. Jika nanti butuh ketersediaan tinggi, antrean perlu dipisah ke penyimpanan bersama dan gateway harus memakai mekanisme kepemilikan tunggal; jangan hanya menambah instance kedua di belakang load balancer.

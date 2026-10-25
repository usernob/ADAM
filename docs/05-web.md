# 5. Web Interface & Interaction Strategy

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Antarmuka web melayani pemanggilan dan visualisasi *real-time* melalui **HTTP**, **Server-Sent Events (SSE)**, dan **MQTT Bridge** yang dijalankan oleh satu **Gateway** persisten.

## 5.1 Prinsip Arsitektur

* **Gateway adalah satu-satunya pemilik antrean.** Browser tidak pernah mengirim MQTT dan tidak menyimpan antrean sendiri.
* Browser hanya berkomunikasi lewat HTTP dan SSE ke gateway.
* Robot adalah eksekutor: ia hanya menerima satu job pada satu waktu dan tidak memiliki backlog target.
* Seluruh state operasional (antrean, status robot, hasil request, log) disimpan di **SQLite** milik gateway.
* **Tidak ada Supabase, OAuth, atau login user.** User publik tidak perlu akun; hanya admin yang login.
* Gateway dan broker MQTT berjalan di VPS yang sama. Detail deployment ada di [docs/10-deployment.md](10-deployment.md).

```text
[ Browser ]
    |
    | HTTPS + SSE
    v
[ Cloudflare Tunnel / Caddy ]
    |
    v
[ Node Gateway ]  (satu proses, satu instance)
    ├── static frontend
    ├── REST API
    ├── SSE /events
    ├── antrean + dispatch
    ├── SQLite
    └── MQTT client
          |
          | MQTT (WSS / localhost)
          v
     [ Mosquitto ]  <---- MQTT ---->  [ Robot A.D.A.M ]
          ^
          |
    GET /graph.bin (HTTP dari robot ke gateway, saat boot)
```

## 5.2 Halaman Antarmuka Web

1. **Halaman Quick-Call Node (`/cmd/:nodeId`)**:
   * Dioptimalkan untuk **QR Code** yang ditempelkan langsung pada titik fisik/node (misal `/cmd/A`).
   * **1-Click Call Button** untuk memanggil robot ke lokasi node tersebut.
   * **Publik, tanpa login.** Dibatasi *rate limit* dan deduplikasi target.
   * Menampilkan posisi robot (`current` dan `next`), status, baterai, serta daftar antrean aktif.
   * Jika target sudah ada di antrean atau sedang dituju, tombol tidak menambah antrean baru; UI menampilkan pesan seperti “Rute ke Node A sudah dijadwalkan”.

2. **Halaman Visualisasi Graf & Peta (`/map` atau `/viewer`)**:
   * Menampilkan denah graf secara visual dari `GET /api/graph`.
   * Mode **Read-Only** untuk publik.
   * Marker posisi robot *real-time*, arah menuju `next_node`, dan persentase baterai.
   * **Interactive Click**: mengklik node pada canvas akan memicu panggilan ke node tersebut (mengikuti aturan deduplikasi).

3. **Halaman Admin (`/admin`)**:
   * Wajib login admin.
   * Editor graf, status versi graf gateway vs robot, riwayat versi, rollback, mode maintenance, admin reset, manajemen antrean, dan riwayat log.
   * Editor memuat graf aktif dari `GET /api/graph` dan menyimpan lewat `POST /save` dalam bentuk JSON `{ nodes, edges }`.
   * Menyimpan graf tidak langsung mengubah robot. Gateway menahan dispatch sampai robot reboot dan melaporkan `graph_version` yang sama.
   * Akun admin dibuat lewat CLI di server; tidak ada registrasi publik.

4. **Rate Limiting & Anti-Spam**:
   * Diterapkan di sisi Gateway pada `POST /api/call`.
   * **Cooldown Window** per IP/`client_id` (default: maksimum 1 permintaan per 10 detik).
   * Deduplikasi target mencegah satu node memenuhi antrean.
   * Kapasitas antrean default 20 request; dapat dikonfigurasi.

## 5.3 Antrean & Deduplikasi

* Gateway memegang antrean; robot hanya memiliki **mailbox 1 slot**.
* Hanya satu request yang boleh *in-flight* pada satu waktu.
* Dispatch hanya dilakukan jika robot `online`, status `IDLE` masih fresh (maksimal 10 detik), tidak ada request aktif, maintenance nonaktif, dan `graph_version` robot sama dengan versi graf aktif.
* Jika versi graf belum cocok, `POST /api/call` ditolak `503 graph_update`; tidak ada request baru yang masuk antrean. UI menampilkan status maintenance sampai robot reboot.
* Sebelum menyimpan graf, admin mengaktifkan maintenance lewat `POST /api/admin/maintenance`; ini menghentikan panggilan baru dan dispatch antrean. `POST /save` ditolak `409` selama masih ada request aktif atau antrean.
* Request memiliki status: `queued`, `dispatched`, `serving`, `done`, `failed`, `cancelled`.
* **Kunci deduplikasi adalah target node**, bukan user atau sesi.
  * Selama masih ada request aktif untuk Node A (`queued`, `dispatched`, `serving`), atau robot sedang berada di Node A dalam status `IDLE`/`DWELL` **dengan versi graf yang cocok**, panggilan baru ke Node A tidak menambah antrean.
  * Semua pemanggil mendapat `request_id` yang sama.
  * Setelah request `done`/`failed`/`cancelled` dan robot meninggalkan node, panggilan baru boleh masuk.
* Deduplikasi dicek **sebelum** kapasitas antrean. Jika Node A sudah ada, user mendapat status duplikat, bukan `429`.
* Detail status, response API, dan lifecycle ada di [docs/06-protocol.md](06-protocol.md).

## 5.4 Stream Real-Time via SSE (`GET /events`)

Gateway menjadi **MQTT-to-SSE Bridge**. Setiap client yang terhubung menerima snapshot awal, lalu pembaruan berikut:

| Event | Data | Keterangan |
| :--- | :--- | :--- |
| `snapshot` | `{ pos, status, status_fresh, battery, trash, online, maintenance, graph: { gateway_version, robot_version, ready }, queue }` | Dikirim sekali saat koneksi SSE dibuka |
| `pos_update` | `{ current, next }` | `next = null`/`0` berarti robot tidak sedang menuju node lain |
| `status_update` | `IDLE`, `MOVING`, `DWELL`, `ERROR` | Status operasional robot |
| `status_fresh_update` | `true`/`false` | `false` jika tidak ada status baru selama 10 detik; dispatch dihentikan |
| `graph_update` | `{ gateway_version, robot_version, ready }` | `ready` = versi robot sama dengan versi gateway dan bukan `0`; client dapat memuat ulang `GET /api/graph` saat versi gateway berubah |
| `maintenance_update` | `true`/`false` | Mode maintenance aktif/nonaktif |
| `battery_update` | `0`–`100` | Persentase baterai |
| `trash_update` | `0`–`100` | Kapasitas sampah terisi |
| `queue_update` | daftar request aktif dan menunggu | Termasuk status dan posisi |
| `call_result` | `{ request_id, status, message }` | Hasil request untuk pemanggil |
| `online_update` | `true`/`false` | Berdasarkan topik LWT `adam/srv/online` |

* `status` retained adalah **last known status** untuk tampilan. Gateway baru menganggap status fresh jika ada pesan baru dalam 10 detik terakhir.
* Jika status tidak fresh, UI menampilkan indikator `stale`; jangan menampilkan robot sebagai siap menerima job.
* Kirim komentar heartbeat setiap 15 detik agar koneksi tidak diputus proxy.
* Client menggunakan `EventSource` dan reconnect otomatis.
* Tidak ada Supabase Realtime; SSE adalah satu-satunya jalur *live update* dari gateway.
* SSE bersifat **read-only publik**. Aksi admin tetap lewat HTTP dengan session.

## 5.5 Autentikasi

* **User publik tidak perlu login.** `/cmd/:nodeId`, `/api/call`, `/api/graph`, `/events`, `/graph.bin`, dan `/map` dapat diakses tanpa akun.
* **Hanya admin** yang memiliki akun.
* Tidak ada registrasi, OAuth, reset password via email, atau TOTP.
* Akun admin dibuat dan di-reset lewat CLI di server.
* Session menggunakan cookie `HttpOnly`, `Secure`, `SameSite=Lax`, dengan token acak yang disimpan sebagai hash di SQLite.
* Endpoint `/api/admin/*` dan `POST /save` wajib session admin.
* Detail endpoint auth ada di [docs/06-protocol.md](06-protocol.md).

## 5.6 Deployment

Topologi VPS, Cloudflare Tunnel, Mosquitto, SQLite, prosedur deployment graf, dan opsi MQTT langsung dibahas di [docs/10-deployment.md](10-deployment.md).

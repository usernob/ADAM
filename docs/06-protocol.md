# 6. Protokol Komunikasi & API Endpoints

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Dokumen ini mendefinisikan protokol target: **Gateway Node** memegang antrean, robot hanya menerima satu job pada satu waktu, seluruh korelasi perintah memakai `req_id`, dan graf hanya diaktifkan saat boot.

## 6.1 Prinsip Protokol

* Gateway adalah pemilik antrean dan satu-satunya pihak yang boleh mengirim `moveto`.
* Robot memiliki **mailbox 1 slot** dan hanya menerima job saat status `IDLE` serta graf sudah siap.
* Semua perintah dan telemetri MQTT memakai **payload biner**, bukan JSON.
* Setiap perintah yang membutuhkan hasil memakai `req_id` 2 byte untuk korelasi `moveto` → `ack`/`error`.
* Graf adalah artefak deployment: robot hanya memuatnya saat boot. Gateway menahan dispatch sampai versi graf robot sama dengan versi graf aktif.
* Node memakai ASCII huruf kapital `A`–`Z`. Label harus unik, `A` wajib ada, dan satu graf maksimal 26 node. Label **tidak harus berurutan**; contoh `A, C, F` valid.
* Graf bersifat **undirected** dan harus connected dari `A`: semua node aktif harus dapat dicapai dari `A`.
* Kode status, hasil `ack`, dan error bersifat **append-only**. Nilai lama tidak diubah; kode baru ditambahkan di akhir. Penerima wajib mengabaikan kode yang tidak dikenal.
* `0x00` bukan sentinel universal: nilainya bisa berarti `IDLE`, `ACCEPTED`, atau `offline` pada field yang berbeda. Makna selalu mengikuti tabel field terkait. Nilai `0` hanya dipakai sebagai sentinel pada `next` (tidak ada node berikutnya), `req_id` (tidak terkait request), `error NONE`, dan `graph_version` (graf belum siap).
* Tidak ada Supabase, browser MQTT, atau OAuth. Browser hanya memakai HTTP/SSE.

## 6.2 HTTP API

| Endpoint | Metode | Akses | Deskripsi |
| :--- | :---: | :--- | :--- |
| **`GET /graph.bin`** | GET | Robot | Mengunduh matriks graf biner saat boot. Body selalu `26 × 26` byte; header hanya membawa versi. |
| **`GET /api/graph`** | GET | Publik | Membaca graf aktif sebagai JSON untuk map/editor web. |
| **`POST /save`** | POST | Admin | Menyimpan versi graf baru ke SQLite. Ditolak jika masih ada request aktif atau antrean. |
| **`GET /api/admin/graph/versions`** | GET | Admin | Riwayat versi graf untuk audit dan rollback. |
| **`POST /api/admin/graph/rollback`** | POST | Admin | Membuat versi baru dari snapshot graf lama. |
| **`POST /api/admin/maintenance`** | POST | Admin | Mengaktifkan/menonaktifkan mode maintenance. |
| **`POST /api/call`** | POST | Publik | Meminta robot menuju satu node. Terkena deduplikasi, rate limit, dan kapasitas antrean. |
| **`GET /cmd/:nodeId`** | GET | Publik | Halaman quick-call untuk QR Code di node fisik. |
| **`GET /events`** | GET | Publik | Stream SSE untuk snapshot dan pembaruan *real-time*. |
| **`POST /api/auth/login`** | POST | Publik | Login admin, mengembalikan session cookie. |
| **`POST /api/auth/logout`** | POST | Admin | Menghapus session aktif. |
| **`GET /api/auth/me`** | GET | Admin | Informasi admin yang sedang login. |
| **`POST /api/auth/password`** | POST | Admin | Mengganti password admin yang sedang login. |
| **`POST /api/admin/reset`** | POST | Admin | Reset robot: batalkan request aktif/antrean, lalu kirim `srv/adam/reset`. Reset tidak memuat ulang graf. |
| **`GET /api/admin/queue`** | GET | Admin | Melihat seluruh request aktif dan antrean. |
| **`POST /api/admin/queue/clear`** | POST | Admin | Membatalkan request `queued`; request aktif tidak dihentikan. |
| **`GET /api/admin/logs`** | GET | Admin | Riwayat kejadian sistem dan request. |
| **`GET /healthz`** | GET | Publik (read-only) | Status gateway, SQLite, MQTT, online/offline robot, status freshness, maintenance, dan kecocokan versi graf. |

Akses admin memakai session cookie `HttpOnly`, `Secure`, `SameSite=Lax`. Tidak ada registrasi publik. Akun admin hanya dibuat/di-reset lewat CLI server (lihat [docs/10-deployment.md](10-deployment.md)).

### `POST /api/call`

Body JSON:

```json
{
  "node": "A",
  "client_id": "qr-A"
}
```

* `node` wajib berupa satu huruf kapital `A`–`Z` yang ada di daftar node aktif.
* `client_id` opsional, dipakai untuk rate limit per IP/klien. Bukan autentikasi.
* Deduplikasi dilakukan berdasarkan **target node**, bukan `client_id`.

Urutan pemeriksaan:

1. Validasi node terhadap graf aktif.
2. Cek mode maintenance.
3. Cek robot online dan tidak `ERROR`.
4. Cek `graph_version` robot sama dengan versi graf aktif.
5. Deduplikasi target.
6. Rate limit.
7. Kapasitas antrean.

Deduplikasi dicek sebelum rate limit agar klik berulang pada node yang sama tetap mengembalikan request yang sama, bukan `429`.

Respons:

| Kondisi | HTTP | `status` | Keterangan |
| :--- | :---: | :--- | :--- |
| Request baru diterima | `202` | `queued` | Request masuk antrean; `queue_position` diisi. |
| Sudah ada request `queued` untuk node itu | `200` | `already_queued` | Memakai `request_id` yang sama. |
| Request untuk node itu `dispatched`/`serving` | `200` | `in_progress` | Robot sedang menuju/sedang melayani node tersebut. |
| Robot `IDLE` di node itu, tidak ada request aktif, dan versi graf cocok | `200` | `already_here` | Tidak membuat request baru. |
| Node tidak valid | `400` | `invalid_node` | Node tidak ada di graf aktif. |
| Robot sedang `ERROR` | `409` | `robot_error` | Menunggu admin reset. |
| Gateway dalam mode maintenance | `503` | `maintenance` | Tidak ada request yang dibuat. |
| `graph_version` robot belum cocok | `503` | `graph_update` | Robot harus reboot dan memuat graf yang sama. |
| Robot offline | `503` | `robot_offline` | Coba lagi setelah robot online. |
| Terlalu sering | `429` | `rate_limited` | Sertakan `Retry-After`. |
| Antrean penuh | `429` | `queue_full` | Kapasitas default 20 request `queued`. |

Contoh respons request baru:

```json
{
  "request_id": 42,
  "node": "A",
  "status": "queued",
  "queue_position": 1
}
```

Contoh respons duplikat:

```json
{
  "request_id": 42,
  "node": "A",
  "status": "already_queued",
  "queue_position": 1
}
```

`queue_position` adalah posisi 1-based di antara request `queued`; request aktif (`dispatched`/`serving`) tidak dihitung. Untuk status `in_progress`, `queue_position` tidak diisi (`null`). Untuk `already_here`, `request_id` boleh `null` karena tidak ada request yang dibuat.

Deduplikasi “robot sudah di node” berlaku jika robot `IDLE` di node tersebut tanpa request aktif, atau jika robot `DWELL` di sana (yang berarti request `serving` untuk node itu sudah ada), **dan** versi graf robot sudah sama dengan versi graf gateway. Jika robot sedang `MOVING`, `current_pos` hanyalah node terakhir yang dikonfirmasi; robot mungkin sudah meninggalkannya, sehingga panggilan ke node itu tetap boleh masuk antrean.

Jika versi graf robot belum cocok, request baru **ditolak** dengan `503 graph_update` dan tidak masuk antrean. UI menampilkan status maintenance sampai robot reboot dan melaporkan versi yang sama. Request `queued` yang sudah ada sebelum reboot tak terduga tetap tersimpan, tetapi tidak akan didispatch sampai versi cocok kembali. Pada deployment graf, antrean sudah kosong karena `POST /save` mensyaratkan itu.

Semua pemanggil ke node yang sama memakai `request_id` yang sama. Hasil akhir dikirim lewat SSE `call_result`.

### `GET /graph.bin`

| HTTP Header | Deskripsi |
| :--- | :--- |
| **`Content-Type: application/octet-stream`** | Body adalah matriks biner tetap `26 × 26` byte (*weight* di bit 2-7, *direction* di bit 0-1 — lihat [docs/02-graph.md](02-graph.md)). |
| **`Content-Length`** | Selalu `676`. |
| **`X-Graph-Version`** | Versi integer 32-bit peta; `0` tidak pernah disajikan. |
| **`Cache-Control: no-store`** | Robot harus menerima graf terbaru saat boot, bukan salinan cache. |

Body selalu 676 byte. Index matrix adalah `label - 'A'`; baris/kolom untuk label yang tidak aktif berisi nol. Node count dan edge count **tidak dikirim** di header; informasi itu hanya dibutuhkan web/admin.

`GET /graph.bin` harus mengambil header versi dan body dari **snapshot graf yang sama**. Jika `POST /save` terjadi bersamaan, robot tidak boleh menerima header versi lama dengan body versi baru, atau sebaliknya. Jika belum ada graf aktif, endpoint mengembalikan `404`.

### `GET /api/graph`

Endpoint publik read-only untuk map dan editor web:

```json
{
  "version": 13,
  "nodes": ["A", "C", "F"],
  "edges": [
    { "from": "A", "to": "C", "weight": 12, "direction": "N" },
    { "from": "C", "to": "F", "weight": 5, "direction": "E" }
  ]
}
```

* `nodes` berisi label aktif, diurutkan ascending.
* `edges` berisi satu entri per koneksi fisik (undirected). `direction` adalah arah dari `from` ke `to`.
* Reverse edge tidak ditampilkan terpisah; gateway menurunkannya saat membentuk matriks.
* Endpoint ini memakai snapshot graf yang sama dengan `GET /graph.bin` dan `POST /save`.
* Jika belum ada graf aktif, endpoint mengembalikan `404`.

### `POST /save`

Body JSON:

```json
{
  "nodes": ["A", "C", "F"],
  "edges": [
    { "from": "A", "to": "C", "weight": 12, "direction": "N" },
    { "from": "C", "to": "F", "weight": 5, "direction": "E" }
  ]
}
```

Gateway memvalidasi seluruh graf sebelum menyimpan:

* `nodes` wajib ada, unik, hanya huruf kapital `A`–`Z`, maksimal 26, dan wajib memuat `A`.
* Label tidak harus contiguous; `A, C, F` valid. `N` adalah jumlah label aktif, bukan nilai huruf tertinggi.
* `edges` hanya boleh merujuk node yang ada di `nodes`.
* Tidak boleh ada self-loop (`from == to`).
* Tidak boleh ada duplicate pair; satu pasangan node hanya boleh muncul sekali, tanpa memandang urutan.
* `weight` harus `1..63`.
* `direction` harus `N`, `E`, `S`, atau `W`.
* Maksimal satu edge per arah mata angin per node.
* Graf harus connected dari `A` melalui edge undirected. Untuk graf satu node, `nodes = ["A"]` dan `edges = []` valid.
* Reverse edge dibuat otomatis: arah berlawanan dan bobot sama. Input tidak boleh memuat kedua arah untuk pasangan yang sama.

`POST /save` ditolak `409` jika masih ada request aktif atau antrean tidak kosong. Admin harus mengaktifkan maintenance, menunggu request aktif selesai, lalu mengosongkan antrean sebelum menyimpan.

| Kondisi | HTTP | Keterangan |
| :--- | :---: | :--- |
| Graf valid dan tersimpan | `200` | Versi baru dibuat; status `pending_reboot`. |
| Body/graf tidak valid | `400` | `invalid_graph`; tidak ada versi baru. |
| Bukan admin | `401`/`403` | Session admin tidak ada/tidak valid. |
| Masih ada request aktif atau antrean | `409` | `active_request` atau `queue_not_empty`; tidak ada versi baru. |

Respons sukses:

```json
{
  "version": 13,
  "node_count": 3,
  "edge_count": 2,
  "status": "pending_reboot"
}
```

`edge_count` adalah jumlah koneksi fisik (satu per pasangan node), bukan jumlah edge berarah hasil reverse. Penyimpanan dilakukan dalam satu transaksi: body matriks 676 byte, daftar node/edge, dan nomor versi baru ditulis ke riwayat, lalu pointer graf aktif diperbarui. Versi pertama adalah `1`; `0` dicadangkan untuk “graf belum siap”. Nomor versi selalu naik dan tidak pernah diturunkan.

Setelah save berhasil, seluruh panggilan baru ditolak `503 graph_update` sampai robot reboot dan melaporkan versi yang sama. Tidak ada refresh graf lewat MQTT. Graf baru berlaku setelah robot di-reboot di Home `A` menghadap Utara, lalu mengunduh `GET /graph.bin` saat boot.

### `GET /api/admin/graph/versions`

Mengembalikan metadata riwayat versi, terbaru lebih dulu:

```json
{
  "versions": [
    { "version": 13, "node_count": 3, "edge_count": 2, "created_at": "2026-10-25T10:00:00Z" },
    { "version": 12, "node_count": 4, "edge_count": 4, "created_at": "2026-10-20T09:00:00Z" }
  ]
}
```

### `POST /api/admin/graph/rollback`

Body:

```json
{ "version": 12 }
```

Gateway mengambil snapshot versi lama, membuat **versi baru** dengan isi graf yang sama, lalu menandainya `pending_reboot`. Nomor versi tidak pernah diturunkan. Endpoint ini memakai aturan `409` yang sama dengan `POST /save`: tidak boleh ada request aktif atau antrean.

### `POST /api/admin/maintenance`

Body:

```json
{ "enabled": true }
```

Saat maintenance aktif:

* `POST /api/call` ditolak `503 maintenance`.
* Dispatch request `queued` dihentikan; request aktif dibiarkan selesai.
* Admin dapat mengosongkan antrean dan menjalankan `POST /save`.
* Maintenance dinonaktifkan secara eksplisit setelah robot melaporkan versi graf yang cocok dan kembali `IDLE`.
* Flag maintenance disimpan di SQLite agar tidak hilang saat gateway restart.

## 6.3 Siklus Request & Antrean

Status request:

| Status | Arti |
| :--- | :--- |
| `queued` | Menunggu giliran; belum dikirim ke robot. |
| `dispatched` | `moveto` sudah dikirim; menunggu `ack`. |
| `serving` | `ack` diterima; robot sedang menuju atau melayani node target. |
| `done` | Robot menyelesaikan dwell dan kembali `IDLE` di node target. |
| `failed` | Error robot, timeout, offline, atau ack ditolak. |
| `cancelled` | Dibatalkan admin atau dibersihkan karena error/reset. |

Alur normal:

1. `POST /api/call` membuat request `queued`.
2. Gateway memilih request `queued` tertua hanya jika robot `online`, status `IDLE` masih fresh, tidak ada request aktif, maintenance nonaktif, **dan `graph_version` robot sama dengan versi graf aktif**.
3. Gateway mengubah status menjadi `dispatched` dan mengirim `srv/adam/moveto` berisi `req_id` + node.
4. Robot membalas `adam/srv/ack`.
   * `ACCEPTED` → request menjadi `serving`.
   * `BUSY` → request dikembalikan ke `queued` dan dicoba lagi setelah robot `IDLE`.
   * `NOT_READY` → jika robot `ERROR`, request menjadi `failed`; jika hanya belum siap, kembalikan ke `queued`.
   * `DUPLICATE` → anggap `serving` (ack sebelumnya kemungkinan hilang).
   * `QUEUE_FULL`/`INVALID_TARGET` → request menjadi `failed`.
5. Robot bergerak, lalu `DWELL` di target. Gateway tetap menandai `serving`.
6. Robot selesai dwell, mem-publish `IDLE` dan `current_pos = [target][0]`. Gateway menandai `done`.
7. Gateway baru boleh mengirim job berikutnya setelah menerima `IDLE` fresh dan versi graf robot cocok.

Aturan penting:

* **Satu request aktif.** Gateway tidak boleh mengirim `moveto` kedua selama masih ada request `dispatched`/`serving`.
* **Tidak ada antrean di robot.** Robot hanya punya mailbox 1 slot; seluruh backlog ada di gateway.
* **Graph version gate.** Gateway tidak boleh mengirim `moveto` jika `graph_version` robot `0` atau berbeda dari versi graf aktif. Panggilan baru ditolak `503 graph_update`.
* **Status gate.** Gateway tidak boleh mengirim `moveto` jika status terakhir bukan `IDLE` atau sudah lebih dari 10 detik tidak ada status fresh.
* **Ack timeout** default 10 detik. Jika `ack` tidak datang, gateway mencocokkan dengan status/posisi retained. Jika tetap tidak jelas, request menjadi `failed` dan dispatch dihentikan sampai robot kembali `IDLE`.
* **Job lease** default 10 menit. Jika robot offline atau tidak ada heartbeat melewati batas, request aktif menjadi `failed`.
* **Gateway restart**: request `queued` tetap ada di SQLite. Status retained hanya ditampilkan sebagai last-known dan belum dihitung fresh; gateway tidak mengirim job baru sampai menerima status fresh dan `IDLE`.

### Kesiapan Robot

Gateway hanya boleh melakukan dispatch jika seluruh kondisi berikut terpenuhi:

1. `online = 1` (retained + LWT).
2. Ada pesan `status` yang diterima dalam **10 detik** terakhir.
3. Status terbaru adalah `IDLE`.
4. `graph_version` robot sama dengan versi graf aktif dan bukan `0`.
5. Tidak ada request aktif.
6. Maintenance nonaktif.

`status` retained adalah **last known status**, bukan bukti bahwa robot siap sekarang. Setelah gateway restart, gateway menunggu status fresh berikutnya (startup grace maksimum 10 detik) sebelum dispatch. Jika tidak ada status selama 10 detik, `ready = false`, dispatch dihentikan, dan UI menampilkan status `stale`; `online` tidak diubah menjadi `0` hanya karena timeout status.

Robot mem-publish `status` setiap **3 detik** agar timeout 10 detik tidak terus-menerus memblokir dispatch.

### Kebijakan saat robot `ERROR`

1. Robot berhenti, membersihkan mailbox/path, mem-publish `error` dengan `req_id`, lalu `status = ERROR`.
2. Gateway menandai request aktif `failed`.
3. Gateway **membatalkan seluruh request `queued`** dan mengirim `call_result` ke UI. Ini mencegah panggilan lama dieksekusi setelah robot pulih.
4. Gateway berhenti melakukan dispatch sampai admin melakukan reset dan robot kembali `IDLE`.

Admin reset juga membatalkan request aktif/antrean sebelum mengirim `srv/adam/reset`. Queue clear hanya membatalkan request `queued`; request aktif dibiarkan sampai selesai atau di-reset. Reset tidak memuat ulang graf.

### Siklus Versi Graf

1. Admin mengaktifkan maintenance; panggilan baru ditolak, dispatch antrean dihentikan.
2. Admin menunggu request aktif selesai, lalu mengosongkan antrean. `POST /save` ditolak `409` selama masih ada request aktif atau antrean.
3. Admin menyimpan graf lewat `POST /save`; gateway menaikkan versi dan tetap menahan dispatch karena versi belum cocok.
4. Admin mengubah marka/lintasan fisik bila perlu, lalu mematikan robot, memindahkannya ke Home `A` menghadap Utara, dan menyalakannya kembali.
5. Saat boot, robot mengunduh `GET /graph.bin` (maksimal 3 percobaan) dan mem-publish `adam/srv/graph_version`.
6. Jika fetch gagal, robot mem-publish `graph_version = 0`, `status = ERROR`, dan tidak mencoba lagi sampai reboot. Jika berhasil, robot mem-publish versi sebenarnya dan `status = IDLE`.
7. Gateway membandingkan versi. Setelah versi cocok dan status fresh `IDLE`, admin menonaktifkan maintenance dan dispatch dilanjutkan.

Rollback memakai prosedur yang sama, tetapi graf baru dibuat dari snapshot riwayat lewat `POST /api/admin/graph/rollback`. Nomor versi tetap naik.

Tidak ada jalur update graf di tengah job. Jika robot reboot tak terduga ketika antrean berisi request, request tetap tersimpan tetapi dispatch dan panggilan baru ditahan sampai versi graf cocok kembali.

## 6.4 MQTT

### Transport & Sesi

* MQTT 3.1.1, QoS 1 untuk seluruh perintah dan telemetri.
* **Clean session = true.** Subscription dipasang ulang setiap reconnect; tidak ada antrean perintah lama di broker. Karena itu `reset` yang dikirim saat robot offline tidak akan tersimpan; gateway hanya mengirim saat robot online, dan admin dapat mengulang setelah robot kembali.
* Keepalive 30 detik.
* Robot terhubung lewat **WSS port 443** (`wss://adam.example.com/mqtt`) melalui Cloudflare Tunnel → Caddy → Mosquitto WebSocket listener di localhost.
* **Browser tidak pernah membuka koneksi MQTT.** Jalur WSS hanya untuk robot; kredensial MQTT tidak boleh dikirim ke frontend. Browser hanya memakai HTTP/SSE ke gateway.
* Gateway terhubung ke Mosquitto lokal (`mqtt://127.0.0.1:1883`).
* Opsi fallback MQTT langsung: `mqtts://vps.example.com:8883` dengan port mapping dan sertifikat DNS-01 (lihat [docs/10-deployment.md](10-deployment.md)).
* Tidak ada anonymous access. ACL memisahkan hak gateway dan robot.
* Last Will and Testament: `adam/srv/online` payload `0x00`, retain `true`. Saat connect, robot mem-publish `0x01`. Sebelum disconnect normal, robot wajib mem-publish `0x00` sendiri karena broker tidak mem-publish LWT pada clean disconnect.

### Prinsip Wire Format

* Payload biner mentah; tidak ada JSON di MQTT.
* Node memakai ASCII huruf kapital (`'A'`–`'Z'`); satu graf maksimal 26 node.
* `req_id` adalah **2 byte little-endian**. Nilai `0x0000` dicadangkan untuk pesan global/tanpa request.
* `req_id` diturunkan dari ID request persisten di SQLite (16 bit rendah). Jika hasilnya `0x0000`, gunakan ID berikutnya. Contoh: `request_id = 42` → wire `2A 00`.
* Deduplikasi robot memakai pasangan `(req_id, node)`, sehingga pengiriman ulang QoS 1 untuk perintah yang sama tidak dieksekusi dua kali.
* Payload perintah maksimum 3 byte; payload telemetri maksimum 4 byte (`graph_version`).
* Semua kode bersifat append-only; penerima mengabaikan kode yang tidak dikenal.

### Tabel Topik

| Topik | Arah | Payload | QoS | Retain | Deskripsi |
| :--- | :--- | :--- | :---: | :---: | :--- |
| `srv/adam/moveto` | Server → Robot | `[req_id:2][node:1]` | 1 | False | Perintah menuju satu node. Hanya dikirim saat robot `IDLE`, status fresh, dan versi graf cocok. |
| `srv/adam/reset` | Server → Robot | *(kosong)* | 1 | False | Admin reset: batalkan job, kembali ke Home `A`, menghadap Utara. Tidak memuat ulang graf. |
| `adam/srv/ack` | Robot → Server | `[req_id:2][result:1]` | 1 | False | Konfirmasi penerimaan perintah. |
| `adam/srv/error` | Robot → Server | `[req_id:2][code:1]` | 1 | False | Error yang terkait request; `req_id = 0` untuk error global. |
| `adam/srv/current_pos` | Robot → Server | `[current:1][next:1]` | 1 | **True** | Node terakhir dan node yang sedang dituju. `next = 0` jika tidak ada. |
| `adam/srv/status` | Robot → Server | 1 byte kode status | 1 | **True** | Status operasional robot; dipublish setiap 3 detik. |
| `adam/srv/graph_version` | Robot → Server | `[version:4]` little-endian | 1 | **True** | Versi graf yang aktif di robot; `0` = graf belum siap. |
| `adam/srv/battery` | Robot → Server | 1 byte (`0`–`100`) | 1 | **True** | Persentase baterai. |
| `adam/srv/trash` | Robot → Server | 1 byte (`0`–`100`) | 1 | **True** | Persentase kapasitas sampah terisi. |
| `adam/srv/online` | Robot → Server | `0x00`/`0x01` | 1 | **True** | Online/offline; `0x00` juga dikirim sebagai LWT. |

`current_pos`, `battery`, dan `trash` dikirim saat berubah, ditambah snapshot periodik 30–60 detik. `status` dikirim setiap 3 detik. `graph_version` dikirim saat boot/perubahan. Semua state menggunakan retain agar subscriber baru langsung mendapat state terakhir.

`status` retained hanya untuk tampilan last-known. Gateway tetap mewajibkan status fresh (maksimal 10 detik) sebelum dispatch, dan tidak menganggap retained status sebagai bukti kesiapan.

Saat boot, robot mem-publish `graph_version = 0` **sebelum** `online = 1`, lalu mengunduh graf. Setelah graf siap, robot mem-publish versi sebenarnya. Jika fetch gagal setelah 3 percobaan, robot mem-publish `status = ERROR` dan tetap `graph_version = 0`; tidak ada retry otomatis sampai reboot. Dengan urutan ini gateway tidak pernah melihat state lama yang membuat robot dianggap siap padahal graf belum dimuat.

### Kode `adam/srv/ack`

| Kode | Nama | Arti |
| :--- | :--- | :--- |
| `0x00` | `ACCEPTED` | Job diterima ke mailbox robot. |
| `0x01` | `BUSY` | Robot tidak `IDLE` atau mailbox masih terisi. |
| `0x02` | `QUEUE_FULL` | Pengaman internal; normalnya tidak terjadi karena gateway hanya mengirim saat `IDLE`. |
| `0x03` | `DUPLICATE` | Pasangan `(req_id, node)` yang sama sudah pernah diterima. |
| `0x04` | `INVALID_TARGET` | Byte node bukan `A`–`Z` atau payload tidak valid. Robot tidak memeriksa membership graf; itu tanggung jawab gateway. |
| `0x05` | `NOT_READY` | Robot sedang boot, graf belum siap, atau status `ERROR`. |

`BUSY` dan `NOT_READY` adalah pengaman. Gateway seharusnya sudah mencegah keduanya dengan hanya mengirim saat robot `online`, status fresh `IDLE`, dan `graph_version` cocok.

### Kode `adam/srv/status`

| Kode | Nama | Arti |
| :--- | :--- | :--- |
| `0x00` | `IDLE` | Menunggu job; mailbox kosong. Job hanya diterima jika graf sudah siap. |
| `0x01` | `MOVING` | Menyusuri garis antar node. |
| `0x02` | `DWELL` | Berhenti di node tujuan (dwell, lihat [docs/04-dwell.md](04-dwell.md)). |
| `0x03` | `ERROR` | Berhenti, menunggu admin reset. |
| `0x04` | `OBSTACLE` | Dicadangkan; tidak pernah dipublish. |

### Kode `adam/srv/error`

| Kode | Nama | Arti |
| :--- | :--- | :--- |
| `0x00` | `NONE` | Tidak ada error; tidak dipublish. |
| `0x01` | `NO_PATH` | Tidak ada rute valid ke target. |
| `0x02` | `LINE_LOST` | Sensor garis kehilangan lintasan melewati batas. |
| `0x03` | `NODE_TIMEOUT` | Tahap menuju node berikutnya tidak selesai dalam batas waktu. |
| `0x04` | `INVALID_TARGET` | Target di luar graf atau tidak dapat diproses. |

### Contoh Byte

```text
srv/adam/moveto        payload 34 12 43       -> req_id 0x1234, target node 'C'
adam/srv/ack           payload 34 12 00       -> req_id 0x1234, ACCEPTED
adam/srv/ack           payload 34 12 01       -> req_id 0x1234, BUSY
adam/srv/error         payload 34 12 01       -> req_id 0x1234, NO_PATH
adam/srv/current_pos   payload 42 43          -> di 'B', menuju 'C'
adam/srv/current_pos   payload 42 00          -> di 'B', tidak menuju node lain
adam/srv/status        payload 01             -> MOVING
adam/srv/graph_version payload 07 00 00 00    -> versi graf 7
adam/srv/battery       payload 57             -> 87%
adam/srv/trash         payload 23             -> kapasitas sampah 35%
adam/srv/online        payload 01             -> online
srv/adam/reset         payload (kosong)
```

### ACL

| User | Publish | Subscribe |
| :--- | :--- | :--- |
| `gateway` | `srv/adam/#` | `adam/srv/#` |
| `robot` | `adam/srv/#` | `srv/adam/#` |

Tidak ada user anonymous. Password MQTT disimpan di secret gateway dan NVS robot.

### Referensi Implementasi Target

Firmware: `src/graph.c` (matriks tetap 26 × 26, dimuat sekali saat boot), `src/mqtt.h` (enum status/error/ack, topik), `src/mqtt.c` (koneksi WSS, clean session, ack, LWT), `src/executor.c` (status, error, current/next), `src/path_resolver.c` (Dijkstra saat job dieksekusi). Gateway: decoder `req_id`/payload, antrean SQLite, version gate graf, bridge SSE.

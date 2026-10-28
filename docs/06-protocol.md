# 6. Protokol Komunikasi & API Endpoints

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Dokumen ini mendefinisikan protokol target: **Gateway Node** memegang antrean, robot hanya menerima satu job pada satu waktu, seluruh korelasi perintah memakai `req_id`, dan graf hanya diaktifkan saat boot.

## 6.1 Prinsip Protokol

* Gateway adalah pemilik antrean dan satu-satunya pihak yang boleh mengirim `moveto`.
* Robot memiliki **mailbox 1 slot** dan hanya menerima job saat status `IDLE`, graf sudah siap, **dan kapasitas sampah di bawah ambang penuh (95%)**.
* Semua perintah dan telemetri MQTT memakai **payload biner**, bukan JSON.
* Telemetri bersifat **event-driven**: `status`, `current_pos`, dan `trash` hanya dipublish saat kejadian; `battery` periodik tiap 30 detik; `online` dipublish tiap 10 detik sebagai **heartbeat liveness**. Karena `status` tidak lagi periodik, kesegaran dispatch ditentukan oleh heartbeat.
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
| **`DELETE /api/request/:id`** | DELETE | Publik (dengan `request_id`) | Membatalkan panggilan sendiri selama masih `queued`. |
| **`GET /cmd/:nodeId`** | GET | Publik | Halaman quick-call untuk QR Code di node fisik. |
| **`GET /events`** | GET | Publik | Stream SSE untuk snapshot dan pembaruan *real-time*. |
| **`POST /api/auth/login`** | POST | Publik | Login admin, mengembalikan session cookie. |
| **`POST /api/auth/logout`** | POST | Admin | Menghapus session aktif. |
| **`GET /api/auth/me`** | GET | Admin | Informasi admin yang sedang login. |
| **`POST /api/auth/password`** | POST | Admin | Mengganti password admin yang sedang login. |
| **`POST /api/admin/reset`** | POST | Admin | Reset robot: bersihkan state, unduh ulang graf, paksa posisi Home `A` menghadap Utara, publish ulang seluruh telemetri. Hanya diterima saat robot tidak melayani (`IDLE`/`ERROR`). |
| **`GET /api/admin/queue`** | GET | Admin | Melihat seluruh request aktif dan antrean. |
| **`POST /api/admin/queue/clear`** | POST | Admin | Membatalkan request `queued`; request aktif tidak dihentikan. |
| **`GET /api/admin/logs`** | GET | Admin | Riwayat kejadian sistem dan request. |
| **`GET /healthz`** | GET | Publik (read-only) | Status gateway, SQLite, MQTT, online/offline robot, kesegaran heartbeat, maintenance, dan kecocokan versi graf. |

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
5. Cek kapasitas sampah terakhir yang diketahui (retained `adam/srv/trash` < 95%).
6. Deduplikasi target.
7. Rate limit.
8. Kapasitas antrean.

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
| Tempat sampah penuh (≥ 95%) | `503` | `bin_full` | Tidak ada request dibuat; robot menunggu di Home. Kosongkan wadah di Home lalu minta admin mereset agar telemetri `trash` segar. |
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

### `DELETE /api/request/:id`

Membatalkan panggilan milik pemanggil. Tidak perlu session; endpoint ini dipublikasikan untuk siapa pun yang mengetahui `request_id`-nya (sejalan dengan deduplikasi tanpa auth di `POST /api/call`; `request_id` dikembalikan saat memanggil dan lewat `queue_update`).

| Kondisi | HTTP | `status` | Keterangan |
| :--- | :---: | :--- | :--- |
| Request `queued` | `200` | `cancelled` | Berhasil dibatalkan; `queue_update` + `call_result` dikirim. |
| Request `dispatched`/`serving` | `409` | `not_cancellable` | `moveto` sudah dikirim; robot tidak punya perintah berhenti — gunakan admin reset bila perlu. |
| Request sudah `done`/`failed`/`cancelled` | `409` | `not_cancellable` | Sudah berstatus final. |
| `request_id` tidak dikenal | `404` | `not_found` | Tidak ada request dengan ID itu. |

Respons sukses: `{ "request_id": 42, "status": "cancelled" }`. Setelah pembatalan, antrean lanjut dengan request `queued` berikutnya sesuai gate normal.

Deduplikasi “robot sudah di node” berlaku jika robot `IDLE` di node tersebut tanpa request aktif, atau jika robot `DWELL` di sana (yang berarti request `serving` untuk node itu sudah ada), **dan** versi graf robot sudah sama dengan versi graf gateway. Jika robot sedang `MOVING`, `current_pos` hanyalah node terakhir yang dikonfirmasi; robot mungkin sudah meninggalkannya, sehingga panggilan ke node itu tetap boleh masuk antrean.

Jika versi graf robot belum cocok, request baru **ditolak** dengan `503 graph_update` dan tidak masuk antrean. UI menampilkan status maintenance sampai robot reboot dan melaporkan versi yang sama. Request `queued` yang sudah ada sebelum reboot tak terduga tetap tersimpan, tetapi tidak akan didispatch sampai versi cocok kembali. Pada deployment graf, antrean sudah kosong karena `POST /save` mensyaratkan itu.

Semua pemanggil ke node yang sama memakai `request_id` yang sama. Hasil akhir dikirim lewat SSE `call_result`.

### `POST /api/admin/reset`

Payload HTTP tidak diperlukan; gateway meneruskannya sebagai topik `srv/adam/reset` (payload MQTT dikosongkan dan diabaikan robot). Semantik lengkap reset ada di [docs/07-firmware.md](07-firmware.md) §7.3.2; sisi gateway:

| Kondisi | HTTP | `status` | Keterangan |
| :--- | :---: | :--- | :--- |
| Robot online dan tidak melayani (`IDLE`/`ERROR`) | `202` | `resetting` | Reset dikirim; lihat konfirmasi di bawah. |
| Robot `MOVING`/`DWELL` | `409` | `robot_busy` | Tunggu robot selesai melayani; reset tidak dikirim. |
| Robot offline | `503` | `robot_offline` | Kirim ulang setelah robot online; reset tidak disimpan broker (clean session). |

* Robot tidak mem-publish `ack`/`error` apa pun untuk reset. **Konfirmasi keberhasilan** adalah telemetry retained berubah menjadi `status = IDLE` dengan `current_pos = [A][0]` dan `graph_version` sama dengan versi graf aktif. UI admin menampilkan status `resetting` sampai konfirmasi tiba atau timeout.
* Reset mem-publish ulang **seluruh** telemetri retained (`graph_version`, `current_pos`, `status`, `battery`, `trash`). Karena itu reset juga adalah cara menyetel ulang state yang basi — misalnya `trash` setelah wadah dikosongkan di Home.
* `reset` tidak pernah ditolak karena kapasitas sampah penuh, agar jalur recovery admin selalu terbuka.

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

`POST /save` ditolak `409` jika **maintenance nonaktif**, masih ada request aktif, atau antrean tidak kosong. Maintenance aktif adalah prasyarat: admin mengaktifkan maintenance, menunggu request aktif selesai, lalu mengosongkan antrean sebelum menyimpan.

| Kondisi | HTTP | Keterangan |
| :--- | :---: | :--- |
| Graf valid dan tersimpan | `200` | Versi baru dibuat; status `pending_reboot`. |
| Body/graf tidak valid | `400` | `invalid_graph`; tidak ada versi baru. |
| Bukan admin | `401`/`403` | Session admin tidak ada/tidak valid. |
| Maintenance nonaktif | `409` | `maintenance`; tidak ada versi baru. |
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

Setelah save berhasil, seluruh panggilan baru ditolak `503 graph_update` sampai robot melaporkan versi yang sama. Tidak ada refresh graf lewat MQTT. Graf baru berlaku setelah robot di-reboot di Home `A` menghadap Utara (mengunduh `GET /graph.bin` saat boot) **atau di-reset** — reset mengunduh `GET /graph.bin` dengan cara dan syarat yang sama: robot berada di `A` menghadap Utara.

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
* Admin dapat mengosongkan antrean dan menjalankan `POST /save` (maintenance adalah prasyarat `POST /save`).
* Maintenance dinonaktifkan secara eksplisit setelah robot melaporkan versi graf yang cocok dan kembali `IDLE`.
* Flag maintenance disimpan di SQLite agar tidak hilang saat gateway restart.

Respons sukses:

```json
{ "maintenance": true }
```

### `GET /api/admin/queue`

Mengembalikan seluruh request aktif dan antrean (urutan dispatch):

```json
{
  "requests": [
    { "request_id": 43, "node": "B", "status": "queued", "queue_position": 2, "created_at": "2026-10-28T10:05:00Z" },
    { "request_id": 42, "node": "C", "status": "serving", "queue_position": null, "created_at": "2026-10-28T10:00:00Z", "dispatched_at": "2026-10-28T10:01:00Z", "req_id": 42 }
  ]
}
```

`queue_position` hanya diisi untuk `queued`; `req_id` adalah nilai wire 16 bit yang dikirim ke robot (berguna melacak `ack`/`error`).

### `POST /api/admin/queue/clear`

Membatalkan seluruh request `queued` (request aktif dibiarkan selesai). Respons: `{ "cancelled": 3 }` — jumlah request yang dibatalkan.

### `GET /api/admin/logs`

Riwayat kejadian sistem dan request, terbaru lebih dulu:

```json
{
  "logs": [
    { "ts": "2026-10-28T10:01:00Z", "level": "info", "event": "request.dispatched", "request_id": 42, "req_id": 42, "message": "moveto terkirim ke robot" },
    { "ts": "2026-10-28T10:04:30Z", "level": "error", "event": "robot.error", "request_id": 42, "req_id": 42, "code": "LINE_LOST", "message": "robot berhenti di tengah edge" }
  ]
}
```

Setiap entri wajib memuat `ts`, `level`, `event`, dan `message`; `request_id`/`req_id` diisi bila terkait satu panggilan (lihat [docs/10-deployment.md](10-deployment.md) §10.10).

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
2. Gateway memilih request `queued` tertua hanya jika robot `online` dan heartbeat fresh (≤ 30 detik), status retained `IDLE`, kapasitas sampah < 95%, tidak ada request aktif, maintenance nonaktif, **dan `graph_version` robot sama dengan versi graf aktif**.
3. Gateway mengubah status menjadi `dispatched` dan mengirim `srv/adam/moveto` berisi `req_id` + node.
4. Robot membalas `adam/srv/ack`.
   * `ACCEPTED` → request menjadi `serving`.
   * `BUSY` → request dikembalikan ke `queued` dan dicoba lagi setelah robot `IDLE`.
   * `NOT_READY` → jika robot `ERROR`, request menjadi `failed`; jika hanya belum siap, kembalikan ke `queued`.
   * `DUPLICATE` → anggap `serving` (ack sebelumnya kemungkinan hilang).
   * `QUEUE_FULL`/`INVALID_TARGET` → request menjadi `failed`.
   * `TRASH_FULL` → request menjadi `failed`; kapasitas sampah harus dikurangi dulu sebelum panggilan baru bisa diterima robot.
5. Robot bergerak, lalu `DWELL` di target. Gateway tetap menandai `serving`.
6. Robot selesai dwell, mem-publish `IDLE` dan `current_pos = [target][0]`. Gateway menandai `done`.
7. Gateway baru boleh mengirim job berikutnya setelah heartbeat fresh, status `IDLE`, dan versi graf robot cocok.

Aturan penting:

* **Satu request aktif.** Gateway tidak boleh mengirim `moveto` kedua selama masih ada request `dispatched`/`serving`.
* **Tidak ada antrean di robot.** Robot hanya punya mailbox 1 slot; seluruh backlog ada di gateway.
* **Graph version gate.** Gateway tidak boleh mengirim `moveto` jika `graph_version` robot `0` atau berbeda dari versi graf aktif. Panggilan baru ditolak `503 graph_update`.
* **Heartbeat gate.** Gateway tidak boleh mengirim `moveto` jika heartbeat `online` terakhir lebih dari 30 detik, atau status retained bukan `IDLE`.
* **Trash gate.** Gateway tidak boleh mengirim `moveto` jika retained `adam/srv/trash` ≥ 95%; panggilan baru ditolak `503 bin_full` sampai kapasitas turun.
* **Ack timeout** default 10 detik. Jika `ack` tidak datang, gateway **merekonsiliasi** request dengan retained telemetry memakai matriks di bawah. Jika hasilnya belum jelas, request tetap `dispatched` sampai **job deadline** habis lalu `failed`.
* **Job deadline** default **3 menit**, dihitung sejak `moveto` dikirim sampai robot **tiba di target** (masuk `DWELL` atau `IDLE` di target). Lewat batas, request menjadi `failed` dan `call_result` dikirim **walau heartbeat fresh** — robot hidup tapi tersangkut tidak mengunci antrean. Waktu setelah tiba dibatasi terpisah oleh batas dwell total firmware (60 detik, [docs/04-dwell.md](04-dwell.md)), jadi keduanya tidak tumpang tindih.
* **Job lease** default 10 menit, berlaku untuk kasus **robot offline / heartbeat hilang**: request aktif menjadi `failed` setelah lease. Untuk robot online, job deadline (3 menit) lebih dulu yang menutup request.
* **Gateway restart**: request `queued` tetap ada di SQLite. Status retained hanya ditampilkan sebagai last-known; gateway tidak mengirim job baru sampai heartbeat fresh diterima dan status retained `IDLE`. Request `dispatched`/`serving` yang kehilangan `ack`/`error` (non-retain, terbit saat gateway down) **direkonsilikan ulang** dari retained telemetry memakai matriks di bawah; timeout ack dihitung setelah restart, bukan di-reset ke nol.
* **Error lintas status.** Gateway memproses `error` dengan `req_id` apa pun status request saat itu, termasuk masih `dispatched` — `ack` dan `error` dikirim ke topik berbeda, sehingga urutan kedatangannya tidak dijamin (QoS 1 hanya mengurutkan per topik).

### Rekonsiliasi Hasil vs Status Request

Saat **ack timeout**, **gateway reconnect/setelah restart**, dan **evaluasi lease**, gateway menurunkan keputusan request dari retained telemetry robot:

| Retained telemetry robot | Keputusan |
| :--- | :--- |
| `status = MOVING` dan `current_pos.next == target` | `serving` — robot sedang menuju target; tunggu. |
| `status = DWELL` dan `current_pos.current == target` | `serving` — robot tiba dan sedang melayani. |
| `status = IDLE` dan `current_pos.current == target` | `done` — robot selesai di target (hasil nyata menang, walau status di SQLite masih `failed`/`dispatched`). |
| `status = IDLE` dan posisi tidak berubah sejak dispatch (robot diam, umumnya di Home `[A][0]`) | `failed` — robot tidak pernah bergerak. |
| `status = ERROR` | `failed` (ikuti kebijakan robot `ERROR`). |
| Selain itu (belum jelas) | Tetap `dispatched`; setelah job deadline habis → `failed`. |

`call_result` dikirim pada setiap transisi final (`done`/`failed`/`cancelled`), sehingga UI selalu mencerminkan keputusan rekonsiliasi terakhir.

### Kesiapan Robot

Gateway hanya boleh melakukan dispatch jika seluruh kondisi berikut terpenuhi:

1. `online = 1` (retained + LWT).
2. Ada pesan heartbeat `adam/srv/online` (`0x01`) yang diterima dalam **30 detik** terakhir.
3. Status retained terbaru adalah `IDLE`.
4. `graph_version` robot sama dengan versi graf aktif dan bukan `0`.
5. Tidak ada request aktif.
6. Maintenance nonaktif.
7. Retained `adam/srv/trash` di bawah **95%**.

`status` retained adalah **last known status**, bukan bukti bahwa robot siap sekarang; bukti kesiapan adalah heartbeat. Setelah gateway restart, gateway menunggu heartbeat pertama (startup grace maksimum 30 detik) sebelum dispatch. Jika tidak ada heartbeat selama 30 detik, `ready = false`, dispatch dihentikan, dan UI menampilkan status `stale`; `online` tidak diubah menjadi `0` hanya karena timeout heartbeat.

Robot mem-publish `status` **hanya saat berubah**; kedatangan pesan `status` tidak lagi menjadi penanda kesegaran. Kesegaran dispatch ditentukan oleh heartbeat `online` setiap **10 detik**.

### Kebijakan saat robot `ERROR`

1. Robot berhenti, membersihkan mailbox/path, mem-publish `error` dengan `req_id`, lalu `status = ERROR`.
2. Gateway menandai request aktif `failed`.
3. Gateway **membatalkan seluruh request `queued`** dan mengirim `call_result` ke UI. Ini mencegah panggilan lama dieksekusi setelah robot pulih.
4. Gateway berhenti melakukan dispatch sampai admin melakukan reset dan robot kembali `IDLE`.

Karena seluruh request sudah `failed`/`cancelled` ketika robot masuk `ERROR`, admin reset tidak perlu membatalkan apa pun lagi: cukup kirim `srv/adam/reset` (gateway menolak dengan `409 robot_busy` bila robot masih `MOVING`/`DWELL`). Queue clear hanya membatalkan request `queued`; request aktif dibiarkan sampai selesai. Reset **memuat ulang** graf lewat `GET /graph.bin` — satu-satunya cara mengganti graf tanpa mematikan daya (lihat [docs/07-firmware.md](07-firmware.md) §7.3.2).

### Siklus Versi Graf

1. Admin mengaktifkan maintenance; panggilan baru ditolak, dispatch antrean dihentikan.
2. Admin menunggu request aktif selesai, lalu mengosongkan antrean. `POST /save` ditolak `409` selama masih ada request aktif atau antrean.
3. Admin menyimpan graf lewat `POST /save`; gateway menaikkan versi dan tetap menahan dispatch karena versi belum cocok.
4. Admin mengubah marka/lintasan fisik bila perlu, lalu mematikan robot, memindahkannya ke Home `A` menghadap Utara, dan menyalakannya kembali — atau, bila robot sudah berada di `A` menghadap Utara dan perubahannya murni software, mengirim `POST /api/admin/reset` (reset mengunduh graf terbaru tanpa mematikan daya; untuk perubahan marka/lintasan fisik, power cycle tetap disarankan).
5. Saat boot, robot mengunduh `GET /graph.bin` (maksimal 3 percobaan) dan mem-publish `adam/srv/graph_version`.
6. Jika fetch gagal, robot mem-publish `adam/srv/error` `FETCH_FAILED` (`req_id = 0`), `graph_version = 0`, `status = ERROR`, dan tidak mencoba lagi otomatis (reset atau reboot mengulang fetch). Jika berhasil, robot mem-publish versi sebenarnya dan `status = IDLE`.
7. Gateway membandingkan versi. Setelah versi cocok, heartbeat fresh, dan status `IDLE`, admin menonaktifkan maintenance dan dispatch dilanjutkan.

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
* Last Will and Testament: `adam/srv/online` payload `0x00`, retain `true`. Saat connect, robot mem-publish `0x01`, lalu mengulangnya **tiap 10 detik** sebagai heartbeat. Sebelum disconnect normal, robot wajib mem-publish `0x00` sendiri karena broker tidak mem-publish LWT pada clean disconnect. Gateway menganggap robot fresh jika heartbeat terakhir ≤ 30 detik.

### Prinsip Wire Format

* Payload biner mentah; tidak ada JSON di MQTT.
* Node memakai ASCII huruf kapital (`'A'`–`'Z'`); satu graf maksimal 26 node.
* `req_id` adalah **2 byte little-endian**. Nilai `0x0000` dicadangkan untuk pesan global/tanpa request.
* `req_id` diturunkan dari ID request persisten di SQLite (16 bit rendah). Jika hasilnya `0x0000`, gunakan ID berikutnya. Contoh: `request_id = 42` → wire `2A 00`.
* Deduplikasi robot memakai pasangan `(req_id, node)`, sehingga pengiriman ulang QoS 1 untuk perintah yang sama tidak dieksekusi dua kali. Cache berukuran tetap (buffer ring, 8 entri) dan **hanya menyimpan pasangan yang di-`ack ACCEPTED`** — perintah yang ditolak (`BUSY`, `TRASH_FULL`, dll.) tidak masuk cache sehingga retry setelah kondisi membaik tetap diproses. Cache dibersihkan saat connect dan saat admin reset.
* Payload perintah maksimum 3 byte; payload telemetri maksimum 4 byte (`graph_version`).
* Semua kode bersifat append-only; penerima mengabaikan kode yang tidak dikenal.

### Tabel Topik

| Topik | Arah | Payload | QoS | Retain | Deskripsi |
| :--- | :--- | :--- | :---: | :---: | :--- |
| `srv/adam/moveto` | Server → Robot | `[req_id:2][node:1]` | 1 | False | Perintah menuju satu node. Hanya dikirim saat robot `IDLE`, heartbeat fresh, kapasitas < 95%, dan versi graf cocok. |
| `srv/adam/reset` | Server → Robot | *(kosong)* | 1 | False | Admin reset: bersihkan state, unduh ulang graf, paksa posisi `[A][0]` menghadap Utara, publish ulang seluruh telemetri. Diterima hanya saat robot `IDLE`/`ERROR`; saat `MOVING`/`DWELL` diabaikan. |
| `adam/srv/ack` | Robot → Server | `[req_id:2][result:1]` | 1 | False | Konfirmasi penerimaan perintah. |
| `adam/srv/error` | Robot → Server | `[req_id:2][code:1]` | 1 | False | Error yang terkait request; `req_id = 0` untuk error global. |
| `adam/srv/current_pos` | Robot → Server | `[current:1][next:1]` | 1 | **True** | Node terakhir dan node yang sedang dituju. `next = 0` jika tidak ada. |
| `adam/srv/status` | Robot → Server | 1 byte kode status | 1 | **True** | Status operasional robot; dipublish saat berubah (event-driven). |
| `adam/srv/graph_version` | Robot → Server | `[version:4]` little-endian | 1 | **True** | Versi graf yang aktif di robot; `0` = graf belum siap. |
| `adam/srv/battery` | Robot → Server | 1 byte (`0`–`100`) | 1 | **True** | Persentase baterai; dipublish periodik tiap 30 detik. |
| `adam/srv/trash` | Robot → Server | 1 byte (`0`–`100`) | 1 | **True** | Persentase kapasitas sampah terisi; dipublish saat boot dan setelah penutup ditutup. Nilai ≥ 95% membuat `POST /api/call` ditolak `503 bin_full` dan `moveto` dibalas `ack TRASH_FULL`. |
| `adam/srv/online` | Robot → Server | `0x00`/`0x01` | 1 | **True** | Online/offline sekaligus heartbeat liveness: `0x01` dipublish tiap 10 detik; `0x00` dikirim sebagai LWT dan sebelum disconnect normal. |

Kadensi bersifat **event-driven**: `status` dikirim saat berubah; `current_pos` saat mulai edge, tiba di node, `DWELL`, `IDLE`, dan `ERROR`; `trash` saat boot dan setelah penutup ditutup; `battery` periodik tiap 30 detik; `graph_version` saat boot/perubahan; `online` sebagai heartbeat tiap 10 detik. Tidak ada snapshot periodik untuk `current_pos` dan `trash`. Semua state menggunakan retain agar subscriber baru langsung mendapat state terakhir. Satu pengecualian publish-ulang: setelah `srv/adam/reset` berhasil, **seluruh** telemetri retained (`graph_version`, `current_pos`, `status`, `battery`, `trash`) dipublish ulang meski nilainya tidak berubah — reset adalah cara menyetel ulang state yang basi.

`status` retained hanya untuk tampilan last-known. Gateway tetap mewajibkan heartbeat fresh (maksimal 30 detik) sebelum dispatch, dan tidak menganggap retained status sebagai bukti kesiapan.

Saat boot, robot mem-publish **state netral sebelum** `online = 1`: `graph_version = 0`, `current_pos = [A][0]`, dan `status = IDLE` (retain) — retained sesi sebelumnya (mis. `MOVING` + `[B][C]`) tidak pernah terlihat saat boot. Setelah itu `online = 1`, lalu robot mengunduh graf. Jika fetch sukses, robot mem-publish versi sebenarnya plus `battery`/`trash`. Jika fetch gagal setelah 3 percobaan, robot mem-publish `adam/srv/error` `FETCH_FAILED` (`req_id = 0`) dan `status = ERROR` dan tetap `graph_version = 0`; tidak ada retry otomatis — reset atau reboot mengulang fetch. `graph_version = 0` menahan dispatch selama window boot, sehingga gateway tidak pernah menganggap robot siap padahal graf belum dimuat.

### Kode `adam/srv/ack`

| Kode | Nama | Arti |
| :--- | :--- | :--- |
| `0x00` | `ACCEPTED` | Job diterima ke mailbox robot. |
| `0x01` | `BUSY` | Robot tidak `IDLE` atau mailbox masih terisi. |
| `0x02` | `QUEUE_FULL` | Pengaman internal; normalnya tidak terjadi karena gateway hanya mengirim saat `IDLE`. |
| `0x03` | `DUPLICATE` | Pasangan `(req_id, node)` yang sama sudah pernah diterima. |
| `0x04` | `INVALID_TARGET` | Byte node bukan `A`–`Z` atau payload tidak valid. Robot tidak memeriksa membership graf; itu tanggung jawab gateway. |
| `0x05` | `NOT_READY` | Robot sedang boot, graf belum siap, atau status `ERROR`. |
| `0x06` | `TRASH_FULL` | Tempat sampah penuh (kapasitas ≥ `SENSORS_TRASH_FULL_PERCENT`, default 95%). Robot tidak menerima job pindah baru sampai kapasitas turun di bawah ambang. |

`BUSY` dan `NOT_READY` adalah pengaman. Gateway seharusnya sudah mencegah keduanya dengan hanya mengirim saat robot `online`, heartbeat fresh, status retained `IDLE`, `graph_version` cocok, dan kapasitas di bawah ambang.

`TRASH_FULL` adalah kondisi pengaman terakhir di sisi robot, bukan satu-satunya penghalang: gateway sudah wajib menahan dispatch dan menolak panggilan selama retained `adam/srv/trash` ≥ 95% (lihat **Trash gate**), sehingga `TRASH_FULL` seharusnya hanya muncul saat nilai retained belum ter-update atau pembacaan robot berbeda dengan cache gateway. `srv/adam/reset` tidak pernah ditolak karena kapasitas penuh, agar jalur recovery admin selalu terbuka.

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
| `0x05` | `FETCH_FAILED` | Unduhan `GET /graph.bin` gagal setelah 3 percobaan (saat boot atau reset); `graph_version` tetap `0`. |

`NO_PATH` dan `INVALID_TARGET` adalah kode **defensif** yang praktis tidak terbit pada operasi normal: gateway sudah memvalidasi node terhadap graf sebelum mengirim `moveto` (robot sendiri tidak memvalidasi membership), dan graf selalu connected dari `A`. `NO_PATH` hanya mungkin bila posisi logika robot tidak sinkan dengan lapangan (mis. berhenti di tengah edge lalu di-reset tanpa diangkut). Keduanya tetap dipertahankan sebagai pengaman; penerima wajib mengabaikan kode yang tidak dikenal (append-only).

### Contoh Byte

```text
srv/adam/moveto        payload 34 12 43       -> req_id 0x1234, target node 'C'
adam/srv/ack           payload 34 12 00       -> req_id 0x1234, ACCEPTED
adam/srv/ack           payload 34 12 01       -> req_id 0x1234, BUSY
adam/srv/ack           payload 34 12 06       -> req_id 0x1234, TRASH_FULL
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

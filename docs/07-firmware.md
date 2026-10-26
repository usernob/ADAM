# 7. Arsitektur Firmware (FreeRTOS) & Penanganan Error

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Firmware dijalankan menggunakan Sistem Operasi Real-Time (**FreeRTOS**) pada mikrokontroler multi-core (misal ESP32). Robot adalah **eksekutor** dengan satu mailbox job; antrean target sepenuhnya dimiliki gateway.

```text
[ Gateway Queue ] --moveto(req_id,node)--> [ Task MQTT ]
                                                |
                                                | mailbox 1 slot
                                                v
                                       [ Task Route Finder ]
                                                |
                                                | path array
                                                v
                                        [ Task Executor ] --current_pos/status/ack/error--> [ MQTT ]
                                                |
                                                v
                                          [ RAM Graph ]

[ Task Fetch ] --GET /graph.bin (boot only)--> [ Gateway ]
```

## 7.1 Alokasi Task & Prioritas

| Task Name | Prioritas | Core Allocation | Fungsi |
| :--- | :--- | :--- | :--- |
| **`Task Executor`** | **High (3)** | Core 1 | PID line follower (100 Hz), sensor garis/warna, kontrol motor, dwell timer dengan sensor IR tangan, update `current_pos`, dan transisi `IDLE`/`DWELL`/`ERROR`. |
| **`Task Route Finder`** | Medium (2) | Core 0 | Menjalankan Dijkstra **saat job dieksekusi**, bukan saat perintah diterima. Tidak memiliki backlog rute. |
| **`Task MQTT`** | Medium (2) | Core 0 | Koneksi WSS, QoS 1, **clean session true**, keepalive 30 detik, LWT, validasi `moveto`, `ack`, deduplikasi `(req_id, node)`, dan publish telemetri. |
| **`Task Fetch`** | Low (1) | Core 0 | Mengambil `GET /graph.bin` **sekali saat boot**, memvalidasi framing (HTTP 200, `X-Graph-Version` bukan `0`, body 676 byte), lalu menyalin ke buffer tetap 26 × 26. Maksimal 3 percobaan dengan jeda 2 detik; jika gagal, `status = ERROR` dan tidak retry sampai reboot. Tidak ada refresh runtime. |

## 7.2 Alur Normal

1. **Boot**
   * Posisi `A` (Home), heading Utara, status `IDLE`.
   * Robot mem-publish `adam/srv/graph_version = 0` (retain) **sebelum** `online = 1`, agar retained version dari sesi sebelumnya tidak membuat gateway menganggap robot siap.
   * `Task Fetch` mengambil graf maksimal 3 kali dengan jeda 2 detik. Jika ketiga percobaan gagal, robot mem-publish `status = ERROR`, `graph_version` tetap `0`, dan tidak menerima job sampai reboot.
   * Setelah graf siap, robot mem-publish `current_pos = [A][0]`, `graph_version` sebenarnya, `status = IDLE`, baterai, dan kapasitas sampah.

2. **Menerima `moveto`**
   * `Task MQTT` hanya menerima jika status `IDLE`, mailbox kosong, graf sudah siap, dan kapasitas sampah di bawah ambang penuh.
   * Cek pasangan `(req_id, node)` di cache deduplikasi. Jika sudah pernah diproses → `ack DUPLICATE` dan jangan masukkan job lagi. Cek ini dilakukan sebelum cek `BUSY` agar retry QoS 1 tidak dianggap job baru.
   * Jika status `ERROR`, graf belum siap, atau `graph_version = 0` → `ack NOT_READY`.
   * Jika byte target bukan `A`–`Z` → `ack INVALID_TARGET`. Robot tidak memvalidasi ulang apakah node itu aktif di graf; gateway adalah source of truth dan hanya mengirim node aktif.
   * Jika tidak `IDLE` atau mailbox terisi → `ack BUSY`.
   * Jika kapasitas sampah ≥ `SENSORS_TRASH_FULL_PERCENT` (default 95%, baca `sensors_trash_level_percent()` saat request masuk) → `ack TRASH_FULL`. Robot menolak seluruh `moveto` baru sampai kapasitas di bawah ambang; tidak ada job yang diantre selama penuh. `srv/adam/reset` tetap diterima.
   * Masukkan `{req_id, node}` ke mailbox 1 slot. Periksa hasil `xQueueSend`; jika gagal → `ack BUSY`.
   * Kirim `ack ACCEPTED` setelah job benar-benar masuk mailbox. Ack berarti “diterima”, bukan “rute sudah ditemukan”.

3. **Menghitung rute**
   * `Task Route Finder` mengambil job dari mailbox dan menjalankan Dijkstra dari `current_node` ke target.
   * Jika tidak ada rute → publish `error(req_id, NO_PATH)` lalu `status = ERROR`; job tidak dieksekusi.
   * Rute dihitung memakai graf yang sama untuk seluruh masa boot. Graf tidak berubah saat job berjalan. Tidak ada perhitungan rute untuk job yang belum dikirim gateway.

4. **Eksekusi**
   * `Task Executor` mengambil alih path array, mem-publish `status = MOVING`, lalu mulai bergerak.
   * Saat memulai setiap edge, publish `current_pos = [from][to]`.
   * Saat tiba di node, publish `current_pos = [node][next]`; jika masih ada edge berikutnya, `next` adalah node berikutnya; jika sudah di target, `next = 0`.
   * Setelah tiba di target, buka tutup, publish `status = DWELL`, jalankan timer, lalu tutup dan publish `status = IDLE` serta `current_pos = [target][0]`.
   * Robot **tidak** mengambil job berikutnya sendiri. Gateway akan mengirim `moveto` berikutnya setelah menerima `IDLE` dan versi graf cocok.

5. **Siklus graf**
   * Robot tidak memiliki perintah refresh graf.
   * Graf baru hanya berlaku pada boot berikutnya. Admin reset tidak memuat ulang graf.
   * Gateway menahan `moveto` sampai `graph_version` robot sama dengan versi graf aktif.
   * Jika fetch/validasi framing gagal setelah 3 percobaan, robot mem-publish `graph_version = 0` dan `status = ERROR`, mencatat log, dan tidak retry sampai reboot. Robot tidak menerima job sampai graf siap.
   * Perubahan marka/lintasan fisik harus dilakukan saat robot tidak bergerak. Matikan robot sebelum dipindahkan, lalu nyalakan di Home `A` menghadap Utara.

## 7.3 Error Handling & Recovery

1. **Kegagalan saat bergerak** (`LINE_LOST`, `NODE_TIMEOUT`, atau error navigasi lain):
   * Motor langsung dimatikan; servo ditutup.
   * Robot berhenti di `current_node` terakhir yang valid.
   * Mailbox dan path aktif dibersihkan. Robot tidak memiliki antrean target, jadi tidak ada *queue flush* di sisi robot.
   * Publish `adam/srv/error` dengan `req_id` job aktif, lalu `current_pos = [current][0]` dan `status = ERROR`.
   * Gateway menandai request aktif `failed` dan membatalkan request `queued`.
   * Robot menunggu admin reset; tidak menerima `moveto` baru selama `ERROR`.

2. **Admin reset** (`srv/adam/reset`):
   * Robot membatalkan path aktif, mengosongkan mailbox, dan menghitung rute terpendek dari `current_node` ke Home `A`.
   * Jika sudah di `A`, robot hanya memutar heading ke Utara lalu kembali `IDLE`.
   * Jika tidak ada rute ke `A`, publish `error(req_id=0, NO_PATH)` dan tetap `ERROR`.
   * Setelah tiba di `A`, status kembali `IDLE`; robot siap menerima job baru setelah gateway memastikan versi graf cocok.
   * Reset **tidak** memuat ulang graf. Jika gateway punya versi graf baru atau fetch graf sebelumnya gagal, robot harus di-reboot.
   * Gateway membatalkan request aktif/antrean sebelum mengirim reset.

3. **Robot reboot di tengah job**:
   * Robot selalu boot di `A`, heading Utara, `IDLE`.
   * Robot mengunduh graf terbaru. Gateway melihat `online = 1`, `current_pos = [A][0]`, `graph_version`, dan status `IDLE`; request yang masih `dispatched`/`serving` untuk target lain ditandai `failed`.
   * Jika fetch graf gagal setelah 3 percobaan, robot masuk `ERROR` dengan `graph_version = 0`; gateway tidak melakukan dispatch sampai robot di-reboot dan berhasil memuat graf.

4. **Robot offline**:
   * LWT mengirim `adam/srv/online = 0`.
   * Gateway berhenti melakukan dispatch. Request aktif ditandai `failed` setelah lease; request `queued` tetap menunggu dan dapat dibersihkan admin.
   * Setelah reconnect, robot mem-publish state retained terbaru dan subscription dipasang ulang.

## 7.4 Telemetri

* **`adam/srv/current_pos`** → `[current][next]`, 2 byte, retain.
  * `current` = node terakhir yang dikonfirmasi.
  * `next` = node yang sedang dituju, atau `0x00` jika tidak ada.
  * Dikirim saat mulai edge, saat tiba di node, saat `DWELL`, saat `IDLE`, dan saat `ERROR`.
* **`adam/srv/status`** → 1 byte (`IDLE`, `MOVING`, `DWELL`, `ERROR`), retain. Dikirim **saat berubah** (event-driven); kesegaran dispatch ditentukan heartbeat `online`, bukan kedatangan pesan status.
* **`adam/srv/graph_version`** → 4 byte little-endian, retain. `0` = graf belum siap; nilai lain = versi graf yang aktif.
* **`adam/srv/battery`** → 1 byte persentase, retain. Dikirim periodik tiap **30 detik**.
* **`adam/srv/trash`** → 1 byte persentase kapasitas sampah, retain. Dikirim saat boot dan **setelah penutup ditutup** (akhir dwell).
* **`adam/srv/online`** → 1 byte, retain, LWT `0`. `0x01` dipublish saat connect lalu diulang tiap **10 detik** sebagai heartbeat. Sebelum disconnect normal, robot mem-publish `0` sendiri karena broker tidak mem-publish LWT pada clean disconnect.
* **`adam/srv/ack`** dan **`adam/srv/error`** tidak di-retain dan selalu membawa `req_id` 2 byte.

Kadensi telemetri **event-driven**: `status` saat berubah; `current_pos` saat mulai edge, tiba di node, `DWELL`, `IDLE`, dan `ERROR`; `trash` saat boot dan setelah penutup ditutup; `graph_version` saat boot/perubahan. `battery` periodik tiap 30 detik dan `online` heartbeat tiap 10 detik adalah satu-satunya pesan periodik. Setelah reconnect, robot mem-publish ulang state retained terbaru dan heartbeat berjalan lagi. Retained `status` hanya untuk last-known di web; gateway tetap mewajibkan heartbeat fresh (≤ 30 detik) sebelum dispatch.

## 7.5 Invarian Firmware

* Hanya satu job aktif; tidak ada antrean target di robot.
* `moveto` hanya diterima saat `IDLE`, mailbox kosong, dan graf sudah siap.
* Rute dihitung saat job dieksekusi, bukan saat perintah masuk.
* Graf memakai kapasitas tetap 26 × 26 byte; label aktif `A`–`Z` unik, boleh tidak berurutan, `A` wajib ada; jumlah node aktif `1..26`; tidak ada realokasi matriks saat runtime.
* Graf dimuat sekali saat boot; tidak ada mutasi graf saat `MOVING`, `DWELL`, atau `IDLE`.
* Robot memvalidasi framing graf (HTTP 200, versi bukan `0`, body 676 byte), tetapi tidak memvalidasi ulang semantik graf; gateway adalah source of truth.
* `req_id` 2 byte little-endian; `0x0000` dicadangkan.
* Clean session true; subscription dipasang ulang setiap connect; cache deduplikasi dibersihkan saat connect.
* Perintah tidak di-retain; telemetri state di-retain.
* `status` dipublish saat berubah dan retained untuk last-known; gateway menuntut heartbeat `online` fresh (≤ 30 detik) dan status retained `IDLE` sebelum dispatch.
* `online` retained dengan LWT `0`; robot mem-publish `online = 0` sebelum disconnect normal.
* `IDLE` adalah sinyal kesiapan robot, tetapi gateway juga wajib menunggu heartbeat fresh, `graph_version` cocok, kapasitas sampah < 95%, dan maintenance nonaktif sebelum mengirim job berikutnya.

## 7.6 Catatan Perilaku Khusus Task Executor

* **Interaksi dwell memakai IR tangan**: sensor FC-51 di dekat bukaan tempat sampah (`GPIO 13`, `LOW` = tangan terdeteksi) me-reset timer dwell selama tangan pengguna berada di area bukaan. Kode status `OBSTACLE` (`0x04`) hanya dicadangkan dan tidak pernah dipublish.
* **Kapasitas sampah**: HC-SR04 internal mengukur jarak ke permukaan sampah, lalu firmware mem-publish persentase 0–100% ke `adam/srv/trash` saat boot dan setelah penutup ditutup. Selama nilai ≥ `SENSORS_TRASH_FULL_PERCENT` (95%), `moveto` baru dibalas `ack TRASH_FULL`; `reset` tetap diterima.
* **Anti false-arrival**: setelah tiba di satu node, persimpangan harus menghilang dari pandangan sensor sebelum node berikutnya boleh terdeteksi.
* **Ack dan error tidak boleh tertukar**: `ack` hanya untuk penerimaan job; kegagalan navigasi selalu lewat `error` dengan `req_id` yang sama.

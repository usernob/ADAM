# 4. Dwell Time & Interaksi Tempat Sampah di Node Tujuan

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Saat robot tiba di Node tujuan panggilan:

1. **Pembukaan Tutup Sampah**: Motor servo/aktuator membuka tutup sampah otomatis.
2. **Dwell Timer & Sensor Interaksi**:
   * Timer internal (15 detik, `DWELL_TIME_MS`) mulai berjalan.
   * **Sensor IR tangan (FC-51)** dipasang di dekat bukaan tempat sampah, bukan di bawah robot. Output sensor aktif-*low*: `LOW` = tangan/objek terdeteksi.
   * **Aturan Reset Timer**: selama sensor IR tangan mendeteksi tangan, timer dwell di-reset ulang. Setelah tangan menjauh, timer berjalan penuh lagi dari deteksi terakhir, sehingga pengguna tetap punya waktu untuk membuang sampah.
3. **Penyelesaian Task**:
   * Jika **timer habis** dan **tidak ada lagi tangan** yang terdeteksi, tempat sampah otomatis menutup.
   * Robot mem-publish `adam/srv/trash` (kapasitas diukur setelah penutup ditutup), `status = IDLE`, dan `current_pos = [node][0]`, lalu **menunggu gateway**.
   * Gateway yang memiliki antrean akan mengirim job berikutnya setelah menerima `IDLE` dan versi graf robot cocok. Jika antrean kosong, robot tetap `IDLE` sampai ada panggilan baru. Robot tidak mengambil target berikutnya sendiri.

## 4.1 Sensor Kapasitas Sampah

* **HC-SR04** tidak dipakai untuk interaksi tangan/dwell. Sensor ini dipasang di bagian atas wadah yang tetap (bukan pada tutup yang bergerak) dan mengukur jarak ke permukaan sampah.
* Jarak dikonversi menjadi persentase kapasitas terisi: `0%` = kosong, `100%` = penuh.
* Kalibrasi ada di `firmware/src/sensors.h`:
  * `SENSORS_TRASH_EMPTY_CM` — jarak sensor ke dasar wadah saat kosong (default `30 cm`).
  * `SENSORS_TRASH_FULL_CM` — jarak sensor ke permukaan saat skala mencapai 100% (default `5 cm`). Ini batas skala, bukan ambang penolakan; ambang ada di `SENSORS_TRASH_FULL_PERCENT`.
* Nilai persentase dikirim sebagai telemetri 1 byte ke topik `adam/srv/trash` (lihat [docs/06-protocol.md](06-protocol.md)).
* **Ambang penuh**: wadah dianggap penuh pada `SENSORS_TRASH_FULL_PERCENT` (default `95%`), bukan 100%, agar noise ultrasonic di tepi ambang tidak membuat keputusan fluktuatif. Selama penuh, robot mem-publish telemetri seperti biasa tetapi membalas `moveto` dengan `ack TRASH_FULL` sampai kapasitas turun di bawah ambang (mis. setelah wadah dikosongkan). Gateway juga menahan panggilan baru: `POST /api/call` ditolak `503 bin_full` selama retained `adam/srv/trash` ≥ ambang.

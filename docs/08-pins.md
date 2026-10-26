# 8. Pemetaan Pin (Pin Mapping)

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Sumber kebenaran tabel ini adalah `#define` di `src/motor.h`, `src/sensors.h`, dan `src/dwell.c`. Kalau pin fisik diubah, ubah `#define`-nya dulu, baru tabel ini.

## ESP32 devkit

## Driver Motor L298N (PWM via LEDC)

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 14 | ENA | PWM out | Kecepatan motor kiri (LEDC channel 0) |
| 27 | IN1 | Out | Arah kiri 1 |
| 26 | IN2 | Out | Arah kiri 2 |
| 32 | ENB | PWM out | Kecepatan motor kanan (LEDC channel 1) |
| 25 | IN3 | Out | Arah kanan 1 |
| 33 | IN4 | Out | Arah kanan 2 |

PWM: frekuensi 1 kHz, resolusi 8-bit (duty 0–255), timer LEDC low-speed.

## Sensor Garis BFD-1000 (3 channel aktif)

Modul BFD-1000 punya 5 channel output, tetapi firmware hanya memakai **3 channel** (kiri, tengah, kanan) sesuai `SENSORS_PIN_LF_*` di `sensors.h`. Pola garis 3 bit: bit0 = kiri, bit1 = tengah, bit2 = kanan; persimpangan = `0b111`.

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 39 | Kiri | Input | Bit0 pola garis |
| 34 | Tengah | Input | Bit1 pola garis |
| 35 | Kanan | Input | Bit2 pola garis |

> ⚠️ **Polaritas:** output digital BFD-1000 aktif-*low* — `LOW` saat sensor melihat garis hitam, `HIGH` di latar putih. Firmware membalik level ini di `sensors.c`; jika modul yang dipakai aktif-*high*, ubah `SENSORS_LINE_ACTIVE_LOW` menjadi `0` di `sensors.h`.
>
> ⚠️ **Catu daya:** hubungkan `VCC` BFD-1000 ke **3.3 V**, bukan 5 V, karena output digital-nya mengikuti tegangan VCC dan GPIO ESP32 tidak toleran 5 V.
>
> ⚠️ GPIO 34, 35, 39 adalah *input-only* pin dan **tidak punya pull internal** — konfigurasi `GPIO_PULLUP_ENABLE` di `sensors.c` tidak berpengaruh pada ketiganya. Output BFD-1000 bersifat *push-pull* sehingga pull-up memang tidak diperlukan. Channel modul yang tidak dipakai (GPIO 21/22 pada tabel versi 5 channel) dibiarkan tidak tersambung.

## Sensor IR Tangan (FC-51) di Bukaan Tempat Sampah

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 13 | OUT | Input | `LOW` = tangan/objek terdeteksi di dekat bukaan; memicu reset dwell timer |

> Sensor ini **bukan** sensor garis dan tidak dipasang di bawah robot. Posisinya di dekat area bukaan tempat sampah untuk mendeteksi tangan pengguna. Output FC-51 bersifat *push-pull*; pull-up internal di `sensors.c` hanya sebagai pengaman.

## Sensor Warna TCS3200

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 19 | S0 | Out | Skala frekuensi out — bitrate 20% (`S0=H`, `S1=L`) |
| 18 | S1 | Out | Skala frekuensi out |
| 5 | S2 | Out | Pilih filter (R/G/B) |
| 17 | S3 | Out | Pilih filter (R/G/B) |
| 16 | OUT | Input | Keluaran frekuensi warna (pull-up enable) |

## Ultrasonik HC-SR04 (kapasitas sampah)

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 4 | TRIG | Out | Trigger pulse |
| 15 | ECHO | Input | Echo (pull-up enable, gunakan divider 1k/2k karena output 5 V) |

> HC-SR04 dipasang di bagian atas wadah yang tetap dan mengukur jarak ke permukaan sampah. Kalibrasi `SENSORS_TRASH_EMPTY_CM` (dasar wadah, skala 0%) dan `SENSORS_TRASH_FULL_CM` (titik skala 100%) di `sensors.h`, plus ambang penolakan `SENSORS_TRASH_FULL_PERCENT` (95%); persentase dikirim ke `adam/srv/trash`.

## Aktuator & Ekstra

| GPIO | Sinyal | Arah | Keterangan |
| :--- | :--- | :--- | :--- |
| 2 | BUZZER | Out | Buzzer indikator (beep saat tiba di node dan saat dwell selesai) |
| 23 | SERVO | PWM out | Servo tutup tempat sampah: 50 Hz, resolusi 14-bit, pulsa 500–2400 µs; terbuka 90°, tertutup 0° |
| 36 | BAT ADC | ADC in | GPIO36 = ADC1_CH0, pembacaan tegangan baterai, pembacaan 0–4095 |

> Catatan baterai: pembacaan hanya memakai ADC1 (GPIO36) karena ADC2 tidak dapat dipakai bersamaan WiFi aktif — jangan pindahkan pin baterai ke ADC2.

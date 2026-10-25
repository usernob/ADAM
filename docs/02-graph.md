# 2. Arsitektur Lingkungan & Model Graf

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

Lingkungan operasional robot direpresentasikan sebagai **Weighted Undirected Graph G = (V, E)**:

* **Node (V)**: Titik persimpangan garis sekaligus lokasi pemberhentian/panggilan robot. Node diberi label huruf kapital **`A`–`Z`**, unik, dengan `A` wajib ada sebagai Home. Satu graf berisi **maksimal 26 node**. Label seperti `AA`, angka, atau huruf kecil tidak valid. Label **tidak harus berurutan**; contoh `A, C, F` valid selama tidak ada duplikasi dan graf connected dari `A`.
* **Edge (E)**: Lintasan garis hitam yang menghubungkan antar node.
* **Bobot (W)**: Jarak relatif atau estimasi waktu tempuh (nilai rentang 1 sampai 63).
* **Kapasitas Edge**: Maksimal 4 edge per node, sejajar dengan arah mata angin (**Utara, Timur, Selatan, Barat**), **dengan satu edge per arah mata angin per node** — karena robot hanya tahu arah hadap (N/E/S/W), dua edge yang searah dari node yang sama membuat tujuan belokan ambigu dan tidak dapat dieksekusi.

Node index internal firmware selalu `label - 'A'`, yaitu `A = 0` sampai `Z = 25`. Karena itu label tidak perlu berurutan: graf dengan label `A, C, F` memakai index `0, 2, 5`, sedangkan baris/kolom untuk label yang tidak aktif bernilai nol. Jumlah node aktif `N` adalah banyaknya label, bukan nilai huruf tertinggi. Representasi di API dan MQTT tetap memakai byte ASCII huruf, bukan index biner.

```text
       [Node A] (Home / Initial Node)
          |
  (N=0)   | (Weight = 12)
          v
       [Node B] <--(E=1, W=5)--> [Node C]
```

## 2.1 Enkoding Memory Data Graf (Bit-Packing Matrix)

Peta disimpan dalam bentuk **Matriks 1D Byte Array** berukuran tetap `26 × 26` byte (676 byte). Setiap elemen matriks (1 byte = 8 bit) merepresentasikan hubungan dari Node i ke Node j, dengan index `i = label_i - 'A'` dan `j = label_j - 'A'`. Baris/kolom untuk label yang tidak aktif berisi nol. Jumlah node aktif `N` (`1..26`) hanya metadata untuk validasi dan UI, bukan ukuran matriks.

| Position Bit | Panjang Bit | Deskripsi Data | Rentang Nilai |
| :--- | :--- | :--- | :--- |
| **Bit 0 - 1** | 2 bit | Arah Mata Angin (*Direction*) | `00` (Utara), `01` (Timur), `10` (Selatan), `11` (Barat) |
| **Bit 2 - 7** | 6 bit | Bobot / Weight Lintasan | 1 - 63 (0 = Tidak ada hubungan/edge) |

#### Format Byte Matriks:

```text
  Bit 7   Bit 6   Bit 5   Bit 4   Bit 3   Bit 2   Bit 1   Bit 0
+-------+-------+-------+-------+-------+-------+-------+-------+
|                    WEIGHT (Bit 7 ... Bit 2)         |  DIR1 | DIR0 |
+-------+-------+-------+-------+-------+-------+-------+-------+
|<----------------- Weight ----------------->|<-- Direction -->|
```

> **Aturan Graf Undirected (Anti-Symmetric Direction)**:
>
> Jika A -> B berarah **Utara (`00`)** dengan bobot `W`, maka B -> A secara otomatis disimpan berarah **Selatan (`10`)** dengan bobot `W` yang sama pada matriks. Input API cukup memuat satu edge per pasangan node; reverse edge diturunkan oleh gateway.

Firmware menyimpan graf dalam **kapasitas tetap 26 × 26 byte (676 byte)**. Jumlah node aktif dapat lebih kecil dari 26, tetapi tidak ada realokasi matriks saat runtime. `GET /graph.bin` juga selalu mengirim 676 byte; label yang tidak aktif bernilai nol. Batas 26 berasal dari label `A`–`Z`, bukan dari ukuran field 1 byte. Jika suatu saat butuh lebih dari 26 node, protokol ID node harus diubah secara eksplisit; matriks tidak diperbesar secara dinamis.

### 2.1.1 Aturan Validasi Graf

Gateway wajib menolak graf yang tidak memenuhi aturan berikut:

* `nodes` unik, hanya `A`–`Z`, maksimal 26, dan wajib memuat `A`.
* Label tidak harus contiguous; `A, C, F` valid.
* Tidak ada self-loop.
* Tidak ada duplicate pair; satu pasangan node hanya boleh muncul sekali.
* `weight` bernilai `1..63`.
* `direction` hanya `N`, `E`, `S`, atau `W`.
* Maksimal satu edge per arah mata angin per node.
* Graf connected dari `A` melalui edge undirected. Graf satu node `["A"]` tanpa edge valid.
* Reverse edge dibuat otomatis dan tidak boleh dikirim sebagai edge kedua untuk pasangan yang sama.

## 2.2 Siklus Versi & Deployment Graf

Graf adalah **artefak deployment**, bukan state yang diubah saat robot bekerja:

* Gateway menyimpan graf aktif beserta versinya (diekspos sebagai `X-Graph-Version`) dan riwayat versi di SQLite.
* Robot hanya memuat graf dari `GET /graph.bin` **saat boot**.
* Perubahan graf tidak dikirim ke robot saat runtime. Gateway menahan dispatch sampai `graph_version` yang dilaporkan robot sama dengan versi graf aktif.
* Saat versi belum cocok, panggilan baru ditolak `503 graph_update`; tidak ada request yang masuk antrean.
* Graf aktif di robot tidak boleh berubah saat ada job berjalan atau request `queued`. `POST /save` hanya boleh dilakukan saat tidak ada job aktif dan antrean kosong.
* Graf baru hanya sah jika cocok dengan marka/lintasan fisik di lantai. Robot harus dimatikan sebelum dipindahkan, lalu dinyalakan di Home `A` menghadap Utara. Perubahan lantai dilakukan saat robot tidak bergerak.
* Robot mencoba fetch graf maksimal 3 kali saat boot. Jika gagal, `graph_version = 0` dan `status = ERROR`; tidak ada retry otomatis sampai reboot.
* Admin reset tidak memuat ulang graf. Graf baru berlaku setelah robot reboot.
* Rollback membuat versi baru dari snapshot graf lama; nomor versi tidak pernah diturunkan.

## 2.3 Deteksi Fisik Persimpangan (Color Marker Sensing)

Untuk membedakan antara jalur lurus biasa dan titik persimpangan/node:

* Setiap titik persimpangan (*Node*) diberi **marka penanda warna khusus** (*Color Marker*) pada lintasan garis hitam.
* Robot menggunakan **Sensor Warna / Color Sensor Module** yang dipasang bersamaan dengan array sensor *line-follower* BFD-1000 (5 channel).
* Persimpangan terdeteksi saat kelima channel BFD-1000 sama-sama melihat garis hitam (`0b11111`), lalu divalidasi oleh color marker.
* Deteksi persimpangan yang tervalidasi akan memicu event `NODE_ARRIVED` pada *Task Executor* untuk mengeksekusi logika belokan/pemberhentian.

# 3. Booting & Navigasi Orientasi Hadap (Heading Control)

*Bagian dari spesifikasi Project A.D.A.M — indeks: [README.md](../README.md)*

## 3.1 Inisialisasi Booting (Initial State)

Saat robot pertama kali dinyalakan (*power-on / restart*):

* **Posisi Default**: Node **'A'** (Home Node).
* **Arah Hadap Default**: **Utara** (`00` / `NORTH`).
* Status awal robot diatur ke `IDLE`.
* **Prasyarat operasional (kewajiban admin)**: penempatan di `A` menghadap Utara adalah syarat operasional, bukan fakta firmware — robot **tidak dapat memverifikasi posisi fisiknya**. Menyalakan robot di luar `A` membuat `current_pos = [A][0]` dipublish palsu dan seluruh navigasi bisa salah arah tanpa error apa pun. Bila itu terjadi: matikan robot, pindahkan ke `A` menghadap Utara, lalu nyalakan kembali atau kirim admin reset (lihat [docs/07-firmware.md](07-firmware.md) §7.3.2). Prasyarat yang sama berlaku untuk reset.

## 3.2 Logika Perubahan Orientasi

Robot menyimpan variabel internal `currentHeading` (Utara, Timur, Selatan, Barat). Saat tiba di node — baik node lurus, belok, ujung garis, maupun persimpangan — robot menghitung manuver berbelok berdasarkan selisih arah hadap saat ini dengan arah edge tujuan berikutnya:

```text
Delta Direction = (Target Direction - Current Direction) mod 4
```

| Delta Direction | Manuver Robot | Orientasi Hadap Baru |
| :--- | :--- | :--- |
| `0` (0 derajat) | **Lurus** (*Forward*) | Tetap |
| `1` (90 derajat Kanan) | **Belok Kanan** (*Turn Right*) | `(Current + 1) mod 4` |
| `2` (180 derajat) | **Putar Balik** (*U-Turn*) | `(Current + 2) mod 4` |
| `3` (270 derajat / Kiri) | **Belok Kiri** (*Turn Left*) | `(Current + 3) mod 4` |

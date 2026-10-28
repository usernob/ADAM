# Project A.D.A.M

**Automated Dustbin And Management** — sistem robot tempat sampah otomatis berbasis *Autonomous Mobile Robot* (AMR). Robot bernavigasi menyusuri garis hitam (*line follower*) di area indoor tertutup, mencari rute terpendek dengan **Dijkstra** yang dijalankan langsung di mikrokontroler berdasarkan peta berbobot (*weighted undirected graph*).

## Fitur Utama

* **Navigasi otonom**: Dijkstra lokal di ESP32, peta graf berbobot dengan arah mata angin (max 4 edge per node, 1 edge per arah), maksimal 26 node berlabel `A`–`Z` yang unik, dengan `A` wajib ada; label tidak harus berurutan.
* **Pemanggilan via web**: halaman panggil cepat per node (QR Code di titik fisik), visualisasi peta *real-time*, dan deduplikasi target.
* **Antrean terpusat**: satu Gateway Node di VPS memegang antrean, mengirim satu job pada satu waktu, dan hanya melanjutkan setelah robot kembali `IDLE` dengan versi graf yang cocok.
* **Graf boot-only**: graf dimuat saat boot atau admin reset ke buffer tetap 26 × 26 byte; perubahan graf berlaku setelah reboot/reset, bukan di tengah job.
* **Telemetri MQTT**: posisi `current`/`next`, status, versi graf, baterai, dan kapasitas sampah — wire format biner 1–4 byte per pesan.
* **Admin-only auth**: user publik tidak perlu akun; hanya admin yang login lewat session. Tidak ada registrasi, OAuth, atau TOTP.
* **Error & recovery**: deteksi garis hilang/step timeout → motor berhenti; admin mengangkut robot ke Home lalu mereset — reset = state fresh + unduh ulang graf (satu-satunya pemulihan tanpa mematikan daya).
* **Interaksi tempat sampah**: sensor IR tangan di dekat bukaan memperpanjang dwell timer; servo membuka/menutup tutup otomatis.

## Arsitektur Tingkat Tinggi

```text
[ Browser ]
    |
    | HTTPS + SSE
    v
[ Cloudflare Tunnel ] -> [ Caddy ] -> [ Node Gateway ]
                                         ├── REST API + SSE
                                         ├── antrean + dispatch
                                         ├── SQLite
                                         └── MQTT client
                                                |
                                                | MQTT (localhost / WSS)
                                                v
                                          [ Mosquitto ]
                                                |
                                                | MQTT (WSS 443)
                                                v
                                         [ Robot A.D.A.M ]
                                                |
                                                | GET /graph.bin (HTTP, saat boot)
                                                v
                                          [ Node Gateway ]
```

Gateway dan Mosquitto berjalan di satu VPS. Browser tidak pernah terhubung langsung ke MQTT; seluruh *live update* datang dari SSE. Detail topologi ada di [docs/10-deployment.md](docs/10-deployment.md).

## Dokumentasi Teknis

| Dokumen | Isi |
| :--- | :--- |
| [docs/02-graph.md](docs/02-graph.md) | Model graf berbobot, label `A`–`Z` unik/non-contiguous, aturan edge, matriks tetap 26 × 26, siklus versi |
| [docs/03-navigation.md](docs/03-navigation.md) | Boot state, heading control, tabel manuver belokan |
| [docs/04-dwell.md](docs/04-dwell.md) | Dwell time & interaksi tempat sampah di node tujuan |
| [docs/05-web.md](docs/05-web.md) | Halaman web, antrean & deduplikasi, admin auth, stream real-time via SSE |
| [docs/06-protocol.md](docs/06-protocol.md) | HTTP API, riwayat graf, antrean, graph version gate, wire format MQTT (`req_id`, ack/error, current/next) |
| [docs/07-firmware.md](docs/07-firmware.md) | Task FreeRTOS, mailbox 1 slot, graf boot-only, ack/error, recovery |
| [docs/08-pins.md](docs/08-pins.md) | Pemetaan pin: motor L298N, sensor garis/warna/IR tangan/ultrasonik, servo, ADC baterai |
| [docs/09-roadmap.md](docs/09-roadmap.md) | Pengembangan Phase 2 |
| [docs/10-deployment.md](docs/10-deployment.md) | VPS, Cloudflare Tunnel, Caddy, Mosquitto WSS, SQLite, deployment graf, admin CLI, backup |

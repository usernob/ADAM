# Firmware A.D.A.M

Firmware ESP32 untuk robot A.D.A.M, dibangun dengan **PlatformIO + ESP-IDF**. Semua perintah di bawah dijalankan dari folder `firmware/`.

## Prasyarat

- **PlatformIO Core** (`pio`) versi terbaru, atau ekstensi PlatformIO di VS Code.
- Board **ESP32** dengan flash **4 MB** (board `esp32dev`).
- Kabel USB data dan akses ke port serial board (`/dev/ttyUSB0` atau `/dev/ttyACM0` di Linux, `/dev/cu.usbserial-*` di macOS, `COM3` di Windows).
- Koneksi internet saat build pertama, karena PlatformIO akan mengunduh framework ESP-IDF.

Cek board dan port yang terdeteksi:

```bash
pio device list
```

Semua perintah di bawah memakai PlatformIO Core CLI (`pio`). Jika memakai ekstensi PlatformIO di VS Code tanpa `pio` di `PATH`, jalankan perintah dari terminal PlatformIO, atau gunakan tombol **Build/Upload/Monitor** di panel **Project Tasks**. Toolchain ESP-IDF dikelola PlatformIO, jadi tidak perlu mengatur `PATH` manual.

## Cara Menjalankan

### 1. Siapkan file kredensial

`nvs_data.csv` tidak disimpan di Git. Untuk pertama kali, salin `nvs_data.csv.example` menjadi `nvs_data.csv` lewat file manager atau perintah sesuai OS:

```bash
# Linux/macOS
cp nvs_data.csv.example nvs_data.csv

# Windows (Command Prompt/PowerShell)
copy nvs_data.csv.example nvs_data.csv
```

Lalu isi kredensialnya.

Detail key dan cara memperbaruinya ada di bagian [Update Credentials](#update-credentials).

### 2. Build firmware

```bash
pio run -e default
```

### 3. Upload firmware ke board

```bash
pio run -e default -t upload
```

Target `upload` menulis bootloader, partition table, dan aplikasi. **NVS tidak ikut ditulis**, jadi board baru atau board yang baru di-erase juga perlu langkah berikut.

### 4. Flash kredensial ke NVS

```bash
pio run -e default -t upload_nvs_bin
```

Perintah ini membuat image NVS dari `nvs_data.csv`, lalu menuliskannya ke partisi NVS pada offset `0x9000`.

Urutan untuk board baru:

```bash
# salin dan edit nvs_data.csv terlebih dahulu
pio run -e default -t upload
pio run -e default -t upload_nvs_bin
```

Setelah itu tekan tombol **EN/RST** pada board atau cabut-pasang USB agar firmware membaca kredensial yang baru.

### Port serial

PlatformIO biasanya otomatis memilih port jika hanya ada satu board terhubung. Jika ada beberapa board, cek port yang tersedia lalu tambahkan opsinya:

```bash
pio device list
pio run -e default -t upload --upload-port <PORT>
pio run -e default -t upload_nvs_bin --upload-port <PORT>
pio device monitor -e default --port <PORT>
```

Ganti `<PORT>` (tanpa tanda kurung) dengan port board. Contoh formatnya: `/dev/ttyUSB0` atau `/dev/ttyACM0` di Linux, `/dev/cu.usbserial-*` di macOS, dan `COM3` di Windows.

## Cara Memonitor

Monitor serial bawaan PlatformIO sudah memakai baudrate `115200` dan filter waktu dari `platformio.ini`:

```bash
pio device monitor -e default
```

Jika ada lebih dari satu board, tentukan port seperti pada bagian [Port serial](#port-serial). Keluar dari monitor dengan `Ctrl+C`.

Log yang menandakan boot dan koneksi berhasil kira-kira seperti ini:

```text
I (...) main: Startup..
I (...) wifi station: got ip:192.168.1.10
I (...) graph_fetch: graph v1 installed: 676 bytes
I (...) mqtt: MQTT_EVENT_CONNECTED
```

Jika muncul pesan seperti berikut, NVS belum berisi kredensial yang benar:

```text
E (...) wifi station: No SSID in NVS - flash the NVS partition first
E (...) mqtt: Failed to open nvs namespace mqtt: ESP_ERR_NVS_NOT_FOUND
```

Setelah firmware online, telemetri robot juga bisa dilihat dari dashboard web gateway. Untuk debug tingkat firmware, gunakan monitor serial di atas.

## Update Credentials

Semua kredensial disimpan di **NVS**, bukan di source code:

- WiFi SSID dan password
- URI, username, dan password broker MQTT
- URL gateway untuk mengunduh graf

File yang diedit adalah `nvs_data.csv` (gitignored). Jangan mengubah nama key, namespace, tipe, atau encoding pada baris yang sudah ada.

### Key yang perlu diisi

| Key | Isi |
| :--- | :--- |
| `sta.ssid` | SSID WiFi |
| `sta.pswd` | Password WiFi |
| `mqtt.uri` | URI broker, misalnya `wss://adam.example.com/mqtt` atau `mqtts://vps.example.com:8883` |
| `mqtt.username` | Username MQTT robot |
| `mqtt.password` | Password MQTT robot |
| `server.url` | Base URL gateway, misalnya `https://adam.example.com` (tanpa `/graph.bin`) |

Catatan:

- `server.url` hanya berisi base URL. Firmware menambahkan `/graph.bin` sendiri.
- Panjang SSID maksimal 32 byte dan password WiFi maksimal 64 byte; jangan mengubah encoding `blob_sz_fill(32;0x00)` dan `blob_fill(64;0x00)`.
- Nilai MQTT dan `server.url` dibaca sebagai string dengan buffer 256 byte, jadi jaga panjangnya di bawah 256 byte.
- `nvs.net80211`, `mqtt`, dan `server` adalah nama namespace. Jangan menghapus barisnya.

### Langkah update

1. Edit `nvs_data.csv`.
2. Simpan file.
3. Flash ulang hanya partisi NVS:

   ```bash
   pio run -e default -t upload_nvs_bin
   ```

4. Restart board (tombol **EN/RST** atau cabut-pasang USB).

Yang terjadi saat update:

- Hanya partisi NVS yang ditimpa; firmware/aplikasi tidak berubah.
- Upload firmware biasa (`pio run -t upload`) **tidak** mengubah kredensial.
- Jika memakai image gabungan `firmware.merged.bin`, image tersebut berisi NVS dari `nvs_data.csv` saat `merge_bin` dijalankan. Jalankan ulang `merge_bin` setelah mengubah kredensial.

## Target dan Environment

### Environment

Semua environment mewarisi pengaturan dari bagian `[env]` di `platformio.ini`. File `sdkconfig.defaults` disalin otomatis menjadi `sdkconfig.<env>` saat build; file hasil salinan itu tidak perlu diedit manual.

| Env | Build | Flag khusus | Kegunaan |
| :--- | :--- | :--- | :--- |
| `default` | release | - | Firmware produksi untuk robot sebenarnya. |
| `debug` | debug | `ADAM_GRAPH_FALLBACK_DUMMY`, `ADAM_SENSOR_MOCK` | Semua sensor disimulasikan dan graf memakai dummy jika fetch gagal. Tetap memakai WiFi dan MQTT asli. |
| `debug-graph-dummy` | debug | `ADAM_GRAPH_FALLBACK_DUMMY` | Hardware asli, tetapi graf dummy dipakai jika WiFi/fetch gagal. |
| `debug-sensor-mock` | debug | `ADAM_SENSOR_MOCK` | Sensor disimulasikan; graf tetap diunduh dari gateway. Cocok untuk menguji navigasi tanpa hardware sensor. |
| `motor-demo` | debug | `ADAM_MOTOR_DEMO` | Hanya menguji motor L298N (maju, mundur, belok). Tanpa WiFi, MQTT, sensor, dan navigasi. |

Contoh build dan upload:

```bash
pio run -e debug -t upload
pio run -e debug-sensor-mock -t upload
pio run -e debug-graph-dummy -t upload
pio run -e motor-demo -t upload
```

Catatan perilaku mock:

- `ADAM_SENSOR_MOCK` mensimulasikan sensor garis, marker warna, IR tangan, kapasitas sampah, dan baterai. Motor tetap memakai driver asli.
- `ADAM_GRAPH_FALLBACK_DUMMY` hanya menjadi fallback. Firmware tetap mencoba WiFi dan `GET /graph.bin`; graf dummy baru dipakai jika WiFi tidak mendapat IP atau fetch gagal setelah 3 percobaan.
- `debug` tetap membutuhkan WiFi dan broker MQTT untuk terhubung. Tanpa WiFi, robot hanya memakai graf dummy dan tidak bisa menerima perintah.

### Target dan Perintah PlatformIO

| Target | Perintah | Fungsi |
| :--- | :--- | :--- |
| `upload` | `pio run -e <env> -t upload` | Build lalu flash bootloader, partition table, dan aplikasi. NVS tidak disentuh. |
| `upload_nvs_bin` | `pio run -e <env> -t upload_nvs_bin` | Generate NVS dari `nvs_data.csv` lalu flash ke offset `0x9000`. |
| `generate_nvs_bin` | `pio run -e <env> -t generate_nvs_bin` | Hanya membuat `.pio/build/<env>/nvs_data.bin` tanpa flash. |
| `merge_bin` | `pio run -e <env> -t merge_bin` | Membuat `.pio/build/<env>/firmware.merged.bin` berisi bootloader, partition table, aplikasi, dan NVS. |
| `clean` | `pio run -e <env> -t clean` | Menghapus hasil build environment tersebut. |
| `check` | `pio check -e <env>` | Menjalankan pemeriksaan `clang-tidy`. |

Contoh membuat image gabungan:

```bash
pio run -e default -t merge_bin
```

Hasilnya dapat di-flash penuh mulai alamat `0x0`. Gunakan `esptool` yang dikelola PlatformIO agar tidak bergantung pada lokasi instalasi di tiap OS:

```bash
pio pkg exec --package "tool-esptoolpy" -- esptool.py --chip esp32 --port <PORT> --baud 460800 write_flash 0x0 .pio/build/default/firmware.merged.bin
```

Ganti `<PORT>` dengan port board, misalnya `/dev/ttyUSB0`, `/dev/cu.usbserial-*`, atau `COM3`.

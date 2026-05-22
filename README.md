# Tap Card

Sistem akses kontrol berbasis RFID dan ESP32 yang melakukan validasi kartu melalui IoT Card Access API v1.0.

## Ringkasan

Proyek ini menggunakan:
- ESP32 (board `esp32dev`)
- Modul RFID MFRC522
- Servo untuk membuka/menutup palang pintu
- Buzzer dan LED sebagai indikator status
- Koneksi Wi-Fi untuk memanggil API server dengan `unix_id` kartu

## Fitur

- Membaca UID kartu RFID
- Mengirim `unix_id` ke server API
- Menyalakan LED hijau dan membuka servo saat kartu valid
- Menyalakan LED merah dan menyala buzzer saat kartu tidak valid
- Koneksi Wi-Fi otomatis ulang bila terputus

## Perangkat keras dan pin

ESP32 pin yang digunakan:
- `G5` (GPIO 5) sebagai `SS_PIN` untuk SDA RFID
- `G22` (GPIO 22) sebagai `RST_PIN` untuk RST RFID
- `G2` (GPIO 2) untuk `PIN_BUZZER`
- `G14` (GPIO 14) untuk `PIN_LED_HIJAU`
- `G15` (GPIO 15) untuk `PIN_LED_MERAH`
- `G13` (GPIO 13) untuk `PIN_SERVO`

## Koneksi kabel

- MFRC522 SDA -> GPIO 5
- MFRC522 RST -> GPIO 22
- MFRC522 MOSI -> GPIO 23 (default SPI)
- MFRC522 MISO -> GPIO 19 (default SPI)
- MFRC522 SCK -> GPIO 18 (default SPI)
- Servo signal -> GPIO 13
- LED hijau -> GPIO 14
- LED merah -> GPIO 15
- Buzzer -> GPIO 2
- Ground dan VCC disesuaikan dengan modul

## Konfigurasi perangkat lunak

File `platformio.ini` sudah dikonfigurasikan sebagai berikut:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
monitor_speed = 115200
upload_port = COM3
monitor_port = COM3

lib_deps =
    miguelbalboa/MFRC522 @ ^1.4.11
    madhephaestus/ESP32Servo @ ^3.0.5
    bblanchon/ArduinoJson @ ^7.0.4
```

## Pengaturan kode

Di `src/main.cpp`, atur:
- `ssid` dan `password` Wi-Fi Anda
- `API_BASE_URL` jika server Anda bukan `https://iot.airashi.biz.id/api/v1`
- `DEVICE_API_KEY` sebagai key awal untuk provisioning otomatis

API yang digunakan:
- Base URL: `https://iot.airashi.biz.id/api/v1`
- Tap kartu: `POST /card/tap`
- Heartbeat device: `GET /devices/me/status`
- Provision otomatis: `POST /devices/provision`

Request tap mengirim JSON:
```json
{ "unix_id": "1715403123456" }
```

Saat pertama kali menyala, device akan:
- membaca hardware ID otomatis dari ESP32 MAC
- mengirim `hardware_id` dan `device_name` ke endpoint provisioning
- menyimpan `node_id` dan `api_key` hasil provisioning ke memori lokal

Header yang wajib dikirim:
- `X-Node-ID`
- `X-API-Key`

Respons sukses diproses dari `data.action`:
- `granted` untuk akses diterima
- `denied` untuk akses ditolak
- `registered` untuk kartu berhasil didaftarkan di mode register

## Cara menggunakan

1. Buka proyek di PlatformIO.
2. Masukkan SSID dan password Wi-Fi di `src/main.cpp`.
3. Isi `DEVICE_API_KEY` jika server Anda memakai key yang sama untuk provisioning otomatis.
4. Hubungkan perangkat keras sesuai pin.
5. Unggah firmware ke ESP32.
6. Buka Serial Monitor dengan baud `115200`.
7. Tempelkan kartu RFID untuk menguji.

## Alur kerja sistem

1. ESP32 terhubung ke Wi-Fi.
2. Sistem menunggu kartu RFID muncul.
3. UID kartu dibaca.
4. UID dikirim ke server menggunakan HTTP POST ke `/api/v1/card/tap`.
5. Server merespons status kartu dalam JSON.
6. ESP32 menyalakan indikator sesuai respons:
    - `granted` → LED hijau + buzzer + servo buka lalu tutup
    - `registered` → LED hijau + buzzer singkat
    - `denied` → LED merah + buzzer singkat

## Penanganan masalah

- Jika Wi-Fi terputus, perangkat otomatis mencoba kembali.
- Jika server tidak bisa dihubungi, LED merah dan buzzer menyala sebagai tanda kesalahan.
- Pastikan value `unix_id` yang dikirim ke API memang sesuai dengan member di database. Jika kartu RFID Anda masih menghasilkan UID hex, Anda perlu memetakan UID itu ke `unix_id` di backend atau menyesuaikan alur provisioning.
- Jika device belum pernah diprovision, ia akan otomatis mendaftarkan hardware ID-nya ke server saat pertama kali terkoneksi.

## Catatan

Pastikan API server Anda dapat menerima payload `unix_id` sesuai dokumentasi backend dan `DEVICE_API_KEY` di firmware cocok dengan konfigurasi server.

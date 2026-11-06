# Interrupt-driven touch sensor

Port input [touch.py](../../app/sensors/touch.py) untuk ESP-IDF 5.3–5.x pada
ESP32-S3. Tidak ada serial Arduino, `std::thread`, atau pembacaan sensor dalam
loop polling. Kejadian mengikuti kontrak Python: satu input sentuh yang diterima,
dengan jarak minimum **400 ms** antar kejadian; ini bukan pengenal arah elusan.

Dua backend tersedia melalui `TouchSensorConfig`:

| Backend | Konfigurasi | Jalur interrupt |
| --- | --- | --- |
| `TTP223_GPIO` | `gpio_num`, `active_high` | GPIO rising edge untuk aktif tinggi, falling edge untuk aktif rendah |
| `INTERNAL_PAD` | `pad_channel` 1–14 | ACTIVE interrupt dari touch peripheral ESP32-S3 |

TTP223 adalah modul kapasitif dengan OUT digital; OUT tidak disambungkan ke API
touch internal. Gunakan TTP223 dalam mode momentary, bukan toggle/latching, dengan
level OUT yang sesuai GPIO ESP32-S3. Pin dan pad belum ditentukan oleh modul N16R8;
aplikasi harus memilih pin yang sesuai wiring dan tidak dipakai periferal lain.
Channel internal 0 disediakan untuk denoise dan ditolak sebagai input pengguna.

## Alur dan API

ISR hanya membaca status interrupt bila diperlukan, mengambil timestamp monotonic,
dan memakai `xQueueSendFromISR`. ISR tidak memanggil rendering, NVS, `esp_event`,
alokasi heap, atau log. Semua debounce dan posting `TOUCH_EVENT_TOUCHED` dilakukan
oleh task pemilik di **Core 0** lewat `wait_for_touch()`. Payload adalah salinan
`TouchSensorEvent`: backend, pin/channel, timestamp ISR, dan sequence.

```cpp
TouchSensorConfig config;
config.backend = TouchBackend::TTP223_GPIO;
config.gpio_num = board_touch_gpio; // OUT TTP223 sesuai wiring.
config.event_loop = core0_event_loop;

// Alternatif: pad tembaga yang terhubung langsung ke pad internal ESP32-S3.
// config.backend = TouchBackend::INTERNAL_PAD;
// config.pad_channel = board_touch_channel; // 1..14.
// config.threshold_ratio = 0.20F;
```

Konstruksi objek pada stack task internal atau static RAM; objek yang ditempatkan
di PSRAM ditolak karena ISR mengaksesnya. Queue FreeRTOS juga memakai alokasi
internal. `init`, `wait_for_touch`, `deinit`, dan destructor dijalankan pada **task
Core-0 yang sama**. Antrean event loop dimiliki aplikasi dan harus tetap hidup.

`wait_for_touch(result, portMAX_DELAY, &event_error)` memblokir sampai ada input;
ketika idle tidak ada loop baca GPIO atau pembacaan kapasitansi. Input yang belum
melewati debounce dilewati sambil tetap mematuhi batas waktu tunggu. Timestamp
ISR digunakan agar antrean yang terlambat diproses tidak mengubah jarak sentuhan.
`ESP_OK` berarti input diterima. Jika event loop penuh, `event_error` berisi error,
sementara hasil lokal tetap valid dan debounce tetap terpakai; jangan menggandakan
respons dari hasil lokal dan callback event untuk sentuhan yang sama.

`request_stop()` boleh dipanggil task lain untuk membangunkan pemilik yang sedang
menunggu. Setelah menerima pesan stop, `wait_for_touch()` mengembalikan
`ESP_ERR_INVALID_STATE`; task pemilik kemudian menjalankan `deinit()`. Aplikasi
harus menunggu acknowledgement task selesai sebelum membebaskan objek/args.
Jika antrean raw penuh, permintaan stop bisa menghasilkan `ESP_ERR_TIMEOUT` dan
dapat dicoba kembali. `deinit()` tidak boleh dipanggil bersamaan dengan waiter.

## Pad internal dan lifecycle

Backend internal memakai driver touch v2 `driver/touch_pad.h` yang tersedia pada
ESP-IDF 5.x, dengan API yang diperiksa terhadap ESP-IDF **5.3.3**. Driver touch baru
dari versi IDF berikutnya tidak boleh diinisialisasi bersamaan pada peripheral ini.
Satu instance internal memiliki peripheral touch secara eksklusif.

FSM hardware melakukan pengukuran. Saat inisialisasi, threshold dinonaktifkan,
filter hardware diaktifkan, task menunggu settling **100 ms** sekali, lalu membaca
benchmark sekali. Jauhkan jari dari pad selama settling. Threshold disetel ke
`benchmark * threshold_ratio`: pada ESP32-S3 ini adalah **delta kenaikan** terhadap
benchmark, bukan threshold absolut yang menurun seperti ESP32 generasi pertama.
Tidak ada pembacaan benchmark berkala pada runtime. Rasio dan waktu settling
merupakan konfigurasi karena ukuran pad/kapasitansi tiap board berbeda.

Interrupt TIMEOUT ditangani di task, FSM dilanjutkan, dan API melaporkan
`ESP_ERR_INVALID_RESPONSE` tanpa membuat kejadian sentuh palsu. Jika antrean penuh
dan pesan timeout hilang, flag terpisah memastikan FSM tetap dilanjutkan. Sentuhan
lain yang hilang akibat antrean penuh dihitung oleh `dropped_interrupts()`.

Backend GPIO memasang atau memakai kembali GPIO ISR service global. Cleanup hanya
menghapus handler/pin milik sensor ini, sehingga handler periferal lain tetap ada;
service global tidak di-uninstall otomatis. Service yang sudah ada harus dipasang
oleh aplikasi pada Core 0, agar affinity dan pelepasan callback mengikuti core pemilik.
Objek dan handler beratribut IRAM; penempatan objek pada RAM internal tetap wajib.

Cleanup mematikan sumber interrupt sebelum melepas handler dan antrean. Kegagalan
melepas handler mempertahankan storage agar ISR tidak merujuk memori yang sudah
dibebaskan; `deinit()` dapat dicoba lagi pada task pemilik. Hentikan task secara
kooperatif dan unregister callback aplikasi sebelum membuang konteks callback.

Contoh penghubung sentuh → pet → mata dan tes host berada di
[led_matrix/README.md](../led_matrix/README.md).
Referensi: [touch API ESP32-S3 5.3.3](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/peripherals/touch_pad.html),
[contoh interrupt resmi](https://github.com/espressif/esp-idf/blob/v5.3.3/examples/peripherals/touch_sensor/touch_sensor_v2/touch_pad_interrupt/main/tp_interrupt_main.c).

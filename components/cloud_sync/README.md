# Offline-first cloud sync

Komponen ESP-IDF **5.3–5.x** ini memakai `nvs_flash`, `esp_event`, FreeRTOS, dan
cJSON bawaan ESP-IDF. [config.py](../../app/config.py) belum mempunyai kode Firebase;
modul menambahkan penyimpanan progres dan placeholder tanpa endpoint/token cloud.

## Progres lokal

`save_progress()` memakai umur dari [PetLogic](../pet_logic/include/pet_logic.hpp),
meng-checkpoint state pet, lalu menyimpan detik/hari perawatan, generation,
revision, dan status belum tersinkron ke namespace NVS `cloud_sync`, key
`progress_v1`. Cache RAM diperbarui setelah `nvs_commit()` berhasil.

Hari adalah `floor(care_age_seconds / 86400)`: waktu hidup selama perangkat aktif,
bukan pergantian tanggal kalender. Waktu mati/deep sleep tidak ditambahkan.
Save/load tetap berfungsi tanpa Wi-Fi. Data rusak/versi tidak dikenal dan error
inisialisasi NVS diteruskan tanpa menghapus flash otomatis.

```cpp
#include "cloud_sync.hpp"

// NVS, PetLogic, default event loop Wi-Fi/IP, dan loop pet Core 0 sudah siap.
CloudSyncConfig config;
config.autosave_interval_seconds = 60;
config.retry_interval_ms = 30000;
// config.device_id = "id-perangkat"; // Null: ID stabil dari factory MAC.

static CloudSync cloud;
ESP_ERROR_CHECK(cloud.init(pet, pet_core0_loop, config));
ESP_ERROR_CHECK(cloud.save_progress()); // Misalnya sebelum restart/deep sleep.

SavedProgress saved;
ESP_ERROR_CHECK(cloud.load_progress(saved));
// saved.care_days, saved.care_seconds, saved.pending_sync.
```

Observer state pet melakukan autosave setiap 60 detik secara default. Bila task
pet berhenti mengirim tick, panggil save secara eksplisit. Hindari save setiap
frame karena checkpoint menulis flash. Mati listrik dapat kehilangan perubahan
sejak checkpoint terakhir. Kegagalan autosave dibatasi intervalnya agar event
hasil checkpoint tidak membentuk loop penulisan. Status otomatis terakhir tersedia
melalui `last_storage_error()`; pemanggilan save eksplisit mengembalikan errornya.

## Reset saat mati

Observer otomatis memanggil **`erase_all_memory()`** saat pet mati. State terkini
diperiksa kembali sehingga event kematian lama tidak menghapus pet baru.
Reset berlangsung dalam tiga langkah:

1. Commit penanda reset tertunda, generation/revision baru, dan progres nol.
2. Hapus/commit save pet melalui `PetLogic::erase_memory(false)`.
3. Commit reset selesai dengan `pending_sync` tetap aktif.

Penanda memungkinkan inisialisasi berikutnya melanjutkan reset yang terputus
sebelum upload diizinkan. Ini bukan transaksi atomik lintas namespace. Kegagalan
reset otomatis dicoba lagi maksimal sekali per detik ketika state pet diperbarui.
Namespace `pet_logic` dihapus; metadata reset pada `cloud_sync` dipertahankan agar
progres sebelum kematian tidak dipakai lagi. **Namespace Wi-Fi dan data komponen
lain tetap ada.** Tidak ada pemanggilan `nvs_flash_erase()` untuk seluruh partisi.

Pet tetap mati di RAM. Tambahan flag pada `PetLogic` mencegah tick/checkpoint
membuat ulang save mati yang sudah dihapus. Reboot memulai pet baru karena save
pet sudah tidak ada. Reset manual saat pet hidup langsung membuat pet baru di RAM.
Reset berulang pada pet mati yang sama tidak menggandakan generation.

```cpp
ESP_ERROR_CHECK(cloud.erase_all_memory());
```

## Background Wi-Fi dan placeholder Firebase

`init()` membuat task FreeRTOS **Core 0**, stack 6144 byte, prioritas 2. Handler
Wi-Fi/IP hanya menyetel event-group bits saat mendapat IP, kehilangan IP,
disconnect, atau stop; handler tidak menulis NVS atau mengirim HTTP. Aplikasi
mengatur SSID/password dan koneksi Wi-Fi. Default station key adalah `WIFI_STA_DEF`;
`station_interface_key` dapat diisi untuk interface custom. Koneksi yang sudah
mendapat IP sebelum inisialisasi juga diperiksa.

Worker memblokir ketika offline/tidak ada pekerjaan. Saat online dan ada progres
tertunda, worker menjalankan **`sync_to_firebase()`**, dengan retry 30 detik secara
default. Mendapat IP belum menjamin akses internet; transport nantinya melaporkan
hasil server. `request_sync()` membangunkan worker dari tombol/callback UI.

Default `transport == nullptr` adalah placeholder: JSON disiapkan, metode
mengembalikan `ESP_ERR_NOT_SUPPORTED`, **tidak ada request Firebase**, dan data tetap
`pending_sync`. Status attempt worker terakhir tersedia lewat `last_sync_error()`.
JSON dapat diambil saat offline:

```cpp
char json[512];
ESP_ERROR_CHECK(cloud.get_progress_json(json, sizeof(json)));
// {"schema_version":1,"device_id":"pely_010203040506","care_days":2,
//  "care_seconds":172842,"generation":"1","revision":"3","reset":false}
```

Generation/revision 64-bit dikirim sebagai string agar tidak dibulatkan oleh
JSON/JavaScript. Hari/detik berada dalam rentang integer yang tepat; device ID
di-escape oleh cJSON. Ini adalah snapshot progres terbaru, bukan histori seluruh
aksi atau mekanisme penggabungan antar perangkat.

Untuk menambahkan implementasi Firebase nanti, isi `CloudSyncConfig::transport`:

```cpp
esp_err_t send_firebase_json(const char* json, std::size_t length, void* context);
// config.transport = send_firebase_json;
// config.transport_context = &firebase_client;
```

Transport berjalan di worker di luar mutex; implementasi nantinya menambahkan URL,
autentikasi, HTTPS, timeout, dan validasi acknowledgement server. Kembalikan
`ESP_OK` hanya setelah acknowledgement. Hanya satu transport berjalan sekaligus.
Acknowledgement versi lama tidak membersihkan save/reset baru yang terjadi saat
upload. Backend nantinya juga perlu memeriksa generation/revision agar tidak
menimpa save baru dengan request lama. Tidak ada penarikan state cloud ke pet lokal.

## Lifecycle dan pengujian

Mutation/lifecycle API dijalankan pada Core 0; getter dilindungi mutex. Pet,
event loop, dan context transport harus tetap hidup sampai class dihentikan.
Panggil `stop()` di luar callback/transport setelah menghentikan pemanggil lain,
sebelum menghancurkan pet atau loop. Worker berhenti kooperatif dan handler
dilepas sebelum storage dibebaskan. Pada timeout/error, pertahankan objek dan
coba stop lagi. Jangan menghapus worker paksa saat memegang NVS/request.

Tambahkan `cloud_sync` pada `REQUIRES` aplikasi. Tidak ada library Firebase
eksternal. Root firmware dan koneksi Wi-Fi disiapkan aplikasi; toolchain ESP-IDF
belum tersedia di lingkungan migrasi ini.

Tes host mengompilasi **CloudSync + PetLogic asli**, menggunakan cJSON asli yang
dibundel ESP-IDF serta facade NVS/Wi-Fi/event/FreeRTOS:

```text
python components/cloud_sync/test/host/run_tests.py --cjson-dir PATH_TO_IDF/components/json/cJSON --compiler clang++
```

Jika `IDF_PATH` tersedia, argumen `--cjson-dir` boleh dihilangkan. Tes mencakup
save offline/reboot, JSON, placeholder, upload gagal, acknowledgement lama, reset
kematian, reset terputus, stale event, pembatasan retry, dan shutdown. Facade NVS
memodelkan commit secara deterministik; uji flash/power-loss dan jaringan nyata
tetap perlu dilakukan pada ESP32-S3.

Referensi: [NVS ESP-IDF 5.3.3](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/storage/nvs_flash.html),
[event loop](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/system/esp_event.html),
[Wi-Fi station](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-guides/wifi.html).

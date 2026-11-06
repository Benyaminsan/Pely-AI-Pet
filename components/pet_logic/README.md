# Pet logic (ESP-IDF 5.x)

Port C++ dari [state.py](../../app/pet/state.py) dan
[behavior.py](../../app/pet/behavior.py). Komponen ini tidak
bergantung pada kamera, ESP-DL, UI, atau audio. Katalog makanan diberikan oleh
aplikasi; ID-nya harus sama dengan label keluaran model. Array katalog, string,
alias, dan event loop harus tetap hidup selama objek `PetLogic` dipakai.

## Integrasi

Tambahkan `pet_logic` pada `REQUIRES` komponen pemakai. Contoh untuk firmware
ESP-IDF yang sudah memiliki `main` dan partisi NVS:

```cpp
#include "pet_logic.hpp"
#include "esp_log.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* const kRiceAliases[] = {"egg rice"};
static const PetFood kFoods[] = {
    {"rice_egg", "Rice and Egg", kRiceAliases, 1},
    {"apple", "Apple"},
};

static void pet_event(void*, esp_event_base_t, int32_t id, void* data) {
    const auto& event = *static_cast<const PetLogicEventData*>(data);
    if (id == PET_EVENT_SPEECH_REQUESTED) {
        // Kirim salinan speech, emotion, priority ke antrean komponen audio/UI.
        ESP_LOGI("pet", "%s", event.speech);
    } else if (id == PET_EVENT_DIED) {
        // Tampilkan kondisi mati dan umur perawatan terakhir.
        ESP_LOGI("pet", "Died at age %.1f seconds", event.state.care_age_seconds);
    }
    // Handler harus singkat; jangan melakukan playback/inferensi yang memblokir.
}

static void pet_task(void* argument) {
    auto& pet = *static_cast<PetLogic*>(argument);
    PetSnapshot state;
    ESP_ERROR_CHECK(pet.get_state(state));
    if (state.alive) { ESP_ERROR_CHECK(pet.announce_startup()); }
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const esp_err_t error = pet.update_tick();
        if (error != ESP_OK) {
            ESP_LOGW("pet", "Tick: %s", esp_err_to_name(error));
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}

extern "C" void app_main() {
    // app_main harus berjalan di Core 0 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0).
    // Tangani kegagalan NVS di aplikasi; jangan menghapus seluruh NVS otomatis.
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_event_loop_args_t args{};
    args.queue_size = 24;
    args.task_name = "pet_events";
    args.task_priority = 4;
    args.task_stack_size = 6144; // ESP-IDF menggunakan byte.
    args.task_core_id = 0;
    esp_event_loop_handle_t loop = nullptr;
    ESP_ERROR_CHECK(esp_event_loop_create(&args, &loop));
    ESP_ERROR_CHECK(esp_event_handler_register_with(
        loop, PET_LOGIC_EVENT, ESP_EVENT_ANY_ID, pet_event, nullptr));

    static PetLogic pet;
    ESP_ERROR_CHECK(pet.init(loop, kFoods, sizeof(kFoods) / sizeof(kFoods[0])));
    const BaseType_t created = xTaskCreatePinnedToCore(
        pet_task, "pet_logic", 6144, &pet, 3, nullptr, 0);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
```

Task kamera/inferensi di Core 1 harus mengirim hasil deteksi yang sudah stabil
melalui antrean FreeRTOS atau `esp_event` ke handler Core 0. Handler tersebut
memanggil `handle_confirmed_food(label, outcome)`. Sensor sentuh dapat memanggil
`give_affection()` dari Core 0. Semua perubahan state dilindungi mutex dan
ditolak jika dipanggil dari Core 1; `get_state()` boleh dipanggil dari kedua core.
Semua API digunakan dari task, bukan ISR. Tidak ada task atau loop yang dibuat
diam-diam oleh kelas. Jangan panggil API pet dari handler yang menunggu event lain
dari loop yang sama. Hentikan seluruh pemakai sebelum menghancurkan objek.

## Perilaku dan aturan tambahan

- Skala lapar mengikuti Python: **0 kenyang, 100 kelaparan**. Nilai awal
  lapar/senang/energi adalah 72/65/80; laju pasif 0,15 / -0,25 / -0,05 per detik.
  Penurunan senang dimulai setelah 20 detik tanpa belaian dan berhenti pada 25.
  Jika sudah di bawah 25 akibat makanan salah, tick tidak menaikkannya otomatis.
- Prioritas keinginan: tidur (energi <25), makanan, kasih sayang, bermain.
  Tick menggunakan waktu monotonic `esp_timer_get_time()`; durasi yang melewati
  ambang kasih sayang/kesehatan dihitung hanya setelah ambang terlampaui.
- `feed_success()` mengurangi lapar 30 dan menambah senang 20.
  `feed_wrong()` mengurangi senang 5. Jalur `handle_confirmed_food()` memakai
  penalti 3 seperti `PetBehaviorEngine`, pencocokan ID/nama/alias tanpa membedakan
  kapital, cooldown 3 detik, dan pemilihan makanan baru tanpa mengulang ID lama
  jika katalog memiliki lebih dari satu item. Katalog kosong/label kosong diabaikan.
- Dialog bahasa Inggris, prioritas suara, pengingat 18–25 detik, dan pengumuman
  makanan baru setelah 4 detik dipertahankan. `announce_startup()` mengirim salam
  dan menjadwalkan permintaan pertama setelah 3,5 detik. Belaian berulang tetap
  menambah senang tetapi suara dibatasi cooldown.
- **Aturan kesehatan baru, bukan berasal dari Python:** awal 100; berkurang
  0,20/detik saat lapar >=90 dan 0,10/detik saat energi habis. Kedua laju dijumlahkan.
  Makanan benar memulihkan 5 kesehatan. Semua nilai ini dapat diubah melalui
  `PetLogicConfig`. `rest()` menambah energi agar aplikasi dapat menyediakan aksi tidur.
- Saat kesehatan mencapai 0, mood menjadi `DEAD`, desire menjadi `NONE`, umur
  berhenti, dan makanan/belaian/istirahat tidak dapat menghidupkannya kembali.
  Tanpa koordinator reset, memori kematian disimpan segera dan dipulihkan saat
  reboot. Tidak ada suara
  atau tindakan perawatan lanjutan pada pet mati. Aplikasi menentukan tampilan kematian.

## Memori dan event

Umur adalah total **waktu hidup saat perangkat aktif**, termasuk saat pet
diistirahatkan. Waktu perangkat mati/deep sleep tidak dihitung karena belum ada
RTC/SNTP dalam modul sumber. NVS menyimpan satu blob berversi dalam namespace
`pet_logic`: metrik, umur, waktu tanpa interaksi/belaian, penghitung perawatan,
mood, dan ID makanan yang diminta. Penulisan otomatis setiap 60 detik, serta
segera saat mati; panggil `save_memory()` sebelum restart/deep sleep terencana.
Mati listrik dapat kehilangan perubahan sejak checkpoint terakhir. Jika penulisan
kematian gagal, tick berikutnya mencoba lagi dan API mengembalikan error NVS.
Data rusak atau versi tidak dikenal menghasilkan error tanpa menghapus memori.

`erase_memory()` menjalankan `nvs_erase_all()` pada handle namespace pet, kemudian
`nvs_commit()`. Setelah keduanya berhasil, state RAM menjadi pet baru dengan umur
0. Pengaturan Wi-Fi dan namespace lain tetap ada. Data pet baru akan ditulis pada
checkpoint berikutnya; reboot sebelum checkpoint juga memulai pet baru. Jika
erase/commit gagal, state RAM tidak direset; aplikasi harus menangani error.

`erase_memory(false)` tersedia khusus untuk pet mati: hapus save di NVS, pertahankan
state mati di RAM, dan cegah checkpoint berikutnya membuat ulang save tersebut.
`save_memory()` kemudian menghasilkan `ESP_ERR_INVALID_STATE` sampai pet baru
dibuat lewat `erase_memory()` atau reboot. Koordinator
[cloud_sync](../cloud_sync/README.md) menggunakan jalur ini untuk reset otomatis
ketika pet mati. Tanpa koordinator, penghapusan tetap dipicu aplikasi secara eksplisit.

Semua event memakai `PET_LOGIC_EVENT` dan payload `PetLogicEventData`:

| ID | Isi |
| --- | --- |
| `PET_EVENT_STATE_CHANGED` | Snapshot setelah tick/tindakan |
| `PET_EVENT_FOOD_EVALUATED` | Outcome, label deteksi, ID permintaan sebelum rotasi |
| `PET_EVENT_SPEECH_REQUESTED` | Teks, emosi, prioritas, snapshot |
| `PET_EVENT_DIED` | Snapshot final; juga dikirim sekali setelah memuat pet mati |
| `PET_EVENT_MEMORY_ERASED` | Snapshot setelah erase; dapat tetap mati bila restart_pet=false |

Payload disalin oleh `esp_event`; jangan menyimpan pointer callback. Pengiriman
tidak memblokir. `ESP_ERR_TIMEOUT` berarti antrean event penuh; tindakan state
**dapat sudah berhasil**, sehingga jangan mengulang pemberian makan/aksi lain
berdasarkan error pengiriman saja. `outcome` tetap menyatakan hasil deteksi.
Event mati dicoba kembali sampai berhasil masuk antrean. Event lain dapat hilang;
UI dapat mengambil `get_state()` dan snapshot pada tick selanjutnya. Salam tidak
otomatis dikirim setelah erase; panggil `announce_startup()` jika diinginkan.

## Verifikasi

Untuk firmware, jalankan `idf.py set-target esp32s3` dan `idf.py build` dari root
proyek ESP-IDF. Entrypoint [main/main.cpp](../../main/main.cpp) sekarang menyediakan
root firmware dan task integrasi. Toolchain/model asli tetap diperlukan;
aplikasi Python tidak diganti oleh komponen ini.

Tes host mengompilasi implementasi C++ asli dengan tiruan clock, core, mutex,
event queue, dan NVS. Jalankan dengan compiler GCC/Clang yang tersedia:

```text
python components/pet_logic/test/host/run_tests.py
python components/pet_logic/test/host/run_tests.py --compiler clang++
```

Tes memeriksa perilaku Python, cooldown, pengingat, threshold kesehatan,
kematian, reboot, erase terisolasi, error NVS, event penuh, dan pembatasan core.
Tes host tidak menggantikan build ESP-IDF atau pengujian SMP/flash pada perangkat.

Referensi API: [ESP-IDF 5.3.3 NVS](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/storage/nvs_flash.html),
[event loop](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/system/esp_event.html),
dan [FreeRTOS](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/system/freertos_idf.html).

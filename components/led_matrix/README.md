# WS2812 eye matrix

Komponen ESP-IDF **5.3–5.x** untuk ESP32-S3, menggunakan library terpin
`espressif/led_strip` **3.0.3** dengan backend **RMT**, WS2812 GRB, resolusi RMT
10 MHz, dan DMA aktif secara default. Library dipasang melalui
[idf_component.yml](idf_component.yml). Tidak memakai FastLED atau bit-banging.

[eye_animation.hpp](include/eye_animation.hpp) mem-port seluruh fungsi matematika
dari [animations.py](../../app/eyes/animations.py): lerp, cubic ease-in/out,
elastic easing, smoothstep, serta kurva buka-tutup kedipan. Renderer memakai
interpolasi dan kurva kedipan tersebut untuk menampilkan dua mata pada matriks.
Ini merupakan rasterisasi baru untuk LED resolusi rendah; subsistem gaze,
micro-motion, dan antrean prioritas controller Pygame belum dipindahkan di modul ini.

## Konfigurasi dan pemakaian

```cpp
LedMatrixConfig config;
config.gpio_num = board_ws2812_gpio;
config.width = 16;
config.height = 8;
config.serpentine = true;
config.brightness = 24; // Skala global 0..255, diterapkan sekali saat transmisi.

// Jika dua panel 8x8 dirangkai, bukan satu panel serpentine 16x8:
// config.tile_width = 8;
// config.tile_height = 8;

LedMatrix eyes;
ESP_ERROR_CHECK(eyes.init(config)); // Core 0.
ESP_ERROR_CHECK(eyes.show_emotion(Emotion::HAPPY));
// eyes.show_emotion(Emotion::SAD);
// eyes.show_emotion(Emotion::ANGRY);
```

Ukuran default **16x8** adalah asumsi yang bisa diganti; GPIO sengaja belum diisi.
Konfigurasi mendukung 8–64 kolom, 4–32 baris, maksimum 1024 pixel. Lebar/tinggi tile
harus membagi ukuran canvas. Pixel dalam tile dapat berurutan per baris atau
kolom, lurus atau serpentine; urutan tile sendiri per baris dengan opsi serpentine.
`mirror_x/mirror_y` membalik canvas logis sebelum pemetaan. Geometri mata mengikuti
ukuran canvas; tidak ada array bitmap yang mengasumsikan wiring board tertentu.

Ekspresi tersedia: `IDLE`, `HAPPY`, `SAD`, `ANGRY`, `HUNGRY`, `LONELY`, `SLEEPY`,
`SURPRISED`, `CONFUSED`, dan `DEAD`. Senang memakai lengkungan mata hijau,
sedih memakai mata biru dengan kelopak turun, dan marah memakai kelopak miring
merah. Palette senang/sedih/marah/idle dapat dikonfigurasi. Mata mati berupa silang
statis dan tidak berkedip. Elusan memicu `HAPPY` pada contoh integrasi; `ANGRY`
tersedia untuk respons yang dipilih aplikasi.

`show_emotion(Emotion state)` memilih ekspresi, memulai crossfade **280 ms**, lalu
mentransmisikan frame awal. Pemanggilan dengan state sama tidak mengulang timer.
Panggil `update_tick()` berkala dari task FreeRTOS Core 0, misalnya 30 FPS, untuk
menjalankan crossfade, bounce ringan saat senang, dan kedipan otomatis 2,5–6 detik.
Tidak ada `sleep` atau loop tunggu dalam animasi. State `DEAD` diterapkan langsung.
Kembalinya emosi sementara ke mood dasar diputuskan orchestrator aplikasi.

`set_brightness()` melakukan skala dari framebuffer logis asli sehingga tidak
menggelapkan warna berulang-ulang. `clear()` mematikan LED dan menjeda animasi
sampai `show_emotion()` berikutnya. Buffer kecil berada di RAM internal; buffer
DMA/transmisi dikelola `led_strip`. RMT dipasang saat `init()` pada Core 0.

Seluruh API hardware dipanggil pada **Core 0 dan bukan dari ISR**. Mutex
menserialisasi callback event dan task renderer. `led_strip_refresh()` menunggu
transmisi selesai; waktu frame bertambah sesuai jumlah LED (sekitar 3,84 ms data
untuk 128 WS2812, ditambah reset). Pilih interval tick yang cukup untuk matriks
yang dipakai. Error konfigurasi, alokasi, driver, dan transmisi dikembalikan sebagai
`esp_err_t`. Jika refresh awal gagal, handle tetap disimpan untuk retry/deinit.
Hentikan semua pemakai sebelum `deinit()` atau menghancurkan objek.

## Penghubung sentuh, pet, dan mata

Contoh callback berikut dijalankan pada **event loop Core 0**. Context, pet, dan
mata sudah diinisialisasi dan harus tetap hidup selama handler terdaftar.
Sentuhan dibatasi debounce oleh komponen sensor; menjaga mata senang selama reaksi
sementara juga mencegah tick mood lapar langsung menimpanya.

```cpp
#include "led_matrix.hpp"
#include "touch_sensor.hpp"
#include "pet_logic.hpp"
#include "esp_timer.h"

struct PetIoContext {
    PetLogic* pet;
    LedMatrix* eyes;
    int64_t happy_until_us = 0; // Hanya diakses handler event loop yang sama.
};

static Emotion mood_emotion(PetMood mood) {
    switch (mood) {
    case PetMood::HUNGRY: return Emotion::HUNGRY;
    case PetMood::HAPPY: return Emotion::HAPPY;
    case PetMood::SAD: return Emotion::SAD;
    case PetMood::LONELY: return Emotion::LONELY;
    case PetMood::SLEEPY: return Emotion::SLEEPY;
    case PetMood::DEAD: return Emotion::DEAD;
    }
    return Emotion::IDLE;
}

static void on_touch(void* argument, esp_event_base_t, int32_t, void*) {
    auto& io = *static_cast<PetIoContext*>(argument);
    const esp_err_t action = io.pet->give_affection();
    (void)action; // Aplikasi dapat mencatat error NVS/event tanpa mengulang sentuhan.
    PetSnapshot state;
    if (io.pet->get_state(state) != ESP_OK) { return; }
    io.happy_until_us = state.alive ? esp_timer_get_time() + 2800000 : 0;
    io.eyes->show_emotion(state.alive ? Emotion::HAPPY : Emotion::DEAD);
}

static void on_pet_state(void* argument, esp_event_base_t, int32_t id, void* data) {
    auto& io = *static_cast<PetIoContext*>(argument);
    if (id != PET_EVENT_STATE_CHANGED && id != PET_EVENT_DIED && id != PET_EVENT_MEMORY_ERASED) { return; }
    const auto& event = *static_cast<const PetLogicEventData*>(data);
    if (!event.state.alive || id == PET_EVENT_MEMORY_ERASED) { io.happy_until_us = 0; }
    if (esp_timer_get_time() >= io.happy_until_us) {
        io.eyes->show_emotion(mood_emotion(event.state.mood));
    }
}

// Pendaftaran dilakukan setelah pet/eyes siap; periksa hasil di aplikasi.
// esp_event_handler_register_with(loop, TOUCH_SENSOR_EVENT, TOUCH_EVENT_TOUCHED, on_touch, &io);
// esp_event_handler_register_with(loop, PET_LOGIC_EVENT, ESP_EVENT_ANY_ID, on_pet_state, &io);
```

Task sensor menunggu antrean ISR, sementara task mata berjalan periodik:

```cpp
struct TouchTaskArgs { TouchSensor* sensor; TouchSensorConfig config; };

static void touch_task(void* argument) {
    auto& args = *static_cast<TouchTaskArgs*>(argument);
    ESP_ERROR_CHECK(args.sensor->init(args.config));
    for (;;) {
        TouchSensorEvent touch;
        esp_err_t post_error = ESP_OK;
        const esp_err_t error = args.sensor->wait_for_touch(touch, portMAX_DELAY, &post_error);
        if (error == ESP_ERR_INVALID_STATE) { break; } // request_stop().
        // Aplikasi mencatat error hardware/post_error; respons lewat on_touch saja.
    }
    ESP_ERROR_CHECK(args.sensor->deinit());
    // Kirim ack aplikasi sebelum task dihapus; args/sensor dimiliki pemanggil.
    vTaskDelete(nullptr);
}

static void eyes_task(void* argument) {
    auto& eyes = *static_cast<LedMatrix*>(argument); // Sudah init pada Core 0.
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const esp_err_t error = eyes.update_tick();
        (void)error; // Catat/tangani error driver sesuai kebijakan aplikasi.
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(33));
    }
}

// Sensor/args/eyes harus punya lifetime yang melampaui task.
// Periksa pdPASS dan simpan handle task pada aplikasi:
// xTaskCreatePinnedToCore(touch_task, "touch", 4096, &touch_args, 4, &touch_handle, 0);
// xTaskCreatePinnedToCore(eyes_task, "eyes", 4096, &eyes, 3, &eyes_handle, 0);
```

Task mata pada contoh berjalan sepanjang lifetime perangkat. Untuk shutdown dinamis,
tambahkan stop/ack pada scheduler UI, lalu unregister handler dan deinit perangkat;
jangan menghapus task yang sedang mengakses LED atau queue secara paksa. Integrasi
root firmware sekarang tersedia di [main/main.cpp](../../main/main.cpp); wiring
diisi lewat menuconfig aplikasi.

## Build dan pengujian

Komponen aplikasi menambahkan `led_matrix`, `touch_sensor`, dan jika memakai
contoh pet, `pet_logic` pada `REQUIRES`. Event loop dibuat aplikasi dengan
`task_core_id = 0`. ESP-IDF Component Manager akan mengambil `led_strip` 3.0.3;
backend pad internal memakai peripheral driver bawaan ESP-IDF.

Tes host mengompilasi kedua implementasi C++ dan menyimulasikan GPIO/touch ISR,
antrean, waktu, allocator, mutex, event, serta output RMT:

```text
python components/led_matrix/test/host/run_tests.py --compiler clang++
```

Opsional, periksa juga API dengan header asli `led_strip` 3.0.3 yang sudah
diunduh, atau hasilkan contact sheet PPM untuk pemeriksaan raster:

```text
python components/led_matrix/test/host/run_tests.py --led-strip-headers PATH_TO_LED_STRIP_INCLUDE --preview eyes.ppm --compiler clang++
```

Tes memeriksa debounce seperti Python, ISR yang menunda pekerjaan, wait blocking,
threshold pad S3, timeout saat queue penuh, event penuh, cleanup/retry, rumus
animasi, ekspresi, crossfade, blink, brightness, wiring, dan pembatasan Core 0.
Tes host tidak mengukur sinyal WS2812, noise kapasitif, atau timing hardware/SMP.
Uji perangkat masih membutuhkan GPIO/layout asli dan toolchain ESP-IDF.

Referensi: [led_strip 3.0.3](https://components.espressif.com/components/espressif/led_strip/versions/3.0.3/readme),
[RMT ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/peripherals/rmt.html),
dan [interrupt touch ESP32-S3](https://docs.espressif.com/projects/esp-idf/en/v5.3.3/esp32s3/api-reference/peripherals/touch_pad.html).

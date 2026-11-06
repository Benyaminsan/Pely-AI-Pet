# FOMO vision AI

Komponen ESP-IDF **5.3–5.x** untuk ESP32-S3 dengan `espressif/esp-dl` **3.3.14**.
Dependency dikelola melalui [idf_component.yml](idf_component.yml); komponen
pemakai cukup menambahkan `vision_ai` pada `REQUIRES`.

Pipeline:

1. Pinjam frame RGB565 dari `CameraHal`, atau berikan `Rgb565FrameView` milik aplikasi.
2. Center-crop sesuai rasio input (default) atau stretch, lalu resize bilinear.
3. Ubah RGB565 ke RGB dan kuantisasi langsung ke tensor input INT8.
4. Jalankan `dl::Model::run(dl::RUNTIME_MODE_SINGLE_CORE)` pada task Core 1.
5. Dekode heatmap FOMO, gabungkan sel tetangga dengan kelas sama, lalu kembalikan
   hingga delapan deteksi terkuat dalam koordinat frame asal.

ORB, pencarian layar putih, YOLO, dan simulasi keyboard dari prototipe Python
digantikan oleh FOMO. Model perlu dilatih dengan kelas background dan contoh
negatif yang sesuai; hasil ini tidak mewarisi pembatas layar putih milik Python.
Temporal confirmation dari [stabilizer.py](../../app/vision/stabilizer.py) masih
menjadi tahap aplikasi sebelum hasil diteruskan ke pet logic.

## Kontrak model

Sediakan header **`assets/model/fomo_model.h`** pada root proyek, berisi definisi
lengkap array **`const unsigned char fomo_model_int8[]`**. Array harus menyimpan
isi file **`.espdl`**, hasil ekspor/kuantisasi ESP-DL untuk ESP32-S3. Menamai array
TFLite INT8 sebagai `fomo_model_int8` tidak mengubah formatnya menjadi ESP-DL.
Header belum ada di repositori saat modul ini dibuat; CMake mengizinkan menuconfig
dan memberi peringatan, sedangkan kompilasi meminta header asli tersebut.
Tidak ada bobot model pengganti dalam firmware.

```cpp
// Bentuk deklarasi yang dianjurkan untuk export header (isi adalah bobot asli):
// alignas(16) const unsigned char fomo_model_int8[] = { ... };
```

Hanya container model tunggal **EDL1/EDL2 tanpa enkripsi** yang diterima. Packed
model PDL/enkripsi tidak didukung wrapper ini. Header container diperiksa untuk
panjang dan mode; ini bukan verifier menyeluruh terhadap graph/FlatBuffers yang
rusak. Gunakan export valid dengan operator yang didukung ESP-DL. Array yang belum
aligned 16 byte disalin sekali ke PSRAM dan dipertahankan sampai model dihancurkan.

Tensor yang didukung:

| Tensor | Bentuk dan tipe |
| --- | --- |
| Input | INT8 NHWC `[1, H, W, 3]`, default `[1, 96, 96, 3]` |
| Output | INT8 NHWC `[1, grid_h, grid_w, jumlah_channel]` |
| Kuantisasi | Per-tensor simetris dengan scale `2^exponent`, zero point 0 |
| Kelas | Satu channel background dan minimal satu channel makanan |

Ukuran input harus cocok dengan `VisionAiConfig::input_width/input_height`;
ukuran heatmap dibaca dari tensor, tidak diasumsikan 12x12. Tensor lain, beberapa
input/output, grayscale/NCHW, dan per-channel activation exponent ditolak secara
eksplisit. Model INT8 dengan output INT16/float membutuhkan penyesuaian decoder.
Kesalahan load/operator tercermin sebagai tensor I/O kosong/tidak sesuai.
ESP-DL sendiri dapat menghentikan program pada sebagian kegagalan alokasi internal;
tes host tidak menjamin pemulihan semua kondisi kehabisan memori di library tersebut.

Urutan `VisionAiConfig::labels` harus **sama persis** dengan channel output model,
termasuk background. Background default di channel 0, tetapi indeksnya dapat
diubah. ID makanan harus cocok dengan ID katalog pet; jangan menganggap urutan
JSON katalog otomatis sama dengan urutan kelas training.

Normalisasi default adalah `real = pixel / 255`, kemudian:

```text
q_input = clamp(round(real / 2^input_exponent), -128, 127)
real_output = q_output * 2^output_exponent
```

Isi `input_scale[3]`, `input_offset[3]`, `bgr_input`, dan mode resize sesuai
preprocessing saat training/export. Contoh normalisasi `[-1,1]` memakai scale
`1/127.5` dan offset `-1` pada setiap channel. Wrapper tidak menebak normalisasi
dari nama file. Gradasi RGB565 diperluas ke 8 bit sebelum interpolasi.

Output default berupa **probabilitas setelah softmax**. Nilai dekuantisasi di luar
0–1 ditolak agar logits tidak dianggap confidence. Jika graph mengeluarkan logits,
pilih `FomoOutputMode::SOFTMAX_LOGITS`; wrapper menerapkan softmax stabil per sel.
Background yang menang atau seri dengan kelas lain tidak menghasilkan deteksi.

## Hasil dan memori

`infer_rgb565(frame, result, &event_error)` mengisi `VisionResult`. `ESP_OK` dengan
`count == 0` berarti tidak ada kartu. Error input/inferensi tidak meninggalkan
deteksi dari frame sebelumnya. `detections[0]` adalah cluster dengan confidence
terbesar; setiap item berisi ID/nama makanan, channel kelas, confidence puncak,
jumlah sel, centroid berbobot confidence, dan region sel aktif. `x2/y2` eksklusif.

**FOMO mendeteksi centroid, bukan ukuran kotak objek.** Region `x1/y1/x2/y2`
merupakan cakupan sel heatmap yang aktif, bukan batas kartu makanan yang presisi.
Sel dengan kelas sama digabung memakai delapan tetangga; dua objek kelas sama
yang menyentuh dapat menyatu. `minimum_cluster_cells` menyaring cluster kecil.
`truncated` menandakan jumlah cluster valid melebihi kapasitas delapan hasil.

Frame kamera, arena model (budget internal 0), serta buffer kerja decoder
ditempatkan/diminta di PSRAM. Salinan parameter dikelola allocator ESP-DL;
wrapper tidak memaksa setiap alokasi kecil library ke PSRAM. Buffer kerja dibuat sekali di
`init()`. Input ditulis langsung pada tensor model setiap frame karena arena
ESP-DL dapat menimpa tensor input setelah inferensi. Tidak ada salinan frame
RGB888 ukuran penuh. Stack worker, objek kontrol, dan metadata kecil tetap memakai
RAM normal. Kelas mengikat satu task pemilik di Core 1; `init`, inferensi,
`deinit`, dan destructor harus berjalan pada task yang sama.

## Integrasi FreeRTOS dan event loop

Contoh berikut dipasang pada komponen aplikasi. `VisionTaskArgs` dan semua array
label/string harus tetap hidup sampai task berhenti. Isi pin board, metadata
model, dan normalisasi **sebelum** memulai task. Event loop dibuat aplikasi dengan
`task_core_id = 0`; jangan mengirim pointer framebuffer ke loop ini.

```cpp
#include "camera_hal.hpp"
#include "vision_ai.hpp"
#include "esp_log.h"

struct VisionTaskArgs {
    CameraHalConfig camera;
    VisionAiConfig vision;
};

static void on_vision(void*, esp_event_base_t, int32_t, void* payload) {
    const auto& result = *static_cast<const VisionResult*>(payload);
    if (result.count) {
        const auto& best = result.detections[0];
        ESP_LOGI("vision", "%s %.2f centroid=(%.1f,%.1f)",
                 best.food_id, best.confidence, best.center_x, best.center_y);
        // Temporal confirmation di Core 0, lalu PetLogic::handle_confirmed_food().
    }
    // count=0 juga diproses agar kandidat lama tidak menetap.
    // Salin payload jika ingin menyimpannya setelah callback selesai.
}

static void vision_worker(void* argument) {
    const auto& args = *static_cast<const VisionTaskArgs*>(argument);
    { // Scope memastikan destructor berjalan SEBELUM vTaskDelete().
        CameraHal camera;
        VisionAI ai;
        esp_err_t error = camera.init(args.camera);
        if (error == ESP_OK) { error = ai.init(args.vision); }
        if (error != ESP_OK) {
            ESP_LOGE("vision", "Init: %s", esp_err_to_name(error));
        } else {
            // Task notification dipakai untuk permintaan stop secara kooperatif.
            while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(80)) == 0) {
                CameraHal::Frame frame;
                error = camera.capture(frame);
                if (error == ESP_OK) {
                    VisionResult result;
                    esp_err_t event_error = ESP_OK;
                    error = ai.infer_rgb565(frame.view(), result, &event_error);
                    if (event_error != ESP_OK) {
                        ESP_LOGW("vision", "Event: %s", esp_err_to_name(event_error));
                    }
                }
                if (error != ESP_OK) {
                    ESP_LOGW("vision", "Frame: %s", esp_err_to_name(error));
                }
                // Lease dilepas otomatis; tidak ada buffer yang tertahan saat delay.
            }
        }
    }
    vTaskDelete(nullptr);
}

esp_err_t start_vision(VisionTaskArgs& persistent_args,
                       esp_event_loop_handle_t core0_loop, TaskHandle_t& task) {
    persistent_args.vision.event_loop = core0_loop;
    esp_err_t error = esp_event_handler_register_with(
        core0_loop, VISION_AI_EVENT, VISION_EVENT_RESULT, on_vision, nullptr);
    if (error != ESP_OK) { return error; }
    if (xTaskCreatePinnedToCore(vision_worker, "food_vision", 8192,
                               &persistent_args, 4, &task, 1) != pdPASS) {
        esp_event_handler_unregister_with(core0_loop, VISION_AI_EVENT,
                                         VISION_EVENT_RESULT, on_vision);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
```

Pemanggil mengirim `xTaskNotifyGive(task)` untuk meminta stop; selesaikan protokol
join/ack aplikasi sebelum membebaskan args, menghapus event loop, atau memakai
kembali handle task. Capture yang sedang berjalan harus selesai/timeout dahulu.
Jangan menghentikan worker paksa dari core lain saat memegang buffer atau model.
Contoh ini bukan pengelola lifecycle task lengkap. Root firmware menggunakan
integrasi tersendiri pada [main/main.cpp](../../main/main.cpp).

Jika `event_loop` di konfigurasi null, hasil hanya dikembalikan lewat argumen.
Pengiriman event memakai timeout 0. Antrean penuh dilaporkan lewat `event_error`,
sementara fungsi tetap mengembalikan `ESP_OK` jika inferensi berhasil; hasil lokal
masih valid. Event berisi salinan `VisionResult`, tanpa pointer pixel atau label
pinjaman. Audio/UI harus segera mengantrekan pekerjaannya sendiri dari callback.

## Build dan pengujian

Gabungkan [konfigurasi kamera/PSRAM](../camera_hal/sdkconfig.defaults.example),
sediakan model asli dan metadata kelas, lalu jalankan `idf.py set-target esp32s3`
dan `idf.py build` dari root firmware ESP-IDF. Panduan root project tersedia di
[main/README.md](../../main/README.md); kode Python tetap tersedia.

Tes host mengompilasi **kedua implementasi C++ asli**, dengan facade camera,
FreeRTOS, clock, allocator, event, dan `dl::Model` yang deterministik:

```text
python components/vision_ai/test/host/run_tests.py --compiler clang++
```

Tes mencakup RGB565/endian, bilinear/stride/crop, kuantisasi, pengisian ulang arena,
decoder FOMO, confidence/background/logits, kapasitas hasil, lifecycle framebuffer,
error alokasi, event penuh, dan kepemilikan task. Fixture model hanya dibuat dalam
folder sementara untuk facade host, bukan bobot yang dapat dipakai ESP-DL.
Varian tambahan memeriksa array yang tidak aligned, penolakan format TFLite,
dan penolakan panjang container model yang terpotong.
Tes ini tidak menjalankan kernel jaringan ESP-DL atau membuktikan akurasi/FPS.
Uji hardware masih memerlukan model asli, wiring OV5640, dan toolchain ESP-IDF.

Referensi: [Model API](https://docs.espressif.com/projects/esp-dl/en/latest/api_reference/model_api.html),
[kuantisasi dan inferensi](https://docs.espressif.com/projects/esp-dl/en/latest/tutorials/how_to_run_model.html),
[source ESP-DL 3.3.14](https://github.com/espressif/esp-dl/tree/v3.3.14/esp-dl),
dan [esp32-camera 2.1.8](https://github.com/espressif/esp32-camera/tree/v2.1.8).

# OV5640 camera HAL

Komponen ESP-IDF **5.3–5.x**, target ESP32-S3, dengan dependency terpin
`espressif/esp32-camera` **2.1.8**. Frame kamera berupa RGB565 QVGA (320x240)
secara default, satu framebuffer di PSRAM, dengan urutan byte tinggi lebih dulu.
Resolusi 96x96 sampai QVGA pada enum driver boleh dipilih melalui konfigurasi.

Isi seluruh pin wajib pada `CameraHalConfig::pins` sesuai skematik board.
`ESP32-S3-N16R8` tidak menentukan pemetaan pin kamera. Nilai awal -1 sengaja
menghasilkan `ESP_ERR_INVALID_ARG`; pin PWDN/RESET boleh tetap -1 jika tidak
terhubung. HAL memeriksa GPIO yang valid dan pin duplikat, tetapi aplikasi tetap
harus menghindari pin yang dipakai flash/PSRAM/periferal board lainnya.

Gabungkan [sdkconfig.defaults.example](sdkconfig.defaults.example) ke konfigurasi
firmware. Untuk N16R8, contoh mengaktifkan PSRAM octal dan ukuran flash 16 MB.
Jika firmware sudah memiliki `sdkconfig`, ubah juga pilihan yang sama lewat
`idf.py menuconfig`: defaults tidak menimpa konfigurasi yang sudah ada.
**`CONFIG_CAMERA_CORE1=y` wajib** karena driver membuat task internalnya sendiri;
menempatkan task aplikasi di Core 1 saja belum memindahkan task internal driver.
HAL menolak inisialisasi jika pilihan ini tidak aktif atau PSRAM tidak tersedia.
PSRAM DMA tetap mengikuti konfigurasi driver; framebuffer di PSRAM tidak berarti
semua descriptor/DMA buffer atau stack task juga harus ditempatkan di PSRAM.

`init()`, `capture()`, `deinit()`, pemindahan/pengembalian frame, dan penghancuran
objek dilakukan pada **satu task pemilik di Core 1**. Sensor harus terdeteksi
sebagai OV5640; sensor lain menghasilkan `ESP_ERR_NOT_SUPPORTED` dan driver
dibersihkan. Mirror/flip dapat dikonfigurasi dan secara default nonaktif.
Autofocus bukan bagian dari HAL ini; mode fokus mengikuti modul lensa/driver.

```cpp
CameraHal camera;
ESP_ERROR_CHECK(camera.init(board_camera_config)); // Di dalam task Core 1.
{
    CameraHal::Frame frame;
    ESP_ERROR_CHECK(camera.capture(frame));
    const Rgb565FrameView pixels = frame.view();
    // Gunakan pixels untuk inferensi sebelum keluar dari scope ini.
} // Frame otomatis dikembalikan dengan esp_camera_fb_return().
ESP_ERROR_CHECK(camera.deinit());
```

`Frame` tidak dapat disalin, tetapi dapat dipindahkan. HAL menolak capture kedua
dan deinit selama lease masih dipegang. View hanyalah pointer pinjaman; jangan
mengantrekannya ke UI atau menyimpannya setelah lease dilepas. `capture()` dapat
memblokir sampai driver menyerahkan frame/timeout, sehingga harus dijalankan di
worker, bukan event handler UI. HAL tidak membuat gambar pengganti ketika kamera
gagal; error diserahkan ke aplikasi.

Contoh pipeline dan pengujian host tersedia di
[vision_ai/README.md](../vision_ai/README.md).
Rujukan driver: [README esp32-camera 2.1.8](https://github.com/espressif/esp32-camera/blob/v2.1.8/README.md),
[API kamera](https://github.com/espressif/esp32-camera/blob/v2.1.8/driver/include/esp_camera.h),
dan [pilihan core task](https://github.com/espressif/esp32-camera/blob/v2.1.8/Kconfig).

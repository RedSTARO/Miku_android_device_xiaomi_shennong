# Device tree for Xiaomi 14 Pro

Miku UI Cinderella (Android 17 / C) port of
[kmiit's lineage-23.0 device tree](https://github.com/kmiit/android_device_xiaomi_shennong),
with the kernel built from source. Use this device repository's `Cinderella` branch with
the [Miku UI Cinderella manifest](https://github.com/Miku-UI/manifesto/tree/Cinderella)
(`android-17.0.0_r1` base) and its `cp2a` release configuration.
[MikuUIBuilder](https://github.com/RedSTARO/MikuUIBuilder) remains on branch `shennong`:

```bash
. build/envsetup.sh
lunch miku_shennong-cp2a-userdebug
make diva
```

This branch preserves the fingerprint, sensor and kernel fixes from the Android 16 port.
It remains an upgrade of an API 34 launch device: shipping API 34, FCM 8 and fingerprint
AIDL V4 are retained. The previous host regression and Android 16 native compilation
results do not establish Android 17 build compatibility; that validation is pending a
complete Cinderella source sync and build.

> [!WARNING]
> **Not tested on a device yet.** Known issues:
> - Camera and torch: kmiit's builds had no working camera. The camera device tree and driver
>   were reworked against stock, still unverified.
> - OTG power: kmiit's builds did not power OTG devices. `init.shennong.rc` now enables the
>   charger firmware's `cc_toggle` (Type-C role toggling) at boot, still unverified.
> - Missing Xiaomi-only features: satellite (Tiantong) calls, touch hand-hold sensor, HyperOS
>   charging/touch/display extras.
>
> Maybe more

Xiaomi 14 Pro (codenamed _"shennong"_) is a high-end smartphone from Xiaomi.

It was announced & released in October 2023.

## Device specifications

|      Basic | Spec Sheet                                                        |
| ---------: | :---------------------------------------------------------------- |
|        SoC | Snapdragon® 8 Gen 3 (SM8650-AB)                                   |
|        CPU | Octa-core CPU with 1x Cortex-X4 & 5x Cortex-A720 & 2x Cortex-A520 |
|        GPU | Adreno 750 (770 MHz)                                              |
|     Memory | 12/16GB RAM (LPDDR5X 8533Mbps)                                  |
| Shipped OS | 14.0 with HyperOS 1.0                                             |
|    Storage | 256/512/1024 GB (UFS 4.0)                                         |
|    Battery | 4880 mAh, non-removable, 120W wired/50W wireless fast-charge       |
|    Display | 3200x1440 pixels, 6.73 inches, 120 Hz, 12bit, LTPO, DCI-P3        |
|     Camera | 50MP primary, 50MP ultra-wide, 75mm floating-telephoto            |

![Xiaomi 14 Pro](https://cdn.cnbj0.fds.api.mi-img.com/b2c-shopapi-pms/pms_1698304641.51322936.png)

## Installation

Neither the vendor repository nor the zip carries the firmware partitions (abl, xbl, modem,
dsp, bluetooth, keymaster, tz, ... in `proprietary-firmware.txt`). The blobs are from HyperOS
`OS2.0.217.0.VNBCNXM` and need that firmware on **both** slots: sideloading the zip writes the
inactive slot and boots from it, with whatever firmware that slot has. A stock OTA only updates
one slot, so the other one usually still holds older firmware.

1. Flash the stock `OS2.0.217.0.VNBCNXM` fastboot ROM with its `flash_all` script (it writes the
   firmware to both slots with `fastboot flash <partition>_ab`), or flash every firmware image
   of that ROM to `<partition>_ab` yourself.
2. Flash `boot`, `dtbo`, `init_boot`, `vbmeta`, `vendor_boot` and `recovery` of this build,
   format data in recovery and sideload the zip.

Factory reset protection is enabled: remove the Google account from stock HyperOS before
flashing, or setup will ask for it.

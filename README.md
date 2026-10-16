### Samsung Galaxy Tab A 10.1 2019 Wifi (SM-T510) pmaports

Currently: Linux 6.15

#### Features

| Feature            | Status   | Notes |
|--------------------|----------|---------------------|
| Flashing           | ⬜U | use recovery zip for now lol. |
| USB Networking     | 🟩W |
| Internal storage   | 🟩W |
| SD card            | 🟩W |
| Battery            | 🟩W |
| Screen             | 🟩W |
| Touchscreen        | 🟩W |
| **Multimedia**     | |
| 3D Acceleration    | 🟩W | 
| Audio              | 🟩W | 
| Camera             | 🟩W |
| **Connectivity**   | |
| WiFi               | 🟩W |
| Bluetooth          | 🟩W |
| GPS                | ⬜U |
| **Miscellaneous**  | |
| FDE                | 🟩W |
| USB OTG            | 🟩W |
| **Sensors**        | |
| Accelerometer      | 🟩W |

#### Contents

These aports go on top of a [pmaports](https://gitlab.postmarketos.org/postmarketOS/pmaports) checkout, at the same paths:

| Path | What it is |
|------|------------|
| `device/testing/device-samsung-gta3xlwifi` | Device package |
| `device/testing/linux-samsung-gta3xlwifi` | Kernel, built from [Mollomm1/linux-exynos7885](https://github.com/Mollomm1/linux-exynos7885) (branch `gta3xl`), with the board DTS and config |
| `device/testing/firmware-samsung-gta3xlwifi` | Firmware (WiFi/Bluetooth, audio, camera, ACPM) |
| `main/mesa` | Mesa with Panfrost fixes for the Mali-G71 r0p0 and the Exynos7885 DPU as a kmsro display |
| `temp/libcamera` | libcamera with the Exynos7885 CSIS, S5K4HA/S5K5E9 sensors and rear-camera autofocus |
| `extra-repos/systemd/xorg-server` | Xorg with modesetting TearFree, for tear-free rotated X11 desktops |
| `main/helium-browser-bin` | Helium browser (glibc build) running on musl |

#### Usage

```sh
cp -r device main temp extra-repos "$(pmbootstrap config aports)"/
pmbootstrap init   # vendor: samsung, device: gta3xlwifi
pmbootstrap install --android-recovery-zip
pmbootstrap export
```

Arch-independent forks are built with `pmbootstrap build --arch aarch64 <pkg>`.

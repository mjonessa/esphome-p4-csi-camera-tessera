# Changelog

## 0.1.2 - 2026-10-15

- The camera's sensor driver is now in the firmware. Before, the build left it out, so the camera never found
  its sensor and stopped with "VIDIOC_G_FMT failed (errno 22)".

## 0.1.1 - 2026-10-15

- When the camera fails to start, the log now says why, also in a log opened over Wi-Fi after the screen
  started.

## 0.1.0 - 2026-10-15

- First version: the OV02C10 camera of the 10.1-inch Guition JC8012P4A1 (first build) as a camera in Home Assistant,
  with its JPEG quality and two flip switches.

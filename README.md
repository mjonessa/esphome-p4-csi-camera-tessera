# Panel camera

The camera built into the 10.1-inch Guition JC8012P4A1 (first build, OV02C10 sensor) as a camera of the screen in
Home Assistant: a picture on a dashboard, a stream when you open it, snapshots in automations.

## What you need

- A Guition JC8012P4A1, first build ("chip revision: v1.3" in the boot log), with the camera module fitted.
- A screen running Tessera firmware with ESPHome 2026.9.0 or newer.

## Set up

1. In Tessera, open **Plugins** and add this repository by its link:
   `https://github.com/mjonessa/esphome-p4-csi-camera-tessera`.
2. Tick your screen. It builds once with the plugin and updates over Wi-Fi.
3. In Home Assistant, open the screen's device: **Panel Camera** is there, with its controls under Configuration.

## Brightness

The OV02C10 has no auto exposure, so the camera starts on the sensor's own exposure and gain (from its start-up
table: about 32 ms at 8x analog gain). In Home Assistant, under the camera's controls:

- **Camera Analog Gain** (1x to 15.5x) brightens with the least noise; try this first.
- **Camera Digital Gain** (1x to 4x) on top of it, for a little more.
- **Camera Exposure** (ms): longer is brighter but blurs movement. Past about 32 ms the sensor slows its frame rate
  to fit the exposure.

What you set is kept across restarts and updates.

## What it is

The driver in `components/p4_csi_camera` is a patched copy of
[sullb/esphome-p4-csi-camera](https://github.com/sullb/esphome-p4-csi-camera) (commit `461aa9b`, MIT):

- **`i2c_id`**: the OV02C10's control bus (SCCB) is on GPIO7/GPIO8, the same wires as the touch panel. Upstream
  creates its own I2C controller on port 0 there, which collides with Tessera's `touch_bus`. With `i2c_id: touch_bus`
  the camera takes that bus's handle (`i2c_master_get_bus_handle`) and passes it to `esp_video` with
  `init_sccb = false`, so the touch panel and the camera share one controller. Without `i2c_id` it behaves as upstream.
- **`sccb_frequency`** (default `100kHz`): the camera's speed on that bus. The touch panel keeps its own 400 kHz.
- **esp-video-components pinned** to commit `ee31057` instead of `master`, so a build doesn't change under you.
- **The sensor driver is linked in**: ESPHome 2026.9 builds the sources into an archive, which left `ov02c10_sensor.c`
  out; referencing `ov02c10_detect` keeps it.
- **Exposure and gain**: numbers for exposure, analog gain and digital gain, written to the sensor's registers over the
  shared bus and kept across restarts.
- **The newest frame**: older queued frames are handed back, so a picture is never minutes old.

`components/panel_camera` only registers the plugin with Tessera.

## Known limits

- About two pictures a second; colours come from fixed white-balance gains and may look off in some light.
- The upstream driver was never run beside a MIPI-DSI display before this plugin. If touch stops reacting after the
  update, remove the plugin and update again.

## Privacy

Pictures go only to your Home Assistant, and only when it asks for one. Everyone who can open the screen's device or
a dashboard with its camera can see through it. Disable the camera entity in Home Assistant to stop that for a while,
or remove the plugin.

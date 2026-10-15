# p4_csi_camera for a Tessera screen (Guition JC8012P4A1)

A patched copy of [sullb/esphome-p4-csi-camera](https://github.com/sullb/esphome-p4-csi-camera) (commit `461aa9b`,
MIT licence, see `LICENSE`) for a 10.1-inch Guition JC8012P4A1 that runs
[Tessera](https://github.com/MaxGramser/homeassistant_espscreen) firmware.

## What changed from upstream

- **`i2c_id`**: the OV02C10's control bus (SCCB) is on GPIO7/GPIO8, the same wires as the touch panel. Upstream
  creates its own I2C controller on port 0 there, which collides with Tessera's `touch_bus`. With `i2c_id: touch_bus`
  the camera gets that bus's handle (`i2c_master_get_bus_handle`) and passes it to `esp_video` with
  `init_sccb = false`, so the touch panel and the camera share one controller. Without `i2c_id` it behaves as upstream.
- **`sccb_frequency`** (default `100kHz`): the camera's speed on that bus. The touch panel keeps its own 400 kHz.
- **esp-video-components pinned** to commit `ee31057` instead of `master`, so a build doesn't change under you.

## Using it

Paste `override.yaml` into the screen's **··· → Override YAML** in Tessera. **Save & check**, then **Update firmware**.

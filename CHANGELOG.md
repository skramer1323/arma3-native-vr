# Changelog

## 0.3.0 Community Preview

### Gameplay and interface

- Keep the stock inventory open in VR on the F11 anchor, with mouse interaction and the stereo world visible behind it.
- Center rangefinder and other static optic overlays separately for each eye. Preserve binocular zoom and magnified, night-vision and thermal views.
- Keep the vehicle gunner view aligned with mouse aim at base magnification. Verified in the Ifrit and BTR.
- Suppress the screen-space sun glare pass that could appear in only one eye.
- Reorganize **Insert** into rendering, view and HUD, weapon and optics, and diagnostics tabs. Show the actual scene size alongside a manual startup request when one is configured.

### Rendering and diagnostics

- Keep the R32 same-frame stereo path. Replace the two peripheral shading radius controls with one sharp-center slider and a preview while dragging.
- Add optional AMD FidelityFX CAS sharpening. With peripheral shading active, its strength fades toward the edges.
- Reduce redundant shading-rate state changes and avoid an intermediate CAS texture copy on compatible targets.
- Add median and 95th-percentile frame pacing measurements and improve the optional stereo diagnostics.

### Test coverage and known limits

The inventory, optics, gunner camera and sun glare fixes were checked in earlier headset test builds. The Windows release build passes the rendering and pose math tests. Further SteamVR and GPU coverage is welcome. A mission-wide FPS gain from peripheral shading has yet to be established.

Replacement inventories may use the flat interface. Squad hexagons remain on the HUD. The profile uses Arma's current Sampling and includes no third-party upscaler or frame generator.

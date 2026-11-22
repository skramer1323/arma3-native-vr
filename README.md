# Arma 3 Native VR

A Windows OpenXR mod for Arma 3 that brings stereoscopic PC VR and headset tracking to first-person gameplay. Keep your normal mouse aim, keyboard bindings and ACE workflows.

This is a community preview. I tested it with more than 50 mods in my own setup and found no conflicts during those sessions. Please report any conflicts you find in your modset. — SKRAMER1323

## Install and launch

1. Close Arma 3 and start your headset's OpenXR runtime. SteamVR and Virtual Desktop's VDXR have been used in testing.
2. Open Arma 3's installed files folder from Steam. Back up any existing `dinput8.dll`, `FreeTrackClient64.dll` and `arma3_native_vr.ini` there.
3. Copy those three files from the Windows preview ZIP into the folder containing `arma3_x64.exe`.
4. Launch 64-bit Arma 3 without BattlEye for an offline single-player test. Load a first-person mission, look straight ahead and press **F11**.
5. Press **Insert** to open the VR settings. **Insert** or **Esc** closes them. Use **Save all settings** to keep your VR settings.

The tested game executable is **Arma 3 2.22.0.154089 x64 (DirectX 11)**. Start in windowed mode. The preview is intended for offline single-player testing. To uninstall, close the game and remove the three files copied in step 3. Restore your backups if you replaced an existing installation.

## Rendering modes

| Mode | How it renders | When to try it |
| --- | --- | --- |
| **Full stereo** | Draws both eyes from the same game frame. | Baseline for checking tracking, geometry, optics and visual comfort. |
| **AER** | Renders one eye per game frame. Stale-eye reprojection can update the previous image for the other eye. | Compare frame time on a GPU that struggles with full stereo. Fast motion can reveal differences between eyes. |
| **AER + DIBR** | Uses depth from the current image to reconstruct the other eye. The previous image can fill some newly visible areas. | Compare against full stereo at the same resolution. Fine edges, foliage and near objects may expose reconstruction artifacts. |

The default preset starts in full stereo and uses the game's render size. Try **Insert → Rendering → AER (one eye per frame)** with **DIBR shift** and **Headset and camera rotation** as a performance comparison. Keep the scene, graphics settings and headset refresh rate fixed for both runs.

## Troubleshooting

- **Head tracking is unavailable.** Verify that `FreeTrackClient64.dll` is next to the game executable and FreeTrack is enabled in Arma's controls.
- **The menu works but the world is black.** Verify that your intended OpenXR runtime is active. Include `arma3_native_vr.log` in a report and indicate whether both eyes are affected.
- **The world judders or the image differs between eyes.** Try full stereo to establish a baseline. Record your runtime, GPU, refresh rate, in-game 3D sampling and the affected render mode.
- **Frame rate is low.** Compare full stereo and AER in the same scene, then try a lower in-game 3D sampling setting if both miss the headset's frame budget.

The optional **Apply manual size** control schedules a Windows PowerShell command to update the display or render size in `Arma3.cfg` after the game exits. You can also set resolution from Arma's video options.

## Related projects

- [SnowRunner VR](https://github.com/Timguin-87/Snowrunner-VR) is a reference for VR rendering and resolution handling.

## License

The original Arma 3 Native VR code is available under the [MIT License](LICENSE). [Third-party notices](THIRD_PARTY_NOTICES.md) list the licenses for included or linked components. The mod is independent of Bohemia Interactive.

See [Building](BUILDING.md) for source builds and [Contributing](CONTRIBUTING.md) for test reports and pull requests.

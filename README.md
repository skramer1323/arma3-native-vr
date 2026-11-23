# Arma 3 Native VR

A Windows OpenXR mod for Arma 3 that brings stereoscopic PC VR and headset tracking to first-person gameplay. Keep your normal mouse aim, keyboard bindings and ACE workflows.

This is a community preview. I tested it with more than 50 mods in my own setup and found no conflicts during those sessions. Please report any conflicts you find in your modset. — SKRAMER1323

## Install and launch

1. Close Arma 3 and start your headset's OpenXR runtime. SteamVR and Virtual Desktop's VDXR have been used in testing.
2. Open Arma 3's installed files folder from Steam. Back up any existing `dinput8.dll`, `FreeTrackClient64.dll` and `arma3_native_vr.ini` there.
3. Copy those three files from the Windows ZIP from [Releases](https://github.com/skramer1323/arma3-native-vr/releases/latest) into the folder containing `arma3_x64.exe`.
4. Launch 64-bit Arma 3 without BattlEye for an offline single-player test. Load a first-person mission, look straight ahead and press **F11**.
5. Press **Insert** to open the VR settings. **Insert** or **Esc** closes them. Use **Save all settings** to keep your VR settings.

The tested game executable is **Arma 3 2.22.0.154089 x64 (DirectX 11)**. Start in windowed mode. The preview is intended for offline single-player testing. To uninstall, close the game and remove the three files copied in step 3. Restore your backups if you replaced an existing installation.

## Rendering

Both eyes render the same game frame in first-person missions. Compatible world prepass draws use a single geometry submission with a separate projection for each eye. On supported NVIDIA hardware, variable-rate shading reduces work in the periphery and keeps the center at full rate.

The camera compensates for the extra movement Arma adds during head turns at normal zoom on foot. Positional head tracking stays active so you can still lean and inspect nearby geometry.

The preset uses the game's current render size. Try a fixed landmark with the mouse still, then lean sideways and repeat the turn. Virtual Desktop was used to validate this build. SteamVR results are welcome.

## Troubleshooting

- **Head tracking is unavailable.** Verify that `FreeTrackClient64.dll` is next to the game executable and FreeTrack is enabled in Arma's controls.
- **The menu works but the world is black.** Verify that your intended OpenXR runtime is active. Include `arma3_native_vr.log` in a report and indicate whether both eyes are affected.
- **The world judders or the image differs between eyes.** Record your runtime, GPU, refresh rate and in-game 3D sampling. If one eye flickers, try the same scene with `SinglePassWorldPrepass=0` and report the result.
- **Frame rate is low.** Try a lower in-game 3D sampling setting or reduce shadow quality. Compare the same scene at the same headset refresh rate.

For timing and camera traces, set `PerfLog=1` and `DetailedCapture=1` in the INI before launching. The CSV contains headset poses, so share it privately.

The optional **Apply manual size** control schedules a Windows PowerShell command to update the display or render size in `Arma3.cfg` after the game exits. You can also set resolution from Arma's video options.

## Related projects

- [SnowRunner VR](https://github.com/Timguin-87/Snowrunner-VR) is a reference for VR rendering and resolution handling.

## License

The original Arma 3 Native VR code is available under the [MIT License](LICENSE). [Third-party notices](THIRD_PARTY_NOTICES.md) list the licenses for included or linked components. The mod is independent of Bohemia Interactive.

See [Building](BUILDING.md) for source builds and [Contributing](CONTRIBUTING.md) for test reports and pull requests.

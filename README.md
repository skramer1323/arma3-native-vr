# Arma 3 Native VR

A Windows OpenXR mod for first-person Arma 3. Explore the world in full stereo with headset tracking and keep your mouse aim, keyboard bindings and ACE shortcuts. Native VR runs in the game process and requires no Arma addon or PBO.

The goal is to add a VR view while keeping existing Arma mods and controls usable. SKRAMER1323 has tested the project with more than 50 mods across local sessions. Compatibility can vary with the mission and modset.

## Install

1. Download the Windows ZIP for the version you want from [GitHub Releases](https://github.com/skramer1323/arma3-native-vr/releases).
2. Close Arma 3. In Steam, open **Arma 3 → Manage → Browse local files**.
3. Back up any existing `dinput8.dll`, `FreeTrackClient64.dll` and `arma3_native_vr.ini`. Copy those three files from the ZIP into the folder containing `arma3_x64.exe`. When upgrading, start with the supplied INI and reapply your preferences in **Insert**.
4. Connect the headset and start the OpenXR runtime you use for PC VR. The project has been tested with SteamVR and Virtual Desktop's VDXR.
5. Start **64-bit Arma 3** in **Windowed** mode without BattlEye for an offline single-player test. The tested game build is **2.22.0.154089** (DirectX 11).
6. Enter a first-person mission, face forwards and press **F11** to recenter. Press **Insert** to open VR settings and **Insert** or **Esc** to close them. Choose **Save all settings** to keep your changes.

To uninstall, close Arma and remove the three installed files. Restore any files you backed up.

## Tune graphics for your headset

Start at **100% Sampling** in Arma and test an outdoor mission. Use a fixed headset refresh rate, stand at the same spot and turn your head through the same view for each comparison. A frame budget is about **13.9 ms at 72 Hz**, **12.5 ms at 80 Hz**, and **11.1 ms at 90 Hz**.

| What you see | What to try |
| --- | --- |
| GPU load is high when you turn toward a dense scene | Lower **Sampling** by one step. If you are still GPU-limited, reduce shadows, ambient occlusion or Picture-in-Picture quality. |
| Dropped frames persist when Sampling is reduced | Lower **Object view distance** first. Compare in the same mission, since active AI and scripts also affect frame time. |
| Motion looks smeared when you turn your head | Set **Rotation Blur** and **Radial Blur** to zero. Check frame pacing in the headset after the change. |
| Image is stable but looks soft | Raise **Sampling** one step if there is GPU headroom. Try **FidelityFX CAS sharpness** at **15–25%** with Arma's sharpening reduced. CAS is a sharpening filter and is off at 0%. |
| You want to try peripheral shading on supported NVIDIA hardware | Turn off Arma's **FSAA/MSAA**. Open **Insert → Rendering**, enable **Peripheral shading**, and start with a **Sharp center radius** of **35°**. Drag the slider to preview the boundary. Compare several seconds at the same spot with shading on and off. |

Arma's **Sampling** controls the 3D render size. **Insert → Rendering → Render resolution** shows the scene size the game actually allocated. **Diagnostics** lists the OpenXR runtime's recommended size per eye. These sizes serve different stages of the render pipeline. The optional **Apply on restart** control schedules an `Arma3.cfg` update after the game exits. If Arma allocates a different size, adjust **Sampling** in the game and check the result in a mission. [Bohemia's video settings and performance guide](https://community.bohemia.net/wiki/Arma_3%3A_Performance_Optimisation)

Peripheral shading applies to eligible world draws. The center remains full rate, the effect pauses while aiming, and performance gains vary by scene. Leave external frame generation and graphics injectors off for the initial comparison so you can judge the base render path.

## VR features and limits

- **Full stereo.** Both eyes render the same game update. Eligible world prepass draws use one submission with a projection for each eye. Head tracking includes physical lean.
- **Inventory.** The stock inventory opens on a mouse-driven panel at the **F11** anchor, over the stereo world. Mods that replace the stock inventory may open in the flat interface.
- **Optics.** Supports magnified sights, rangefinders, binoculars, night vision and thermal views on the tested paths. Vehicle gunner aiming stays with the mouse.
- **HUD.** The gameplay HUD can sit on a room-anchored panel. Stock squad hexagons still use the game's HUD layer.

See the [changelog](CHANGELOG.md) for the release changes and current test coverage.

## Troubleshooting

- **Head tracking is unavailable.** Confirm that `FreeTrackClient64.dll` is beside the executable and enable FreeTrack in Arma's controls.
- **The menu works but the world is black.** Check the active OpenXR runtime and start with a stock first-person mission. Report whether the issue affects one eye or both and include `arma3_native_vr.log`.
- **One eye flickers or shows a different image.** In a separate test, close Arma, set `SinglePassWorldPrepass=0` in the INI and repeat the same scene. Include the result with your report.
- **Settings or overlays interfere.** Test once with the supplied INI and with other graphics injectors disabled. Restore your original settings after the comparison.

For a short timing trace, close Arma and set `PerfLog=1` in the INI. Review logs before sharing them. The optional detailed capture can contain headset poses and screenshots. See [Contributing](CONTRIBUTING.md) for what to include in a report.

## Related projects

- [A3VR Hybrid](https://github.com/gborgogno/a3vr-arma3) by gborgogno is an earlier Arma 3 VR project. Try it if you prefer motion-controller aiming and input.
- [SnowRunner VR](https://github.com/Timguin-87/Snowrunner-VR) is a reference for VR rendering and resolution handling.

## Building and licensing

See [Building](BUILDING.md) for Windows build instructions. Original project code is [MIT licensed](LICENSE). [Third-party notices](THIRD_PARTY_NOTICES.md) cover included and linked components. The mod is independent of Bohemia Interactive.

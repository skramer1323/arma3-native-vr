# Contributing

Test runs, reproducible bug reports and code contributions are welcome. Compatibility with existing Arma mods and keyboard/mouse bindings is a project priority.

## Reporting a problem

- Include your Arma build, GPU, headset, OpenXR runtime, refresh rate, render method and in-game 3D sampling.
- Name the mission and relevant mods. Describe the shortest sequence that reproduces the issue, including whether it affects one eye, both eyes or the desktop view.
- For a performance comparison, stay in the same place and repeat the same movement in full stereo and AER with the same graphics settings. Note any trade-off in image quality or tracking.
- For a short diagnostic run, close Arma and set `PerfLog=1` and `DetailedCapture=1` in `arma3_native_vr.ini`. Save `arma3_native_vr.log` and `arma3_native_vr.frames.csv` after exiting. The CSV is overwritten on the next run. Review the files before sharing them. They include runtime and hardware data, timings and headset pose samples.

## Submitting changes

Keep each pull request focused and describe what you tested. Rendering changes benefit from an in-headset comparison, especially for the horizon, nearby geometry, aiming, the HUD and menu cursor. Changes to shared settings or graphics paths should account for both SteamVR and VDXR. See [Building](BUILDING.md) for the existing tests.

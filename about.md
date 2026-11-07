# GPU Helper

GPU Helper is an experimental iOS Metal visibility-measurement and crash-diagnostics mod for Geometry Dash 2.2081 / Geode 5.11.2.

Version 0.3.8 prioritizes crash isolation. It removes the GameObject::visit renderer hook and does not retain a GameObject pointer cache. It keeps the asynchronous Metal bounds test but does not apply its returned visibility mask to rendering. This is a diagnostic stability build, not an FPS optimization release.

The large in-game HUD reports device status, level-scan filter counts, submitted and completed Metal jobs, GPU failures and last error, visibility measurements, timings, and live HUD/driver counters.

A persistent diagnostics.log is written in the mod's Geode save directory. It records startup, device and setting state, level scan, Metal setup/dispatch/completion, failures, HUD/driver heartbeats, and level exit. The file rotates at approximately 1 MiB. This is a breadcrumb recorder, not a native crash catcher.

The mod includes a 256×256 root-level logo.png icon.

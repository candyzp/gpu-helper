# GPU Helper

## v0.4.2 — Selectable legacy and hybrid graphics paths

The Geode settings page now has one selector: **GPU Helper graphics path**.

- **legacy** — the earlier Metal visibility-measurement path, with the OpenGL ES interop probe inactive.
- **hybrid** — the same visibility measurements plus the opt-in Core Video shared-texture path that creates a Metal texture and an OpenGL ES texture over the same pixel buffer.

The default is **legacy**. You can switch modes while playing; the setting is read live by the HUD and the Cocos draw callback. Switching to hybrid runs the interop probe once when Geometry Dash's EAGL context is current. Switching back to legacy marks the probe inactive and keeps the legacy visibility path.

**Important:** hybrid mode currently verifies shared-texture creation only. It does not yet write visibility results into the shared texture, pass the results to Cocos/OpenGL ES drawing, or skip rendering. Both modes leave game rendering, physics, collisions, and triggers unchanged. The next step is to validate a Metal write followed by an OpenGL ES read before connecting any of this to the renderer.

## v0.3.9 — Safe diagnostic mode

This build removes the GameObject::visit override completely and no longer keeps a per-object GameObject pointer cache. The Metal kernel still measures whether eligible static-decoration bounds intersect the expanded camera view, but returned visibility data is diagnostic only and is not applied to the renderer. Geometry Dash keeps its normal drawing path.

This is a stability and measurement build, not an FPS optimization release.

## Metal workload

- Detects the system Metal device with MTLCreateSystemDefaultDevice.
- Builds the gpuhelper_cull_decoration_bounds compute pipeline.
- Scans the level for decoration-type objects, the static-decoration flag, ungrouped static objects, and valid world bounds.
- Uploads eligible bounds to persistent shared Metal buffers.
- Enqueues asynchronous MTLCommandBuffer compute dispatches.
- Receives completed visibility masks and records actual completed-work counters.
- Records GPU command-buffer failures and their last reported error.
- Does not wait synchronously for the GPU.
- Does not retain GameObject pointers after the level scan.
- Does not change object drawing, physics, collisions, triggers, or object updates.

The minimum batch remains 96 eligible objects. The HUD reports every filter count so a zero-job result has a specific explanation rather than being mistaken for a working GPU path.

## Large live HUD

The text-only HUD is left-aligned, larger (0.20 scale), and refreshes every 0.25 seconds. HUD UPDATE proves the overlay update method is executing. DRIVER TICKS independently proves the measurement driver is updating.

It reports:
- Metal device availability and setting state.
- Level-scan completion and all eligibility filter counts.
- Eligible batch size and the reason a batch was rejected, when applicable.
- Jobs submitted versus jobs completed.
- Completed compute failures and the last Metal error.
- Total processed objects and visible versus would-cull counts.
- GPU execution and CPU submission timings when available.
- Explicit confirmation that renderer interception is removed and drawing is unchanged.

## Persistent crash diagnostics

GPU Helper mirrors diagnostic events to the normal Geode log and writes a chronological diagnostics.log in its Geode mod save directory. It records session start, device detection, settings, level-scan start and completion, candidate filter counts, Metal setup/dispatch errors, asynchronous completion results and failures, HUD heartbeats, driver heartbeats, and level exit.

Driver and HUD heartbeats are aggregated at roughly two-second intervals so the trace remains useful without writing one line for every rendered frame or every GPU submission. Event lines include monotonic elapsed time and a sequence number. The log is capped at approximately 1 MiB; when exceeded, diagnostics.previous.log is rotated in. Storage errors are caught so logging cannot interrupt gameplay.

This is a persistent breadcrumb recorder, not a native crash catcher. It does not install unsafe signal handlers or claim to recover from segmentation faults. Its purpose is to preserve the last known mod state for correlation with a Geode crash report.

## Icon and package size

The project includes a 256×256 root-level logo.png mod icon. A .geode is a compressed archive. The v0.3.7 build contained an approximately 245 KB uncompressed iOS dynamic library inside an approximately 61 KB compressed package. The small download size is compression, not a stub; adding filler bytes would not improve the mod.

## Build target

- Geometry Dash: 2.2081
- Geode: 5.11.2
- Platform: iOS arm64

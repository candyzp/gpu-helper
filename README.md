# GPU Helper

An iOS-only Geometry Dash 2.2081 / Geode 5.11.2 Metal visibility-measurement and crash-diagnostics experiment.

## v0.3.8 — Safe diagnostic mode

This build removes the GameObject::visit override completely. The Metal kernel still measures whether eligible static-decoration bounds intersect the expanded camera view, but the result is not applied to the renderer. Geometry Dash keeps its normal drawing path. This is intentional because the reported fault was inside CCNode::visit; this build removes GPU Helper's render-time interception while gathering evidence.

This is a stability and measurement build, not an FPS optimization release.

## Metal workload
- Detects the system Metal device with MTLCreateSystemDefaultDevice.
- Builds the gpuhelper_cull_decoration_bounds compute pipeline.
- Collects eligible ungrouped static-decoration bounds.
- Uploads bounds to persistent shared Metal buffers.
- Enqueues asynchronous MTLCommandBuffer compute dispatches.
- Receives completed visibility masks and reports real counters.
- Does not wait synchronously for the GPU.
- Does not modify physics, collisions, triggers, object updates, or Cocos rendering.

The HUD separately counts total level objects, decoration-type objects, static-flagged decorations, ungrouped static decorations, valid bounds, submitted jobs, completed jobs, and returned visibility results.

## Live diagnostics HUD
The larger left-aligned text HUD refreshes every 0.25 seconds. HUD UPDATE proves the overlay update method is executing. DRIVER TICKS independently proves the measurement driver is updating.

It reports device availability, setting state, per-level scan filter counts, batch size, submitted/completed GPU jobs, processed objects, visible/would-cull counts, and GPU/CPU submit timings.

## Persistent crash diagnostics
GPU Helper writes chronological diagnostics.log to its Geode mod save directory and mirrors events to the normal Geode log. Events include session start, HUD and driver attachment, PlayLayer setup/exit, scan results, Metal setup failures, dispatch failures, completed jobs, and HUD/driver heartbeats.

The file is capped at approximately 1 MiB; when exceeded, diagnostics.previous.log is rotated in. Logging is best-effort. This is a breadcrumb recorder, not a native crash catcher: it does not install signal handlers or attempt to recover from a segmentation fault.

## Package size
A .geode file is a compressed archive. The v0.3.7 build contained an approximately 245 KB uncompressed iOS dynamic library inside an approximately 61 KB compressed package. The small download size is compression, not a stub. This version adds a real logo.png icon and diagnostic functionality rather than artificial padding.

## Build target
- Geometry Dash: 2.2081
- Geode: 5.11.2
- Platform: iOS arm64

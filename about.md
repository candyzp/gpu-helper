# GPU Helper

GPU Helper is an experimental iOS Metal visibility-measurement and crash-diagnostics mod for Geometry Dash 2.2081 / Geode 5.11.2.

Version 0.3.8 prioritizes crash isolation. It retains the asynchronous Metal bounds test and reports completed results, but does not apply those results to rendering. The previous GameObject::visit render hook has been removed.

The HUD reports the device, per-level object scan, eligibility filters, submitted and completed Metal jobs, visibility results, GPU timings, and a changing update counter.

A persistent diagnostics.log is written in the mod's Geode save directory. It records lifecycle, scan, driver, Metal, and HUD breadcrumbs to help correlate a crash report with the last actions performed by GPU Helper.

Important: v0.3.8 is a diagnostic stability build, not an FPS optimization release.

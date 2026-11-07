# Changelog

## v0.5.3
- Guard the exact null dereference in Cocos' CCSpriteBatchNode::updateBlendFunc path. If texture-atlas initialization leaves no texture, GPU Helper keeps the initializer's default blend function rather than calling hasPremultipliedAlpha on a null pointer.
- Valid texture initialization continues through the original Cocos blend-function implementation unchanged.
- Keep texture-initialization hooks removed; record the missing-atlas-texture condition once in the Geode log.


## v0.5.2
- Remove the speculative null-texture hooks on CCSpriteBatchNode::initWithTexture and CCTextureAtlas::initWithTexture. GPU Helper no longer intercepts Cocos texture initialization used by loading-screen bitmap fonts.
- Move the OpenGL ES version probe until after a registered gameplay atlas produces a valid culling result; untracked loading-screen atlas draws now go directly to the original draw method.
- Keep broad CCSpriteBatchNode lifecycle hooks removed and preserve the GPU-driven atlas culling path.


## v0.5.1
- Add first-priority null-texture guards to CCSpriteBatchNode::initWithTexture and CCTextureAtlas::initWithTexture, preventing a null texture from reaching Cocos code that dereferences it.
- Keep the existing atlas draw-range culling implementation and remove no additional batch-node lifecycle hooks.
- Add a unique v0.5.1 startup diagnostic marker for build identification.


## v0.4.2
- Replace the standalone hybrid-probe boolean with a single-choice graphics path selector: legacy or hybrid.
- Keep legacy Metal visibility measurement as the default and preserve its behavior.
- Hybrid mode runs the same visibility measurements and activates the shared Metal/OpenGL ES texture probe from the current Cocos draw callback.
- Add live HUD mode reporting and persistent mode-switch breadcrumbs.
- Clarify that hybrid mode is currently an interoperability foundation probe, not yet GPU-driven rendering or actual culling.


## v0.4.1
- Restrict the Core Video OpenGL ES interop headers and implementation to iOS builds, preserving the macOS Metal regression test target.
- Keep the v0.4.0 opt-in shared-texture probe behavior unchanged.


## v0.4.0
- Add an opt-in Metal/OpenGL ES interop resource probe using a Core Video pixel buffer with both compatibility flags enabled.
- Create Metal and OpenGL ES texture views from the same pixel buffer during the active Cocos draw callback.
- Validate the OpenGL ES texture in the current game context and report the result in the live HUD and persistent diagnostics.
- Link CoreVideo and OpenGLES frameworks. This is an interop foundation test only; it does not change rendering or apply visibility results.


## v0.3.9
- Use object-layer-local bounds directly when a decoration is already parented to the object layer, avoiding an unnecessary world-space round trip.
- Add a clearly labeled detached-object bounds fallback for entries in PlayLayer::m_objects that have no Cocos parent; this is measurement-only and never affects rendering.
- Track direct-layer, transformed-parent, detached-fallback, and invalid bounds separately in the HUD and persistent diagnostics.
- Keep the GameObject render hook removed and preserve safe measurement-only behavior.


## v0.3.8
- Added a bundled 256×256 root-level logo.png mod icon.
- Removed the GameObject::visit render interception completely.
- Removed the per-object GameObject pointer cache from safe mode.
- Changed GPU visibility results to measurement-only; normal Geometry Dash drawing remains unchanged.
- Attached the measurement driver to the PlayLayer HUD lifetime rather than the global running scene.
- Added persistent chronological crash breadcrumbs in the Geode mod save directory, with a rotating 1 MiB log.
- Added level-scan filter counts, device/settings records, HUD and driver heartbeats, and Metal setup/dispatch/completion event logging.
- Added asynchronous Metal completion failure counters and last-error reporting.
- Increased HUD readability and made update and driver counters visible.

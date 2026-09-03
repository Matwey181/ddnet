# AGENT MEMORY — READ FIRST

## What "ported version" actually means

**The REAL ported version is `Pioooooo/ddnet` branch `ddnet-ios`.**

NOT `sh1zoooo/ddnet_iOS` — that one is identical to upstream ddnet and has
no iOS porting. The user's own `Matwey181/ddnet_iOS` (renamed to `Matwey181/ddnet`)
is also upstream + a non-functional iOS workflow.

## What Pioooooo/ddnet@ddnet-ios has that the others don't

- `scripts/compile_libs/gen_libs.sh` — supports `ios` target (the others
  only support android/linux/windows/webasm and crash with
  "Specify the target system" when given `ios`)
- `scripts/compile_libs/_build_common.sh` — iOS toolchain helpers
  (assert_xcode_found, etc.)
- `CMakeLists.txt` line 4: `set(IS_IOS TRUE)` detection
- `src/base/detect.h` line 83: `#define CONF_PLATFORM_IOS 1` (split from
  CONF_PLATFORM_MACOS using TargetConditionals.h)
- `CMakeLists.txt` line ~2912: full iOS bundle properties, Info.plist
  generation, targeted device family, ObjC link flag, post-build data copy
- `scripts/ios/files/Info.plist.in` — proper iOS app plist template
- `.github/workflows/build-libraries-ios.yml` — workflow that builds the
  iOS static libraries (openssl/curl/freetype/png/opus/opusfile/sdl/etc)
  and uploads them as an artifact

## What Pioooooo/ddnet@ddnet-ios does NOT have

- No `.ipa` packaging workflow (only `build-libraries-ios.yml`)
- No Pushin client feature

## The plan (do exactly this, nothing else)

1. Fork/clone `Pioooooo/ddnet@ddnet-ios` into the user's repo
   (`Matwey181/ddnet`) so CI can run there.
2. Add a new workflow `.github/workflows/build-ios-ipa.yml` that:
   - Downloads the `ddnet-libs-ios` artifact produced by
     `build-libraries-ios.yml` (or rebuilds if missing)
   - Configures CMake for iOS (Xcode generator, arm64, iphoneos sysroot)
   - Builds `game-client` target
   - Packages the resulting `.app` into a `Payload/` folder and zips it
     as `DDNet-unsigned.ipa`
3. Add the Pushin client var/team toggle feature inline in
   `src/game/client/components/menus_settings.cpp` inside the existing
   skin list loop in `RenderSettingsAppearance`. Keep it minimal: click a
   skin row → two buttons (var/team) appear in the row. No new files, no
   new config vars, no CMake changes.
4. Add a prominent comment block at the top of `menus_settings.cpp`
   warning future agents (me) that this is the ported version and the
   ONLY thing to touch is the Pushin feature.

## DO NOT

- Do NOT touch CMakeLists.txt, detect.h, system.cpp, notifications.cpp,
  updater.cpp, macos/*.mm, gen_libs.sh, _build_common.sh — they are
  already correctly ported. Touching them WILL break the iOS build.
- Do NOT create `src/ios/hvf_stub.cpp`, `opusfile_stub.cpp` or any other
  stub files — the ported version does not need them.
- Do NOT switch from Xcode to Ninja generator.
- Do NOT add `-Wl,-force_load` or `-Wl,-all_load`.
- Do NOT download Khronos GLES3 headers — the ported version already
  handles OpenGLES correctly via detect.h.
- Do NOT add new source files to the game-client target.
- Do NOT touch `ddnet-libs/` submodule.

## Unofficial macOS port of **OLM OpenTools**, an Adobe After Effects plug-in suite distributed by [OLM Digital R&amp;D](https://www.olm.co.jp/rd/categories/opentools). It's officially distributed for Windows with very limited macOS support.

No source code is provided for the originals, so each plug-in was reverse-engineered from the Windows binary and reimplemented. Every load-bearing detail was cross-checked against the original in IDA Pro to stay as faithful as possible to the original look.

- **Original author:** [OLM Digital R&amp;D](https://www.olm.co.jp/rd/categories/opentools)
- **Ported from:** the Windows "AE 2026" version provided by OLM Digital R&amp;D
- **Built with:** the Adobe After Effects **26.5 macOS SDK** (universal: arm64 / x86_64)

> [!NOTE]
> This repository is an unofficial, personal project and is not affiliated with OLM Digital R&amp;D in any way.

### Included plug-ins

| Plug-in | Version | Status |
|---|---|---|
| OLM DirectionalBlur | 1.1.1 | ✅ Verified in Mac AE |
| OLM RadialBlur | 1.3.0 | ✅ Verified in Mac AE |
| OLM Color Key | 2.3.1 | ✅ Verified in Mac AE |
| Color Keep | 1.0.1 | Testing |
| OLM Blur | 1.2.1 | Build/verify pending |
| Distance Gradation | 0.8.2α | Build/verify pending |
| OLM Smoother v2 | 2.1.0 | Build/verify pending |
| OLM Toon Dilate | 1.1.1 | Build/verify pending |
| OLM Kira Kira | 3.3 | Build/verify pending |

In After Effects they appear under the **`OLM Plug-ins`** effect category.

### Requirements

- macOS (Apple Silicon / Intel — universal build)
- Adobe After Effects 2026
- To build:
  - Full Xcode (the build uses `Rez` for the PiPL and `codesign`; Command Line Tools alone are not enough)
  - CMake 3.28 or newer

### Installation

1. Clone the repository.
   ```sh
   git clone https://github.com/festivities/olm-opentools-mac.git
   cd olm-opentools-mac
   ```
2. Build every plug-in. Signed `.plugin` bundles land in `./dist`.
   ```sh
   ./build-all.sh
   ```
   - Change the output folder: `OUT=~/Desktop/OLM ./build-all.sh`
   - Point at a specific SDK: `AE_SDK=/path/to/SDK/Examples ./build-all.sh`
   - Build a subset: `./build-all.sh OLMBlur OLMKiraKira`
   - Clean rebuild: `./build-all.sh --clean`
3. Copy the `*.plugin` bundles from `dist/` into your After Effects Plug-ins folder,
   e.g. `/Applications/Adobe After Effects 2026/Plug-ins/`.
4. **Clear After Effects' plug-in cache**, then relaunch.
   (A stale cache is the most common reason a freshly built plug-in appears to do nothing.)

### Building individually

You can also build a single plug-in without `build-all.sh`:

```sh
cmake -S OLMColorKey -B build-colorkey \
  -DAE_SDK="/abs/path/AfterEffectsSDK_26.5_MacOS/AdobeAfterEffectsSDK_26.5_MacOS/Examples" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-colorkey
```

Use a separate build directory per plug-in.

### Known limitations

- **OLM Kira Kira**: its color ramps render, save, load, interpolate, and round-trip with Windows projects, but the in-UI ramp *editor* (custom UI widget) is not ported — you can't redraw ramp stops on macOS.
- The port targets the Windows "AE 2026" variant. Bit-exact parity with the Windows build is not a goal; the aim is a visually near-identical result (≈99%).

### Transparency on AI

This project was developed with AI assistance. The reverse-engineering of the Windows binaries and the macOS port were done with AI, and every load-bearing part (PiPL, parameters, render math) was verified against the original binary in IDA Pro as it was ported.

### License

No license is granted for this repository.

It is a decompilation / reimplementation of the original software, which puts it in a legal grey area, and I'm not in a position to assign a redistribution license. All rights to the originals belong to [OLM Digital R&amp;D](https://www.olm.co.jp/rd/categories/opentools).

### Credits

**Original**

- OLM Digital R&amp;D — [https://www.olm.co.jp/rd/categories/opentools](https://www.olm.co.jp/)

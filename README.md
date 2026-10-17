# Native NV12 overlay reproducer

A small Windows D3D11 / DirectComposition program comparing native NV12
presentation with independently prepared, final-size CPU RGB references.
It investigates extra colored text distortion, often red pixels or fringes,
seen on the physical display while ordinary screenshots look cleaner.

The suspected behavior is sensitive to **odd visible widths**, including
clipping with identity source/destination sizes. It reproduces independently of
Chrome, YouTube playback, video decoding, and browser extensions.
This repository does **not** establish whether Windows or NVIDIA is responsible.

## Run without building

1. Download **[nv12-overlay-repro-win64.zip from Releases](https://github.com/CryZe/nv12-overlay-repro/releases/latest)**.
2. Extract the entire ZIP into a writable folder, preserving the `assets` directory.
3. Run `reference-test.exe`.

The program uses its own directory to find assets, regardless of the current
working directory. No installation, administrator access, browser, Python,
FFmpeg, or separate Visual C++ runtime installation is needed to run the supplied
static-runtime executable. It writes `run.log` beside the executable. It does not
change Windows, driver, browser, or display settings, and makes no network requests.

Tested on Windows 11 x64 with an RTX 4090. The program uses the default D3D11
hardware adapter; it does not force a particular GPU or force Windows to use an
overlay. Keep the complete image inside the window and the window on one monitor.

## Compare the paths

The title and status show the **actual presentation mode** separately from the
selected surface format. Wait for **HARDWARE OVERLAY**. If it stays **COMPOSED**,
a clean result does not test the affected NV12 overlay path. Overlay availability
can change with display topology, occlusion, and which monitor contains the window.

| Key | Action |
| --- | --- |
| `1` | Native NV12 presentation |
| `2` | CPU zimg bilinear reference, uploaded as final-size BGRA |
| `3` | CPU zimg bicubic reference, uploaded as final-size BGRA |
| `4` | CPU RGB-first conversion, then bilinear resize |
| `5` | Independent NumPy bilinear Y/Cb/Cr reconstruction |
| `T` | Cycle full-image widths 640, 639, 638, 637, 636, then 640 |
| `0` | Full 640-pixel width, no scaling or explicit clipping |
| `X` | Toggle between full-source resize to 639 and right-edge clipping by one pixel |
| `A` / `C` | Optionally record an affected / clean observation in the log |
| `Esc` | Close |

Start at the default width 639 and compare `1`, `2`, and `3` at the same position.
Focus on the **text**. Press `X` and compare `1` and `2` again. In the clipping
case the NV12 source and destination remain 640 by 360; only the rightmost
pixel is hidden. The CPU reference contains the corresponding 639-pixel crop.
Use `0` as the full-width control and `T` to compare odd and even widths.

On the investigated system, additional text distortion appeared in NV12 at
639/637 pixels and in the one-pixel clipping case, while the CPU bilinear and
bicubic references looked clean in that respect. Both NV12 and RGB could use
actual hardware overlays. The behavior reproduced on two different monitors
after Windows reassigned overlay resources. Ordinary screenshots did not capture
the additional live-display distortion reliably.

Some colored edges already exist in the source frame. Their presence alone is
not the suspected defect. A thin vertical line is also sensitive to legitimate
filter differences, making it less useful than the text for this comparison.

## Build from source

Install Visual Studio C++ Build Tools with the x64 C++ tools and a Windows SDK,
then run `build.cmd`. It discovers the installed tools using `vswhere`; no fixed
Visual Studio version, SDK version, user directory, or machine-specific path is
required. The executable is written beside `reference-test.cpp`.

```bat
build.cmd
reference-test.exe
reference-test.exe --self-test
```

The hidden self-test checks byte-exact GPU uploads and successfully presents all
five modes, widths, and crop geometries. It does not assess physical display
appearance or prove that its hidden window is using an overlay.

Optional interactive arguments are `--position=x,y`, `--size=width,height`, and
`--topmost`. `--capture-cycle` runs four eight-second stages, temporarily topmost,
then closes. It only writes the program's log; it does not start an ETW trace.

## Data and reference generation

The 25 raw files in `assets/` are ready to use. `assets/manifest.json` records
their sizes and SHA-256 hashes. They contain one 640 by 360 NV12 frame and CPU
BGRA reference images for each tested width and crop.

To regenerate the CPU references, optionally install Python with NumPy/Pillow
and FFmpeg built with `libzimg`, then run:

```bat
python prepare-references.py
```

FFmpeg is found on `PATH`; `--ffmpeg`, `--source`, and `--output` override its
location, the input, and the output directory. Defaults resolve relative to the
script's directory. The generator also writes PNG previews and validation JSON.

The references use limited-range BT.709 Y'CbCr, LEFT chroma siting, and full-range
nonlinear RGB output without changing primaries or transfer function. The
explicit NumPy and independent FFmpeg/zimg bilinear implementations agreed at
every final 8-bit RGB pixel for widths 640 through 636 in the supplied validation.
See `prepare-references.py` and `reference-validation.json` for the conventions
and numerical comparisons.

These are defined reference filters, **not a normative bit-exact driver
conformance test**. Legitimate reconstruction filters can differ. The physical
NV12 output has not been measured with calibrated optical capture.

`geometry-evidence.json` summarizes an actual Windows plane-configuration
capture: the affected NV12 clipping case had a 640 by 360 source and destination
with a 639-pixel visible clip. Personal filesystem paths and raw machine logs
are not included.

## Sharing observations

Please include GPU, driver, Windows build, monitor configuration, selected mode,
width/crop setting, and whether the status actually showed HARDWARE OVERLAY in
each comparison. `run.log` records mode changes, physical window positions, DPI,
byte-upload verification, and any optional `A`/`C` notes. A photograph or camera
video may be more informative than a screenshot for the live-only effect.

## Attribution and sources

The diagnostic frame is an excerpt of the graph around 2:02 in
[this Veritasium video](https://www.youtube.com/watch?v=HBluLfX2F_k).
See `NOTICE.txt` for the distinction between the code license and video data.

- [Microsoft: recommended 8-bit YUV formats](https://learn.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering)
- [Microsoft: multiplane overlay hardware requirements](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/multiplane-overlay-hardware-requirements)
- [FFmpeg zscale documentation](https://ffmpeg.org/ffmpeg-filters.html#zscale-1)
- [zimg](https://github.com/sekrit-twc/zimg)

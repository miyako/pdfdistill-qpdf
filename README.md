# pdfdistill-qpdf

A small, self-contained command-line tool that shrinks bloated PDFs — in
particular files produced by **Microsoft Print to PDF**, which embeds complete
TrueType fonts and stores photos as lossless Flate images.

It is written in C++17 on top of [qpdf](https://github.com/qpdf/qpdf) and is one
of three sibling implementations (Rust/lopdf, C++/qpdf, Go/pdfcpu) that share
the same command-line interface so they can be benchmarked against each other.

| Sample (`tests/fixtures/ms-print-to-pdf-1.pdf`, 1 page) | Size | Saved |
|---|---:|---:|
| Original | 720,651 B | — |
| `--no-images --no-subset --no-dedup` (structure only) | 711,053 B | 1.3 % |
| `--lossless` (dedup + font subsetting + structure) | 577,618 B | 19.8 % |
| **default** (`--quality 75 --max-dpi 150`) | **115,056 B** | **84.0 %** |
| `--max-dpi 30 --quality 60` | 42,323 B | 94.1 % |

Rendered output is pixel-identical outside the two photos (JPEG artifacts only);
text extraction and every used glyph's outline/metrics are preserved (checked by
the test suite).

## Usage

```
pdfdistill-qpdf <INPUT|-> [-o <OUTPUT|->] [options]
  INPUT  path or '-' for stdin
  -o     output path or '-' for stdout (default: stdout if input is '-', else <input>.distilled.pdf)
  --quality <1-100>      JPEG quality for recompressed images (default 75)
  --max-dpi <n>          downsample images whose effective resolution exceeds n (default 150; 0 = never)
  --no-images            skip image recompression
  --no-subset            skip font subsetting
  --no-dedup             skip duplicate object/image/font removal
  --lossless             only lossless optimizations (implies no JPEG recompression / downsampling)
  -v, --verbose          print per-step size statistics to stderr
  -q, --quiet
  --version, --help
```

* The default output name replaces a trailing `.pdf` (case-insensitive) with
  `.distilled.pdf` (`report.pdf` → `report.distilled.pdf`); other names get
  `.distilled.pdf` appended.
* Exit code `0` on success, `1` on processing/I/O errors, `2` on usage errors.
* All diagnostics go to stderr; stdout carries only PDF bytes when `-o -` is
  used (stdin/stdout are switched to binary mode on Windows).
* The output is **never larger than the input**: if the optimized file would be
  bigger (or equal), the original bytes are written unchanged.
* **Encrypted inputs** (with or without a user password) are passed through
  byte-for-byte unchanged, matching the sibling implementations.
* Output is deterministic (same input + options → identical bytes).

Examples:

```sh
pdfdistill-qpdf scan.pdf                       # writes scan.distilled.pdf
pdfdistill-qpdf in.pdf -o out.pdf -v           # per-step statistics
cat in.pdf | pdfdistill-qpdf - > out.pdf       # filter mode
pdfdistill-qpdf in.pdf -o out.pdf --lossless   # no lossy image changes
```

`-v` output for the sample:

```
[dedup] merged 0 stream(s), 0 dict/array object(s); size 720651 -> 711053 bytes (1.3%)
  font 8 0: kept 15 glyph(s), program 446508 -> 247312 bytes (uncompressed)
[fonts] subset 1 font(s), ...; size 711053 -> 577618 bytes (18.8%)
  image 15 0 512x512 -> 512x512 JPEG: 227054 -> 23356 bytes
  image 13 0 512x512 -> 512x512 JPEG: 296825 -> 35549 bytes
[images] 2 image(s) JPEG-encoded, 0 downsampled, ...; size 577618 -> 115056 bytes (80.1%)
[structure] object streams + max Flate; size 115056 -> 115056 bytes (0.0%)
```

(Each step's size is measured by writing the document with the final writer
settings, so object streams/max Flate are already included in the first line.)

## What it does

The passes run in this order on an in-memory `QPDF` document:

1. **Dedup** (`src/dedup.cpp`). Every reachable stream is hashed on its
   *decoded* data (qpdf "generalized" decode level; data that can't be decoded
   losslessly is hashed raw together with its filter) plus its dictionary
   without `/Length`, `/Filter`, `/DecodeParms`. Value-like objects are merged
   too: indirect arrays/strings/numbers (e.g. `/W`, `/FontBBox`), font,
   font-descriptor, ExtGState, encoding, pattern and shading dictionaries, and
   untyped dictionaries that have no identity keys (`/Parent`, `/P`, `/Kids`,
   `/Rect`, …). Candidates are byte-compared before merging, all references are
   rewritten, and the process repeats until a fixed point — so once SMasks or
   FontFiles merge, the images/fonts that point to them merge in the next round.
   Pages, annotations, outline items, fields and structure elements are never
   merged.
2. **Font subsetting** (`src/content.cpp`, `src/fonts.cpp`). All content is
   walked with qpdf's content-stream parser: page contents, Form XObjects
   (recursively), annotation appearance streams, tiling patterns, soft-mask
   groups and Type 3 glyph procedures, tracking `q/Q`, `cm`, `Tf`, `gs /Font`
   and `Tj`/`TJ`/`'`/`"`. Form XObjects inherit the caller's current font; text
   shown with no known font protects every font in that resource scope. For `Type0` fonts with `Identity-H`/`Identity-V`
   encoding and a `CIDFontType2` descendant with `FontFile2`, the used CIDs are
   mapped through `/CIDToGIDMap` (Identity or stream) to glyph IDs. The font
   program is then subset with HarfBuzz `hb-subset` using
   `HB_SUBSET_FLAGS_RETAIN_GIDS`, so glyph IDs — and therefore `CIDToGIDMap`
   and the content streams — stay valid; nothing in the content is rewritten.
   Composite-glyph components are kept automatically, `cmap` entries for the
   kept glyphs are retained, and layout tables (`GSUB`, `GPOS`, `GDEF`, `kern`,
   …) are dropped because text in a PDF is already shaped. `BaseFont`/`FontName`
   get a 6-letter subset tag (`ABCDEF+Name`), `/Length1` is updated and the
   now-inaccurate optional `/CIDSet` is removed.
   A font program is left untouched when it is shared with a simple
   (non-CID) font, uses a non-Identity CMap, appears in `/AcroForm /DR` (form
   fields may need more glyphs later), belongs to a font that is not reachable
   through any walked resource dictionary, or when content using it fails to
   parse.
3. **Images** (`src/images.cpp`). For 8-bit `DeviceGray`/`DeviceRGB`/`CalGray`/
   `CalRGB`/`ICCBased`(N=1|3) images with a default `/Decode` array and lossless
   filters (Flate/LZW/RunLength/ASCII*), the pixels are re-encoded as baseline
   JPEG (`DCTDecode`, libjpeg-turbo, optimized Huffman tables) at `--quality`
   and only kept if smaller. The effective resolution is computed from the CTM
   at each `Do`; if the lowest resolution at which an image is drawn exceeds
   `--max-dpi` (with 5 % tolerance), it is downsampled with an area-averaging
   filter to exactly `--max-dpi`. Images whose placement is unknown (patterns,
   annotation appearances, soft-mask groups, Type 3 glyphs, unparsable
   content) are never downsampled. Existing JPEGs are only touched when they
   need downsampling. `/SMask` images stay lossless (Flate, level 9) and are
   resampled to the parent's new size when the parent was downsampled.
   Skipped: `/ImageMask`, `/Mask` color-key arrays, Indexed/Separation/DeviceN/
   Lab/CMYK, 1/2/4/16-bit, JPX/JBIG2/CCITT, non-default `/Decode`.
4. **Structure** (`QPDFWriter`): object streams + cross-reference streams
   (`qpdf_o_generate`), generalized decode + Flate recompression at level 9
   (`setRecompressFlate`, `Pl_Flate::setCompressionLevel(9)`), deterministic
   `/ID`, and only objects reachable from the trailer are written (unreferenced
   and merged duplicates disappear). Streams that use PNG/TIFF predictors are
   kept as-is unless plain Flate is actually smaller, because the writer would
   otherwise drop the predictor.

`--lossless` runs passes 1, 2 and 4. With all three `--no-*` flags only the
structural rewrite remains.

## Building

Dependencies are provided by **vcpkg in manifest mode** (`vcpkg.json`, pinned
`builtin-baseline`) and linked **statically**. Custom overlay triplets in
`cmake/triplets/` build release-only static libraries:

| Triplet | Use |
|---|---|
| `arm64-osx-pdfdistill` / `x64-osx-pdfdistill` | macOS 11+, static libs (system `libc++`/`libSystem` dynamic) |
| `x64-windows-static-pdfdistill` | Windows x64, static libs, static CRT (`/MT`) |

`CMakeLists.txt` points `VCPKG_OVERLAY_TRIPLETS` at that directory and sets
`CMAKE_MSVC_RUNTIME_LIBRARY` to `MultiThreaded`, so the whole executable uses
`/MT`. qpdf is built by its vcpkg port with the **native crypto provider only**
(`REQUIRE_CRYPTO_NATIVE=ON`, no OpenSSL/GnuTLS); HarfBuzz is built without
default features (no FreeType/ICU/GLib).

Prerequisites: CMake ≥ 3.21, a C++17 compiler, Git, and a vcpkg checkout:

```sh
git clone https://github.com/microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh -disableMetrics   # bootstrap-vcpkg.bat on Windows
export VCPKG_ROOT=$PWD/vcpkg
```

### macOS (universal arm64 + x86_64)

vcpkg builds one architecture per triplet, so the script configures both
architectures separately (each with its own static dependency tree) and merges
the executables with `lipo`:

```sh
brew install ninja
./scripts/build-macos-universal.sh        # -> build/universal/pdfdistill-qpdf
```

Single-architecture build:

```sh
cmake -S . -B build/arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-pdfdistill -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/arm64
ctest --test-dir build/arm64 --output-on-failure
```

`otool -L` on the result only lists `libc++`, `libSystem` and the
`ApplicationServices` framework.

> Note: recent Apple clang searches `/usr/local/include` *before* `-isystem`
> directories. The build therefore uses `-I` for dependency headers
> (`CMAKE_NO_SYSTEM_FROM_IMPORTED`) and the macOS triplets inject vcpkg's
> include directory into port builds, so stray Homebrew/MacPorts headers (for
> example a libjpeg 9 `jpeglib.h`) cannot be mixed with the vcpkg libraries.

### Windows (x64, static, /MT)

From a "x64 Native Tools" prompt or any shell with Visual Studio 2022:

```bat
cmake -S . -B build/x64 -G "Visual Studio 17 2022" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static-pdfdistill
cmake --build build/x64 --config Release
ctest --test-dir build/x64 -C Release --output-on-failure
```

The result `build/x64/Release/pdfdistill-qpdf.exe` depends only on system DLLs
(`KERNEL32`, …).

### CI / releases

`.github/workflows/build.yml` builds, tests and uploads
`pdfdistill-qpdf-macos-universal.tar.gz` and `pdfdistill-qpdf-windows-x64.zip`
on every push/PR (vcpkg binary packages are cached with `actions/cache`, keyed by
the vcpkg commit, `vcpkg.json` and the triplets). Pushing a tag `v*` also
creates a GitHub release with both archives. Each archive contains the
binary, this README, `LICENSE`, `THIRD_PARTY_LICENSES.md` and the dependency
license texts.

## Tests

`ctest` runs 36 tests against `tests/fixtures/ms-print-to-pdf-1.pdf` using a
helper, `pdfcheck` (`tests/pdfcheck.cpp`, built with qpdf + HarfBuzz):

* `pdfcheck check` – `qpdf --check`-equivalent: loads the file, decodes every
  stream (including JPEG), parses all page content, fails on any qpdf warning.
* `pdfcheck compare` – same page count; identical sequence of text-showing
  operations (font name without subset tag + string bytes); for every used CID
  the glyph's extents and advance in the subset font equal the original; same
  number of drawn images, SMasks preserved, image data decodable.
* `pdfcheck maxsize` – size regression guard (default limit 150,000 bytes,
  configurable with `-DPDFDISTILL_SIZE_LIMIT=`).
* CLI behaviour: `--help`, `--version`, usage errors (exit 2), missing input
  (exit 1); stdin→stdout round trip (also with explicit `-o -`) must be
  byte-identical to file output; `--lossless`, `--max-dpi 30`, structure-only;
  re-distilling an already optimized file never grows it; a document made of
  three independently imported copies of the fixture (`pdfcheck concat`) must
  dedup back to roughly the size of one; a Form XObject that shows a glyph with
  the font inherited from the page (`pdfcheck inherit-font`) keeps that glyph;
  RC4/AES-256 encrypted copies (`pdfcheck encrypt`) are passed through unchanged.

## Licenses

pdfdistill-qpdf is MIT licensed (see `LICENSE`). All dependencies are
permissively licensed; see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

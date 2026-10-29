# Third-party components

The release binaries statically link the following libraries (versions as
resolved by vcpkg baseline `d4c0a46cb8f63e5c9fa007d13baf1cb3e43bf7e3`). Full
license texts are shipped in the `licenses/` directory of each release archive
(taken from vcpkg's `share/<port>/copyright`).

| Library | Version | License | Used for |
|---|---|---|---|
| [qpdf](https://github.com/qpdf/qpdf) | 12.4.2 | Apache-2.0 (some files MIT) | PDF parsing, object model, writing; built with the native crypto provider only (no OpenSSL/GnuTLS) |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) (`libharfbuzz`, `libharfbuzz-subset`) | 14.6.0 | MIT ("Old MIT") | TrueType font subsetting |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.0 | IJG + BSD-3-Clause + Zlib | JPEG encoding (and qpdf's DCT decoding) |
| [zlib](https://zlib.net) | 1.3.2 | Zlib | Flate compression |

Build-time only (not linked into the executable): vcpkg (MIT), CMake
(BSD-3-Clause), Meson (Apache-2.0), libspng (BSD-2-Clause, installed by the
HarfBuzz port but unused).

This software is based in part on the work of the Independent JPEG Group.

No GPL/LGPL/AGPL or commercially licensed code is used.

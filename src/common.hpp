// SPDX-License-Identifier: MIT
#pragma once

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace pdfdistill {

struct Options {
    int quality = 75;
    double max_dpi = 150.0;
    bool images = true;
    bool subset = true;
    bool dedup = true;
    bool lossless = false;
    bool verbose = false;
    bool quiet = false;
};

// Logging helpers (always stderr; stdout may carry PDF data).
struct Log {
    bool verbose = false;
    bool quiet = false;
    template <typename... Args>
    void info(Args const&... args) const
    {
        if (verbose && !quiet) {
            (std::cerr << ... << args) << '\n';
        }
    }
    template <typename... Args>
    void warn(Args const&... args) const
    {
        if (!quiet) {
            std::cerr << "pdfdistill-qpdf: warning: ";
            (std::cerr << ... << args) << '\n';
        }
    }
};

// All indirect objects reachable from the trailer (excluding the trailer itself).
std::vector<QPDFObjectHandle> reachable_objects(QPDF& pdf);

// Prevent QPDFWriter from dropping PNG/TIFF predictors when that would grow a stream.
size_t keep_predictor_streams(QPDF& pdf, Log const& log);

// 64-bit FNV-1a hash.
uint64_t fnv1a(unsigned char const* data, size_t len, uint64_t seed = 0xcbf29ce484222325ULL);

inline std::string name_of(QPDFObjectHandle const& oh)
{
    return oh.isName() ? oh.getName() : std::string();
}

// 2D affine matrix [a b c d e f] in PDF convention (row vector * M).
struct Matrix {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
    // Returns this * other  (apply this first, then other).
    Matrix operator*(Matrix const& o) const
    {
        return {a * o.a + b * o.c,
                a * o.b + b * o.d,
                c * o.a + d * o.c,
                c * o.b + d * o.d,
                e * o.a + f * o.c + o.e,
                e * o.b + f * o.d + o.f};
    }
};

bool matrix_from_array(QPDFObjectHandle const& arr, Matrix& m);

// Results of walking all content streams of the document.
struct ContentAnalysis {
    // GIDs used, keyed by the FontFile2 stream they belong to.
    std::map<QPDFObjGen, std::set<uint32_t>> fontfile_gids;
    // FontFile2 streams that must not be subset (unknown/unsupported usage).
    std::set<QPDFObjGen> fontfile_blacklist;
    // Minimum effective DPI (over all placements, min of x/y) per image XObject.
    std::map<QPDFObjGen, double> image_min_dpi;
    // Images whose placement could not be determined (never downsample).
    std::set<QPDFObjGen> image_unknown_dpi;
};

ContentAnalysis analyze_content(QPDF& pdf, Log const& log);

// Optimization passes. Each returns a short human-readable summary.
std::string pass_dedup(QPDF& pdf, Log const& log);
std::string pass_subset_fonts(QPDF& pdf, ContentAnalysis const& ca, Log const& log);
std::string pass_images(QPDF& pdf, ContentAnalysis const& ca, Options const& opt, Log const& log);

// JPEG encoder (libjpeg-turbo). components = 1 (gray) or 3 (RGB).
bool jpeg_encode(
    unsigned char const* pixels,
    int width,
    int height,
    int components,
    int quality,
    std::string& out);

} // namespace pdfdistill

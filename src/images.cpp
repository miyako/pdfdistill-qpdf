// SPDX-License-Identifier: MIT
// Image optimization: downsampling to --max-dpi (based on placement CTM) and
// lossy re-encoding of 8-bit Gray/RGB images as JPEG when that is smaller.
#include "common.hpp"

#include <qpdf/Buffer.hh>

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include <sstream>

namespace pdfdistill {
namespace {

// Area-averaging (box filter) resampler for 8-bit interleaved pixels.
std::string resample(std::string const& src, int w, int h, int nc, int nw, int nh)
{
    auto axis = [](int from, int to) {
        // For each destination index: list of (source index, weight)
        std::vector<std::vector<std::pair<int, double>>> taps(static_cast<size_t>(to));
        double scale = static_cast<double>(from) / to;
        for (int i = 0; i < to; ++i) {
            double s0 = i * scale, s1 = (i + 1) * scale;
            for (int s = static_cast<int>(std::floor(s0)); s < std::min(from, static_cast<int>(std::ceil(s1))); ++s) {
                double wgt = std::min<double>(s + 1, s1) - std::max<double>(s, s0);
                if (wgt > 1e-9) {
                    taps[static_cast<size_t>(i)].emplace_back(s, wgt / scale);
                }
            }
        }
        return taps;
    };
    auto tx = axis(w, nw);
    auto ty = axis(h, nh);
    std::vector<double> tmp(static_cast<size_t>(nw) * h * nc);
    auto in = reinterpret_cast<unsigned char const*>(src.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < nw; ++x) {
            for (int c = 0; c < nc; ++c) {
                double acc = 0;
                for (auto& [s, wgt]: tx[static_cast<size_t>(x)]) {
                    acc += wgt * in[(static_cast<size_t>(y) * w + s) * nc + c];
                }
                tmp[(static_cast<size_t>(y) * nw + x) * nc + c] = acc;
            }
        }
    }
    std::string out(static_cast<size_t>(nw) * nh * nc, '\0');
    for (int y = 0; y < nh; ++y) {
        for (int x = 0; x < nw; ++x) {
            for (int c = 0; c < nc; ++c) {
                double acc = 0;
                for (auto& [s, wgt]: ty[static_cast<size_t>(y)]) {
                    acc += wgt * tmp[(static_cast<size_t>(s) * nw + x) * nc + c];
                }
                long v = std::lround(acc);
                out[(static_cast<size_t>(y) * nw + x) * nc + c] = static_cast<char>(std::clamp(v, 0L, 255L));
            }
        }
    }
    return out;
}

size_t deflated_size(std::string const& data)
{
    uLongf len = compressBound(static_cast<uLong>(data.size()));
    std::vector<Bytef> buf(len);
    if (compress2(buf.data(), &len, reinterpret_cast<Bytef const*>(data.data()), static_cast<uLong>(data.size()), 9) != Z_OK) {
        return data.size();
    }
    return len;
}

// Number of color components if the color space can be stored in a baseline
// JPEG without changing its meaning; 0 otherwise.
int jpeg_components(QPDFObjectHandle cs)
{
    if (cs.isName()) {
        auto n = cs.getName();
        if (n == "/DeviceRGB") {
            return 3;
        }
        if (n == "/DeviceGray") {
            return 1;
        }
        return 0;
    }
    if (cs.isArray() && cs.getArrayNItems() >= 1) {
        auto family = name_of(cs.getArrayItem(0));
        if (family == "/CalRGB") {
            return 3;
        }
        if (family == "/CalGray") {
            return 1;
        }
        if (family == "/ICCBased" && cs.getArrayNItems() >= 2 && cs.getArrayItem(1).isStream()) {
            double n = 0;
            cs.getArrayItem(1).getDict().getKey("/N").getValueAsNumber(n);
            if (n == 1 || n == 3) {
                return static_cast<int>(n);
            }
        }
    }
    return 0;
}

bool default_decode(QPDFObjectHandle decode, int nc)
{
    if (decode.isNull()) {
        return true;
    }
    if (!decode.isArray() || decode.getArrayNItems() != 2 * nc) {
        return false;
    }
    for (int i = 0; i < 2 * nc; ++i) {
        double v = -1;
        if (!decode.getArrayItem(i).getValueAsNumber(v) || v != (i % 2 ? 1.0 : 0.0)) {
            return false;
        }
    }
    return true;
}

// Filters that qpdf can decode losslessly at qpdf_dl_generalized.
enum class Source { Lossless, Dct, Unsupported };

Source classify_filters(QPDFObjectHandle filter)
{
    std::vector<std::string> names;
    if (filter.isName()) {
        names.push_back(filter.getName());
    } else if (filter.isArray()) {
        for (auto const& f: filter.getArrayAsVector()) {
            if (!f.isName()) {
                return Source::Unsupported;
            }
            names.push_back(f.getName());
        }
    } else if (!filter.isNull()) {
        return Source::Unsupported;
    }
    bool dct = false;
    for (size_t i = 0; i < names.size(); ++i) {
        auto const& n = names[i];
        if (n == "/DCTDecode" && i + 1 == names.size()) {
            dct = true;
        } else if (n != "/FlateDecode" && n != "/LZWDecode" && n != "/ASCIIHexDecode" && n != "/ASCII85Decode" &&
                   n != "/RunLengthDecode") {
            return Source::Unsupported;
        }
    }
    return dct ? Source::Dct : Source::Lossless;
}

struct Resized {
    int ow, oh, nw, nh;
};

} // namespace

std::string pass_images(QPDF& pdf, ContentAnalysis const& ca, Options const& opt, Log const& log)
{
    if (opt.lossless) {
        return "skipped (--lossless)";
    }
    auto objects = reachable_objects(pdf);

    // Objects used as soft masks / stencil masks are never JPEG-encoded.
    std::map<QPDFObjGen, std::vector<QPDFObjGen>> smask_parents;
    std::set<QPDFObjGen> masks;
    for (auto& oh: objects) {
        if (!oh.isStream()) {
            continue;
        }
        auto dict = oh.getDict();
        if (name_of(dict.getKey("/Subtype")) != "/Image") {
            continue;
        }
        auto sm = dict.getKey("/SMask");
        if (sm.isStream() && sm.isIndirect()) {
            masks.insert(sm.getObjGen());
            smask_parents[sm.getObjGen()].push_back(oh.getObjGen());
        }
        auto m = dict.getKey("/Mask");
        if (m.isStream() && m.isIndirect()) {
            masks.insert(m.getObjGen());
        }
    }

    std::map<QPDFObjGen, Resized> resized;
    size_t n_jpeg = 0, n_down = 0, before = 0, after = 0;

    for (auto& oh: objects) {
        if (!oh.isStream()) {
            continue;
        }
        auto og = oh.getObjGen();
        auto dict = oh.getDict();
        if (name_of(dict.getKey("/Subtype")) != "/Image" || masks.count(og)) {
            continue;
        }
        if (dict.getKey("/ImageMask").isBool() && dict.getKey("/ImageMask").getBoolValue()) {
            continue;
        }
        double wd = 0, hd = 0, bpc = 0;
        if (!dict.getKey("/Width").getValueAsNumber(wd) || !dict.getKey("/Height").getValueAsNumber(hd) ||
            !dict.getKey("/BitsPerComponent").getValueAsNumber(bpc) || bpc != 8 || wd < 1 || hd < 1 ||
            wd * hd > 2.0e8) {
            continue;
        }
        int w = static_cast<int>(wd), h = static_cast<int>(hd);
        int nc = jpeg_components(dict.getKey("/ColorSpace"));
        if (nc == 0 || !default_decode(dict.getKey("/Decode"), nc) || dict.getKey("/Mask").isArray() ||
            dict.hasKey("/SMaskInData")) {
            continue;
        }
        auto source = classify_filters(dict.getKey("/Filter"));
        if (source == Source::Unsupported) {
            continue;
        }

        int nw = w, nh = h;
        if (opt.max_dpi > 0 && !ca.image_unknown_dpi.count(og)) {
            auto it = ca.image_min_dpi.find(og);
            if (it != ca.image_min_dpi.end() && it->second > opt.max_dpi * 1.05) {
                double scale = opt.max_dpi / it->second;
                nw = std::max(1, static_cast<int>(std::lround(w * scale)));
                nh = std::max(1, static_cast<int>(std::lround(h * scale)));
            }
        }
        bool downsample = nw != w || nh != h;
        if (source == Source::Dct && !downsample) {
            continue; // already JPEG; do not recompress lossy data
        }

        size_t raw_size = 0;
        std::string pixels;
        try {
            raw_size = oh.getRawStreamData()->getSize();
            pixels = oh.getStreamData(source == Source::Dct ? qpdf_dl_all : qpdf_dl_generalized)->move();
        } catch (std::exception& e) {
            log.info("  image ", og.unparse(' '), ": cannot decode: ", e.what());
            continue;
        }
        if (pixels.size() < static_cast<size_t>(w) * h * nc) {
            continue;
        }
        pixels.resize(static_cast<size_t>(w) * h * nc);
        if (downsample) {
            pixels = resample(pixels, w, h, nc, nw, nh);
        }
        size_t baseline = downsample ? deflated_size(pixels) : raw_size;
        std::string jpeg;
        bool use_jpeg = jpeg_encode(
                            reinterpret_cast<unsigned char const*>(pixels.data()), nw, nh, nc, opt.quality, jpeg) &&
            jpeg.size() < baseline;
        size_t chosen = use_jpeg ? jpeg.size() : baseline;
        if (chosen >= raw_size) {
            log.info("  image ", og.unparse(' '), " ", w, "x", h, ": no smaller encoding found, kept");
            continue;
        }
        if (!use_jpeg && !downsample) {
            log.info("  image ", og.unparse(' '), " ", w, "x", h, ": JPEG not smaller, kept");
            continue;
        }
        if (use_jpeg) {
            oh.replaceStreamData(jpeg, QPDFObjectHandle::newName("/DCTDecode"), QPDFObjectHandle::newNull());
            ++n_jpeg;
        } else {
            oh.replaceStreamData(pixels, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        }
        if (downsample) {
            dict.replaceKey("/Width", QPDFObjectHandle::newInteger(nw));
            dict.replaceKey("/Height", QPDFObjectHandle::newInteger(nh));
            resized[og] = Resized{w, h, nw, nh};
            ++n_down;
        }
        size_t new_size = use_jpeg ? jpeg.size() : baseline;
        log.info(
            "  image ",
            og.unparse(' '),
            " ",
            w,
            "x",
            h,
            " -> ",
            nw,
            "x",
            nh,
            use_jpeg ? " JPEG" : " Flate",
            ": ",
            raw_size,
            " -> ",
            new_size,
            " bytes");
        before += raw_size;
        after += new_size;
    }

    // Soft masks follow their parent's new resolution (kept lossless).
    for (auto const& [smog, parents]: smask_parents) {
        std::optional<Resized> target;
        bool consistent = true;
        for (auto const& p: parents) {
            auto it = resized.find(p);
            if (it == resized.end()) {
                consistent = false;
                break;
            }
            if (target && (target->nw != it->second.nw || target->nh != it->second.nh)) {
                consistent = false;
                break;
            }
            target = it->second;
        }
        if (!consistent || !target) {
            continue;
        }
        auto sm = pdf.getObject(smog);
        auto d = sm.getDict();
        double sw = 0, sh = 0, bpc = 0;
        d.getKey("/Width").getValueAsNumber(sw);
        d.getKey("/Height").getValueAsNumber(sh);
        d.getKey("/BitsPerComponent").getValueAsNumber(bpc);
        if (bpc != 8 || static_cast<int>(sw) != target->ow || static_cast<int>(sh) != target->oh ||
            classify_filters(d.getKey("/Filter")) != Source::Lossless || !default_decode(d.getKey("/Decode"), 1)) {
            continue;
        }
        std::string px;
        try {
            px = sm.getStreamData(qpdf_dl_generalized)->move();
        } catch (std::exception&) {
            continue;
        }
        if (px.size() < static_cast<size_t>(target->ow) * target->oh) {
            continue;
        }
        px.resize(static_cast<size_t>(target->ow) * target->oh);
        px = resample(px, target->ow, target->oh, 1, target->nw, target->nh);
        sm.replaceStreamData(px, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        d.replaceKey("/Width", QPDFObjectHandle::newInteger(target->nw));
        d.replaceKey("/Height", QPDFObjectHandle::newInteger(target->nh));
        log.info("  soft mask ", smog.unparse(' '), " resampled to ", target->nw, "x", target->nh);
    }

    std::ostringstream s;
    s << n_jpeg << " image(s) JPEG-encoded, " << n_down << " downsampled, " << before << " -> " << after
      << " bytes";
    return s.str();
}

} // namespace pdfdistill

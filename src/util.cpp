// SPDX-License-Identifier: MIT
#include "common.hpp"

#include <qpdf/Buffer.hh>

#include <zlib.h>

#include <unordered_set>
#include <vector>

namespace pdfdistill {

uint64_t fnv1a(unsigned char const* data, size_t len, uint64_t seed)
{
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool matrix_from_array(QPDFObjectHandle const& arr, Matrix& m)
{
    if (!arr.isArray() || arr.getArrayNItems() != 6) {
        return false;
    }
    double v[6];
    for (int i = 0; i < 6; ++i) {
        if (!arr.getArrayItem(i).getValueAsNumber(v[i])) {
            return false;
        }
    }
    m = {v[0], v[1], v[2], v[3], v[4], v[5]};
    return true;
}

std::vector<QPDFObjectHandle> reachable_objects(QPDF& pdf)
{
    std::vector<QPDFObjectHandle> result;
    std::set<QPDFObjGen> seen;
    std::vector<QPDFObjectHandle> stack{pdf.getTrailer()};
    while (!stack.empty()) {
        QPDFObjectHandle oh = stack.back();
        stack.pop_back();
        if (oh.isIndirect()) {
            auto og = oh.getObjGen();
            if (!seen.insert(og).second) {
                continue;
            }
            if (oh.isNull()) {
                continue; // dangling reference
            }
            result.push_back(oh);
        }
        if (oh.isStream()) {
            stack.push_back(oh.getDict());
        } else if (oh.isArray()) {
            for (auto const& item: oh.getArrayAsVector()) {
                if (item.isIndirect() || item.isArray() || item.isDictionary()) {
                    stack.push_back(item);
                }
            }
        } else if (oh.isDictionary()) {
            for (auto const& key: oh.getKeys()) {
                auto item = oh.getKey(key);
                if (item.isIndirect() || item.isArray() || item.isDictionary()) {
                    stack.push_back(item);
                }
            }
        }
    }
    return result;
}

} // namespace pdfdistill

namespace pdfdistill {

size_t keep_predictor_streams(QPDF& pdf, Log const& log)
{
    // QPDFWriter's generalized decode + Flate recompression drops PNG/TIFF
    // predictors, which can make image-like streams much larger. Keep such a
    // stream as-is unless plain max-level Flate is actually smaller.
    size_t kept = 0;
    for (auto& oh: reachable_objects(pdf)) {
        if (!oh.isStream()) {
            continue;
        }
        auto parms = oh.getDict().getKey("/DecodeParms");
        if (parms.isArray() && parms.getArrayNItems() > 0) {
            parms = parms.getArrayItem(parms.getArrayNItems() - 1);
        }
        double predictor = 1;
        if (!parms.isDictionary() || !parms.getKey("/Predictor").getValueAsNumber(predictor) || predictor < 2) {
            continue;
        }
        try {
            size_t raw = oh.getRawStreamData()->getSize();
            auto decoded = oh.getStreamData(qpdf_dl_generalized);
            uLongf len = compressBound(static_cast<uLong>(decoded->getSize()));
            std::vector<Bytef> buf(len);
            if (compress2(buf.data(), &len, decoded->getBuffer(), static_cast<uLong>(decoded->getSize()), 9) ==
                    Z_OK &&
                len < raw) {
                continue;
            }
        } catch (std::exception&) {
        }
        oh.setFilterOnWrite(false);
        ++kept;
    }
    if (kept) {
        log.info("  kept ", kept, " predictor-compressed stream(s) as-is");
    }
    return kept;
}

} // namespace pdfdistill

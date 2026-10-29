// SPDX-License-Identifier: MIT
// Duplicate object removal: identical streams (images, fonts, forms, ...) and
// identical font-related dictionaries are merged and all references rewritten.
#include "common.hpp"

#include <qpdf/Buffer.hh>

#include <cstring>
#include <map>
#include <sstream>

namespace pdfdistill {
namespace {

struct StreamDigest {
    bool decoded = false;
    uint64_t hash = 0;
    size_t size = 0;
};

bool get_decoded(QPDFObjectHandle& s, std::shared_ptr<Buffer>& data, bool& decoded)
{
    try {
        data = s.getStreamData(qpdf_dl_generalized);
        decoded = true;
        return true;
    } catch (std::exception&) {
    }
    try {
        data = s.getRawStreamData();
        decoded = false;
        return true;
    } catch (std::exception&) {
    }
    return false;
}

std::string dict_key(QPDFObjectHandle const& dict, bool skip_filter)
{
    std::string out;
    for (auto const& key: dict.getKeys()) {
        if (key == "/Length" || key == "/DL") {
            continue;
        }
        if (skip_filter && (key == "/Filter" || key == "/DecodeParms")) {
            continue;
        }
        out += key;
        out += ' ';
        out += dict.getKey(key).unparse();
        out += '\n';
    }
    return out;
}

bool is_dedupable_dict(QPDFObjectHandle const& oh)
{
    if (oh.isArray() || oh.isString() || oh.isName() || oh.isNumber() || oh.isBool()) {
        // Value-like objects (e.g. /W widths, /FontBBox, /CIDSystemInfo strings).
        return true;
    }
    if (!oh.isDictionary()) {
        return false;
    }
    auto type = name_of(oh.getKey("/Type"));
    if (type == "/Font" || type == "/FontDescriptor" || type == "/ExtGState" || type == "/Encoding" ||
        type == "/Pattern" || type == "/Shading") {
        return true;
    }
    if (!type.empty()) {
        return false;
    }
    // Untyped dictionaries are merged only when they look like plain values,
    // never when they could be nodes with identity (pages, annotations,
    // outline items, structure elements, form fields, ...).
    for (char const* k: {"/Parent", "/P", "/Kids", "/Rect", "/Next", "/Prev", "/First", "/Last", "/FT", "/T",
                         "/Subtype", "/S", "/K", "/Pg"}) {
        if (oh.hasKey(k)) {
            return false;
        }
    }
    return true;
}

bool is_dedupable_stream(QPDFObjectHandle const& oh)
{
    auto type = name_of(oh.getDict().getKey("/Type"));
    return type != "/XRef" && type != "/ObjStm";
}

void rewrite_refs(QPDFObjectHandle oh, std::map<QPDFObjGen, QPDFObjectHandle> const& map, int depth = 0)
{
    if (depth > 256) {
        return;
    }
    if (oh.isStream()) {
        rewrite_refs(oh.getDict(), map, depth + 1);
        return;
    }
    if (oh.isArray()) {
        int n = oh.getArrayNItems();
        for (int i = 0; i < n; ++i) {
            auto item = oh.getArrayItem(i);
            if (item.isIndirect()) {
                auto it = map.find(item.getObjGen());
                if (it != map.end()) {
                    oh.setArrayItem(i, it->second);
                }
            } else if (item.isArray() || item.isDictionary()) {
                rewrite_refs(item, map, depth + 1);
            }
        }
    } else if (oh.isDictionary()) {
        for (auto const& key: oh.getKeys()) {
            auto item = oh.getKey(key);
            if (item.isIndirect()) {
                auto it = map.find(item.getObjGen());
                if (it != map.end()) {
                    oh.replaceKey(key, it->second);
                }
            } else if (item.isArray() || item.isDictionary()) {
                rewrite_refs(item, map, depth + 1);
            }
        }
    }
}

} // namespace

std::string pass_dedup(QPDF& pdf, Log const& log)
{
    std::map<QPDFObjGen, StreamDigest> digest_cache;
    size_t merged_streams = 0;
    size_t merged_objects = 0;

    for (int round = 0; round < 16; ++round) {
        auto objects = reachable_objects(pdf);
        std::map<std::string, std::vector<QPDFObjectHandle>> groups;
        for (auto& oh: objects) {
            if (oh.isStream()) {
                if (!is_dedupable_stream(oh)) {
                    continue;
                }
                auto og = oh.getObjGen();
                auto it = digest_cache.find(og);
                if (it == digest_cache.end()) {
                    std::shared_ptr<Buffer> data;
                    StreamDigest d;
                    if (!get_decoded(oh, data, d.decoded)) {
                        continue;
                    }
                    d.size = data->getSize();
                    d.hash = fnv1a(data->getBuffer(), data->getSize());
                    it = digest_cache.emplace(og, d).first;
                }
                auto const& d = it->second;
                std::ostringstream key;
                key << "S" << d.decoded << ':' << d.size << ':' << std::hex << d.hash << '\n'
                    << dict_key(oh.getDict(), d.decoded);
                groups[key.str()].push_back(oh);
            } else if (is_dedupable_dict(oh)) {
                groups[oh.isDictionary() ? "D" + dict_key(oh, false) : "V" + oh.unparseResolved()].push_back(oh);
            }
        }

        std::map<QPDFObjGen, QPDFObjectHandle> replace;
        for (auto& [key, members]: groups) {
            if (members.size() < 2) {
                continue;
            }
            auto& rep = members.front();
            std::shared_ptr<Buffer> rep_data;
            bool rep_decoded = false;
            if (rep.isStream() && !get_decoded(rep, rep_data, rep_decoded)) {
                continue;
            }
            for (size_t i = 1; i < members.size(); ++i) {
                auto& m = members[i];
                if (m.isStream()) {
                    std::shared_ptr<Buffer> data;
                    bool decoded = false;
                    if (!get_decoded(m, data, decoded) || decoded != rep_decoded ||
                        data->getSize() != rep_data->getSize() ||
                        (data->getSize() &&
                         memcmp(data->getBuffer(), rep_data->getBuffer(), data->getSize()) != 0)) {
                        continue;
                    }
                    ++merged_streams;
                } else {
                    ++merged_objects;
                }
                replace[m.getObjGen()] = rep;
            }
        }
        if (replace.empty()) {
            break;
        }
        log.info("  dedup round ", round + 1, ": merging ", replace.size(), " object(s)");
        rewrite_refs(pdf.getTrailer(), replace);
        for (auto& oh: objects) {
            if (!replace.count(oh.getObjGen())) {
                rewrite_refs(oh, replace);
            }
        }
    }
    std::ostringstream s;
    s << "merged " << merged_streams << " stream(s), " << merged_objects << " dict/array object(s)";
    return s.str();
}

} // namespace pdfdistill

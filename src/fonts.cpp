// SPDX-License-Identifier: MIT
// TrueType font subsetting with HarfBuzz hb-subset. Glyph IDs are retained so
// /CIDToGIDMap (Identity or stream) and the content streams stay valid.
#include "common.hpp"

#include <hb-subset.h>
#include <hb.h>

#include <qpdf/Buffer.hh>

#include <sstream>

namespace pdfdistill {
namespace {

std::string subset_tag(std::set<uint32_t> const& gids, QPDFObjGen og)
{
    uint64_t h = 0xcbf29ce484222325ULL ^ static_cast<uint64_t>(og.getObj());
    for (auto g: gids) {
        unsigned char b[4] = {
            static_cast<unsigned char>(g), static_cast<unsigned char>(g >> 8), static_cast<unsigned char>(g >> 16), 0};
        h = fnv1a(b, 4, h);
    }
    std::string tag;
    for (int i = 0; i < 6; ++i) {
        tag += static_cast<char>('A' + (h % 26));
        h /= 26;
    }
    return tag;
}

std::string tagged(std::string const& name, std::string const& tag)
{
    // name includes the leading '/'
    std::string base = name.substr(1);
    if (base.size() > 7 && base[6] == '+') {
        bool is_tag = true;
        for (int i = 0; i < 6; ++i) {
            is_tag = is_tag && base[i] >= 'A' && base[i] <= 'Z';
        }
        if (is_tag) {
            base = base.substr(7);
        }
    }
    return "/" + tag + "+" + base;
}

bool subset_font(std::string const& in, std::set<uint32_t> const& gids, std::string& out)
{
    hb_blob_t* blob = hb_blob_create(in.data(), static_cast<unsigned>(in.size()), HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    unsigned num_glyphs = hb_face_get_glyph_count(face);
    if (num_glyphs == 0) {
        hb_face_destroy(face);
        return false;
    }
    hb_subset_input_t* input = hb_subset_input_create_or_fail();
    if (!input) {
        hb_face_destroy(face);
        return false;
    }
    hb_set_t* glyph_set = hb_subset_input_glyph_set(input);
    for (auto g: gids) {
        if (g < num_glyphs) {
            hb_set_add(glyph_set, g);
        }
    }
    // Keep cmap entries for the retained glyphs so the subset font remains a
    // well-formed TrueType font for picky consumers.
    hb_map_t* mapping = hb_map_create();
    hb_set_t* unicodes = hb_set_create();
    hb_face_collect_nominal_glyph_mapping(face, mapping, unicodes);
    hb_set_t* in_unicodes = hb_subset_input_unicode_set(input);
    hb_codepoint_t u = HB_SET_VALUE_INVALID;
    while (hb_set_next(unicodes, &u)) {
        if (hb_set_has(glyph_set, hb_map_get(mapping, u))) {
            hb_set_add(in_unicodes, u);
        }
    }
    hb_set_destroy(unicodes);
    hb_map_destroy(mapping);

    // Layout tables are useless inside a PDF (text is already shaped) and would
    // pull extra glyphs into the closure.
    hb_set_t* drop = hb_subset_input_set(input, HB_SUBSET_SETS_DROP_TABLE_TAG);
    for (auto tag: {"GSUB", "GPOS", "GDEF", "BASE", "JSTF", "MATH", "morx", "mort", "kerx", "kern", "DSIG"}) {
        hb_set_add(drop, hb_tag_from_string(tag, 4));
    }
    hb_subset_input_set_flags(input, HB_SUBSET_FLAGS_RETAIN_GIDS);

    hb_face_t* sub = hb_subset_or_fail(face, input);
    hb_subset_input_destroy(input);
    hb_face_destroy(face);
    if (!sub) {
        return false;
    }
    hb_blob_t* result = hb_face_reference_blob(sub);
    unsigned len = 0;
    char const* data = hb_blob_get_data(result, &len);
    bool ok = data && len > 0;
    if (ok) {
        out.assign(data, len);
    }
    hb_blob_destroy(result);
    hb_face_destroy(sub);
    return ok;
}

} // namespace

std::string pass_subset_fonts(QPDF& pdf, ContentAnalysis const& ca, Log const& log)
{
    // FontFile2 -> list of (Type0 font, CIDFont, FontDescriptor)
    struct Users {
        std::vector<QPDFObjectHandle> type0, cidfont, descriptor;
    };
    std::map<QPDFObjGen, Users> users;
    for (auto& oh: reachable_objects(pdf)) {
        if (!oh.isDictionary() || name_of(oh.getKey("/Subtype")) != "/Type0") {
            continue;
        }
        auto desc = oh.getKey("/DescendantFonts");
        if (!desc.isArray() || desc.getArrayNItems() == 0) {
            continue;
        }
        auto cid = desc.getArrayItem(0);
        auto fd = cid.isDictionary() ? cid.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
        auto ff = fd.isDictionary() ? fd.getKey("/FontFile2") : QPDFObjectHandle::newNull();
        if (ff.isStream() && ff.isIndirect()) {
            auto& u = users[ff.getObjGen()];
            u.type0.push_back(oh);
            u.cidfont.push_back(cid);
            u.descriptor.push_back(fd);
        }
    }

    size_t count = 0;
    size_t before = 0, after = 0;
    for (auto const& [og, gids]: ca.fontfile_gids) {
        if (ca.fontfile_blacklist.count(og) || !users.count(og)) {
            continue;
        }
        auto ff = pdf.getObject(og);
        if (!ff.isStream()) {
            continue;
        }
        std::string original;
        try {
            original = ff.getStreamData(qpdf_dl_generalized)->move();
        } catch (std::exception& e) {
            log.info("  cannot decode font program ", og.unparse(' '), ": ", e.what());
            continue;
        }
        std::string subset;
        if (!subset_font(original, gids, subset) || subset.size() >= original.size()) {
            log.info("  font ", og.unparse(' '), ": subsetting not beneficial/possible");
            continue;
        }
        ff.replaceStreamData(subset, QPDFObjectHandle::newNull(), QPDFObjectHandle::newNull());
        ff.getDict().replaceKey("/Length1", QPDFObjectHandle::newInteger(static_cast<long long>(subset.size())));
        auto tag = subset_tag(gids, og);
        auto& u = users[og];
        std::set<QPDFObjGen> renamed;
        auto rename = [&](QPDFObjectHandle& dict, char const* key) {
            if (dict.isIndirect() && !renamed.insert(dict.getObjGen()).second) {
                return;
            }
            auto n = dict.getKey(key);
            if (n.isName()) {
                dict.replaceKey(key, QPDFObjectHandle::newName(tagged(n.getName(), tag)));
            }
        };
        for (size_t i = 0; i < u.type0.size(); ++i) {
            rename(u.type0[i], "/BaseFont");
            rename(u.cidfont[i], "/BaseFont");
            rename(u.descriptor[i], "/FontName");
            // CIDSet would now be inaccurate; it is optional.
            u.descriptor[i].removeKey("/CIDSet");
        }
        log.info(
            "  font ",
            og.unparse(' '),
            ": kept ",
            gids.size(),
            " glyph(s), program ",
            original.size(),
            " -> ",
            subset.size(),
            " bytes (uncompressed)");
        before += original.size();
        after += subset.size();
        ++count;
    }
    std::ostringstream s;
    s << "subset " << count << " font(s), " << before << " -> " << after << " bytes uncompressed";
    return s.str();
}

} // namespace pdfdistill

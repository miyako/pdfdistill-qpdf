// SPDX-License-Identifier: MIT
// Content stream analysis: which glyphs of embedded TrueType CID fonts are
// actually used, and at what effective resolution each image is drawn.
#include "common.hpp"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <memory>

namespace pdfdistill {
namespace {

struct FontInfo {
    bool subsettable = false; // Type0 / Identity-H|V / CIDFontType2 / FontFile2
    bool has_fontfile = false;
    QPDFObjGen fontfile;
    bool identity = true;
    std::vector<uint16_t> cid_to_gid;
    QPDFObjectHandle type3; // set for Type3 fonts
};

class Walker
{
  public:
    Walker(QPDF& pdf, ContentAnalysis& ca, Log const& log) :
        pdf(pdf),
        ca(ca),
        log(log)
    {
    }

    void run();

  private:
    struct GState {
        Matrix ctm;
        std::shared_ptr<FontInfo> font;
    };

    class Callbacks: public QPDFObjectHandle::ParserCallbacks
    {
      public:
        Callbacks(
            Walker& w,
            QPDFObjectHandle resources,
            Matrix ctm,
            bool ctm_known,
            int depth,
            std::shared_ptr<FontInfo> font) :
            w(w),
            resources(std::move(resources)),
            ctm_known(ctm_known),
            depth(depth)
        {
            stack.push_back(GState{ctm, std::move(font)});
        }
        void handleObject(QPDFObjectHandle obj) override;
        void handleEOF() override {}

      private:
        void show(QPDFObjectHandle const& str);
        Walker& w;
        QPDFObjectHandle resources;
        bool ctm_known;
        int depth;
        std::vector<GState> stack;
        std::vector<QPDFObjectHandle> operands;
    };

    void walk(
        QPDFObjectHandle contents,
        QPDFObjectHandle resources,
        Matrix const& ctm,
        bool ctm_known,
        int depth,
        std::shared_ptr<FontInfo> font = nullptr);
    void visit_resources(QPDFObjectHandle resources, int depth);
    void walk_form(
        QPDFObjectHandle form,
        QPDFObjectHandle parent_resources,
        Matrix const& ctm,
        bool ctm_known,
        int depth,
        std::shared_ptr<FontInfo> font = nullptr);
    void poison_fonts(QPDFObjectHandle resources);
    void walk_unknown_placement(QPDFObjectHandle form, QPDFObjectHandle parent_resources, int depth);
    std::shared_ptr<FontInfo> font_info(QPDFObjectHandle font, int depth);
    void poison(QPDFObjectHandle resources);
    void image_drawn(QPDFObjectHandle image, Matrix const& ctm, bool ctm_known);

    QPDF& pdf;
    ContentAnalysis& ca;
    Log const& log;
    std::map<QPDFObjGen, std::shared_ptr<FontInfo>> font_cache;
    std::set<QPDFObjGen> seen_fonts;
    std::set<QPDFObjGen> visited_resources;
    std::set<QPDFObjGen> form_stack;
    std::set<std::string> walked_forms;
    size_t budget = 200000; // max number of stream walks
};

std::shared_ptr<FontInfo> Walker::font_info(QPDFObjectHandle font, int depth)
{
    if (!font.isDictionary()) {
        return nullptr;
    }
    if (font.isIndirect()) {
        auto it = font_cache.find(font.getObjGen());
        if (it != font_cache.end()) {
            return it->second;
        }
        seen_fonts.insert(font.getObjGen());
    }
    auto info = std::make_shared<FontInfo>();
    if (font.isIndirect()) {
        font_cache[font.getObjGen()] = info;
    }
    auto subtype = name_of(font.getKey("/Subtype"));
    if (subtype == "/Type3") {
        info->type3 = font;
        auto procs = font.getKey("/CharProcs");
        auto res = font.getKey("/Resources");
        if (procs.isDictionary()) {
            for (auto const& k: procs.getKeys()) {
                auto proc = procs.getKey(k);
                if (proc.isStream()) {
                    walk(proc, res, Matrix{}, false, depth + 1);
                }
            }
        }
        return info;
    }
    if (subtype == "/Type0") {
        auto desc = font.getKey("/DescendantFonts");
        if (desc.isArray() && desc.getArrayNItems() > 0) {
            desc = desc.getArrayItem(0);
        }
        if (desc.isIndirect()) {
            seen_fonts.insert(desc.getObjGen());
        }
        auto fd = desc.isDictionary() ? desc.getKey("/FontDescriptor") : QPDFObjectHandle::newNull();
        auto ff = fd.isDictionary() ? fd.getKey("/FontFile2") : QPDFObjectHandle::newNull();
        if (!ff.isStream() || !ff.isIndirect()) {
            return info;
        }
        info->has_fontfile = true;
        info->fontfile = ff.getObjGen();
        auto enc = name_of(font.getKey("/Encoding"));
        bool ok = (enc == "/Identity-H" || enc == "/Identity-V") &&
            name_of(desc.getKey("/Subtype")) == "/CIDFontType2";
        auto map = desc.getKey("/CIDToGIDMap");
        if (ok && map.isStream()) {
            try {
                auto data = map.getStreamData(qpdf_dl_generalized);
                auto p = data->getBuffer();
                info->identity = false;
                for (size_t i = 0; i + 1 < data->getSize(); i += 2) {
                    info->cid_to_gid.push_back(static_cast<uint16_t>((p[i] << 8) | p[i + 1]));
                }
            } catch (std::exception&) {
                ok = false;
            }
        } else if (ok && !(map.isNull() || name_of(map) == "/Identity")) {
            ok = false;
        }
        info->subsettable = ok;
        if (!ok) {
            ca.fontfile_blacklist.insert(info->fontfile);
        } else {
            ca.fontfile_gids[info->fontfile].insert(0); // .notdef
        }
        return info;
    }
    // Simple fonts: never subset their TrueType programs (shared programs are
    // also protected this way).
    auto fd = font.getKey("/FontDescriptor");
    if (fd.isDictionary()) {
        auto ff = fd.getKey("/FontFile2");
        if (ff.isStream() && ff.isIndirect()) {
            ca.fontfile_blacklist.insert(ff.getObjGen());
        }
    }
    return info;
}

void Walker::poison_fonts(QPDFObjectHandle resources)
{
    if (!resources.isDictionary()) {
        return;
    }
    auto fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        for (auto const& k: fonts.getKeys()) {
            auto info = font_info(fonts.getKey(k), 0);
            if (info && info->has_fontfile) {
                ca.fontfile_blacklist.insert(info->fontfile);
            }
        }
    }
}

void Walker::poison(QPDFObjectHandle resources)
{
    if (!resources.isDictionary()) {
        return;
    }
    auto fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        for (auto const& k: fonts.getKeys()) {
            auto info = font_info(fonts.getKey(k), 0);
            if (info && info->has_fontfile) {
                ca.fontfile_blacklist.insert(info->fontfile);
            }
        }
    }
    auto xobjs = resources.getKey("/XObject");
    if (xobjs.isDictionary()) {
        for (auto const& k: xobjs.getKeys()) {
            auto x = xobjs.getKey(k);
            if (x.isStream() && x.isIndirect()) {
                ca.image_unknown_dpi.insert(x.getObjGen());
            }
        }
    }
}

void Walker::image_drawn(QPDFObjectHandle image, Matrix const& ctm, bool ctm_known)
{
    if (!image.isIndirect()) {
        return;
    }
    auto og = image.getObjGen();
    auto dict = image.getDict();
    double w = 0, h = 0;
    dict.getKey("/Width").getValueAsNumber(w);
    dict.getKey("/Height").getValueAsNumber(h);
    double wp = std::hypot(ctm.a, ctm.b);
    double hp = std::hypot(ctm.c, ctm.d);
    if (!ctm_known || wp < 1e-3 || hp < 1e-3 || w <= 0 || h <= 0) {
        ca.image_unknown_dpi.insert(og);
        return;
    }
    double dpi = std::min(w * 72.0 / wp, h * 72.0 / hp);
    auto it = ca.image_min_dpi.find(og);
    if (it == ca.image_min_dpi.end() || dpi < it->second) {
        ca.image_min_dpi[og] = dpi;
    }
}

void Walker::visit_resources(QPDFObjectHandle resources, int depth)
{
    if (!resources.isDictionary()) {
        return;
    }
    if (resources.isIndirect() && !visited_resources.insert(resources.getObjGen()).second) {
        return;
    }
    auto fonts = resources.getKey("/Font");
    if (fonts.isDictionary()) {
        for (auto const& k: fonts.getKeys()) {
            font_info(fonts.getKey(k), depth);
        }
    }
    // Tiling patterns and soft-mask groups carry content streams too. Their
    // placement is not tracked, so images inside are never downsampled.
    auto patterns = resources.getKey("/Pattern");
    if (patterns.isDictionary()) {
        for (auto const& k: patterns.getKeys()) {
            auto p = patterns.getKey(k);
            if (p.isStream()) {
                walk_unknown_placement(p, resources, depth);
            }
        }
    }
    auto gstates = resources.getKey("/ExtGState");
    if (gstates.isDictionary()) {
        for (auto const& k: gstates.getKeys()) {
            auto gs = gstates.getKey(k);
            if (!gs.isDictionary()) {
                continue;
            }
            auto smask = gs.getKey("/SMask");
            if (smask.isDictionary() && smask.getKey("/G").isStream()) {
                walk_unknown_placement(smask.getKey("/G"), resources, depth);
            }
            auto f = gs.getKey("/Font");
            if (f.isArray() && f.getArrayNItems() > 0) {
                font_info(f.getArrayItem(0), depth);
            }
        }
    }
}

void Walker::walk_unknown_placement(QPDFObjectHandle form, QPDFObjectHandle parent_resources, int depth)
{
    walk_form(form, parent_resources, Matrix{}, false, depth);
}

void Walker::walk_form(
    QPDFObjectHandle form,
    QPDFObjectHandle parent_resources,
    Matrix const& ctm,
    bool ctm_known,
    int depth,
    std::shared_ptr<FontInfo> font)
{
    if (!form.isStream()) {
        return;
    }
    auto dict = form.getDict();
    Matrix m;
    Matrix fm;
    if (matrix_from_array(dict.getKey("/Matrix"), fm)) {
        m = fm * ctm;
    } else {
        m = ctm;
    }
    auto res = dict.getKey("/Resources");
    if (!res.isDictionary()) {
        res = parent_resources;
    }
    QPDFObjGen og = form.isIndirect() ? form.getObjGen() : QPDFObjGen();
    if (form.isIndirect()) {
        if (form_stack.count(og)) {
            return; // recursive form
        }
        char key[256];
        snprintf(
            key,
            sizeof(key),
            "%d %d %d %p %.3g %.3g %.3g %.3g %.3g %.3g",
            og.getObj(),
            og.getGen(),
            ctm_known ? 1 : 0,
            static_cast<void*>(font.get()),
            m.a,
            m.b,
            m.c,
            m.d,
            m.e,
            m.f);
        if (!walked_forms.insert(key).second) {
            return;
        }
        form_stack.insert(og);
    }
    walk(form, res, m, ctm_known, depth + 1, std::move(font));
    if (form.isIndirect()) {
        form_stack.erase(og);
    }
}

void Walker::walk(
    QPDFObjectHandle contents,
    QPDFObjectHandle resources,
    Matrix const& ctm,
    bool ctm_known,
    int depth,
    std::shared_ptr<FontInfo> font)
{
    if (depth > 64 || budget == 0) {
        poison(resources);
        return;
    }
    --budget;
    visit_resources(resources, depth);
    Callbacks cb(*this, resources, ctm, ctm_known, depth, std::move(font));
    try {
        QPDFObjectHandle::parseContentStream(contents, &cb);
    } catch (std::exception& e) {
        log.info("  content parse problem: ", e.what());
        poison(resources);
    }
}

void Walker::Callbacks::show(QPDFObjectHandle const& str)
{
    auto& font = stack.back().font;
    if (!str.isString()) {
        return;
    }
    if (!font) {
        // Text shown without a known font: protect every font in reach.
        w.poison_fonts(resources);
        return;
    }
    if (!font->subsettable) {
        return;
    }
    auto s = str.getStringValue();
    auto& gids = w.ca.fontfile_gids[font->fontfile];
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        uint32_t cid = (static_cast<unsigned char>(s[i]) << 8) | static_cast<unsigned char>(s[i + 1]);
        if (font->identity) {
            gids.insert(cid);
        } else if (cid < font->cid_to_gid.size()) {
            gids.insert(font->cid_to_gid[cid]);
        }
    }
}

void Walker::Callbacks::handleObject(QPDFObjectHandle obj)
{
    if (!obj.isOperator()) {
        operands.push_back(obj);
        return;
    }
    auto op = obj.getOperatorValue();
    auto& gs = stack.back();
    if (op == "q") {
        stack.push_back(gs);
    } else if (op == "Q") {
        if (stack.size() > 1) {
            stack.pop_back();
        }
    } else if (op == "cm" && operands.size() == 6) {
        double v[6];
        bool ok = true;
        for (int i = 0; i < 6; ++i) {
            ok = ok && operands[i].getValueAsNumber(v[i]);
        }
        if (ok) {
            gs.ctm = Matrix{v[0], v[1], v[2], v[3], v[4], v[5]} * gs.ctm;
        }
    } else if (op == "Tf" && operands.size() == 2 && operands[0].isName()) {
        auto fonts = resources.isDictionary() ? resources.getKey("/Font") : QPDFObjectHandle::newNull();
        auto f = fonts.isDictionary() ? fonts.getKey(operands[0].getName()) : QPDFObjectHandle::newNull();
        gs.font = w.font_info(f, depth);
    } else if (op == "gs" && operands.size() == 1 && operands[0].isName()) {
        auto ext = resources.isDictionary() ? resources.getKey("/ExtGState") : QPDFObjectHandle::newNull();
        auto g = ext.isDictionary() ? ext.getKey(operands[0].getName()) : QPDFObjectHandle::newNull();
        if (g.isDictionary()) {
            auto f = g.getKey("/Font");
            if (f.isArray() && f.getArrayNItems() > 0) {
                gs.font = w.font_info(f.getArrayItem(0), depth);
            }
        }
    } else if ((op == "Tj" || op == "'") && !operands.empty()) {
        show(operands.back());
    } else if (op == "\"" && operands.size() == 3) {
        show(operands[2]);
    } else if (op == "TJ" && !operands.empty() && operands.back().isArray()) {
        for (auto const& item: operands.back().getArrayAsVector()) {
            show(item);
        }
    } else if (op == "Do" && operands.size() == 1 && operands[0].isName()) {
        auto xobjs = resources.isDictionary() ? resources.getKey("/XObject") : QPDFObjectHandle::newNull();
        auto x = xobjs.isDictionary() ? xobjs.getKey(operands[0].getName()) : QPDFObjectHandle::newNull();
        if (x.isStream()) {
            auto subtype = name_of(x.getDict().getKey("/Subtype"));
            if (subtype == "/Image") {
                w.image_drawn(x, gs.ctm, ctm_known);
            } else if (subtype == "/Form") {
                w.walk_form(x, resources, gs.ctm, ctm_known, depth, gs.font);
            }
        }
    }
    operands.clear();
}

void Walker::run()
{
    QPDFPageDocumentHelper dh(pdf);
    for (auto& page: dh.getAllPages()) {
        auto pobj = page.getObjectHandle();
        Matrix base;
        double uu = 1.0;
        if (pobj.getKey("/UserUnit").getValueAsNumber(uu) && uu > 0) {
            base = Matrix{uu, 0, 0, uu, 0, 0};
        }
        auto resources = page.getAttribute("/Resources", false);
        auto contents = pobj.getKey("/Contents");
        if (contents.isStream() || contents.isArray()) {
            walk(contents, resources, base, true, 0);
        }
        auto annots = pobj.getKey("/Annots");
        if (annots.isArray()) {
            for (auto const& annot: annots.getArrayAsVector()) {
                if (!annot.isDictionary()) {
                    continue;
                }
                auto ap = annot.getKey("/AP");
                if (!ap.isDictionary()) {
                    continue;
                }
                for (auto const& k: ap.getKeys()) {
                    auto entry = ap.getKey(k);
                    if (entry.isStream()) {
                        walk_unknown_placement(entry, QPDFObjectHandle::newNull(), 0);
                    } else if (entry.isDictionary()) {
                        for (auto const& s: entry.getKeys()) {
                            auto st = entry.getKey(s);
                            if (st.isStream()) {
                                walk_unknown_placement(st, QPDFObjectHandle::newNull(), 0);
                            }
                        }
                    }
                }
            }
        }
    }

    // Fonts in the interactive form default resources may be used to
    // regenerate field appearances: never subset them.
    auto acroform = pdf.getRoot().getKey("/AcroForm");
    if (acroform.isDictionary()) {
        auto dr = acroform.getKey("/DR");
        auto fonts = dr.isDictionary() ? dr.getKey("/Font") : QPDFObjectHandle::newNull();
        if (fonts.isDictionary()) {
            for (auto const& k: fonts.getKeys()) {
                auto info = font_info(fonts.getKey(k), 0);
                if (info && info->has_fontfile) {
                    ca.fontfile_blacklist.insert(info->fontfile);
                }
            }
        }
    }

    // Any font program referenced by a font we did not encounter while
    // walking content is used in an unknown way: protect it.
    for (auto& oh: reachable_objects(pdf)) {
        if (!oh.isDictionary() || name_of(oh.getKey("/Type")) != "/Font") {
            continue;
        }
        if (seen_fonts.count(oh.getObjGen())) {
            continue;
        }
        auto fd = oh.getKey("/FontDescriptor");
        auto desc = oh.getKey("/DescendantFonts");
        if (desc.isArray() && desc.getArrayNItems() > 0) {
            auto d0 = desc.getArrayItem(0);
            if (d0.isDictionary()) {
                fd = d0.getKey("/FontDescriptor");
            }
        }
        if (fd.isDictionary()) {
            auto ff = fd.getKey("/FontFile2");
            if (ff.isStream() && ff.isIndirect()) {
                ca.fontfile_blacklist.insert(ff.getObjGen());
            }
        }
    }
}

} // namespace

ContentAnalysis analyze_content(QPDF& pdf, Log const& log)
{
    ContentAnalysis ca;
    Walker w(pdf, ca, log);
    w.run();
    return ca;
}

} // namespace pdfdistill

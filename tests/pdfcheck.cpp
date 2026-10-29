// SPDX-License-Identifier: MIT
// Test helper for pdfdistill-qpdf.
//
//   pdfcheck check <file>            structural validity (like `qpdf --check`)
//   pdfcheck compare <orig> <out>    text, glyph and image preservation
//   pdfcheck maxsize <file> <bytes>  size regression guard
//   pdfcheck concat <in> <out> <n>   build an N-times duplicated document
#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFLogger.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <hb.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void fail(std::string const& msg)
{
    std::cerr << "FAIL: " << msg << "\n";
    ++failures;
}

std::string strip_tag(std::string name)
{
    if (name.size() > 8 && name[7] == '+') {
        return "/" + name.substr(8);
    }
    return name;
}

struct Shown {
    std::string font; // BaseFont without subset tag
    std::string bytes;
};

struct Glyph {
    bool has = false;
    hb_glyph_extents_t ext{};
    int advance = 0;
};

// Collects text-showing operations and image draws from page content.
class Collector: public QPDFObjectHandle::ParserCallbacks
{
  public:
    explicit Collector(QPDFObjectHandle res) :
        resources(std::move(res))
    {
    }
    void handleObject(QPDFObjectHandle obj) override
    {
        if (!obj.isOperator()) {
            operands.push_back(obj);
            return;
        }
        auto op = obj.getOperatorValue();
        if (op == "Tf" && operands.size() == 2 && operands[0].isName()) {
            font = resources.getKey("/Font").getKey(operands[0].getName());
        } else if ((op == "Tj" || op == "'") && !operands.empty()) {
            add(operands.back());
        } else if (op == "\"" && operands.size() == 3) {
            add(operands[2]);
        } else if (op == "TJ" && !operands.empty() && operands.back().isArray()) {
            for (auto const& it: operands.back().getArrayAsVector()) {
                add(it);
            }
        } else if (op == "Do" && operands.size() == 1 && operands[0].isName()) {
            auto x = resources.getKey("/XObject").getKey(operands[0].getName());
            if (x.isStream() && x.getDict().getKey("/Subtype").isNameAndEquals("/Image")) {
                images.push_back(x);
            } else if (x.isStream()) {
                auto r = x.getDict().getKey("/Resources");
                Collector sub(r.isDictionary() ? r : resources);
                QPDFObjectHandle::parseContentStream(x, &sub);
                shown.insert(shown.end(), sub.shown.begin(), sub.shown.end());
                images.insert(images.end(), sub.images.begin(), sub.images.end());
                for (auto& [k, v]: sub.used) {
                    used[k].insert(v.begin(), v.end());
                }
            }
        }
        operands.clear();
    }
    void handleEOF() override {}

    std::vector<Shown> shown;
    std::vector<QPDFObjectHandle> images;
    std::map<std::string, std::set<unsigned>> used; // font name -> codes

  private:
    void add(QPDFObjectHandle const& s)
    {
        if (!s.isString()) {
            return;
        }
        std::string name = strip_tag(font.isDictionary() && font.getKey("/BaseFont").isName()
                                         ? font.getKey("/BaseFont").getName()
                                         : "?");
        shown.push_back({name, s.getStringValue()});
        if (font.isDictionary() && font.getKey("/Subtype").isNameAndEquals("/Type0")) {
            auto str = s.getStringValue();
            for (size_t i = 0; i + 1 < str.size(); i += 2) {
                used[name].insert((static_cast<unsigned char>(str[i]) << 8) | static_cast<unsigned char>(str[i + 1]));
            }
            fonts[name] = font;
        }
    }

  public:
    std::map<std::string, QPDFObjectHandle> fonts;

  private:
    QPDFObjectHandle resources;
    QPDFObjectHandle font;
    std::vector<QPDFObjectHandle> operands;
};

std::vector<std::unique_ptr<Collector>> collect(QPDF& pdf)
{
    std::vector<std::unique_ptr<Collector>> out;
    for (auto& page: QPDFPageDocumentHelper(pdf).getAllPages()) {
        auto c = std::make_unique<Collector>(page.getAttribute("/Resources", false));
        page.parseContents(c.get());
        out.push_back(std::move(c));
    }
    return out;
}

std::map<unsigned, Glyph> glyphs(QPDFObjectHandle type0, std::set<unsigned> const& gids)
{
    std::map<unsigned, Glyph> out;
    auto ff = type0.getKey("/DescendantFonts").getArrayItem(0).getKey("/FontDescriptor").getKey("/FontFile2");
    if (!ff.isStream()) {
        return out;
    }
    auto data = ff.getStreamData(qpdf_dl_generalized);
    hb_blob_t* blob = hb_blob_create(
        reinterpret_cast<char const*>(data->getBuffer()),
        static_cast<unsigned>(data->getSize()),
        HB_MEMORY_MODE_DUPLICATE,
        nullptr,
        nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    for (auto g: gids) {
        Glyph gl;
        gl.has = hb_font_get_glyph_extents(font, g, &gl.ext);
        gl.advance = hb_font_get_glyph_h_advance(font, g);
        out[g] = gl;
    }
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
    return out;
}

void open(QPDF& pdf, char const* path)
{
    pdf.processFile(path);
}

int cmd_check(char const* path)
{
    QPDF pdf;
    open(pdf, path);
    // Touch every object and decode every stream (including JPEG data).
    for (auto& oh: pdf.getAllObjects()) {
        if (oh.isStream()) {
            try {
                oh.getStreamData(qpdf_dl_all);
            } catch (std::exception& e) {
                fail(std::string("stream ") + oh.unparse() + ": " + e.what());
            }
        }
    }
    auto pages = collect(pdf);
    if (pages.empty()) {
        fail("no pages");
    }
    if (pdf.anyWarnings()) {
        fail("qpdf reported warnings");
    }
    std::cout << "pages: " << pages.size() << ", objects: " << pdf.getObjectCount() << "\n";
    return failures ? 1 : 0;
}

int cmd_compare(char const* a_path, char const* b_path)
{
    QPDF a, b;
    open(a, a_path);
    open(b, b_path);
    auto pa = collect(a);
    auto pb = collect(b);
    if (pa.size() != pb.size()) {
        fail("page count differs");
        return 1;
    }
    size_t glyph_count = 0, image_count = 0, text_count = 0;
    for (size_t p = 0; p < pa.size(); ++p) {
        auto& ca = *pa[p];
        auto& cb = *pb[p];
        if (ca.shown.size() != cb.shown.size()) {
            fail("page " + std::to_string(p + 1) + ": number of text operations differs");
            continue;
        }
        for (size_t i = 0; i < ca.shown.size(); ++i) {
            if (ca.shown[i].font != cb.shown[i].font || ca.shown[i].bytes != cb.shown[i].bytes) {
                fail("page " + std::to_string(p + 1) + ": text operation " + std::to_string(i) + " differs");
            }
        }
        text_count += ca.shown.size();
        // Glyph outlines/metrics for every used CID must be unchanged.
        for (auto& [name, codes]: ca.used) {
            if (!ca.fonts.count(name) || !cb.fonts.count(name)) {
                continue;
            }
            auto ga = glyphs(ca.fonts[name], codes);
            auto gb = glyphs(cb.fonts[name], codes);
            for (auto c: codes) {
                auto const& x = ga[c];
                auto const& y = gb[c];
                if (x.has != y.has || x.advance != y.advance || x.ext.width != y.ext.width ||
                    x.ext.height != y.ext.height || x.ext.x_bearing != y.ext.x_bearing ||
                    x.ext.y_bearing != y.ext.y_bearing) {
                    fail(name + ": glyph " + std::to_string(c) + " changed");
                }
                ++glyph_count;
            }
        }
        if (ca.images.size() != cb.images.size()) {
            fail("page " + std::to_string(p + 1) + ": number of drawn images differs");
            continue;
        }
        for (size_t i = 0; i < cb.images.size(); ++i) {
            auto d = cb.images[i].getDict();
            if (!d.getKey("/Width").isInteger() || d.getKey("/Width").getIntValue() < 1) {
                fail("image without width");
            }
            if (ca.images[i].getDict().hasKey("/SMask") != d.hasKey("/SMask")) {
                fail("soft mask lost on image " + std::to_string(i));
            }
            try {
                auto px = cb.images[i].getStreamData(qpdf_dl_all);
                if (px->getSize() == 0) {
                    fail("empty image data");
                }
            } catch (std::exception& e) {
                fail(std::string("image decode: ") + e.what());
            }
            ++image_count;
        }
    }
    std::cout << "pages: " << pa.size() << ", text ops: " << text_count << ", glyphs verified: " << glyph_count
              << ", images: " << image_count << "\n";
    if (text_count == 0) {
        fail("no text found");
    }
    return failures ? 1 : 0;
}

// Builds a multi-page file by importing the same input N times from separate
// QPDF instances, which yields fully duplicated images and fonts.
int cmd_concat(char const* in, char const* out, char const* count)
{
    int n = std::stoi(count);
    QPDF result;
    result.emptyPDF();
    QPDFPageDocumentHelper dh(result);
    std::vector<std::unique_ptr<QPDF>> sources;
    for (int i = 0; i < n; ++i) {
        sources.push_back(std::make_unique<QPDF>());
        sources.back()->processFile(in);
        for (auto& page: QPDFPageDocumentHelper(*sources.back()).getAllPages()) {
            dh.addPage(page, false);
        }
    }
    QPDFWriter w(result, out);
    w.write();
    return 0;
}

int cmd_maxsize(char const* path, char const* limit)
{
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        fail(std::string("cannot open ") + path);
        return 1;
    }
    auto size = static_cast<long long>(in.tellg());
    long long max = std::stoll(limit);
    std::cout << path << ": " << size << " bytes (limit " << max << ")\n";
    if (size > max) {
        fail("file too large");
    }
    return failures ? 1 : 0;
}

} // namespace

int main(int argc, char* argv[])
{
    QPDFLogger::defaultLogger()->setOutputStreams(&std::cerr, &std::cerr);
    try {
        std::string cmd = argc > 1 ? argv[1] : "";
        if (cmd == "check" && argc == 3) {
            return cmd_check(argv[2]);
        }
        if (cmd == "compare" && argc == 4) {
            return cmd_compare(argv[2], argv[3]);
        }
        if (cmd == "concat" && argc == 5) {
            return cmd_concat(argv[2], argv[3], argv[4]);
        }
        if (cmd == "maxsize" && argc == 4) {
            return cmd_maxsize(argv[2], argv[3]);
        }
        std::cerr << "usage: pdfcheck check <file> | compare <orig> <out> | maxsize <file> <bytes> | concat <in> <out> <n>\n";
        return 2;
    } catch (std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}

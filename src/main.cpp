// SPDX-License-Identifier: MIT
// pdfdistill-qpdf: shrink bloated PDFs (e.g. "Microsoft Print to PDF" output).
#include "common.hpp"

#include <qpdf/Buffer.hh>
#include <qpdf/Pl_Flate.hh>
#include <qpdf/QPDFLogger.hh>
#include <qpdf/QPDFWriter.hh>

#include <cstdio>
#include <cctype>
#include <cmath>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#ifndef PDFDISTILL_VERSION
#define PDFDISTILL_VERSION "0.0.0"
#endif

using namespace pdfdistill;

namespace {

char const* const usage_text =
    "Usage: pdfdistill-qpdf <INPUT|-> [-o <OUTPUT|->] [options]\n"
    "\n"
    "Reduce the size of bloated PDF files (e.g. produced by Microsoft Print to PDF).\n"
    "\n"
    "Arguments:\n"
    "  INPUT                  input PDF path, or '-' for stdin\n"
    "\n"
    "Options:\n"
    "  -o <OUTPUT|->          output path or '-' for stdout\n"
    "                         (default: stdout if input is '-', else <input>.distilled.pdf)\n"
    "  --quality <1-100>      JPEG quality for recompressed images (default 75)\n"
    "  --max-dpi <n>          downsample images whose effective resolution exceeds n\n"
    "                         (default 150; 0 = never)\n"
    "  --no-images            skip image recompression\n"
    "  --no-subset            skip font subsetting\n"
    "  --no-dedup             skip duplicate object/image/font removal\n"
    "  --lossless             only lossless optimizations (no JPEG recompression/downsampling)\n"
    "  -v, --verbose          print per-step size statistics to stderr\n"
    "  -q, --quiet            suppress warnings and the summary line\n"
    "  --version              print version and exit\n"
    "  --help                 print this help and exit\n";

struct UsageError: std::runtime_error {
    using std::runtime_error::runtime_error;
};

bool parse_number(std::string const& s, double& out)
{
    if (s.empty()) {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

std::string default_output(std::string const& input)
{
    auto n = input.size();
    if (n >= 4) {
        std::string ext = input.substr(n - 4);
        for (auto& c: ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".pdf") {
            return input.substr(0, n - 4) + ".distilled.pdf";
        }
    }
    return input + ".distilled.pdf";
}

std::string read_all(std::string const& path)
{
    if (path == "-") {
        std::string data;
        char buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) {
            data.append(buf, n);
        }
        if (std::ferror(stdin)) {
            throw std::runtime_error("error reading from stdin");
        }
        return data;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_all(std::string const& path, std::string const& data)
{
    if (path == "-") {
        if (std::fwrite(data.data(), 1, data.size(), stdout) != data.size() || std::fflush(stdout) != 0) {
            throw std::runtime_error("error writing to stdout");
        }
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot open output file: " + path);
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close();
    if (!out) {
        throw std::runtime_error("error writing output file: " + path);
    }
}

std::string write_pdf(QPDF& pdf)
{
    QPDFWriter w(pdf);
    w.setOutputMemory();
    w.setObjectStreamMode(qpdf_o_generate);
    w.setCompressStreams(true);
    w.setDecodeLevel(qpdf_dl_generalized);
    w.setRecompressFlate(true);
    if (!pdf.isEncrypted()) {
        w.setDeterministicID(true);
    }
    w.write();
    auto buf = w.getBufferSharedPointer();
    return std::string(reinterpret_cast<char const*>(buf->getBuffer()), buf->getSize());
}

std::string percent(size_t from, size_t to)
{
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(1);
    s << (from ? 100.0 * (static_cast<double>(from) - static_cast<double>(to)) / static_cast<double>(from) : 0.0)
      << "%";
    return s.str();
}

struct Args {
    std::string input;
    std::string output;
    bool have_output = false;
    Options opt;
    bool help = false;
    bool version = false;
};

Args parse_args(int argc, char* argv[])
{
    Args a;
    bool have_input = false;
    auto value = [&](int& i, std::string const& name, std::string const& inline_value, bool has_inline) {
        if (has_inline) {
            return inline_value;
        }
        if (i + 1 >= argc) {
            throw UsageError("missing value for " + name);
        }
        return std::string(argv[++i]);
    };
    bool options_done = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (options_done || arg == "-" || arg.empty() || arg[0] != '-') {
            if (have_input) {
                throw UsageError("unexpected extra argument: " + arg);
            }
            a.input = arg;
            have_input = true;
            continue;
        }
        if (arg == "--") {
            options_done = true;
            continue;
        }
        std::string name = arg, inline_value;
        bool has_inline = false;
        if (arg.rfind("--", 0) == 0) {
            auto eq = arg.find('=');
            if (eq != std::string::npos) {
                name = arg.substr(0, eq);
                inline_value = arg.substr(eq + 1);
                has_inline = true;
            }
        }
        auto no_value = [&]() {
            if (has_inline) {
                throw UsageError("option " + name + " does not take a value");
            }
        };
        if (name == "-o" || name == "--output") {
            a.output = value(i, name, inline_value, has_inline);
            a.have_output = true;
        } else if (name == "--quality") {
            double q;
            auto v = value(i, name, inline_value, has_inline);
            if (!parse_number(v, q) || q < 1 || q > 100 || q != std::floor(q)) {
                throw UsageError("--quality must be an integer between 1 and 100");
            }
            a.opt.quality = static_cast<int>(q);
        } else if (name == "--max-dpi") {
            double d;
            auto v = value(i, name, inline_value, has_inline);
            if (!parse_number(v, d) || d < 0) {
                throw UsageError("--max-dpi must be a non-negative number");
            }
            a.opt.max_dpi = d;
        } else if (name == "--no-images") {
            no_value();
            a.opt.images = false;
        } else if (name == "--no-subset") {
            no_value();
            a.opt.subset = false;
        } else if (name == "--no-dedup") {
            no_value();
            a.opt.dedup = false;
        } else if (name == "--lossless") {
            no_value();
            a.opt.lossless = true;
        } else if (name == "-v" || name == "--verbose") {
            no_value();
            a.opt.verbose = true;
        } else if (name == "-q" || name == "--quiet") {
            no_value();
            a.opt.quiet = true;
        } else if (name == "--version" || name == "-V") {
            a.version = true;
        } else if (name == "--help" || name == "-h") {
            a.help = true;
        } else {
            throw UsageError("unknown option: " + arg);
        }
    }
    if (!a.help && !a.version && !have_input) {
        throw UsageError("missing INPUT argument");
    }
    if (!a.have_output) {
        a.output = a.input == "-" ? "-" : default_output(a.input);
    }
    return a;
}

int run(Args const& args)
{
    Options const& opt = args.opt;
    Log log{opt.verbose, opt.quiet};

    std::string input = read_all(args.input);
    std::string const input_name = args.input == "-" ? "stdin" : args.input;

    QPDFLogger::defaultLogger()->setOutputStreams(&std::cerr, &std::cerr);
    Pl_Flate::setCompressionLevel(9);

    QPDF pdf;
    pdf.setSuppressWarnings(opt.quiet);
    pdf.processMemoryFile(input_name.c_str(), input.data(), input.size());

    size_t last = input.size();
    auto stats = [&](char const* step, std::string const& detail) {
        if (!opt.verbose || opt.quiet) {
            return;
        }
        size_t now = write_pdf(pdf).size();
        std::cerr << "[" << step << "] " << detail << "; size " << last << " -> " << now << " bytes ("
                  << percent(last, now) << ")\n";
        last = now;
    };

    log.info("input: ", input_name, " (", input.size(), " bytes)");
    if (opt.dedup) {
        stats("dedup", pass_dedup(pdf, log));
    }
    bool do_images = opt.images && !opt.lossless;
    if (opt.subset || do_images) {
        auto ca = analyze_content(pdf, log);
        if (opt.subset) {
            stats("fonts", pass_subset_fonts(pdf, ca, log));
        }
        if (do_images) {
            stats("images", pass_images(pdf, ca, opt, log));
        }
    }
    keep_predictor_streams(pdf, log);
    std::string result = write_pdf(pdf);
    if (opt.verbose && !opt.quiet) {
        std::cerr << "[structure] object streams + max Flate; size " << last << " -> " << result.size()
                  << " bytes (" << percent(last, result.size()) << ")\n";
    }

    bool kept_original = result.size() >= input.size();
    write_all(args.output, kept_original ? input : result);
    if (!opt.quiet) {
        size_t out_size = kept_original ? input.size() : result.size();
        std::cerr << "pdfdistill-qpdf: " << input_name << ": " << input.size() << " -> " << out_size << " bytes ("
                  << percent(input.size(), out_size) << " smaller)"
                  << (kept_original ? " [no gain; original kept]" : "") << "\n";
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (UsageError& e) {
        std::cerr << "pdfdistill-qpdf: error: " << e.what() << "\n"
                  << "Try 'pdfdistill-qpdf --help' for more information.\n";
        return 2;
    }
    if (args.help) {
        std::cout << usage_text;
        return 0;
    }
    if (args.version) {
        std::cout << "pdfdistill-qpdf " << PDFDISTILL_VERSION << " (qpdf " << QPDF::QPDFVersion() << ")\n";
        return 0;
    }
    try {
        return run(args);
    } catch (std::exception& e) {
        std::cerr << "pdfdistill-qpdf: error: " << e.what() << "\n";
        return 1;
    }
}

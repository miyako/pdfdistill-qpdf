// SPDX-License-Identifier: MIT
#include "common.hpp"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include <jpeglib.h>
}

namespace pdfdistill {
namespace {

struct ErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

void on_error(j_common_ptr cinfo)
{
    auto* err = reinterpret_cast<ErrorMgr*>(cinfo->err);

    longjmp(err->jump, 1);
}

void on_message(j_common_ptr) {}

} // namespace

bool jpeg_encode(
    unsigned char const* pixels, int width, int height, int components, int quality, std::string& out)
{
    jpeg_compress_struct cinfo;
    ErrorMgr err;
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    cinfo.err = jpeg_std_error(&err.pub);
    err.pub.error_exit = on_error;
    err.pub.output_message = on_message;
    if (setjmp(err.jump)) {
        jpeg_destroy_compress(&cinfo);
        free(buffer);
        return false;
    }
    jpeg_create_compress(&cinfo);
    jpeg_mem_dest(&cinfo, &buffer, &size);
    cinfo.image_width = static_cast<JDIMENSION>(width);
    cinfo.image_height = static_cast<JDIMENSION>(height);
    cinfo.input_components = components;
    cinfo.in_color_space = components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.optimize_coding = TRUE;
    jpeg_start_compress(&cinfo, TRUE);
    size_t stride = static_cast<size_t>(width) * static_cast<size_t>(components);
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW row = const_cast<JSAMPROW>(pixels + cinfo.next_scanline * stride);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    out.assign(reinterpret_cast<char*>(buffer), size);
    free(buffer);
    return true;
}

} // namespace pdfdistill

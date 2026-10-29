add_executable(pdfcheck pdfcheck.cpp)
target_link_libraries(pdfcheck PRIVATE qpdf::libqpdf harfbuzz::harfbuzz ZLIB::ZLIB JPEG::JPEG)
if(MSVC)
  target_compile_options(pdfcheck PRIVATE /utf-8 /EHsc)
endif()

set(FIXTURE "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/ms-print-to-pdf-1.pdf")
set(OUT "${CMAKE_CURRENT_BINARY_DIR}/out")
file(MAKE_DIRECTORY "${OUT}")
set(CLI $<TARGET_FILE:pdfdistill-qpdf>)
set(CHECK $<TARGET_FILE:pdfcheck>)
set(RUN ${CMAKE_COMMAND} -DEXE=${CLI})
set(RUNNER -P ${CMAKE_CURRENT_SOURCE_DIR}/run_cli.cmake)

# Size regression limit for the fixture with default settings (bytes).
set(PDFDISTILL_SIZE_LIMIT 150000 CACHE STRING "Max output size for the MS Print to PDF fixture")

# --- CLI behaviour ---------------------------------------------------------
add_test(NAME cli.help COMMAND ${CLI} --help)
add_test(NAME cli.version COMMAND ${CLI} --version)
add_test(NAME cli.unknown_option COMMAND ${RUN} "-DARGS=--bogus" -DEXPECT_EXIT=2 ${RUNNER})
add_test(NAME cli.missing_input COMMAND ${RUN} "-DARGS=-q" -DEXPECT_EXIT=2 ${RUNNER})
add_test(NAME cli.bad_quality COMMAND ${RUN} "-DARGS=${FIXTURE}|--quality|101" -DEXPECT_EXIT=2 ${RUNNER})
add_test(NAME cli.nonexistent_input COMMAND ${RUN} "-DARGS=${OUT}/does-not-exist.pdf" -DEXPECT_EXIT=1 ${RUNNER})

# --- default settings ------------------------------------------------------
add_test(NAME default.run COMMAND ${CLI} ${FIXTURE} -o ${OUT}/default.pdf -v)
set_tests_properties(default.run PROPERTIES FIXTURES_SETUP default_out)
add_test(NAME default.valid COMMAND ${CHECK} check ${OUT}/default.pdf)
add_test(NAME default.text_glyphs_images COMMAND ${CHECK} compare ${FIXTURE} ${OUT}/default.pdf)
add_test(NAME default.size_regression COMMAND ${CHECK} maxsize ${OUT}/default.pdf ${PDFDISTILL_SIZE_LIMIT})
set_tests_properties(default.valid default.text_glyphs_images default.size_regression
  PROPERTIES FIXTURES_REQUIRED default_out)

# --- stdin -> stdout round trip (must equal file output: deterministic) ----
add_test(NAME stdio.roundtrip COMMAND ${RUN} "-DARGS=-|-q" -DSTDIN=${FIXTURE} -DSTDOUT=${OUT}/stdio.pdf
  -DSAME_AS=${OUT}/default.pdf ${RUNNER})
add_test(NAME stdio.explicit_dash COMMAND ${RUN} "-DARGS=-|-o|-|-q" -DSTDIN=${FIXTURE} -DSTDOUT=${OUT}/stdio2.pdf
  -DSAME_AS=${OUT}/default.pdf ${RUNNER})
add_test(NAME stdio.valid COMMAND ${CHECK} check ${OUT}/stdio.pdf)
set_tests_properties(stdio.roundtrip stdio.explicit_dash PROPERTIES FIXTURES_REQUIRED default_out FIXTURES_SETUP stdio_out)
set_tests_properties(stdio.valid PROPERTIES FIXTURES_REQUIRED stdio_out)

# --- lossless --------------------------------------------------------------
add_test(NAME lossless.run COMMAND ${RUN} "-DARGS=${FIXTURE}|-o|${OUT}/lossless.pdf|--lossless"
  -DOUTPUT=${OUT}/lossless.pdf -DNOT_LARGER_THAN=${FIXTURE} ${RUNNER})
set_tests_properties(lossless.run PROPERTIES FIXTURES_SETUP lossless_out)
add_test(NAME lossless.valid COMMAND ${CHECK} check ${OUT}/lossless.pdf)
add_test(NAME lossless.text_glyphs_images COMMAND ${CHECK} compare ${FIXTURE} ${OUT}/lossless.pdf)
set_tests_properties(lossless.valid lossless.text_glyphs_images PROPERTIES FIXTURES_REQUIRED lossless_out)

# --- aggressive downsampling ---------------------------------------------
add_test(NAME downsample.run COMMAND ${CLI} ${FIXTURE} -o ${OUT}/dpi30.pdf --max-dpi 30 --quality 60)
set_tests_properties(downsample.run PROPERTIES FIXTURES_SETUP dpi_out)
add_test(NAME downsample.valid COMMAND ${CHECK} check ${OUT}/dpi30.pdf)
add_test(NAME downsample.text_glyphs_images COMMAND ${CHECK} compare ${FIXTURE} ${OUT}/dpi30.pdf)
set_tests_properties(downsample.valid downsample.text_glyphs_images PROPERTIES FIXTURES_REQUIRED dpi_out)

# --- everything disabled: structural optimisation only --------------------
add_test(NAME structural.run COMMAND ${RUN}
  "-DARGS=${FIXTURE}|-o|${OUT}/structural.pdf|--no-images|--no-subset|--no-dedup"
  -DOUTPUT=${OUT}/structural.pdf -DNOT_LARGER_THAN=${FIXTURE} ${RUNNER})
set_tests_properties(structural.run PROPERTIES FIXTURES_SETUP structural_out)
add_test(NAME structural.valid COMMAND ${CHECK} check ${OUT}/structural.pdf)
set_tests_properties(structural.valid PROPERTIES FIXTURES_REQUIRED structural_out)

# --- never larger than the input (re-distilling an optimised file) -------
add_test(NAME never_larger.run COMMAND ${RUN} "-DARGS=${OUT}/default.pdf|-o|${OUT}/again.pdf"
  -DOUTPUT=${OUT}/again.pdf -DNOT_LARGER_THAN=${OUT}/default.pdf ${RUNNER})
set_tests_properties(never_larger.run PROPERTIES FIXTURES_REQUIRED default_out)

# --- dedup: same document imported 3x => duplicated fonts/images ----------
add_test(NAME dedup.make_input COMMAND ${CHECK} concat ${FIXTURE} ${OUT}/triple.pdf 3)
set_tests_properties(dedup.make_input PROPERTIES FIXTURES_SETUP triple_in)
add_test(NAME dedup.run COMMAND ${CLI} ${OUT}/triple.pdf -o ${OUT}/triple.distilled.pdf -v)
set_tests_properties(dedup.run PROPERTIES FIXTURES_REQUIRED triple_in FIXTURES_SETUP triple_out)
add_test(NAME dedup.valid COMMAND ${CHECK} check ${OUT}/triple.distilled.pdf)
add_test(NAME dedup.text_glyphs_images COMMAND ${CHECK} compare ${OUT}/triple.pdf ${OUT}/triple.distilled.pdf)
# Duplicates must collapse: 3 copies should cost barely more than one.
math(EXPR PDFDISTILL_TRIPLE_LIMIT "${PDFDISTILL_SIZE_LIMIT} + 20000")
add_test(NAME dedup.size_regression COMMAND ${CHECK} maxsize ${OUT}/triple.distilled.pdf ${PDFDISTILL_TRIPLE_LIMIT})
set_tests_properties(dedup.valid dedup.text_glyphs_images dedup.size_regression PROPERTIES FIXTURES_REQUIRED triple_out)

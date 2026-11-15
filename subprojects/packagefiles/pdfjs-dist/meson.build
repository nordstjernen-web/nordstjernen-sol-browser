project('pdfjs-dist', version: '6.5.438', license: 'Apache-2.0',
        meson_version: '>=1.1')

pdfjs_files = files(
  'package/build/pdf.min.mjs',
  'package/build/pdf.worker.min.mjs',
  'package/wasm/openjpeg.wasm',
  'package/wasm/jbig2.wasm',
  'package/wasm/qcms_bg.wasm',
  'package/LICENSE',
)

pdfjs_data_dirs = {
  'cmaps': meson.current_source_dir() / 'package' / 'cmaps',
  'standard_fonts': meson.current_source_dir() / 'package' / 'standard_fonts',
}

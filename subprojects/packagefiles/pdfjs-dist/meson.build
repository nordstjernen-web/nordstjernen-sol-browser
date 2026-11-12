project('pdfjs-dist', version: '6.5.438', license: 'Apache-2.0',
        meson_version: '>=1.1')

pdfjs_embedded = {
  'pdfjs_lib': files('package/build/pdf.min.mjs'),
  'pdfjs_worker': files('package/build/pdf.worker.min.mjs'),
  'pdfjs_openjpeg': files('package/wasm/openjpeg.wasm'),
  'pdfjs_jbig2': files('package/wasm/jbig2.wasm'),
  'pdfjs_qcms': files('package/wasm/qcms_bg.wasm'),
}

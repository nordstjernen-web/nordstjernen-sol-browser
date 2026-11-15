# PDF.js runtime files, copied from the pdfjs-dist wrap into the build tree and installed under share/nordstjernen/pdfjs.
fs = import('fs')
foreach name, src : pdfjs_sub.get_variable('pdfjs_embedded')
  fs.copyfile(src[0], fs.name(src[0]), install: true,
              install_dir: get_option('datadir') / 'nordstjernen' / 'pdfjs')
endforeach

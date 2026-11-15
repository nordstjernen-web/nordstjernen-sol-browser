# PDF.js runtime files, copied from the pdfjs-dist wrap into the build tree and installed under share/nordstjernen/pdfjs.
fs = import('fs')
pdfjs_install_dir = get_option('datadir') / 'nordstjernen' / 'pdfjs'
foreach src : pdfjs_sub.get_variable('pdfjs_files')
  fs.copyfile(src, fs.name(src), install: true,
              install_dir: pdfjs_install_dir)
endforeach
foreach dir, path : pdfjs_sub.get_variable('pdfjs_data_dirs')
  custom_target('pdfjs-' + dir,
    output: dir,
    command: [python, '-c',
              'import shutil, sys; shutil.copytree(sys.argv[1], sys.argv[2], dirs_exist_ok=True)',
              path, '@OUTPUT@'],
    build_by_default: true,
    install: true,
    install_dir: pdfjs_install_dir)
endforeach

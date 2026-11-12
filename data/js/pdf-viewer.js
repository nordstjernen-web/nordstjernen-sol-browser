(async () => {
    const byId = id => document.getElementById(id);
    const embedded = id => byId(id).textContent;
    const moduleUrl = id =>
        URL.createObjectURL(new Blob([embedded(id)], { type: 'text/javascript' }));
    const embeddedBytes = id => Uint8Array.fromBase64(embedded(id));
    const pagesEl = byId('pages');
    const statusEl = byId('status');
    const fail = message => {
        statusEl.textContent = message;
        statusEl.hidden = false;
    };

    class EmbeddedDataFactory {
        async fetch({ kind, filename }) {
            const el = kind === 'wasmUrl' && byId('wasm:' + filename);
            if (!el) throw new Error('Not bundled: ' + filename);
            return Uint8Array.fromBase64(el.textContent);
        }
    }

    let pdfjs, pdf;
    try {
        pdfjs = await import(moduleUrl('pdfjs-lib'));
        pdfjs.GlobalWorkerOptions.workerSrc = moduleUrl('pdfjs-worker');
        pdf = await pdfjs.getDocument({
            data: embeddedBytes('pdf-data'),
            BinaryDataFactory: EmbeddedDataFactory,
            useWorkerFetch: false,
            isEvalSupported: false,
            isOffscreenCanvasSupported: false,
            isImageDecoderSupported: false,
            enableXfa: false,
            stopAtErrors: false,
        }).promise;
    } catch (e) {
        fail(e && e.name === 'PasswordException'
            ? 'This PDF is password-protected and cannot be shown.'
            : 'This PDF could not be displayed.');
        return;
    }
    byId('pdf-data').remove();

    const outputScale = () => Math.min(window.devicePixelRatio || 1, 2);
    const fitScale = page => {
        const natural = page.getViewport({ scale: 1 });
        const room = Math.max(320, Math.min(pagesEl.clientWidth - 32, 1100));
        return room / natural.width;
    };

    const first = await pdf.getPage(1);
    const firstView = first.getViewport({ scale: fitScale(first) });
    const slots = [];
    for (let n = 1; n <= pdf.numPages; n++) {
        const slot = document.createElement('div');
        slot.className = 'page';
        slot.style.width = Math.floor(firstView.width) + 'px';
        slot.style.height = Math.floor(firstView.height) + 'px';
        slot.dataset.page = String(n);
        pagesEl.appendChild(slot);
        slots.push(slot);
    }

    const addLinks = async (page, viewport, slot) => {
        const layer = document.createElement('div');
        layer.className = 'linkLayer';
        for (const a of await page.getAnnotations()) {
            if (a.subtype !== 'Link' || !a.rect) continue;
            const [x1, y1] = viewport.convertToViewportPoint(a.rect[0], a.rect[1]);
            const [x2, y2] = viewport.convertToViewportPoint(a.rect[2], a.rect[3]);
            const link = document.createElement('a');
            link.style.left = Math.min(x1, x2) + 'px';
            link.style.top = Math.min(y1, y2) + 'px';
            link.style.width = Math.abs(x2 - x1) + 'px';
            link.style.height = Math.abs(y2 - y1) + 'px';
            if (a.url && /^(https?|mailto):/i.test(a.url)) {
                link.href = a.url;
                link.rel = 'noopener noreferrer';
            } else if (a.dest) {
                link.href = '#';
                link.addEventListener('click', async ev => {
                    ev.preventDefault();
                    const dest = typeof a.dest === 'string'
                        ? await pdf.getDestination(a.dest) : a.dest;
                    if (!dest) return;
                    const index = typeof dest[0] === 'object'
                        ? await pdf.getPageIndex(dest[0]) : dest[0];
                    if (slots[index]) slots[index].scrollIntoView();
                });
            } else {
                continue;
            }
            layer.appendChild(link);
        }
        slot.appendChild(layer);
    };

    const render = async slot => {
        if (slot.dataset.state) return;
        slot.dataset.state = 'rendering';
        try {
            const page = await pdf.getPage(Number(slot.dataset.page));
            const viewport = page.getViewport({ scale: fitScale(page) });
            const ratio = outputScale();
            slot.style.width = Math.floor(viewport.width) + 'px';
            slot.style.height = Math.floor(viewport.height) + 'px';
            slot.style.setProperty('--scale-factor', String(viewport.scale));
            slot.style.setProperty('--total-scale-factor', String(viewport.scale));
            const canvas = document.createElement('canvas');
            canvas.width = Math.floor(viewport.width * ratio);
            canvas.height = Math.floor(viewport.height * ratio);
            slot.appendChild(canvas);
            await page.render({
                canvas,
                canvasContext: canvas.getContext('2d'),
                viewport,
                transform: ratio !== 1 ? [ratio, 0, 0, ratio, 0, 0] : null,
            }).promise;
            const textDiv = document.createElement('div');
            textDiv.className = 'textLayer';
            slot.appendChild(textDiv);
            await new pdfjs.TextLayer({
                textContentSource: page.streamTextContent(),
                container: textDiv,
                viewport,
            }).render();
            await addLinks(page, viewport, slot);
            slot.dataset.state = 'done';
            page.cleanup();
        } catch (e) {
            slot.dataset.state = 'failed';
            slot.dataset.error = String(e && e.message || e);
        }
    };

    const observer = new IntersectionObserver(entries => {
        for (const entry of entries)
            if (entry.isIntersecting) render(entry.target);
    }, { rootMargin: '600px 0px' });
    for (const slot of slots) observer.observe(slot);
    render(slots[0]);

    const info = await pdf.getMetadata().catch(() => null);
    const title = info && info.info && info.info.Title;
    if (title) document.title = title;
})();

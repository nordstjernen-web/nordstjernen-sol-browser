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

    const askPassword = (retry, submit) => {
        const box = byId('password');
        const input = byId('password-input');
        const accept = () => {
            box.hidden = true;
            statusEl.hidden = true;
            submit(input.value);
        };
        fail(retry ? 'Incorrect password. Try again.'
                   : 'This PDF is password-protected.');
        box.hidden = false;
        input.value = '';
        input.focus();
        byId('password-ok').onclick = accept;
        input.onkeydown = ev => { if (ev.key === 'Enter') accept(); };
    };

    let pdfjs, pdf;
    try {
        pdfjs = await import(moduleUrl('pdfjs-lib'));
        pdfjs.GlobalWorkerOptions.workerSrc = moduleUrl('pdfjs-worker');
        const loading = pdfjs.getDocument({
            data: embeddedBytes('pdf-data'),
            BinaryDataFactory: EmbeddedDataFactory,
            useWorkerFetch: false,
            isEvalSupported: false,
            isOffscreenCanvasSupported: false,
            isImageDecoderSupported: false,
            enableXfa: false,
            stopAtErrors: false,
        });
        loading.onPassword = (submit, reason) =>
            askPassword(reason === pdfjs.PasswordResponses.INCORRECT_PASSWORD,
                        submit);
        pdf = await loading.promise;
    } catch (e) {
        fail('This PDF could not be displayed.');
        return;
    }
    byId('pdf-data').remove();

    const MAX_CANVAS_SIDE = 16384;
    const MAX_CANVAS_AREA = 32 * 1024 * 1024;
    const outputScale = () => Math.min(window.devicePixelRatio || 1, 2);
    const ACTUAL_SIZE = 96 / 72;
    const AUTO_SCALE_LIMIT = 1.25 * ACTUAL_SIZE;
    const ZOOM_STEP = 1.25;
    const ZOOM_MIN = 0.25;
    const ZOOM_MAX = 5;
    let zoom = 1;
    let fitWidth = false;
    let layoutGeneration = 0;
    const availableWidth = () => Math.max(320, document.body.clientWidth - 32);
    const fitScale = page => {
        const natural = page.getViewport({ scale: 1 });
        const ratio = outputScale();
        const side = MAX_CANVAS_SIDE / ratio;
        const area = MAX_CANVAS_AREA / (ratio * ratio);
        const widthScale = availableWidth() / natural.width;
        const base = fitWidth ? widthScale
                              : Math.min(widthScale, AUTO_SCALE_LIMIT);
        return Math.min(base * zoom,
                        side / natural.width, side / natural.height,
                        Math.sqrt(area / (natural.width * natural.height)));
    };
    const sizeSlot = (slot, viewport) => {
        slot.style.width = Math.floor(viewport.width) + 'px';
        slot.style.height = Math.floor(viewport.height) + 'px';
    };

    const first = await pdf.getPage(1);
    const slots = [];
    for (let n = 1; n <= pdf.numPages; n++) {
        const slot = document.createElement('div');
        slot.className = 'page';
        slot.dataset.page = String(n);
        pagesEl.appendChild(slot);
        slots.push(slot);
    }
    const sizeUnrenderedSlots = () => {
        const estimate = first.getViewport({ scale: fitScale(first) });
        for (const slot of slots)
            if (!slot.dataset.state) sizeSlot(slot, estimate);
    };
    sizeUnrenderedSlots();

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

    const release = slot => {
        slot.replaceChildren();
        delete slot.dataset.state;
    };

    const render = async slot => {
        if (slot.dataset.state) return;
        slot.dataset.state = 'rendering';
        const generation = layoutGeneration;
        try {
            const page = await pdf.getPage(Number(slot.dataset.page));
            const viewport = page.getViewport({ scale: fitScale(page) });
            const ratio = outputScale();
            sizeSlot(slot, viewport);
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
            page.cleanup();
            if (generation !== layoutGeneration) {
                release(slot);
                watch(slot);
                return;
            }
            slot.dataset.state = 'done';
        } catch (e) {
            slot.replaceChildren();
            slot.dataset.state = 'failed';
            slot.dataset.error = String(e && e.message || e);
            const note = document.createElement('p');
            note.className = 'pageError';
            note.textContent = 'This page could not be displayed.';
            slot.appendChild(note);
        }
    };

    const nearby = new IntersectionObserver(entries => {
        for (const entry of entries)
            if (entry.isIntersecting) render(entry.target);
    }, { rootMargin: '600px 0px' });
    const retained = new IntersectionObserver(entries => {
        for (const entry of entries)
            if (!entry.isIntersecting && entry.target.dataset.state === 'done')
                release(entry.target);
    }, { rootMargin: '4000px 0px' });
    const watch = slot => {
        nearby.unobserve(slot);
        nearby.observe(slot);
    };
    for (const slot of slots) {
        nearby.observe(slot);
        retained.observe(slot);
    }

    const currentPage = () => {
        for (let i = 0; i < slots.length; i++)
            if (slots[i].getBoundingClientRect().bottom > 48) return i;
        return slots.length - 1;
    };
    const showPosition = () => {
        byId('page-number').textContent =
            (currentPage() + 1) + ' / ' + slots.length;
    };
    const relayout = () => {
        const anchor = currentPage();
        layoutGeneration++;
        for (const slot of slots)
            if (slot.dataset.state !== 'rendering') release(slot);
        sizeUnrenderedSlots();
        for (const slot of slots) watch(slot);
        slots[anchor].scrollIntoView();
        const percent = Math.round(first.getViewport({ scale: fitScale(first) })
                                        .scale / ACTUAL_SIZE * 100);
        byId('zoom-level').textContent = percent + '%';
        showPosition();
    };
    const setZoom = (next, fit) => {
        zoom = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, next));
        fitWidth = fit;
        relayout();
    };
    byId('zoom-in').onclick = () => setZoom(zoom * ZOOM_STEP, fitWidth);
    byId('zoom-out').onclick = () => setZoom(zoom / ZOOM_STEP, fitWidth);
    byId('zoom-fit').onclick = () => setZoom(1, !fitWidth);

    let laidOutWidth = availableWidth();
    let resizeTimer = 0;
    window.addEventListener('resize', () => {
        clearTimeout(resizeTimer);
        resizeTimer = setTimeout(() => {
            if (availableWidth() === laidOutWidth) return;
            laidOutWidth = availableWidth();
            relayout();
        }, 150);
    });
    let positionQueued = false;
    window.addEventListener('scroll', () => {
        if (positionQueued) return;
        positionQueued = true;
        requestAnimationFrame(() => {
            positionQueued = false;
            showPosition();
        });
    }, { passive: true });
    byId('zoom-level').textContent = Math.round(
        first.getViewport({ scale: fitScale(first) }).scale / ACTUAL_SIZE * 100) + '%';
    showPosition();
    byId('toolbar').hidden = false;

    const requested = /(?:^|[#&])page=(\d+)/.exec(location.hash);
    const start = requested ? slots[Number(requested[1]) - 1] : null;
    if (start) start.scrollIntoView();

    const info = await pdf.getMetadata().catch(() => null);
    const title = info && info.info && info.info.Title;
    if (title) document.title = title;
})();

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
    const ACTUAL_SIZE = 96 / 72;
    const AUTO_SCALE_LIMIT = 1.25 * ACTUAL_SIZE;
    const ZOOM_STEP = 1.25;
    const ZOOM_MIN = 0.25;
    const ZOOM_MAX = 5;
    const PRINT_SCALE = 150 / 72;
    const PRINT_PAGE_LIMIT = 500;
    let zoom = 1;
    let fitWidth = false;
    let layoutGeneration = 0;
    const outputScale = () => Math.min(window.devicePixelRatio || 1, 2);
    const availableWidth = () => Math.max(320, document.body.clientWidth - 32);
    const clampScale = (scale, size, ratio) =>
        Math.min(scale, MAX_CANVAS_SIDE / ratio / size.width,
                 MAX_CANVAS_SIDE / ratio / size.height,
                 Math.sqrt(MAX_CANVAS_AREA / (ratio * ratio) /
                           (size.width * size.height)));
    const viewScale = size => {
        const widthScale = availableWidth() / size.width;
        const base = fitWidth ? widthScale
                              : Math.min(widthScale, AUTO_SCALE_LIMIT);
        return clampScale(base * zoom, size, outputScale());
    };

    const naturalSize = page => {
        const view = page.getViewport({ scale: 1 });
        return { width: view.width, height: view.height };
    };
    const first = await pdf.getPage(1);
    const sizes = [naturalSize(first)];
    const sizeOf = index => sizes[index] || sizes[0];
    const slots = [];
    for (let n = 1; n <= pdf.numPages; n++) {
        const slot = document.createElement('div');
        slot.className = 'page';
        slot.dataset.page = String(n);
        pagesEl.appendChild(slot);
        slots.push(slot);
    }
    const sizeSlot = index => {
        const size = sizeOf(index);
        const scale = viewScale(size);
        const slot = slots[index];
        slot.style.width = Math.floor(size.width * scale) + 'px';
        slot.style.height = Math.floor(size.height * scale) + 'px';
        slot.style.setProperty('--scale-factor', String(scale));
        slot.style.setProperty('--total-scale-factor', String(scale));
        return scale;
    };
    const sizeAllSlots = () => {
        for (let i = 0; i < slots.length; i++) sizeSlot(i);
    };
    sizeAllSlots();

    const addText = async (slot, page) => {
        const textDiv = document.createElement('div');
        textDiv.className = 'textLayer';
        slot.appendChild(textDiv);
        await new pdfjs.TextLayer({
            textContentSource: page.streamTextContent(),
            container: textDiv,
            viewport: page.getViewport({ scale: 1 }),
        }).render();
    };

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
        delete slot.dataset.error;
    };

    const render = async slot => {
        if (slot.dataset.state) return;
        slot.dataset.state = 'rendering';
        const generation = layoutGeneration;
        const index = Number(slot.dataset.page) - 1;
        try {
            const page = await pdf.getPage(index + 1);
            sizes[index] = naturalSize(page);
            const viewport = page.getViewport({ scale: sizeSlot(index) });
            const ratio = outputScale();
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
            await addText(slot, page);
            await addLinks(page, viewport, slot);
            page.cleanup();
            if (generation !== layoutGeneration) {
                release(slot);
                watch(slot);
                return;
            }
            slot.dataset.state = 'done';
        } catch (e) {
            release(slot);
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

    const pause = () => new Promise(resolve => setTimeout(resolve, 0));

    const currentPage = () => {
        for (let i = 0; i < slots.length; i++)
            if (slots[i].getBoundingClientRect().bottom > 48) return i;
        return slots.length - 1;
    };
    const showPosition = () => {
        byId('page-number').textContent =
            (currentPage() + 1) + ' / ' + slots.length;
    };
    const showZoom = () => {
        byId('zoom-level').textContent =
            Math.round(viewScale(sizes[0]) / ACTUAL_SIZE * 100) + '%';
    };
    const renderVisible = () => {
        const limit = window.innerHeight + 600;
        for (const slot of slots) {
            const box = slot.getBoundingClientRect();
            if (box.bottom > -600 && box.top < limit) render(slot);
            if (box.top >= limit) break;
        }
    };
    const relayout = () => {
        const anchor = currentPage();
        layoutGeneration++;
        for (const slot of slots)
            if (slot.dataset.state !== 'rendering') release(slot);
        sizeAllSlots();
        slots[anchor].scrollIntoView();
        showZoom();
        byId('page-number').textContent = (anchor + 1) + ' / ' + slots.length;
        renderVisible();
    };
    const setZoom = (next, fit) => {
        zoom = Math.min(ZOOM_MAX, Math.max(ZOOM_MIN, next));
        fitWidth = fit;
        relayout();
    };
    byId('zoom-in').onclick = () => setZoom(zoom * ZOOM_STEP, fitWidth);
    byId('zoom-out').onclick = () => setZoom(zoom / ZOOM_STEP, fitWidth);
    byId('zoom-fit').onclick = () => setZoom(1, !fitWidth);

    const pageText = [];
    const textOf = async index => {
        if (pageText[index] === undefined) {
            const page = await pdf.getPage(index + 1);
            const content = await page.getTextContent();
            const parts = content.items.map(item => item.str || '');
            pageText[index] = (parts.join(' ') + '\n' + parts.join(''))
                .toLowerCase();
            if (!slots[index].dataset.state) page.cleanup();
        }
        return pageText[index];
    };
    const findInput = byId('find');
    const findStatus = byId('find-status');
    let found = { query: '', index: -1 };
    let searching = false;
    const markMatches = (slot, query) => {
        for (const span of pagesEl.querySelectorAll('.textLayer .found'))
            span.classList.remove('found');
        let firstMatch = null;
        for (const span of slot.querySelectorAll('.textLayer span')) {
            if (!span.textContent.toLowerCase().includes(query)) continue;
            span.classList.add('found');
            firstMatch = firstMatch || span;
        }
        if (firstMatch) firstMatch.scrollIntoView({ block: 'center' });
    };
    const showMatch = async (index, query) => {
        const slot = slots[index];
        slot.scrollIntoView();
        render(slot);
        for (let tries = 0; tries < 200 && slot.dataset.state === 'rendering';
             tries++)
            await new Promise(resolve => setTimeout(resolve, 50));
        markMatches(slot, query);
    };
    const find = async direction => {
        const query = findInput.value.trim().toLowerCase();
        if (!query || searching) return;
        searching = true;
        const from = query === found.query ? found.index + direction
                                           : currentPage();
        findStatus.textContent = '…';
        try {
            for (let step = 0; step < slots.length; step++) {
                const index = (((from + direction * step) % slots.length) +
                               slots.length) % slots.length;
                if (!(await textOf(index)).includes(query)) continue;
                found = { query, index };
                findStatus.textContent = 'Page ' + (index + 1);
                await showMatch(index, query);
                return;
            }
            found = { query: '', index: -1 };
            findStatus.textContent = 'Not found';
            for (const span of pagesEl.querySelectorAll('.textLayer .found'))
                span.classList.remove('found');
        } finally {
            searching = false;
        }
    };
    findInput.onkeydown = ev => {
        if (ev.key === 'Enter') find(ev.shiftKey ? -1 : 1);
    };
    byId('find-next').onclick = () => find(1);

    const printPages = byId('print-pages');
    const clearPrintPages = () => {
        for (const img of printPages.querySelectorAll('img'))
            URL.revokeObjectURL(img.src);
        printPages.replaceChildren();
        document.body.classList.remove('printing');
    };
    const printButton = byId('print');
    printButton.onclick = async () => {
        if (printButton.disabled) return;
        printButton.disabled = true;
        clearPrintPages();
        const count = Math.min(pdf.numPages, PRINT_PAGE_LIMIT);
        try {
            for (let n = 1; n <= count; n++) {
                printButton.textContent = n + ' / ' + count;
                const page = await pdf.getPage(n);
                const size = naturalSize(page);
                const viewport = page.getViewport({
                    scale: clampScale(PRINT_SCALE, size, 1),
                });
                const canvas = document.createElement('canvas');
                canvas.width = Math.floor(viewport.width);
                canvas.height = Math.floor(viewport.height);
                await page.render({
                    canvas,
                    canvasContext: canvas.getContext('2d'),
                    viewport,
                    intent: 'print',
                }).promise;
                const blob = await new Promise(done => canvas.toBlob(done));
                canvas.width = canvas.height = 0;
                const sheet = document.createElement('div');
                sheet.className = 'printPage';
                const img = document.createElement('img');
                img.src = URL.createObjectURL(blob);
                img.style.width = (size.width / 72) + 'in';
                sheet.appendChild(img);
                printPages.appendChild(sheet);
                if (!slots[n - 1].dataset.state) page.cleanup();
            }
            document.body.classList.add('printing');
            document.body.dataset.printReady = String(count);
            await pause();
            window.print();
        } finally {
            printButton.textContent = 'Print';
            printButton.disabled = false;
        }
    };

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
    showZoom();
    showPosition();
    byId('toolbar').hidden = false;

    const requested = /(?:^|[#&])page=(\d+)/.exec(location.hash);
    const start = requested ? slots[Number(requested[1]) - 1] : null;
    if (start) start.scrollIntoView();

    const info = await pdf.getMetadata().catch(() => null);
    const title = info && info.info && info.info.Title;
    if (title) document.title = title;
})();

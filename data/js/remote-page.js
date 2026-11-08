/* Nordstjernen — the remote-control (MCP) helper: run in a realm of its own, holding its state where page script cannot reach it, it reads and acts on the page passed in. */
(function (page) {
  'use strict';
  var document = page.document;
  var location = page.location;

  var refByEl = new WeakMap();
  var elByRef = new Map();
  var nextRef = 1;
  var pending = new Map();
  var nextPending = 1;
  var MAX_NODES = 30000;

  var INTERACTIVE_ROLES = {
    button: 1, link: 1, checkbox: 1, radio: 1, tab: 1, menuitem: 1,
    menuitemcheckbox: 1, menuitemradio: 1, option: 1, switch: 1, textbox: 1,
    combobox: 1, searchbox: 1, slider: 1, spinbutton: 1, treeitem: 1,
    listbox: 1, gridcell: 1
  };
  var LANDMARKS = {
    NAV: 'navigation', MAIN: 'main', HEADER: 'banner', FOOTER: 'contentinfo',
    ASIDE: 'complementary', FORM: 'form', DIALOG: 'dialog', TABLE: 'table',
    UL: 'list', OL: 'list', IFRAME: 'iframe'
  };
  var SKIP = { SCRIPT: 1, STYLE: 1, NOSCRIPT: 1, TEMPLATE: 1, HEAD: 1, META: 1, LINK: 1 };
  var NAME_FROM_CONTENT = {
    button: 1, link: 1, heading: 1, tab: 1, menuitem: 1, menuitemcheckbox: 1,
    menuitemradio: 1, option: 1, checkbox: 1, radio: 1, switch: 1, cell: 1,
    gridcell: 1, columnheader: 1, rowheader: 1, label: 1, treeitem: 1,
    tooltip: 1, video: 1, audio: 1
  };

  function suppressed(el) {
    var rect = rectOf(el);
    if (rect && (rect.width > 0 || rect.height > 0)) return false;
    try {
      var cs = page.getComputedStyle(el);
      return cs.display === 'none' || cs.visibility === 'hidden';
    } catch (e) {
      return false;
    }
  }

  function controlFor(el) {
    var lab = el.closest ? el.closest('label') : null;
    if (!lab) return null;
    var ctl = lab.control || (lab.htmlFor && document.getElementById(lab.htmlFor));
    return ctl || lab.querySelector('input,select,textarea');
  }

  function refOf(el) {
    var r = refByEl.get(el);
    if (!r) {
      r = 'ref_' + nextRef++;
      refByEl.set(el, r);
      elByRef.set(r, el);
    }
    return r;
  }

  function byRef(ref) {
    var el = elByRef.get(String(ref));
    if (!el || !el.isConnected)
      throw new Error('unknown or stale ref ' + ref + ' (call read_page or find again)');
    return el;
  }

  function clip(s, n) {
    s = String(s == null ? '' : s).replace(/\s+/g, ' ').trim();
    return s.length > n ? s.slice(0, n - 1) + '\u2026' : s;
  }

  function attr(el, name) {
    return el.getAttribute ? el.getAttribute(name) : null;
  }

  function role(el) {
    var explicit = attr(el, 'role');
    if (explicit) return explicit.split(/\s+/)[0];
    var tag = el.tagName;
    switch (tag) {
      case 'A': case 'AREA': return el.hasAttribute('href') ? 'link' : null;
      case 'BUTTON': case 'SUMMARY': return 'button';
      case 'SELECT': return el.multiple ? 'listbox' : 'combobox';
      case 'TEXTAREA': return 'textbox';
      case 'IMG': return 'img';
      case 'VIDEO': return 'video';
      case 'AUDIO': return 'audio';
      case 'OPTION': return 'option';
      case 'LABEL': return 'label';
      case 'H1': case 'H2': case 'H3': case 'H4': case 'H5': case 'H6':
        return 'heading';
      case 'INPUT': {
        var t = (el.type || 'text').toLowerCase();
        if (t === 'checkbox' || t === 'radio') return t;
        if (t === 'submit' || t === 'reset' || t === 'button' || t === 'image') return 'button';
        if (t === 'range') return 'slider';
        if (t === 'number') return 'spinbutton';
        if (t === 'search') return 'searchbox';
        if (t === 'hidden') return null;
        if (t === 'file') return 'button';
        return 'textbox';
      }
    }
    if (el.isContentEditable || attr(el, 'contenteditable') === '' ||
        attr(el, 'contenteditable') === 'true')
      return 'textbox';
    return LANDMARKS[tag] || null;
  }

  function isInteractive(el, r) {
    var tag = el.tagName;
    if (tag === 'INPUT') return (el.type || '').toLowerCase() !== 'hidden';
    if (tag === 'BUTTON' || tag === 'SELECT' || tag === 'TEXTAREA' || tag === 'SUMMARY')
      return true;
    if ((tag === 'A' || tag === 'AREA') && el.hasAttribute('href')) return true;
    if ((tag === 'VIDEO' || tag === 'AUDIO') && el.hasAttribute('controls')) return true;
    if (r && INTERACTIVE_ROLES[r]) return true;
    if (r === 'textbox') return true;
    if (el.hasAttribute('onclick')) return true;
    var ti = attr(el, 'tabindex');
    return ti !== null && parseInt(ti, 10) >= 0;
  }

  function textOf(el) {
    var t = '';
    try { t = el.innerText; } catch (e) { t = ''; }
    if (typeof t !== 'string' || !t) t = el.textContent || '';
    return t;
  }

  function labelFor(el) {
    var id = el.id;
    if (id) {
      try {
        var l = document.querySelector('label[for="' + id.replace(/["\\]/g, '\\$&') + '"]');
        if (l) return textOf(l);
      } catch (e) {}
    }
    var p = el.closest ? el.closest('label') : null;
    return p ? textOf(p) : '';
  }

  function name(el, r) {
    var v = attr(el, 'aria-label');
    if (v) return v;
    var lb = attr(el, 'aria-labelledby');
    if (lb) {
      var parts = lb.split(/\s+/).map(function (id) {
        var n = document.getElementById(id);
        return n ? textOf(n) : '';
      }).join(' ').trim();
      if (parts) return parts;
    }
    var tag = el.tagName;
    if (tag === 'IMG' || tag === 'AREA') return attr(el, 'alt') || attr(el, 'title') || '';
    if (r && !NAME_FROM_CONTENT[r] && tag !== 'INPUT' && tag !== 'SELECT' && tag !== 'TEXTAREA')
      return attr(el, 'title') || '';
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') {
      var t = (el.type || '').toLowerCase();
      if (tag === 'INPUT' && (t === 'submit' || t === 'reset' || t === 'button'))
        return el.value || t;
      if (tag === 'INPUT' && t === 'image') return attr(el, 'alt') || 'submit';
      return labelFor(el) || attr(el, 'placeholder') || attr(el, 'title') ||
        attr(el, 'name') || '';
    }
    if (r === 'textbox' || r === 'combobox' || r === 'listbox')
      return attr(el, 'placeholder') || attr(el, 'title') || '';
    return textOf(el) || attr(el, 'title') || '';
  }

  function rectOf(el) {
    try {
      var rs = el.getClientRects ? el.getClientRects() : null;
      if (rs && rs.length > 0) {
        for (var i = 0; i < rs.length; i++)
          if (rs[i].width > 0 && rs[i].height > 0) return rs[i];
      }
      return el.getBoundingClientRect();
    } catch (e) {
      return null;
    }
  }

  function hidden(el, rect) {
    if (!rect || (rect.width <= 0 && rect.height <= 0)) return true;
    try {
      var cs = page.getComputedStyle(el);
      if (cs.visibility === 'hidden' || cs.display === 'none') return true;
      if (cs.opacity === '0') return true;
    } catch (e) {}
    return false;
  }

  function describe(el, r, rect) {
    var parts = [];
    var label = r || el.tagName.toLowerCase();
    parts.push(label);
    var n = clip(name(el, r), 100);
    if (n) parts.push(JSON.stringify(n));
    parts.push('[' + refOf(el) + ']');
    var tag = el.tagName;
    if (r === 'heading') parts.push('level=' + (tag.charAt(0) === 'H' ? tag.slice(1) : (attr(el, 'aria-level') || '?')));
    if (tag === 'A' && el.href) parts.push('href=' + JSON.stringify(clip(el.href, 160)));
    if (tag === 'INPUT') {
      var t = (el.type || 'text').toLowerCase();
      parts.push('type=' + t);
      if (t === 'checkbox' || t === 'radio') parts.push(el.checked ? 'checked' : 'unchecked');
      else if (t === 'password') { if (el.value) parts.push('value=\u2022\u2022\u2022'); }
      else if (el.value && t !== 'submit' && t !== 'button' && t !== 'reset')
        parts.push('value=' + JSON.stringify(clip(el.value, 80)));
    }
    if (tag === 'TEXTAREA' && el.value) parts.push('value=' + JSON.stringify(clip(el.value, 80)));
    if (tag === 'SELECT') {
      var opt = el.options && el.selectedIndex >= 0 ? el.options[el.selectedIndex] : null;
      if (opt) parts.push('selected=' + JSON.stringify(clip(opt.textContent, 60)));
      if (el.options) parts.push('options=' + el.options.length);
    }
    if (tag === 'IMG' && el.src) parts.push('src=' + JSON.stringify(clip(el.currentSrc || el.src, 120)));
    if (el.disabled || attr(el, 'aria-disabled') === 'true') parts.push('disabled');
    var ex = attr(el, 'aria-expanded');
    if (ex) parts.push('expanded=' + ex);
    var ch = attr(el, 'aria-checked');
    if (ch) parts.push('checked=' + ch);
    var sel = attr(el, 'aria-selected');
    if (sel === 'true') parts.push('selected');
    if (document.activeElement === el && el !== document.body) parts.push('focused');
    if (rect) {
      parts.push('(x=' + Math.round(rect.left) + ',y=' + Math.round(rect.top) +
                 ',w=' + Math.round(rect.width) + ',h=' + Math.round(rect.height) + ')');
      if (rect.bottom < 0 || rect.top > page.innerHeight || rect.right < 0 || rect.left > page.innerWidth)
        parts.push('offscreen');
    }
    return parts.join(' ');
  }

  function childrenOf(node) {
    var out = [];
    if (node.shadowRoot) out = out.concat(Array.prototype.slice.call(node.shadowRoot.childNodes));
    return out.concat(Array.prototype.slice.call(node.childNodes));
  }

  function header() {
    var de = document.documentElement;
    return 'Page: ' + clip(document.title, 200) + '\nURL: ' + location.href +
      '\nViewport: ' + page.innerWidth + 'x' + page.innerHeight + ', scroll ' +
      Math.round(page.scrollX) + ',' + Math.round(page.scrollY) + ', document ' +
      (de ? de.scrollWidth + 'x' + de.scrollHeight : '?') + '\n';
  }

  function readPage(filter, maxChars) {
    var interactiveOnly = filter === 'interactive';
    maxChars = maxChars > 0 ? maxChars : 40000;
    var lines = [];
    var size = 0;
    var visited = 0;
    var truncated = false;

    function emit(depth, text) {
      if (truncated) return;
      var line = (interactiveOnly ? '' : new Array(depth + 1).join('  ')) + '- ' + text;
      if (size + line.length > maxChars) { truncated = true; return; }
      size += line.length + 1;
      lines.push(line);
    }

    function walk(node, depth) {
      if (truncated || ++visited > MAX_NODES) { truncated = true; return; }
      if (node.nodeType === 3) {
        if (interactiveOnly) return;
        var t = clip(node.data, 200);
        if (t) emit(depth, 'text ' + JSON.stringify(t));
        return;
      }
      if (node.nodeType !== 1 && node.nodeType !== 11 && node.nodeType !== 9) return;
      var el = node;
      if (node.nodeType === 1) {
        if (SKIP[el.tagName] || suppressed(el)) return;
        var r = role(el);
        var inter = isInteractive(el, r);
        var structural = !inter && r && (r === 'heading' || r === 'img' || LANDMARKS[el.tagName] || attr(el, 'role'));
        if (inter || (!interactiveOnly && structural)) {
          var rect = rectOf(el);
          if (!hidden(el, rect)) {
            emit(depth, describe(el, r, rect));
            if (inter && el.tagName !== 'SELECT' && r !== 'textbox' && !el.querySelector('a[href],button,input,select,textarea,[role],[tabindex],[contenteditable]'))
              return;
            if (el.tagName === 'SELECT' || el.tagName === 'IMG' || el.tagName === 'IFRAME') return;
            depth++;
          } else if (rect && rect.width <= 0 && rect.height <= 0 && el.tagName !== 'DIV' && el.tagName !== 'SPAN') {
            return;
          }
        }
      }
      var kids = childrenOf(node);
      for (var i = 0; i < kids.length && !truncated; i++) walk(kids[i], depth);
    }

    walk(document.body || document.documentElement, 0);
    var out = header() + lines.join('\n');
    if (truncated) out += '\n[output truncated; use find, or filter="interactive", or a larger max_chars]';
    return out;
  }

  function find(query, limit) {
    limit = limit > 0 ? limit : 20;
    var q = String(query || '').trim();
    if (!q) throw new Error('empty query');
    var hits = [];
    var seen = new Set();
    function add(el) {
      if (!el || seen.has(el) || hits.length >= limit) return;
      var rect = rectOf(el);
      if (hidden(el, rect)) return;
      seen.add(el);
      hits.push(describe(el, role(el), rect));
    }
    var bySelector = null;
    try { bySelector = document.querySelectorAll(q); } catch (e) { bySelector = null; }
    if (bySelector && bySelector.length) {
      for (var i = 0; i < bySelector.length && hits.length < limit; i++) add(bySelector[i]);
      if (hits.length) return 'Matches for selector ' + JSON.stringify(q) + ':\n' + hits.join('\n');
    }
    var needle = q.toLowerCase();
    var visited = 0;
    function matches(s) { return s && String(s).toLowerCase().indexOf(needle) >= 0; }
    function interactiveAncestor(el) {
      for (var p = el; p && p.nodeType === 1; p = p.parentNode || p.host) {
        if (isInteractive(p, role(p))) return p;
      }
      return null;
    }
    function walk(node) {
      if (hits.length >= limit || ++visited > MAX_NODES) return;
      if (node.nodeType === 1) {
        if (SKIP[node.tagName] || suppressed(node)) return;
        var r = role(node);
        if (isInteractive(node, r) &&
            (matches(attr(node, 'aria-label')) || matches(attr(node, 'placeholder')) ||
             matches(attr(node, 'title')) || matches(attr(node, 'name')) ||
             matches(attr(node, 'alt')) || matches(node.value) || matches(name(node, r))))
          add(node);
      } else if (node.nodeType === 3) {
        if (matches(node.data) && node.parentNode && node.parentNode.nodeType === 1)
          add(interactiveAncestor(node.parentNode) || controlFor(node.parentNode) || node.parentNode);
        return;
      }
      var kids = childrenOf(node);
      for (var i = 0; i < kids.length; i++) walk(kids[i]);
    }
    walk(document.body || document.documentElement);
    if (!hits.length) return 'No elements match ' + JSON.stringify(q);
    return 'Matches for ' + JSON.stringify(q) + ':\n' + hits.join('\n');
  }

  function point(ref) {
    var el = byRef(ref);
    var rect = rectOf(el);
    var scrolled = false;
    if (!rect || rect.top < 0 || rect.left < 0 || rect.bottom > page.innerHeight || rect.right > page.innerWidth) {
      try { el.scrollIntoView({ block: 'center', inline: 'center' }); } catch (e) { el.scrollIntoView(); }
      rect = rectOf(el);
      scrolled = true;
    }
    if (!rect || (rect.width <= 0 && rect.height <= 0))
      throw new Error(ref + ' has no visible box');
    var x = rect.left + rect.width / 2;
    var y = rect.top + rect.height / 2;
    var res = { x: x, y: y, px: x + page.scrollX, py: y + page.scrollY, scrolled: scrolled,
                desc: describe(el, role(el), rect) };
    try {
      var top = document.elementFromPoint(x, y);
      if (top && top !== el && !el.contains(top) && !(top.contains && top.contains(el)))
        res.covered_by = describe(top, role(top), rectOf(top));
    } catch (e) {}
    return JSON.stringify(res);
  }

  function nativeSet(el, value) {
    var proto = Object.getPrototypeOf(el);
    var desc = null;
    while (proto && !desc) {
      desc = Object.getOwnPropertyDescriptor(proto, 'value');
      proto = Object.getPrototypeOf(proto);
    }
    if (desc && desc.set) desc.set.call(el, value);
    else el.value = value;
  }

  function fire(el, type) {
    var ev;
    try { ev = new page.Event(type, { bubbles: true }); } catch (e) {
      ev = document.createEvent('Event');
      ev.initEvent(type, true, false);
    }
    el.dispatchEvent(ev);
  }

  function setValue(ref, value) {
    var el = byRef(ref);
    var tag = el.tagName;
    if (el.disabled) throw new Error(ref + ' is disabled');
    if (typeof el.focus === 'function') el.focus();
    if (tag === 'SELECT') {
      var want = Array.isArray(value) ? value.map(String) : [String(value)];
      var found = 0;
      for (var i = 0; i < el.options.length; i++) {
        var o = el.options[i];
        var hit = want.some(function (w) {
          return o.value === w || clip(o.textContent, 1000).toLowerCase() === w.trim().toLowerCase();
        });
        if (el.multiple) o.selected = hit;
        else if (hit && !found) el.selectedIndex = i;
        if (hit) found++;
      }
      if (!found) throw new Error('no option of ' + ref + ' matches ' + JSON.stringify(value));
    } else if (tag === 'INPUT' && (el.type === 'checkbox' || el.type === 'radio')) {
      var on = value === true || /^(true|on|1|yes|checked)$/i.test(String(value));
      el.checked = on;
    } else if (tag === 'INPUT' && el.type === 'file') {
      throw new Error('file inputs cannot be set remotely');
    } else if (tag === 'INPUT' || tag === 'TEXTAREA') {
      nativeSet(el, String(value));
    } else if (el.isContentEditable || attr(el, 'contenteditable') !== null) {
      el.textContent = String(value);
    } else {
      throw new Error(ref + ' is not a form field');
    }
    fire(el, 'input');
    fire(el, 'change');
    return 'Set ' + describe(el, role(el), rectOf(el));
  }

  function pageText(maxChars) {
    maxChars = maxChars > 0 ? maxChars : 100000;
    var body = document.body || document.documentElement;
    var t = body ? textOf(body) : '';
    t = String(t).replace(/[ \t\f\v]+/g, ' ').replace(/\s*\n\s*/g, '\n').trim();
    if (t.length > maxChars) t = t.slice(0, maxChars) + '\n[text truncated]';
    return 'Title: ' + document.title + '\nURL: ' + location.href + '\n\n' + t;
  }

  function format(v) {
    if (v === undefined) return 'undefined';
    if (typeof v === 'string') return v;
    if (typeof v === 'function' || typeof v === 'symbol' || typeof v === 'bigint') return String(v);
    if (v && v.nodeType === 1) return clip(v.outerHTML, 4000);
    if (v && v.nodeType) return clip(v.textContent, 4000);
    try {
      var s = JSON.stringify(v, null, 2);
      return s === undefined ? String(v) : s;
    } catch (e) {
      return String(v);
    }
  }

  function errorText(e) {
    return String(e && e.stack ? e + '\n' + e.stack : e);
  }

  function settle(r, threw) {
    if (threw) return JSON.stringify({ error: errorText(r) });
    if (r && typeof r.then === 'function') {
      var id = nextPending++;
      pending.set(id, null);
      r.then(function (v) { pending.set(id, { value: format(v) }); },
             function (e) { pending.set(id, { error: errorText(e) }); });
      return JSON.stringify({ pending: id });
    }
    return JSON.stringify({ value: format(r) });
  }

  function run(code) {
    return { code: String(code), settle: settle };
  }

  function poll(id) {
    var p = pending.get(id);
    if (p === undefined) return JSON.stringify({ error: 'unknown evaluation' });
    if (p === null) return JSON.stringify({ pending: id });
    pending.delete(id);
    return JSON.stringify(p);
  }

  var api = {
    readPage: readPage, find: find, point: point, setValue: setValue,
    pageText: pageText, run: run, poll: poll
  };
  return function (request) {
    var call = JSON.parse(request);
    var fn = Object.prototype.hasOwnProperty.call(api, call.fn) ? api[call.fn] : null;
    if (!fn) throw new Error('unknown remote call ' + call.fn);
    document = page.document;
    location = page.location;
    return fn.apply(null, Array.isArray(call.args) ? call.args : []);
  };
})

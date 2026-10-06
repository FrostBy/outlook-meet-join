(() => {
  if (window.__outlookMeetJoin) return;
  window.__outlookMeetJoin = 1;

  const NS = 'omjv1:';
  const BTN = 'omj-join-btn';
  const MAX_ENTRIES = 400;

  const normalize = raw => {
    if (!raw) return null;
    const m = String(raw).match(/meet\.google\.com\/([a-z]{3,}-[a-z]{3,4}-[a-z]{3,})/i);
    return m ? 'https://meet.google.com/' + m[1].toLowerCase() : null;
  };

  const isTel = u => /tel\.meet/i.test(String(u || ''));

  const extractFromHtml = html => {
    if (!html) return null;
    const anchors = String(html).match(/https?:\/\/meet\.google\.com\/[a-z0-9-]+/gi) || [];
    for (const a of anchors) { if (!isTel(a)) { const n = normalize(a); if (n) return n; } }
    return normalize(html);
  };

  const cacheKeys = () => {
    try { return Object.keys(localStorage).filter(k => k.startsWith(NS)); } catch (e) { return []; }
  };

  const load = key => {
    if (!key) return null;
    try { const v = localStorage.getItem(NS + key); return v ? JSON.parse(v) : null; } catch (e) { return null; }
  };

  const prune = () => {
    const keys = cacheKeys();
    if (keys.length <= MAX_ENTRIES) return;
    keys.map(k => [k, (load(k.slice(NS.length)) || {}).t || 0])
      .sort((a, b) => a[1] - b[1])
      .slice(0, keys.length - MAX_ENTRIES + 50)
      .forEach(([k]) => { try { localStorage.removeItem(k); } catch (e) {} });
  };

  const store = (key, link, email) => {
    if (!key || !link) return;
    try { localStorage.setItem(NS + key, JSON.stringify({ link, email: email || null, t: Date.now() })); } catch (e) {}
  };

  const subjectKey = s => {
    const t = String(s || '').trim().toLowerCase();
    return t ? 'subj:' + t : null;
  };

  const parseEvent = obj => {
    if (!obj || typeof obj !== 'object' || !obj.Body || typeof obj.Body.Value !== 'string') return null;
    const link = extractFromHtml(obj.Body.Value);
    if (!link) return null;
    const mb = obj.mailboxInfo || (obj.ItemId && obj.ItemId.mailboxInfo) || (obj.SeriesMasterItemId && obj.SeriesMasterItemId.mailboxInfo);
    return {
      link,
      email: (mb && mb.mailboxSmtpAddress) || null,
      keys: [obj.ItemId && obj.ItemId.Id, typeof obj.Id === 'string' ? obj.Id : null, obj.SeriesMasterItemId && obj.SeriesMasterItemId.Id, obj.UID, subjectKey(obj.Subject)].filter(Boolean)
    };
  };

  const collectEvents = (node, out, depth) => {
    if (!node || typeof node !== 'object' || depth > 16) return;
    const ev = parseEvent(node);
    if (ev) out.push(ev);
    for (const k in node) { const v = node[k]; if (v && typeof v === 'object') collectEvents(v, out, depth + 1); }
  };

  const scanStream = data => {
    let obj = data;
    if (typeof obj === 'string') {
      if (obj.indexOf('meet.google.com') < 0) return;
      try { obj = JSON.parse(obj); } catch (e) { return; }
    } else {
      let s;
      try { s = JSON.stringify(obj); } catch (e) { return; }
      if (!s || s.indexOf('meet.google.com') < 0) return;
    }
    const events = [];
    collectEvents(obj, events, 0);
    for (const ev of events) for (const k of ev.keys) store(k, ev.link, ev.email);
    if (events.length) { prune(); refresh(); }
  };

  try {
    const AEL = MessagePort.prototype.addEventListener;
    MessagePort.prototype.addEventListener = function (t, l, o) {
      if (t === 'message' && !this.__omj) {
        this.__omj = 1;
        try { AEL.call(this, t, ev => { try { scanStream(ev.data); } catch (e) {} }, o); } catch (e) {}
      }
      return AEL.apply(this, arguments);
    };
  } catch (e) {}

  const buildJoinUrl = (link, email) => {
    if (!link) return null;
    if (!email) return link;
    const u = new URL(link);
    u.searchParams.set('authuser', email);
    return u.toString();
  };

  const openJoin = (link, email) => {
    const url = buildJoinUrl(link, email);
    if (!url) return;
    const a = document.createElement('a');
    a.href = url; a.target = '_blank'; a.rel = 'noopener';
    document.body.appendChild(a); a.click(); a.remove();
  };

  const makeBtn = (link, email) => {
    const b = document.createElement('button');
    b.className = BTN;
    b.type = 'button';
    b.textContent = 'Join';
    b.dataset.link = link;
    b.setAttribute('aria-label', 'Join Google Meet');
    b.style.cssText = 'margin:0 6px;padding:4px 12px;border:0;border-radius:4px;background:#1a73e8;color:#fff;font:600 13px/1.4 inherit;cursor:pointer;';
    b.addEventListener('click', e => { e.preventDefault(); e.stopPropagation(); openJoin(link, email); });
    return b;
  };

  const emailForLink = link => {
    let best = null;
    for (const k of cacheKeys()) {
      const v = load(k.slice(NS.length));
      if (v && v.link === link && v.email && (!best || v.t > best.t)) best = v;
    }
    return best && best.email;
  };

  const formMeetLink = form => {
    for (const a of form.querySelectorAll('a[href*="meet.google.com/"]')) {
      if (!isTel(a.href)) { const n = normalize(a.href); if (n) return n; }
    }
    return null;
  };

  const fillForm = () => {
    const form = document.querySelector('[data-app-section="Form_Content"]');
    const bar = document.querySelector('[data-app-section="Toolbar"][role="toolbar"]');
    const host = bar && bar.parentElement;
    const existing = host && host.querySelector(':scope > .' + BTN);
    const link = form && formMeetLink(form);
    if (!link || !host) { if (existing) existing.remove(); return; }
    if (existing && existing.dataset.link === link) return;
    if (existing) existing.remove();
    const b = makeBtn(link, emailForLink(link));
    b.style.alignSelf = 'center';
    host.insertBefore(b, bar);
  };

  let lastCalId = null, lastLabel = '';
  const trackTile = e => {
    const tile = e.target && e.target.closest && e.target.closest('[data-calitemid]');
    if (!tile) return;
    lastCalId = tile.getAttribute('data-calitemid');
    const lab = tile.getAttribute('aria-label') ? tile : tile.querySelector('[aria-label]');
    lastLabel = lab ? lab.getAttribute('aria-label') : '';
  };
  document.addEventListener('pointerdown', trackTile, true);
  document.addEventListener('keydown', trackTile, true);

  const byCardLines = text => {
    for (const line of String(text || '').split('\n')) {
      const k = subjectKey(line);
      const rec = k && load(k);
      if (rec) return rec;
    }
    return null;
  };

  const byTileLabel = label => {
    const l = String(label || '').toLowerCase();
    let best = null, bestLen = 0;
    for (const k of cacheKeys()) {
      if (!k.startsWith(NS + 'subj:')) continue;
      const t = k.slice(NS.length + 5);
      if (t.length > bestLen && l.startsWith(t + ',')) { best = load('subj:' + t); bestLen = t.length; }
    }
    return best;
  };

  const fillPeek = () => {
    const card = document.querySelector('[data-app-section="CalendarItemPeek"]');
    if (!card) return;
    const existing = card.querySelector('.' + BTN);
    const rec = byCardLines(card.innerText) || load(lastCalId) || byTileLabel(lastLabel);
    if (!rec) { if (existing) existing.remove(); return; }
    if (existing && existing.dataset.link === rec.link) return;
    if (existing) existing.remove();
    const b = makeBtn(rec.link, rec.email);
    b.style.margin = '4px 12px 12px';
    b.style.padding = '7px 20px';
    b.style.alignSelf = 'flex-start';
    b.style.justifySelf = 'start';
    b.style.width = 'auto';
    card.appendChild(b);
  };

  let raf = 0;
  const refresh = () => {
    if (raf) return;
    raf = requestAnimationFrame(() => {
      raf = 0;
      try { fillForm(); } catch (e) {}
      try { fillPeek(); } catch (e) {}
    });
  };

  try { new MutationObserver(refresh).observe(document.documentElement || document, { childList: true, subtree: true }); } catch (e) {}
  refresh();

  window.__outlookMeetJoinTest = { normalize, isTel, extractFromHtml, parseEvent, scanStream, buildJoinUrl, load, emailForLink, byCardLines, byTileLabel, prune };
})();

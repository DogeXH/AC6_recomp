// ac6lobby moderation panel. Talks to /admin/v1/ on this origin with the admin token as a
// Bearer header. Everything from the server (gamertags, reasons) is rendered as text, never
// as HTML.
'use strict';

(() => {
  const API = '/admin/v1/';
  const TOKEN_KEY = 'ac6lobby-admin-token';
  const POLL_MS = 5000;
  const TABS = ['players', 'sessions', 'bans', 'audit', 'metrics'];

  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

  // h builds an element. Children are nodes or text; null/false children are skipped.
  function h(tag, props, ...kids) {
    const el = document.createElement(tag);
    for (const [k, v] of Object.entries(props || {})) {
      if (v == null || v === false) continue;
      if (k === 'class') el.className = v;
      else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
      else el.setAttribute(k, v === true ? '' : v);
    }
    for (const kid of kids.flat()) {
      if (kid == null || kid === false) continue;
      el.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    }
    return el;
  }

  // ---- token storage (sessionStorage; localStorage only with "Remember") ----
  function readStore(kind) {
    try { return window[kind].getItem(TOKEN_KEY) || ''; } catch { return ''; }
  }
  function writeStore(kind, value) {
    try {
      if (value) window[kind].setItem(TOKEN_KEY, value);
      else window[kind].removeItem(TOKEN_KEY);
    } catch { /* storage blocked: the token lives in memory only */ }
  }
  const loadToken = () => readStore('sessionStorage') || readStore('localStorage');
  function saveToken(tok, remember) {
    writeStore('sessionStorage', tok);
    writeStore('localStorage', remember ? tok : '');
  }
  function clearToken() {
    writeStore('sessionStorage', '');
    writeStore('localStorage', '');
  }

  // ---- state ----
  const state = {
    token: '',
    tab: 'players',
    filter: '',
    players: [], sessions: [], bans: [], audit: [], metrics: null,
    prev: null, rates: null,
    ips: new Map(), // peer_id -> address, only after "Show"
    lastOK: 0, error: '', loading: false, timer: 0,
  };

  class AuthError extends Error {}

  async function api(method, path, body) {
    const headers = { 'Authorization': 'Bearer ' + state.token, 'X-AC6-Admin-Client': 'panel' };
    const init = { method, headers, cache: 'no-store', credentials: 'omit', redirect: 'error' };
    if (body !== undefined) {
      headers['Content-Type'] = 'application/json';
      init.body = JSON.stringify(body);
    }
    let res;
    try {
      res = await fetch(API + path, init);
    } catch {
      throw new Error("Can't reach the server. Is the SSH tunnel still open?");
    }
    let doc = null;
    try { doc = await res.json(); } catch { /* not JSON */ }
    if (res.status === 401) throw new AuthError((doc && doc.error) || 'The server rejected the token.');
    if (!res.ok) throw new Error((doc && doc.error) || 'HTTP ' + res.status);
    return doc || {};
  }

  // ---- formatting ----
  const nf = new Intl.NumberFormat();
  const dtf = new Intl.DateTimeFormat(undefined, { year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
  const tf = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const num = (n) => nf.format(n || 0);
  function bytes(n) {
    n = n || 0;
    if (n < 1024) return n + ' B';
    const units = ['KB', 'MB', 'GB', 'TB'];
    let i = -1;
    do { n /= 1024; i++; } while (n >= 1024 && i < units.length - 1);
    return n.toFixed(n < 10 ? 1 : 0) + ' ' + units[i];
  }
  function dur(s) {
    s = Math.max(0, Math.floor(s || 0));
    if (s < 60) return s + 's';
    const m = Math.floor(s / 60);
    if (m < 60) return m + 'm ' + String(s % 60).padStart(2, '0') + 's';
    const hr = Math.floor(m / 60);
    if (hr < 24) return hr + 'h ' + String(m % 60).padStart(2, '0') + 'm';
    return Math.floor(hr / 24) + 'd ' + (hr % 24) + 'h';
  }
  const date = (iso) => dtf.format(new Date(iso));
  function rel(iso) {
    const diff = (Date.parse(iso) - Date.now()) / 1000;
    return diff >= 0 ? 'in ' + dur(diff) : dur(-diff) + ' ago';
  }
  function rate(v) {
    if (v == null) return '–';
    return v >= 100 ? num(Math.round(v)) : v.toFixed(v >= 10 ? 1 : 2).replace(/\.?0+$/, '') || '0';
  }

  // ---- targets ----
  // parseWho mirrors the CLI: 16 hex digits is a peer ID, else a gamertag; peer:/tag: force one.
  function parseWho(s) {
    s = s.trim();
    if (s.startsWith('peer:')) return { peer_id: s.slice(5).trim() };
    if (s.startsWith('tag:')) return { gamertag: s.slice(4).trim() };
    if (/^[0-9a-fA-F]{16}$/.test(s)) return { peer_id: s.toLowerCase() };
    return { gamertag: s };
  }
  // known fills in the other half of a target from the current lists.
  function known(t) {
    const match = (x) => t.peer_id ? x.peer_id === t.peer_id : (x.gamertag || '').toLowerCase() === t.gamertag.toLowerCase();
    const p = state.players.find(match);
    const b = state.bans.find(match);
    const hit = p || b;
    return { peer_id: t.peer_id || (hit && hit.peer_id) || '', gamertag: t.gamertag || (hit && hit.gamertag) || '', online: !!p, ban: b || null };
  }
  // body sends exactly one of peer_id or gamertag, as the API requires.
  const body = (t) => (t.peer_id ? { peer_id: t.peer_id } : { gamertag: t.gamertag });
  const name = (t) => t.gamertag || t.peer_id;
  function targetLine(t) {
    const bits = [];
    if (t.gamertag && t.peer_id) bits.push(h('span', { class: 'mono' }, t.peer_id));
    if (t.online) bits.push('online now');
    else if (t.peer_id || t.gamertag) bits.push('not online');
    return bits.flatMap((b, i) => (i ? [' · ', b] : [b]));
  }

  // ---- toasts ----
  function toast(msg, error) {
    const el = h('div', { class: 'toast' + (error ? ' error' : ''), role: error ? 'alert' : 'status' }, msg);
    $('#toasts').append(el);
    setTimeout(() => el.remove(), error ? 7000 : 4000);
  }

  // ---- sheets: in-page dialogs for every action ----
  let sheetReturn = null;
  let sheetOpen = false;

  function openSheet(o) {
    let busy = false;
    const err = h('p', { class: 'form-error', role: 'alert' });
    const confirm = h('button', { type: 'submit', class: 'btn ' + (o.danger ? 'danger' : 'primary') }, o.confirm);
    const cancel = h('button', { type: 'button', class: 'btn ghost', onclick: closeSheet }, 'Cancel');
    const form = h('form', { novalidate: true },
      h('div', { class: 'sheet-head' },
        h('h1', { id: 'sheet-title' }, o.title),
        o.target ? h('div', { class: 'sheet-target' }, o.target) : null),
      h('div', { class: 'sheet-body' }, o.body, err),
      h('div', { class: 'sheet-foot' }, cancel, confirm));
    const update = () => {
      confirm.disabled = busy || (o.valid ? !o.valid() : false);
      if (o.label) confirm.textContent = busy ? 'Working…' : o.label();
      if (o.onUpdate) o.onUpdate();
    };
    form.addEventListener('input', update);
    form.addEventListener('change', update);
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      if (confirm.disabled) return;
      busy = true;
      err.textContent = '';
      update();
      try {
        const msg = await o.run();
        closeSheet();
        toast(msg);
        if (o.after) o.after();
        refresh();
      } catch (e) {
        if (e instanceof AuthError) { closeSheet(); lock(e.message); return; }
        err.textContent = e.message;
      } finally {
        busy = false;
        if (sheetOpen) update();
      }
    });
    $('#sheet').replaceChildren(form);
    sheetReturn = document.activeElement;
    sheetOpen = true;
    $('#sheet-backdrop').hidden = false;
    $('#app').inert = true;
    update();
    (form.querySelector('[data-autofocus]') || form.querySelector('input, textarea') || cancel).focus();
  }

  function closeSheet() {
    if (!sheetOpen) return;
    sheetOpen = false;
    $('#sheet-backdrop').hidden = true;
    $('#app').inert = false;
    $('#sheet').replaceChildren();
    if (sheetReturn && document.contains(sheetReturn)) sheetReturn.focus();
    sheetReturn = null;
  }

  function textField(label, opts = {}) {
    const max = opts.max || 200;
    const counter = h('span', { class: 'counter' }, '0 / ' + max);
    const input = h(opts.multiline ? 'textarea' : 'input', {
      type: opts.multiline ? null : 'text', rows: opts.multiline ? 3 : null, maxlength: max,
      placeholder: opts.placeholder || '', spellcheck: opts.spellcheck === false ? 'false' : null,
      autocapitalize: 'off', 'data-autofocus': opts.autofocus || null,
    });
    input.addEventListener('input', () => {
      counter.textContent = input.value.length + ' / ' + max;
      counter.classList.toggle('full', input.value.length >= max);
    });
    const el = h('label', { class: 'field' }, h('span', { class: 'field-row' }, h('span', null, label), counter), input);
    return { el, input, value: () => input.value.trim() };
  }

  const PRESETS = [
    { v: '24h', label: '24 hours', button: 'Ban for 24 hours', ms: 864e5 },
    { v: '7d', label: '7 days', button: 'Ban for 7 days', ms: 7 * 864e5 },
    { v: '30d', label: '30 days', button: 'Ban for 30 days', ms: 30 * 864e5 },
    { v: '', label: 'Permanent', button: 'Ban permanently', ms: 0 },
  ];

  function banSheet(t, after) {
    const radios = PRESETS.map((p, i) => h('label', null,
      h('input', { type: 'radio', name: 'dur', value: p.v, checked: i === 0 }),
      h('span', null, p.label)));
    const pick = () => PRESETS.find((p) => p.v === (radios.map((r) => r.firstChild).find((r) => r.checked) || {}).value) || PRESETS[0];
    const ends = h('p', { class: 'ends' });
    const reason = textField('Reason (required)', { multiline: true, placeholder: 'What happened, for appeals', autofocus: true });
    const already = t.ban ? h('p', null, 'This account is already banned (' +
      (t.ban.expires ? 'until ' + date(t.ban.expires) : 'permanently') + '). Banning again replaces the end date and reason.') : null;
    openSheet({
      after,
      title: 'Ban ' + name(t),
      target: targetLine(t),
      danger: true,
      body: [
        h('p', null, 'The player is disconnected at once and every sign-in with this account key is refused. Their game receives the reason.'),
        already,
        h('div', { class: 'field' }, h('span', null, 'Duration'), h('div', { class: 'presets', role: 'radiogroup', 'aria-label': 'Duration' }, radios)),
        ends,
        reason.el,
      ],
      valid: () => reason.value() !== '',
      label: () => pick().button,
      onUpdate: () => {
        const p = pick();
        ends.textContent = p.ms ? 'Ends ' + date(new Date(Date.now() + p.ms).toISOString()) + '.' : 'No end date. Lift it with Unban.';
      },
      run: async () => {
        const r = await api('POST', 'ban', { ...body(t), reason: reason.value(), duration: pick().v });
        const b = r.banned || {};
        return 'Banned ' + (b.gamertag || name(t)) + (b.expires ? ' until ' + date(b.expires) : ' permanently') +
          (b.was_online ? ' and disconnected them.' : '.');
      },
    });
  }

  function kickSheet(t, after) {
    const reason = textField('Reason (optional, sent to their game)', { placeholder: 'Spamming the lobby' });
    openSheet({
      after,
      title: 'Kick ' + name(t),
      target: targetLine(t),
      danger: true,
      confirm: 'Kick',
      body: [
        h('p', null, 'Disconnects the player and ends the sessions they host. They can sign in again at once; ban them to keep them out.'),
        reason.el,
      ],
      run: async () => {
        const r = await api('POST', 'kick', { ...body(t), reason: reason.value() });
        return 'Kicked ' + ((r.kicked && r.kicked.gamertag) || name(t)) + '.';
      },
    });
  }

  function unbanSheet(t, after) {
    const note = textField('Note (optional, for the audit log)', { placeholder: 'Appeal accepted' });
    const b = t.ban;
    openSheet({
      after,
      title: 'Lift the ban on ' + name(t),
      target: targetLine(t),
      confirm: 'Lift ban',
      body: [
        b ? h('dl', { class: 'kv' },
          h('dt', null, 'Banned'), h('dd', null, date(b.created)),
          h('dt', null, 'Ends'), h('dd', null, b.expires ? date(b.expires) : 'Never'),
          h('dt', null, 'Reason'), h('dd', null, b.reason || '–')) : h('p', null, 'The ban list does not show this account; the server will say if it is not banned.'),
        h('p', null, 'The player can sign in again at once.'),
        note.el,
      ],
      run: async () => {
        const r = await api('POST', 'unban', { ...body(t), reason: note.value() });
        return 'Lifted the ban on ' + ((r.unbanned && r.unbanned.gamertag) || name(t)) + '.';
      },
    });
  }

  function forgetSheet(t, after) {
    const word = t.gamertag || t.peer_id;
    const note = textField('Note (optional, for the audit log)', { placeholder: 'Deletion request by email' });
    const typed = h('input', { type: 'text', spellcheck: 'false', autocapitalize: 'off', autocomplete: 'off', 'data-autofocus': true });
    openSheet({
      after,
      title: 'Delete ' + name(t) + "'s account",
      target: targetLine(t),
      danger: true,
      confirm: 'Delete account',
      body: [
        h('p', { class: 'callout' }, h('b', null, 'This cannot be undone. '),
          'It deletes the account (key, peer ID, XUID, gamertag) and its leaderboard rows and disconnects the player. ',
          'If their game connects again it gets a new, empty account. The gamertag is also removed from the audit log. A ban on the key is kept.'),
        note.el,
        h('label', { class: 'field' }, h('span', null, 'Type ', h('code', null, word), ' to confirm'), typed),
      ],
      valid: () => typed.value.trim() === word,
      run: async () => {
        const r = await api('POST', 'forget', { ...body(t), reason: note.value() });
        const f = r.forgotten || {};
        return 'Deleted ' + (f.gamertag || name(t)) + "'s account and " + num(f.stats_rows) + ' leaderboard rows.' +
          (f.banned ? ' The ban on the key is kept.' : '');
      },
    });
  }

  const ACTIONS = { ban: banSheet, kick: kickSheet, unban: unbanSheet, forget: forgetSheet };

  async function showIP(peer) {
    try {
      const r = await api('GET', 'players?addr=1&peer_id=' + encodeURIComponent(peer));
      const p = (r.players || [])[0];
      if (!p) { toast('That player is no longer online.', true); return; }
      state.ips.set(peer, p.addr);
      renderView(true);
      refreshAuditSoon();
    } catch (e) {
      if (e instanceof AuthError) return lock(e.message);
      toast(e.message, true);
    }
  }

  async function copy(text) {
    try { await navigator.clipboard.writeText(text); toast('Copied ' + text); } catch { /* clipboard blocked */ }
  }

  // ---- views ----
  const matches = (...fields) => {
    const q = state.filter.toLowerCase();
    return !q || fields.some((f) => (f || '').toString().toLowerCase().includes(q));
  };

  function table(head, rows, emptyTitle, emptyText) {
    if (!rows.length) {
      const filtered = state.filter && emptyTitle;
      return h('div', { class: 'empty' },
        h('b', null, filtered ? 'No matches for “' + state.filter + '”' : emptyTitle),
        filtered ? null : emptyText);
    }
    return h('div', { class: 'scroll' }, h('table', null,
      h('thead', null, h('tr', null, head.map((c) => h('th', { class: c.endsWith('#') ? 'num' : null }, c.replace(/#$/, ''))))),
      h('tbody', null, rows)));
  }

  const peerCell = (id) => h('button', { type: 'button', class: 'btn link mono', title: 'Copy', onclick: () => copy(id) }, id);
  const who = (gt, sub, badges, title) => h('div', { class: 'who-cell' },
    h('span', null, h('span', { class: 'name', title }, gt || '–'), badges && badges.length ? h('span', { class: 'badges' }, badges) : null),
    sub ? h('span', { class: 'sub' }, sub) : null);
  const badge = (text, kind, title) => h('span', { class: 'badge' + (kind ? ' ' + kind : ''), title }, text);
  const actionBtn = (label, kind, t, cls) => h('button', {
    type: 'button', class: 'btn tiny' + (cls ? ' ' + cls : ''), 'data-key': kind + ':' + (t.peer_id || t.gamertag),
    'aria-label': label + ' ' + name(t), onclick: () => ACTIONS[kind](known(t)),
  }, label);

  function playersView() {
    const rows = state.players.filter((p) => matches(p.gamertag, p.peer_id, p.xuid)).map((p) => {
      const t = { peer_id: p.peer_id, gamertag: p.gamertag };
      const badges = [];
      if (p.old_server_key) badges.push(badge('old key', 'warn', 'Signed in with the previous server key (rotation grace period)'));
      if (p.privacy) badges.push(badge('privacy', null, 'Asked the server not to share its addresses with peers'));
      if (state.bans.some((b) => b.peer_id === p.peer_id)) badges.push(badge('banned', 'danger'));
      const ip = state.ips.get(p.peer_id);
      return h('tr', null,
        h('td', null, who(p.gamertag, p.client || 'unknown client', badges, 'XUID ' + p.xuid + ' · virtual IP ' + p.vip)),
        h('td', null, peerCell(p.peer_id)),
        h('td', { class: 'num nowrap' }, dur(p.online_s)),
        h('td', { class: 'num nowrap' }, dur(p.idle_s)),
        h('td', { class: 'num' }, num(p.hosting)),
        h('td', { class: 'num' }, num(p.peers)),
        h('td', { class: 'num nowrap' }, p.relay_pkts ? [num(p.relay_pkts), h('div', { class: 'sub' }, bytes(p.relay_bytes))] : h('span', { class: 'sub' }, 'direct')),
        h('td', { class: 'nowrap' }, ip
          ? h('span', { class: 'ip' }, h('span', { class: 'mono' }, ip),
            h('button', { type: 'button', class: 'btn link', onclick: () => { state.ips.delete(p.peer_id); renderView(true); } }, 'Hide'))
          : h('button', { type: 'button', class: 'btn tiny', 'data-key': 'ip:' + p.peer_id, 'aria-label': 'Show the IP address of ' + p.gamertag,
            title: 'Shows this player\'s IP address. The lookup is recorded in the audit log.', onclick: () => showIP(p.peer_id) }, 'Show')),
        h('td', { class: 'actions' }, actionBtn('Kick', 'kick', t), actionBtn('Ban', 'ban', t, 'danger-soft')));
    });
    return table(['Pilot', 'Peer ID', 'Online#', 'Idle#', 'Hosting#', 'Peers#', 'Relay#', 'IP', ''], rows,
      'No pilots online', 'Players appear here while their game is signed in.');
  }

  function sessionsView() {
    const rows = state.sessions.filter((s) => matches(s.host, s.host_peer_id, s.session_id, s.mode_label, ...(s.members || []))).map((s) => {
      const mode = s.mode_label || s.mode_name || (s.mode >= 0 ? 'Mode ' + s.mode : '–');
      const filled = (s.filled_public || 0) + (s.filled_private || 0);
      const slots = (s.public_slots || 0) + (s.private_slots || 0);
      return h('tr', null,
        h('td', null, h('span', { class: 'mono' }, s.session_id)),
        h('td', null, who(s.host, s.host_peer_id, s.host_online ? null : [badge('host offline', 'warn')])),
        h('td', null, mode, s.title && s.title !== '4E4D07D1' ? h('div', { class: 'sub' }, 'title ' + s.title) : null),
        h('td', null, s.type === 'ranked' ? badge('Ranked', 'accent') : s.type === 'standard' ? badge('Standard') : '–'),
        h('td', null, s.state === 'ingame' ? badge('In battle', 'ok') : badge('Lobby')),
        h('td', { class: 'num nowrap' }, filled + ' / ' + slots),
        h('td', { class: 'num nowrap' }, dur(s.age_s)),
        h('td', { class: 'wrap' }, (s.members || []).join(', ') || h('span', { class: 'sub' }, 'none')));
    });
    return table(['Session', 'Host', 'Mode', 'Type', 'State', 'Players#', 'Age#', 'Members'], rows,
      'No sessions', 'Lobbies and matches appear here while they exist.');
  }

  function bansView() {
    const rows = state.bans.filter((b) => matches(b.gamertag, b.peer_id, b.reason)).map((b) => {
      const t = { peer_id: b.peer_id, gamertag: b.gamertag };
      return h('tr', null,
        h('td', null, who(b.gamertag)),
        h('td', null, peerCell(b.peer_id)),
        h('td', { class: 'nowrap' }, date(b.created), h('div', { class: 'sub' }, rel(b.created))),
        h('td', { class: 'nowrap' }, b.expires ? [date(b.expires), h('div', { class: 'sub' }, rel(b.expires))] : badge('Permanent', 'danger')),
        h('td', { class: 'wrap' }, b.reason || h('span', { class: 'sub' }, 'no reason given')),
        h('td', { class: 'actions' }, actionBtn('Unban', 'unban', t)));
    });
    return table(['Pilot', 'Peer ID', 'Banned', 'Ends', 'Reason', ''], rows,
      'No active bans', 'Bans you add here or with "ac6lobby admin ban" appear in this list.');
  }

  const ACTION_LABEL = {
    kick: ['Kick', 'warn'], ban: ['Ban', 'danger'], unban: ['Unban', 'ok'], broadcast: ['Broadcast', 'accent'],
    forget: ['Delete account', 'danger'], view_ip: ['IP lookup', null],
  };

  function auditView() {
    const rows = state.audit.filter((e) => matches(e.gamertag, e.peer_id, e.action, e.reason, e.operator, e.message, e.via)).map((e) => {
      const [label, kind] = ACTION_LABEL[e.action] || [e.action, null];
      const details = [];
      if (e.reason) details.push(h('div', null, '“' + e.reason + '”'));
      if (e.action === 'ban' && e.ok) details.push(h('div', { class: 'sub' }, e.duration === 'permanent' || !e.expires ? 'permanent' : e.duration + ', until ' + date(e.expires)));
      if (e.message) details.push(h('div', null, '“' + e.message + '”'));
      if (e.detail) details.push(h('div', { class: 'sub' }, e.detail));
      if (e.action === 'view_ip' && e.ok) details.push(h('div', { class: 'sub' }, 'the address itself is not recorded'));
      return h('tr', null,
        h('td', { class: 'nowrap' }, date(e.time), h('div', { class: 'sub' }, rel(e.time))),
        h('td', { class: 'nowrap' }, badge(e.via), e.operator ? ' ' + e.operator : null, h('div', { class: 'sub', title: 'First 8 hex digits of the SHA-256 of the admin token used' }, 'token ' + e.token)),
        h('td', null, badge(label, kind)),
        h('td', null, e.gamertag ? who(e.gamertag, e.peer_id)
          : e.peer_id ? h('div', { class: 'who-cell' }, h('span', { class: 'mono' }, e.peer_id),
            e.action === 'forget' ? h('span', { class: 'sub' }, 'gamertag not kept') : null)
            : h('span', { class: 'sub' }, e.action === 'broadcast' ? 'everyone' : '–')),
        h('td', { class: 'wrap' }, details.length ? details : h('span', { class: 'sub' }, '–')),
        h('td', null, e.ok ? badge('OK', 'ok') : [badge('Failed', 'danger'), h('div', { class: 'sub' }, e.error)]));
    });
    return table(['Time', 'Who', 'Action', 'Target', 'Details', 'Result'], rows,
      'Nothing recorded yet', 'Kicks, bans, unbans, broadcasts, account deletions and IP lookups appear here, newest first.');
  }

  function metricsView() {
    const m = state.metrics;
    if (!m) return h('div', { class: 'empty' }, 'Loading…');
    const s = m.server || {}, p = m.process || {};
    const group = (title, pairs) => h('div', { class: 'kv-group' }, h('h2', null, title),
      h('dl', { class: 'kv' }, pairs.flatMap(([k, v]) => [h('dt', null, k), h('dd', null, v)])));
    return h('div', { class: 'kv-groups' },
      group('Now', [
        ['Players online', num(s.online)], ['Sessions', num(s.sessions)], ['Accounts', num(s.accounts)],
        ['Ban records', num(s.bans)], ['Replaced sign-ins still hosting', num(s.retired)],
        ['Online on the previous key', num(s.online_old_key)], ['Uptime', dur(s.uptime_s)],
      ]),
      group('Since start', [
        ['Sign-ins (HELLO)', num(s.hellos)], ['RPCs', num(s.rpcs)], ['Introductions', num(s.intros)],
        ['Relayed packets', num(s.relayed)], ['Relayed data', bytes(s.relayed_bytes)], ['Relay drops', num(s.relay_dropped)],
        ['Malformed packets', num(s.malformed)], ['REHELLO replies', num(s.rehellos)],
        ['Banned sign-ins refused', num(s.banned_refused)], ['Kicks', num(s.kicks)],
      ]),
      group('Process', [
        ['Version', m.version || '–'], ['CPU time', (p.cpu_seconds || 0).toFixed(1) + ' s'],
        ['Memory (RSS)', bytes(p.rss_bytes)], ['Peak RSS', bytes(p.max_rss_bytes)], ['Go heap', bytes(p.heap_bytes)],
        ['Go runtime total', bytes(p.sys_bytes)], ['Goroutines', num(p.goroutines)], ['GC cycles', num(p.num_gc)],
        ['Go', (p.go_version || '–') + ' · ' + (p.gomaxprocs || '?') + ' procs'],
      ]));
  }

  const VIEWS = { players: playersView, sessions: sessionsView, bans: bansView, audit: auditView, metrics: metricsView };

  // renderView redraws the table, keeping keyboard focus and skipping the redraw while the
  // user is selecting text in it (unless forced).
  function renderView(force) {
    const view = $('#view');
    const sel = window.getSelection();
    if (!force && sel && !sel.isCollapsed && view.contains(sel.anchorNode)) return;
    const focusKey = document.activeElement && view.contains(document.activeElement) ? document.activeElement.getAttribute('data-key') : null;
    view.replaceChildren(VIEWS[state.tab]());
    if (focusKey) {
      const again = $$('[data-key]', view).find((el) => el.getAttribute('data-key') === focusKey);
      if (again) again.focus();
    }
  }

  function tile(k, v, sub, cls, title) {
    return h('div', { class: 'tile' + (cls ? ' ' + cls : ''), title },
      h('span', { class: 'v' }, v, sub ? h('span', { class: 's' }, sub) : null), h('span', { class: 'k' }, k));
  }

  function renderTiles() {
    const m = state.metrics;
    if (!m) return;
    const s = m.server || {}, p = m.process || {}, r = state.rates || {};
    const inBattle = state.sessions.filter((x) => x.state === 'ingame').length;
    $('#tiles').replaceChildren(
      tile('Pilots online', num(s.online)),
      tile('Sessions', num(s.sessions), inBattle ? inBattle + ' in battle' : null),
      tile('Accounts', num(s.accounts)),
      tile('Active bans', num(state.bans.length)),
      tile('Relay', rate(r.relayPkts), 'pkt/s'),
      tile('Relay', rate(r.relayKbit), 'kbit/s'),
      tile('Relay drops', rate(r.drops), '/s', r.drops > 0 ? 'warn' : null, 'Relay packets dropped (over the limit or not introduced)'),
      tile('CPU', rate(r.cpu), '%', r.cpu > 50 ? 'warn' : null, 'Share of one core used since the last update'),
      tile('Memory', bytes(p.rss_bytes)));
  }

  function renderMeta() {
    const m = state.metrics;
    if (!m) return;
    const s = m.server || {};
    $('#meta').replaceChildren(
      h('span', null, 'Version ', h('b', null, m.version || 'dev')),
      h('span', null, 'Up ', h('b', null, dur(s.uptime_s))),
      h('span', null, 'Kicks ', h('b', null, num(s.kicks))),
      h('span', null, 'Refused (banned) ', h('b', null, num(s.banned_refused))));
  }

  function renderCounts() {
    const counts = { players: state.players.length, sessions: state.sessions.length, bans: state.bans.length };
    for (const el of $$('[data-count]')) {
      const n = counts[el.getAttribute('data-count')];
      el.textContent = state.metrics ? String(n) : '';
    }
  }

  function renderConn() {
    const el = $('#conn'), txt = $('#conn-text');
    el.classList.remove('live', 'stale', 'down');
    const age = (Date.now() - state.lastOK) / 1000;
    if (state.error) {
      el.classList.add('down');
      txt.textContent = state.error + (state.lastOK ? ' Last update ' + dur(age) + ' ago.' : '');
    } else if (!state.lastOK) {
      txt.textContent = 'Connecting…';
    } else if (age > 15) {
      el.classList.add('stale');
      txt.textContent = 'Last update ' + dur(age) + ' ago';
    } else {
      el.classList.add('live');
      txt.textContent = 'Live · updated ' + tf.format(new Date(state.lastOK));
    }
  }

  function render() {
    renderConn();
    renderMeta();
    renderTiles();
    renderCounts();
    renderView(false);
  }

  function setTab(tab, focus) {
    if (!TABS.includes(tab)) tab = 'players';
    state.tab = tab;
    for (const b of $$('[role=tab]')) {
      const on = b.getAttribute('data-tab') === tab;
      b.setAttribute('aria-selected', on ? 'true' : 'false');
      b.tabIndex = on ? 0 : -1;
      if (on && focus) b.focus();
    }
    $('#filter').hidden = tab === 'metrics';
    try { history.replaceState(null, '', '#' + tab); } catch { /* ignore */ }
    renderView(true);
  }

  // ---- data ----
  function computeRates(m) {
    const now = performance.now();
    const s = m.server || {}, p = m.process || {};
    const prev = state.prev;
    if (prev && now - prev.t > 500) {
      const dt = (now - prev.t) / 1000;
      const d = (a, b) => Math.max(0, (a || 0) - (b || 0)) / dt;
      state.rates = {
        relayPkts: d(s.relayed, prev.s.relayed),
        relayKbit: d(s.relayed_bytes, prev.s.relayed_bytes) * 8 / 1000,
        drops: d(s.relay_dropped, prev.s.relay_dropped),
        cpu: d(p.cpu_seconds, prev.p.cpu_seconds) * 100,
      };
    }
    state.prev = { t: now, s, p };
  }

  async function refresh() {
    if (!state.token || state.loading) return;
    state.loading = true;
    try {
      const [p, s, b, m, a] = await Promise.all([
        api('GET', 'players'), api('GET', 'sessions'), api('GET', 'bans'), api('GET', 'metrics'), api('GET', 'audit?limit=200'),
      ]);
      state.players = p.players || [];
      state.sessions = s.sessions || [];
      state.bans = b.bans || [];
      state.audit = a.audit || [];
      computeRates(m);
      state.metrics = m;
      const online = new Set(state.players.map((x) => x.peer_id));
      for (const k of [...state.ips.keys()]) if (!online.has(k)) state.ips.delete(k);
      state.lastOK = Date.now();
      state.error = '';
    } catch (e) {
      if (e instanceof AuthError) { state.loading = false; lock(e.message); return; }
      state.error = e.message;
    }
    state.loading = false;
    if (state.token) render();
  }

  let auditTimer = 0;
  function refreshAuditSoon() {
    clearTimeout(auditTimer);
    auditTimer = setTimeout(async () => {
      try {
        state.audit = (await api('GET', 'audit?limit=200')).audit || [];
        if (state.tab === 'audit') renderView(false);
      } catch { /* the next poll reports errors */ }
    }, 150);
  }

  function schedule() {
    clearTimeout(state.timer);
    state.timer = setTimeout(async () => {
      if (!document.hidden) await refresh();
      if (state.token) schedule();
    }, POLL_MS);
  }

  // ---- login / lock ----
  function showLogin(message) {
    $('#app').hidden = true;
    $('#login').hidden = false;
    $('#login-error').textContent = message || '';
    const input = $('#token-input');
    input.value = '';
    input.focus();
  }

  function showApp() {
    $('#login').hidden = true;
    $('#app').hidden = false;
    setTab((location.hash || '').slice(1));
    renderConn();
  }

  function lock(message) {
    closeSheet();
    clearTimeout(state.timer);
    clearToken();
    Object.assign(state, {
      token: '', players: [], sessions: [], bans: [], audit: [], metrics: null, prev: null, rates: null,
      lastOK: 0, error: '',
    });
    state.ips.clear();
    $('#tiles').replaceChildren();
    $('#meta').replaceChildren();
    $('#view').replaceChildren();
    showLogin(message);
  }

  async function unlock(tok, remember) {
    state.token = tok;
    const btn = $('#login-btn');
    btn.disabled = true;
    btn.textContent = 'Checking…';
    try {
      await api('GET', 'metrics');
      saveToken(tok, remember);
      showApp();
      await refresh();
      schedule();
    } catch (e) {
      state.token = '';
      $('#login-error').textContent = e instanceof AuthError ? 'That token is not right. Copy it again from admin.token.' : e.message;
    } finally {
      btn.disabled = false;
      btn.textContent = 'Unlock';
    }
  }

  // ---- wiring ----
  function init() {
    $('#login-form').addEventListener('submit', (ev) => {
      ev.preventDefault();
      const tok = $('#token-input').value.trim();
      if (tok.length < 32) {
        $('#login-error').textContent = 'The token is at least 32 characters long.';
        return;
      }
      unlock(tok, $('#remember').checked);
    });
    $('#lock-btn').addEventListener('click', () => lock('Locked. The token was removed from this browser.'));

    for (const b of $$('[role=tab]')) b.addEventListener('click', () => setTab(b.getAttribute('data-tab')));
    $('.tabs').addEventListener('keydown', (ev) => {
      if (ev.key !== 'ArrowRight' && ev.key !== 'ArrowLeft') return;
      const i = TABS.indexOf(state.tab) + (ev.key === 'ArrowRight' ? 1 : -1);
      setTab(TABS[(i + TABS.length) % TABS.length], true);
      ev.preventDefault();
    });
    $('#filter').addEventListener('input', (ev) => { state.filter = ev.target.value.trim(); renderView(true); });

    for (const b of $$('[data-who]')) {
      b.addEventListener('click', () => {
        const raw = $('#who').value.trim();
        if (!raw) {
          toast('Enter a gamertag or a peer ID first.', true);
          $('#who').focus();
          return;
        }
        ACTIONS[b.getAttribute('data-who')](known(parseWho(raw)), () => { $('#who').value = ''; });
      });
    }
    $('#who').addEventListener('keydown', (ev) => {
      if (ev.key === 'Enter') { ev.preventDefault(); $('[data-who=ban]').click(); }
    });

    const msg = $('#bc-msg'), bcBtn = $('#bc-btn'), bcCount = $('#bc-count');
    const bcUpdate = () => {
      bcCount.textContent = msg.value.length + ' / 300';
      bcCount.classList.toggle('full', msg.value.length >= 300);
      bcBtn.disabled = !msg.value.trim();
      bcBtn.textContent = state.metrics ? 'Send to ' + num(state.players.length) + ' online' : 'Send';
    };
    msg.addEventListener('input', bcUpdate);
    $('#broadcast-form').addEventListener('submit', async (ev) => {
      ev.preventDefault();
      if (bcBtn.disabled) return;
      bcBtn.disabled = true;
      try {
        const r = await api('POST', 'broadcast', { msg: msg.value.trim() });
        toast('Sent to ' + num(r.sent) + (r.sent === 1 ? ' client.' : ' clients.'));
        msg.value = '';
        refresh();
      } catch (e) {
        if (e instanceof AuthError) return lock(e.message);
        toast(e.message, true);
      } finally {
        bcUpdate();
      }
    });

    $('#sheet-backdrop').addEventListener('mousedown', (ev) => { if (ev.target === ev.currentTarget) closeSheet(); });
    document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape' && sheetOpen) closeSheet(); });
    document.addEventListener('visibilitychange', () => { if (!document.hidden && state.token) refresh(); });
    setInterval(() => { if (state.token) { renderConn(); bcUpdate(); } }, 1000);

    const saved = loadToken();
    if (saved) {
      state.token = saved;
      showApp();
      refresh().then(() => { if (state.token) schedule(); });
    } else {
      showLogin();
    }
  }

  init();
})();

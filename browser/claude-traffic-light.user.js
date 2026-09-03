// ==UserScript==
// @name         Claude Traffic Light (claude.ai)
// @namespace    claude-traffic-light
// @version      2.0
// @description  Reports claude.ai's state to the Claude Traffic Light desktop app
// @match        https://claude.ai/*
// @match        https://*.claude.ai/*
// @run-at       document-idle
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// @connect      localhost
// ==/UserScript==

(function () {
  'use strict';

  // Must match "port" in traffic-light.ini
  const PORT = 8787;

  // How long after the last sign of activity we call it "done", in ms.
  const QUIET_MS = 1600;

  // While Claude is writing we keep sending "running" as a heartbeat, and we
  // ask the desktop app to fall back to green if the heartbeat stops for this
  // long. The countdown lives in the app on purpose: browsers throttle timers
  // in background tabs, which is exactly when this has to be right.
  const WATCH_MS = 2500;
  const HEARTBEAT_MS = 700;

  // A single isolated change is not Claude working: opening the composer,
  // a placeholder disappearing or a thumbnail appearing each move the page
  // once. A real answer being written moves it many times per second, so we
  // only call it "running" after a few batches in a row.
  const STREAK_NEEDED = 3;

  let last = null;
  let lastSentAt = 0;
  let lastBusyAt = 0;
  let streak = 0;
  let doneTimer = null;

  function report(state) {
    const now = Date.now();
    const heartbeat = (state === 'running');
    if (state === last && !(heartbeat && now - lastSentAt >= HEARTBEAT_MS)) return;
    last = state;
    lastSentAt = now;

    const url = 'http://127.0.0.1:' + PORT + '/state?s=' + state +
                (heartbeat ? '&w=' + WATCH_MS : '');
    try {
      GM_xmlhttpRequest({
        method: 'GET',
        url: url,
        timeout: 2000,
        onerror: function () {},
        ontimeout: function () {}
      });
    } catch (e) { /* app not running: stay quiet */ }
  }

  const text = (el) =>
    ((el && (el.getAttribute('aria-label') || el.textContent)) || '')
      .toLowerCase().trim();

  // ---- RED: something is waiting for your confirmation ---------------
  function waitingForYou() {
    const dialogs = document.querySelectorAll('[role="dialog"], [role="alertdialog"]');
    for (const d of dialogs) {
      if (/allow|approve|grant access|always allow|permitir|aprobar|autoriz/.test(text(d)))
        return true;
    }
    for (const b of document.querySelectorAll('button')) {
      if (/^(allow|approve|always allow|permitir|aprobar)/.test(text(b)))
        return true;
    }
    return false;
  }

  // ---- YELLOW, signal 1: the stop button, or a running tool ------------
  // These stay present during the quiet gaps while Claude thinks between
  // steps, which is exactly when the activity heartbeat below goes silent.
  function visible(el) {
    if (!el) return false;
    // NOT offsetParent: it is null for anything inside a position:fixed
    // ancestor, and Claude's composer is fixed. That single mistake is why
    // the stop button was found and then thrown away as "hidden".
    if (el.getClientRects().length === 0) return false;
    const st = getComputedStyle(el);
    return st.visibility !== 'hidden' && st.display !== 'none' && st.opacity !== '0';
  }

  // The real button is called exactly "Detener respuesta" / "Stop response":
  // short, and starting with the verb. Matching the word anywhere in the label
  // was a trap - the "message actions" button embeds the whole message text in
  // its label, so any message merely MENTIONING the word looked like the stop
  // button, and the light stayed yellow forever.
  const STOP_WORDS = /^(detener|stop|parar|deten[e\u00e9]|cancelar|cancel)\b/;
  const STOP_LABEL_MAX = 40;

  let stopLabel = '';
  // Once we have seen a positive sign we consider a turn to be in progress and
  // hold it, instead of falling back to green in every silent gap. The turn is
  // closed when the composer's own send button is back, or after a long
  // silence as a safety net.
  let inTurn = false;
  let lastPositiveAt = 0;
  const TURN_GRACE_MS = 8000;

  function sendButtonVisible() {
    const b = document.querySelector('[data-testid="chat-input-send"]');
    return !!(b && visible(b));
  }

  function stopButtonVisible() {
    for (const b of document.querySelectorAll('button')) {
      const label = (b.getAttribute('aria-label') ||
                     b.getAttribute('data-testid') || '').trim();
      if (!label || label.length > STOP_LABEL_MAX) continue;
      if (!STOP_WORDS.test(label.toLowerCase())) continue;
      if (visible(b)) { stopLabel = label; return true; }
    }

    // A tool running counts too: the status pill sits there through the pause.
    for (const pill of document.querySelectorAll('[data-testid="tool-status-pill"]')) {
      const txt = (pill.textContent || '').toLowerCase();
      if (visible(pill) && /ejecutando|running|executing/.test(txt)) {
        stopLabel = 'tool-status-pill';
        return true;
      }
    }
    stopLabel = '';
    return false;
  }

  // ---- YELLOW, signal 2: the answer is physically being written ------
  // Markup-independent: while Claude streams a reply, text nodes change
  // many times per second. When it stops, the page goes quiet. This keeps
  // working even if claude.ai redesigns everything.
  // Typing also mutates the DOM: the text itself, and the send button, the
  // Enter hint, the box growing around it. We drop anything that happens
  // inside an editable region, walking up from the mutated node itself.
  // isContentEditable is inherited, so it is true for the <p> elements the
  // editor creates, whatever the attribute literally says.
  function insideEditor(node) {
    let el = node && (node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement);
    for (let i = 0; el && i < 25; i++, el = el.parentElement) {
      if (el.isContentEditable) return true;
      const tag = el.tagName;
      if (tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'FORM') return true;
      // Claude never streams its answer inside a button, so anything changing
      // in one is chrome reacting to the user: the send button lighting up,
      // an icon swapping, a tooltip. Never a sign that Claude is working.
      if (tag === 'BUTTON') return true;
      if (el.hasAttribute && el.hasAttribute('contenteditable')) return true;
      const role = el.getAttribute && el.getAttribute('role');
      if (role === 'textbox' || role === 'button') return true;
    }
    return false;
  }

  // Last element that changed, for diagnostics.
  let lastPath = '';
  function describe(node) {
    const el = node && (node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement);
    if (!el) return '(none)';
    const bits = [];
    let cur = el;
    for (let i = 0; i < 3 && cur; i++, cur = cur.parentElement) {
      bits.unshift(cur.tagName.toLowerCase() +
        (cur.className && typeof cur.className === 'string'
          ? '.' + cur.className.trim().split(/\s+/).slice(0, 2).join('.')
          : ''));
    }
    return bits.join(' > ');
  }

  // Diagnostics are published as an attribute on <html>. Userscripts run in an
  // isolated world - in Edge even unsafeWindow does not bridge it - but the DOM
  // is shared, so this is readable from the page console with:
  //     document.documentElement.dataset.semaforo
  function publishDebug() {
    try {
      document.documentElement.setAttribute('data-semaforo', JSON.stringify({
        v: '2.0',
        reportando: last,
        msDesdeActividad: lastBusyAt ? Date.now() - lastBusyAt : null,
        racha: streak,
        botonStop: stopButtonVisible(),
        senal: stopLabel,
        turnoEnCurso: inTurn,
        botonEnviar: sendButtonVisible(),
        ultimoCambio: lastPath
      }));
    } catch (e) {}
  }

  function noteActivity(records) {
    let streaming = false;
    let streamingTarget = null;
    for (const r of records) {
      if (insideEditor(r.target)) continue;
      if (r.type === 'characterData') { streaming = true; streamingTarget = r.target; break; }
      if (r.type === 'childList') {
        for (const n of r.addedNodes) {
          if (n.nodeType === Node.TEXT_NODE ||
              (n.nodeType === Node.ELEMENT_NODE && n.textContent)) {
            streaming = true;
            streamingTarget = n;
            break;
          }
        }
      }
      if (streaming) break;
    }
    if (streaming) {
      const now = Date.now();
      if (now - lastBusyAt > 1500) streak = 0;   /* se corto: volvemos a cero */
      streak++;
      lastBusyAt = now;
      lastPath = describe(streamingTarget);
    }
    check();
    if (!streaming) return;

    // Re-check once the page has been quiet, so we can turn green even if
    // the tab is in the background and interval timers get throttled.
    clearTimeout(doneTimer);
    doneTimer = setTimeout(check, QUIET_MS + 200);
  }

  function busy() {
    const now = Date.now();

    if (stopButtonVisible()) {          /* stop button or a running tool */
      inTurn = true;
      lastPositiveAt = now;
      return true;
    }

    if (inTurn) {
      const quiet = now - lastPositiveAt;
      // The composer's send button coming back is the reliable end of turn.
      if ((sendButtonVisible() && quiet > 1500) || quiet > TURN_GRACE_MS) {
        inTurn = false;
      } else {
        return true;                    /* bridging a silent gap */
      }
    }

    return streak >= STREAK_NEEDED && (now - lastBusyAt) < QUIET_MS;
  }

  function check() {
    if (waitingForYou())  report('waiting');
    else if (busy())      report('running');
    else                  report('done');
    publishDebug();
  }

  new MutationObserver(noteActivity).observe(document.documentElement, {
    childList: true,
    subtree: true,
    characterData: true
  });

  setInterval(check, 700);
  document.addEventListener('visibilitychange', check);

  check();

})();

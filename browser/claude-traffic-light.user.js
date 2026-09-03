// ==UserScript==
// @name         Claude Traffic Light (claude.ai)
// @namespace    claude-traffic-light
// @version      3.1
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

  // While Claude works we send "running" as a heartbeat and ask the app to
  // fall back to green if it stops arriving. The countdown lives in the app
  // because browsers throttle timers in background tabs — but for the same
  // reason the window has to be generous: a throttled tab can go a while
  // without sending anything. The explicit "done" below is the fast path;
  // this is only the safety net.
  const WATCH_MS = 20000;
  const HEARTBEAT_MS = 1200;

  // Re-send the current state this often even when nothing changed, so the
  // app picks the state up again after being restarted.
  const RESYNC_MS = 15000;

  // Fallback signal: how long after the last page activity we call it done.
  const QUIET_MS = 1600;

  // A single isolated change is not Claude working: opening the composer, a
  // placeholder disappearing or a thumbnail appearing each move the page once.
  // A real answer moves it many times per second.
  const STREAK_NEEDED = 3;

  // Once a positive sign is seen the turn is held open, so silent gaps while
  // Claude thinks between steps don't read as "finished".
  const TURN_GRACE_MS = 8000;

  let last = 'done';        // assume idle: a tab that just loaded must not
                            // announce "done" over another tab's work
  let lastSentAt = 0;
  let lastBusyAt = 0;
  let lastCheckAt = 0;
  let streak = 0;
  let inTurn = false;
  let lastPositiveAt = 0;
  let signal = '';
  let lastPath = '';

  // ---- Several tabs open at once -------------------------------------
  // Every tab reports independently, so an idle one used to send "done"
  // while another was still working and knock the light green.
  let otherTabBusyUntil = 0;
  let composerRoot = null;
  const startedAt = Date.now();
  let channel = null;
  try {
    channel = new BroadcastChannel('claude-traffic-light');
    channel.onmessage = function (e) {
      if (!e || !e.data || !e.data.busy) return;
      otherTabBusyUntil = Date.now() + 5000;
      // Cuando vence, volver a mirar enseguida en vez de esperar al resync:
      // si la otra pestana se cerro a mitad de respuesta, si no tardabamos
      // hasta 15 s en poder decir que termino.
      setTimeout(function () { check(true); }, 5100);
    };
  } catch (e) { /* no BroadcastChannel: each tab is on its own */ }

  function send(state, watchMs) {
    const url = 'http://127.0.0.1:' + PORT + '/state?s=' + state +
                (watchMs ? '&w=' + watchMs : '');
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

  function report(state) {
    const now = Date.now();

    // Another tab is mid-answer: not our place to say it finished.
    if (state === 'done' && now < otherTabBusyUntil) return;

    // Una pestana recien abierta no opina hasta enterarse de que hacen las
    // otras: si no, anunciaba "done" en el instante de cargar, encima de otra
    // pestana que estaba en plena respuesta.
    if (state === 'done' && now - startedAt < 1500) return;

    // El rojo tambien ocupa la luz: antes solo se difundia el amarillo, asi
    // que una pestana ociosa pisaba con verde el rojo de otra.
    const holds = (state === 'running' || state === 'waiting');
    if (holds && channel) {
      try { channel.postMessage({ busy: true }); } catch (e) {}
    }

    const due = now - lastSentAt >= (holds ? HEARTBEAT_MS - 150 : RESYNC_MS);
    if (state === last && !due) return;

    last = state;
    lastSentAt = now;
    // El rojo tambien se manda con vigilancia: si cerras la pestana con el
    // cartel de permiso abierto, algo tiene que devolver la luz a verde.
    send(state, holds ? WATCH_MS : 0);
  }

  const label = (el) =>
    ((el && (el.getAttribute('aria-label') || el.getAttribute('data-testid'))) || '')
      .trim();

  function visible(el) {
    if (!el) return false;
    // NOT offsetParent: it is null for anything inside a position:fixed
    // ancestor, and Claude's composer is fixed. That single mistake is why
    // the stop button was found and then thrown away as "hidden".
    if (el.getClientRects().length === 0) return false;
    const st = getComputedStyle(el);
    return st.visibility !== 'hidden' && st.display !== 'none' && st.opacity !== '0';
  }

  const inDialog = (el) => !!el.closest('[role="dialog"], [role="alertdialog"]');

  // ---- RED: something is waiting for your confirmation ---------------
  // Only inside a dialog. Scanning every button on the page meant a settings
  // toggle reading "Allow analytics" pinned the light red forever.
  function waitingForYou() {
    // Mirando el texto entero del dialogo, un cartel de cookies o un mensaje
    // que dijera "allow me to explain" alcanzaba para dejar la luz en rojo.
    // El permiso se pide con un boton, asi que miramos los botones.
    for (const d of document.querySelectorAll('[role="dialog"], [role="alertdialog"]')) {
      if (!visible(d)) continue;
      for (const b of d.querySelectorAll('button')) {
        const l = ((b.getAttribute('aria-label') || b.textContent) || '')
                    .toLowerCase().trim();
        if (!l || l.length > 40) continue;
        if (/^(allow|approve|grant|permitir|aprobar|autorizar)\b/.test(l))
          return true;
      }
    }
    return false;
  }

  // ---- YELLOW, signal 1: the stop button, or a running tool ----------
  // These stay present through the silent gaps while Claude thinks between
  // steps, which is exactly when the activity heartbeat below goes quiet.
  //
  // The label must START with the verb and be short: the "message actions"
  // button embeds the whole message text in its label, so a loose match read
  // any message mentioning the word as a permanent stop button. And buttons
  // inside a dialog are skipped — "Cancel" in a modal is not Claude working,
  // which is also why "cancel" is not in this list at all.
  const STOP_WORDS = /^(detener|stop|parar|deten[eé])\b/;
  const STOP_LABEL_MAX = 40;

  function positiveSignal() {
    // Buscamos primero dentro del compositor, que es donde el boton de enviar
    // se convierte en el de detener. Sin eso, un "Stop sharing screen" de otra
    // extension dejaba la luz amarilla para siempre.
    const scope = (composerRoot && document.contains(composerRoot))
                    ? composerRoot : document;
    for (const b of scope.querySelectorAll('button')) {
      const l = label(b);
      if (!l || l.length > STOP_LABEL_MAX) continue;
      if (!STOP_WORDS.test(l.toLowerCase())) continue;
      if (inDialog(b)) continue;
      if (visible(b)) { signal = 'stop-button'; return true; }
    }

    for (const pill of document.querySelectorAll('[data-testid="tool-status-pill"]')) {
      const txt = (pill.textContent || '').toLowerCase();
      if (visible(pill) && /ejecutando|running|executing/.test(txt)) {
        signal = 'tool-status-pill';
        return true;
      }
    }

    signal = '';
    return false;
  }

  function sendButtonVisible() {
    const b = document.querySelector('[data-testid="chat-input-send"]');
    if (!b || !visible(b)) return false;
    // Nos guardamos donde vive el compositor mientras podemos verlo.
    let root = b.closest('form');
    if (!root) {
      root = b;
      for (let i = 0; i < 4 && root.parentElement; i++) root = root.parentElement;
    }
    composerRoot = root;
    return true;
  }

  // ---- YELLOW, signal 2: the answer is physically being written ------
  // Markup-independent fallback: while Claude streams, text nodes change many
  // times per second. Anything inside an editable region or a button is
  // ignored — that is the user typing, or the send button lighting up.
  function insideEditor(node) {
    let el = node && (node.nodeType === Node.ELEMENT_NODE ? node : node.parentElement);
    for (let i = 0; el && i < 25; i++, el = el.parentElement) {
      if (el.isContentEditable) return true;
      const tag = el.tagName;
      if (tag === 'TEXTAREA' || tag === 'INPUT' || tag === 'FORM' || tag === 'BUTTON')
        return true;
      if (el.hasAttribute && el.hasAttribute('contenteditable')) return true;
      const role = el.getAttribute && el.getAttribute('role');
      if (role === 'textbox' || role === 'button') return true;
    }
    return false;
  }

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

  function busy(positive) {
    const now = Date.now();

    if (positive) {
      inTurn = true;
      lastPositiveAt = now;
      return true;
    }

    if (inTurn) {
      const quiet = now - lastPositiveAt;
      if ((sendButtonVisible() && quiet > 1500) || quiet > TURN_GRACE_MS) {
        inTurn = false;
      } else {
        return true;               /* bridging a silent gap */
      }
    }

    // El respaldo por actividad solo vale si el compositor NO esta listo para
    // escribir: con el boton de enviar a la vista, Claude no esta respondiendo,
    // y cualquier cosa que refresque texto cada tanto (un reloj relativo en el
    // panel lateral, un aviso) alcanzaba para dejarlo amarillo indefinidamente.
    if (sendButtonVisible()) return false;
    return streak >= STREAK_NEEDED && (now - lastBusyAt) < QUIET_MS;
  }

  // Diagnostics are published as an attribute on <html>. Userscripts run in an
  // isolated world - in Edge even unsafeWindow does not bridge it - but the DOM
  // is shared, so this is readable from the page console with:
  //     document.documentElement.dataset.semaforo
  // Only the KIND of signal is published, never the button's own text: labels
  // can carry file names or fragments of the conversation.
  function publishDebug(positive) {
    try {
      document.documentElement.setAttribute('data-semaforo', JSON.stringify({
        v: '3.1',
        reportando: last,
        msDesdeActividad: lastBusyAt ? Date.now() - lastBusyAt : null,
        racha: streak,
        senal: positive ? signal : '',
        turnoEnCurso: inTurn,
        botonEnviar: sendButtonVisible(),
        otraPestanaTrabajando: Date.now() < otherTabBusyUntil,
        ultimoCambio: lastPath
      }));
    } catch (e) {}
  }

  // One pass over the buttons per check, shared by busy() and the diagnostics:
  // this used to scan the whole page twice, forcing layout each time, dozens
  // of times a second on a long conversation.
  function check(force) {
    const now = Date.now();
    if (!force && now - lastCheckAt < 200) return;
    lastCheckAt = now;

    const positive = positiveSignal();

    if (waitingForYou())        report('waiting');
    else if (busy(positive))    report('running');
    else                        report('done');

    publishDebug(positive);
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
    check(false);
  }

  new MutationObserver(noteActivity).observe(document.documentElement, {
    childList: true,
    subtree: true,
    characterData: true
  });

  setInterval(function () { check(true); }, HEARTBEAT_MS);
  document.addEventListener('visibilitychange', function () { check(true); });

  check(true);
})();

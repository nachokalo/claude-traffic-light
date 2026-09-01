// ==UserScript==
// @name         Claude Traffic Light (claude.ai)
// @namespace    claude-traffic-light
// @version      1.0
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

  let last = null;
  let queued = false;

  function report(state) {
    if (state === last) return;
    last = state;
    try {
      GM_xmlhttpRequest({
        method: 'GET',
        url: 'http://127.0.0.1:' + PORT + '/state?s=' + state,
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
  // These selectors are the fragile part. If claude.ai changes its markup,
  // this is the place to patch.
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

  // ---- YELLOW: it is generating --------------------------------------
  function working() {
    // the stop button only exists while a response is streaming
    const stop = document.querySelector(
      'button[aria-label*="Stop" i], button[aria-label*="Detener" i], ' +
      'button[data-testid="stop-button"], [data-testid="stop-response"]'
    );
    if (stop && stop.offsetParent !== null) return true;

    return !!document.querySelector('[aria-busy="true"], [data-is-streaming="true"]');
  }

  function check() {
    queued = false;
    if (waitingForYou())  report('waiting');
    else if (working())   report('running');
    else                  report('done');
  }

  function schedule() {
    if (queued) return;
    queued = true;
    setTimeout(check, 250);
  }

  // A MutationObserver keeps firing while the tab is in the background,
  // which is exactly when you need the light. Timers alone get throttled.
  new MutationObserver(schedule).observe(document.documentElement, {
    childList: true,
    subtree: true,
    attributes: true,
    attributeFilter: ['aria-label', 'aria-busy', 'data-is-streaming', 'disabled']
  });

  // safety net in case a change doesn't trip the observer
  setInterval(check, 5000);
  document.addEventListener('visibilitychange', check);

  check();
})();

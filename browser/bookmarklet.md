# Bookmarklet — no extension required

Use this if you can't install browser extensions (a locked-down work machine,
for example). It's a bookmark that connects the current claude.ai tab to the
traffic light.

**Differences from the userscript.** The userscript runs by itself every time
you open claude.ai; the bookmarklet has to be clicked once per tab, after the
page loads, and again after every reload. It sends the same heartbeat, so the
light returns to green on its own if you close the tab mid-answer.

It is deliberately simpler, and you should know what it gives up:

- It reads Claude's state **only** from the stop button and the tool status
  pill. The userscript also falls back to watching the page for activity, so
  if claude.ai renames that button the userscript keeps working and the
  bookmarklet goes quiet.
- It searches the whole page for the stop button instead of just the composer,
  so another tab-mate — a screen-sharing bar with a "Stop sharing" button, say
  — can hold the light yellow.
- It does not coordinate between tabs, so with two claude.ai tabs open the
  light can flicker.
- It does not publish the `data-semaforo` diagnostics the README describes.
- Like the userscript, it never reports red: that state comes from the Claude
  Code hooks, which know for certain when Claude is waiting for you.

There is also one thing neither of us can promise from here: the bookmarklet
talks to the app with a plain `fetch` from the page, and if claude.ai's content
security policy blocks connections to `127.0.0.1`, it will fail silently — the
alert will still say it connected. The userscript does not have this problem
because `GM_xmlhttpRequest` runs outside the page. **Test it once** (see the
last section) before relying on it.

## Install

1. Show the bookmarks bar: <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>B</kbd>
2. Right-click the bar → **Add page** / **Add favourite**
3. Name: `Traffic Light`
   URL: the long line below — copy it **whole**, it's a single line
4. Save

## Use

1. Make sure `claude-traffic-light.exe` is running (icon in the notification area).
2. Open claude.ai and let it load.
3. Click the `Traffic Light` bookmark. It confirms with an alert.
4. If you reload the page (F5), click it again.

## The line

```
javascript:(function(){var P=8787,W=90000,u=null,t=null,hb=0;if(window.__ctl){alert('Traffic light already active in this tab');return}window.__ctl=1;function send(s,w){try{fetch('http://127.0.0.1:'+P+'/state?s='+s+(w?'&w='+w:''),{mode:'no-cors'}).catch(function(){})}catch(e){}}function rep(s){var n=Date.now();var h=(s==='running'||s==='waiting');var due=n-hb>=(h?1200:15000);if(s===u&&!due)return;u=s;hb=n;send(s,h?W:0)}function lab(e){return((e&&(e.getAttribute('aria-label')||e.getAttribute('data-testid')))||'').trim()}function vis(e){if(!e)return 0;if(e.getClientRects().length===0)return 0;var s=getComputedStyle(e);return s.visibility!=='hidden'&&s.display!=='none'&&s.opacity!=='0'}function dlg(e){return!!e.closest('[role=dialog],[role=alertdialog]')}function pos(){var b=document.querySelectorAll('button');for(var i=0;i<b.length;i++){var l=lab(b[i]);if(!l||l.length>40)continue;if(!/^(detener|stop|parar|deten[eé]|det[eé]n)(\s|$|[-:.])/.test(l.toLowerCase()))continue;if(dlg(b[i]))continue;if(vis(b[i]))return 1}var p=document.querySelectorAll('[data-testid=tool-status-pill]');for(i=0;i<p.length;i++){if(vis(p[i])&&/ejecutando|running|executing/.test((p[i].textContent||'').toLowerCase()))return 1}return 0}function chk(){t=null;rep(pos()?'running':'done')}function sch(){if(!t)t=setTimeout(chk,250)}new MutationObserver(sch).observe(document.documentElement,{childList:true,subtree:true,characterData:true});setInterval(chk,1200);chk();alert('Traffic light connected to this tab')})()
```

## If it doesn't work

- Check that the app is actually running (icon next to the clock).
- Chrome and Edge may ask for permission the first time a web page talks to
  your own machine. Allow it.
- Test the app on its own first — open this in the browser:
  <http://127.0.0.1:8787/state?s=running>
  The light should turn yellow and the page should print `ok`. That proves the
  app is listening; it does not prove the bookmarklet can reach it.
- To prove the bookmarklet itself works: click it, then ask Claude something
  that takes a few seconds and watch the light go yellow. If the app responds
  to the URL above but never reacts to the bookmarklet, the page's content
  security policy is blocking it and you need the userscript instead.

Unlike the userscript, this path needs the browser to allow a page on `https`
to reach `127.0.0.1`. The app sends the
`Access-Control-Allow-Private-Network` header so that request is permitted.

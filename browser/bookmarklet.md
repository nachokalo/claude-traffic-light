# Bookmarklet — no extension required

Use this if you can't install browser extensions (a locked-down work machine,
for example). It's a bookmark that connects the current claude.ai tab to the
traffic light.

**Differences from the userscript.** The userscript runs by itself every time
you open claude.ai; the bookmarklet has to be clicked once per tab, after the
page loads, and again after every reload. It uses the same signals to read
Claude's state (the stop button, the tool status pill, and page activity) and
sends the same heartbeat, so the light returns to green on its own if you
close the tab mid-answer. What it does not have is the coordination between
several open tabs, so with two claude.ai tabs at once the light can flicker.

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
javascript:(function(){var P=8787,W=20000,u=null,t=null,hb=0;if(window.__ctl){alert('Traffic light already active in this tab');return}window.__ctl=1;function send(s,w){try{fetch('http://127.0.0.1:'+P+'/state?s='+s+(w?'&w='+w:''),{mode:'no-cors'}).catch(function(){})}catch(e){}}function rep(s){var n=Date.now();var h=(s==='running'||s==='waiting');var due=n-hb>=(h?1200:15000);if(s===u&&!due)return;u=s;hb=n;send(s,h?W:0)}function lab(e){return((e&&(e.getAttribute('aria-label')||e.getAttribute('data-testid')))||'').trim()}function vis(e){if(!e)return 0;if(e.getClientRects().length===0)return 0;var s=getComputedStyle(e);return s.visibility!=='hidden'&&s.display!=='none'&&s.opacity!=='0'}function dlg(e){return!!e.closest('[role=dialog],[role=alertdialog]')}function wait(){var d=document.querySelectorAll('[role=dialog],[role=alertdialog]');for(var i=0;i<d.length;i++){if(!vis(d[i]))continue;var b=d[i].querySelectorAll('button');for(var j=0;j<b.length;j++){var l=((b[j].getAttribute('aria-label')||b[j].textContent)||'').toLowerCase().trim();if(l&&l.length<=40&&/^(allow|approve|grant|permitir|aprobar|autorizar)\b/.test(l))return 1}}return 0}function pos(){var b=document.querySelectorAll('button');for(var i=0;i<b.length;i++){var l=lab(b[i]);if(!l||l.length>40)continue;if(!/^(detener|stop|parar|deten[eé])\b/.test(l.toLowerCase()))continue;if(dlg(b[i]))continue;if(vis(b[i]))return 1}var p=document.querySelectorAll('[data-testid=tool-status-pill]');for(i=0;i<p.length;i++){if(vis(p[i])&&/ejecutando|running|executing/.test((p[i].textContent||'').toLowerCase()))return 1}return 0}function chk(){t=null;rep(wait()?'waiting':(pos()?'running':'done'))}function sch(){if(!t)t=setTimeout(chk,250)}new MutationObserver(sch).observe(document.documentElement,{childList:true,subtree:true,characterData:true});setInterval(chk,1200);chk();alert('Traffic light connected to this tab')})()
```

## If it doesn't work

- Check that the app is actually running (icon next to the clock).
- Chrome and Edge may ask for permission the first time a web page talks to
  your own machine. Allow it.
- Test the app on its own first — open this in the browser:
  <http://127.0.0.1:8787/state?s=running>
  The light should turn yellow and the page should print `ok`.

Unlike the userscript, this path needs the browser to allow a page on `https`
to reach `127.0.0.1`. The app sends the
`Access-Control-Allow-Private-Network` header so that request is permitted.

# Bookmarklet — no extension required

Use this if you can't install browser extensions (a locked-down work machine,
for example). It's a bookmark that connects the current claude.ai tab to the
traffic light.

**Difference from the userscript:** the userscript runs by itself every time
you open claude.ai; the bookmarklet has to be clicked once per tab, after the
page loads. As long as you don't reload, it keeps working for the whole
session.

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
javascript:(function(){var P=8787,u=null,t=null;if(window.__ctl){alert('Traffic light already active in this tab');return}window.__ctl=1;function send(s){if(s===u)return;u=s;fetch('http://127.0.0.1:'+P+'/state?s='+s,{mode:'no-cors'}).catch(function(){})}function tx(e){return((e&&((e.getAttribute&&e.getAttribute('aria-label'))||e.textContent))||'').toLowerCase().trim()}function wait(){var d=document.querySelectorAll('[role=dialog],[role=alertdialog]'),i;for(i=0;i<d.length;i++){if(/allow|approve|grant access|permitir|aprobar|autoriz/.test(tx(d[i])))return 1}var b=document.querySelectorAll('button');for(i=0;i<b.length;i++){if(/^(allow|approve|permitir|aprobar)/.test(tx(b[i])))return 1}return 0}function busy(){var s=document.querySelector('button[aria-label*=Stop i],button[aria-label*=Detener i],[data-testid=stop-button]');if(s&&s.offsetParent!==null)return 1;return document.querySelector('[aria-busy=true],[data-is-streaming=true]')?1:0}function chk(){t=null;send(wait()?'waiting':(busy()?'running':'done'))}function sched(){if(!t)t=setTimeout(chk,250)}new MutationObserver(sched).observe(document.documentElement,{childList:true,subtree:true,attributes:true,attributeFilter:['aria-label','aria-busy','data-is-streaming','disabled']});setInterval(chk,5000);chk();alert('Traffic light connected to this tab')})()
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

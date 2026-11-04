// HTML snippets of the web interface (status page and firmware update page).
// The dynamic parts are filled in by the handlers in src/twitchDisplay.cpp.
#pragma once

// Page header up to the start of the content. %TITLE% and %NAV% are replaced.
static const char WEB_PAGE_HEAD[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>twitchDisplay - %TITLE%</title>
<style>
:root{--bg:#18181b;--card:#26262c;--text:#efeff1;--muted:#adadb8;--accent:#a970ff;--ok:#00c853;--err:#ff4f4f}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.5 system-ui,sans-serif}
main{max-width:640px;margin:0 auto;padding:16px}
nav{display:flex;gap:16px;align-items:center;margin-bottom:16px}
nav b{font-size:18px;margin-right:auto}
a{color:var(--accent);text-decoration:none}
.card{background:var(--card);border-radius:8px;padding:16px;margin-bottom:16px}
h2{margin:0 0 12px;font-size:16px}
table{width:100%;border-collapse:collapse}
td{padding:4px 0;vertical-align:top}
td:first-child{color:var(--muted);width:40%;padding-right:12px}
button{background:var(--accent);color:#fff;border:0;border-radius:6px;padding:8px 16px;font:inherit;cursor:pointer}
button:disabled{opacity:.5;cursor:default}
progress{width:100%;height:16px;margin:12px 0}
.muted{color:var(--muted)}
.ok{color:var(--ok)}
.err{color:var(--err)}
input[type=file]{color:var(--text);max-width:100%}
h3{margin:20px 0 8px;font-size:14px;color:var(--muted);text-transform:uppercase;letter-spacing:.05em}
label{display:block;margin-bottom:10px}
input[type=text],input[type=password],textarea{display:block;width:100%;margin-top:4px;padding:8px;border:1px solid #3a3a44;border-radius:6px;background:var(--bg);color:var(--text);font:inherit}
label.check{display:flex;gap:8px;align-items:center;color:var(--muted);margin-top:-4px}
button.danger{background:var(--err)}
.hint{font-size:13px;color:var(--muted)}
</style></head><body><main>
<nav><b>twitchDisplay</b>%NAV%</nav>
)HTML";

static const char WEB_PAGE_FOOT[] = R"HTML(</main></body></html>
)HTML";

// Firmware update page. %ONLINE% (online update card or nothing) and
// %MAXSIZE% (bytes) are replaced.
static const char WEB_UPDATE_BODY[] = R"HTML(%ONLINE%<div class="card">
<h2>Upload firmware</h2>
<p class="muted">Upload <b>firmware.bin</b> from <code>.pio/build/twitchDisplay/</code>.
The display restarts with the new firmware afterwards. If the new firmware does not
come online within a minute, the previous one is restored on the next restart.</p>
<input type="file" id="file" accept=".bin">
<p><button id="go">Upload</button></p>
<progress id="bar" max="100" value="0" hidden></progress>
<div id="msg"></div>
</div>
<script>
const maxSize = %MAXSIZE%;
const $ = id => document.getElementById(id);
function show(text, cls) { $('msg').textContent = text; $('msg').className = cls || ''; }
function waitForRestart() {
  fetch('/', {cache: 'no-store'})
    .then(() => { location.href = '/'; })
    .catch(() => setTimeout(waitForRestart, 2000));
}
$('go').onclick = () => {
  const f = $('file').files[0];
  if (!f) { show('Choose a file first.', 'err'); return; }
  if (!f.name.endsWith('.bin')) { show('Please choose a .bin file.', 'err'); return; }
  if (f.size > maxSize) { show('File is too large (' + f.size + ' bytes, max ' + maxSize + ').', 'err'); return; }
  const data = new FormData();
  data.append('firmware', f, f.name);
  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  xhr.upload.onprogress = e => { if (e.lengthComputable) $('bar').value = e.loaded / e.total * 100; };
  xhr.onload = () => {
    let r;
    try { r = JSON.parse(xhr.responseText); } catch (e) { r = {ok: false, message: 'Error ' + xhr.status + ' ' + xhr.statusText}; }
    if (r.ok) { show(r.message + ' Waiting for the display to come back...', 'ok'); setTimeout(waitForRestart, 5000); }
    else { show(r.message, 'err'); $('go').disabled = false; }
  };
  xhr.onerror = () => { show('Upload failed: connection lost.', 'err'); $('go').disabled = false; };
  $('go').disabled = true;
  $('bar').hidden = false;
  $('bar').value = 0;
  show('Uploading...');
  xhr.send(data);
};
</script>
)HTML";

// Online update card (installs the newest GitHub release). %VERSION% is replaced.
static const char WEB_ONLINE_UPDATE_CARD[] = R"HTML(<div class="card">
<h2>Online update</h2>
<p class="muted">Installed version: <b>%VERSION%</b></p>
<p><button id="check">Check for updates</button></p>
<div id="online"></div>
</div>
<script>
(() => {
  const box = document.getElementById('online');
  // release data is only inserted as text, never as HTML
  function line(text, cls) { const p = document.createElement('p'); p.textContent = text; if (cls) p.className = cls; box.appendChild(p); return p; }
  function fetchTimeout(url, opts, ms) {
    const c = new AbortController(); setTimeout(() => c.abort(), ms);
    return fetch(url, Object.assign({cache: 'no-store', signal: c.signal}, opts || {}));
  }
  function poll() {
    fetchTimeout('/update/state', {}, 3000).then(r => r.json()).then(s => {
      if (s.state === 'failed') { box.textContent = ''; line('Update failed: ' + s.message, 'err'); check.disabled = false; }
      else if (s.state === 'installing') setTimeout(poll, 2000);
      else location.href = '/';
    }).catch(() => setTimeout(poll, 2000)); // busy installing or restarting
  }
  function install(btn) {
    btn.disabled = true; check.disabled = true;
    line('Starting...');
    fetch('/update/install', {method: 'POST'}).then(r => r.json()).then(r => {
      box.textContent = '';
      if (!r.ok) { line(r.message, 'err'); check.disabled = false; return; }
      line(r.message + ' The display shows the progress and restarts when done.', 'ok');
      setTimeout(poll, 5000);
    }).catch(() => { line('Request failed.', 'err'); check.disabled = false; });
  }
  const check = document.getElementById('check');
  check.onclick = () => {
    check.disabled = true; box.textContent = ''; line('Checking...');
    fetch('/update/check', {cache: 'no-store'}).then(r => r.json()).then(r => {
      box.textContent = ''; check.disabled = false;
      if (!r.ok) { line(r.message, 'err'); return; }
      if (r.upToDate) { line('You have the newest version (' + r.latest + ').', 'ok'); return; }
      line('Version ' + r.latest + ' is available' + (r.published ? ' (released ' + r.published.slice(0, 10) + ').' : '.'));
      if (r.notes) { const a = document.createElement('a'); a.href = r.notes; a.target = '_blank'; a.rel = 'noopener'; a.textContent = 'Release notes'; line('').appendChild(a); }
      const p = line(''); const b = document.createElement('button'); b.textContent = 'Install ' + r.latest; b.onclick = () => install(b); p.appendChild(b);
    }).catch(() => { box.textContent = ''; line('Request failed.', 'err'); check.disabled = false; });
  };
})();
</script>
)HTML";

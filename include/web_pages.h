// HTML snippets of the web interface (status page and firmware update page).
// The dynamic parts are filled in by the handlers in src/twitchDisplay.cpp.
#pragma once

// Page header up to the start of the content. %TITLE% is replaced.
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
</style></head><body><main>
<nav><b>twitchDisplay</b><a href="/">Status</a><a href="/update">Update</a></nav>
)HTML";

static const char WEB_PAGE_FOOT[] = R"HTML(</main></body></html>
)HTML";

// Firmware update page. %MAXSIZE% (bytes) is replaced.
static const char WEB_UPDATE_BODY[] = R"HTML(<div class="card">
<h2>Firmware update</h2>
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

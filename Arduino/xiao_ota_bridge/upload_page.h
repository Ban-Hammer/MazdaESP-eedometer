#pragma once
static const char UPLOAD_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1">
<title>Mazda Display OTA</title>
<style>body{font-family:sans-serif;margin:2rem;max-width:32rem}
#bar{height:1rem;background:#0a0;width:0;transition:width .2s}#log{white-space:pre-wrap}
button{padding:.5rem 1rem}</style></head><body>
<h2>Mazda Display Firmware Update</h2>
<p>Running version: <b id=ver>...</b></p>
<input type=file id=f accept=".bin"><button onclick=up()>Upload &amp; Flash</button>
<div style="border:1px solid #888;margin:1rem 0"><div id=bar></div></div>
<div id=log></div>
<script>
fetch('/ping').then(r=>r.text()).then(t=>ver.textContent=t).catch(()=>ver.textContent='(no reply)');
function up(){let file=f.files[0];if(!file){log.textContent='pick a .bin';return;}
 log.textContent='uploading...';
 let x=new XMLHttpRequest();x.open('POST','/update');
 x.upload.onprogress=e=>{bar.style.width=(60*e.loaded/e.total)+'%';};
 x.onload=()=>{bar.style.width='100%';log.textContent=x.responseText;};
 x.onerror=()=>log.textContent='upload error';
 let fd=new FormData();fd.append('fw',file);x.send(fd);}
</script></body></html>
)HTML";

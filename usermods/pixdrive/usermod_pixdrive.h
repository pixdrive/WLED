#pragma once

#include "wled.h"

// Requires the SD card usermod (WLED_USE_SD_SPI or WLED_USE_SD_MMC)
#ifndef SD_ADAPTER
  #error "PixDrive requires SD card support. Define WLED_USE_SD_SPI or WLED_USE_SD_MMC."
#endif

// ---- .wled binary file format (20-byte header) ----
// Flags bit 0: RGBW (0=RGB/3bpp, 1=RGBW/4bpp)
#define WLED_FLAG_RGBW 0x01

struct __attribute__((packed)) WledFileHeader {
  char     magic[4];      // "WLED"
  uint8_t  fps;           // 1-255
  uint8_t  flags;         // bit 0: RGBW
  uint16_t ledCount;      // little-endian
  uint32_t frameCount;    // little-endian
  uint32_t crc32;         // CRC32 over all frame data
  uint8_t  reserved[4];   // must be 0
};
static_assert(sizeof(WledFileHeader) == 20, "WledFileHeader must be 20 bytes");

#define WLED_FILE_HEADER_SIZE 20
#define WLED_FILE_MAGIC "WLED"
#define PIXDRIVE_DIR "/pixdrive"

// ---- Per-segment playback state ----
#define PIXDRIVE_MAX_PATH 64
struct PixDrivePlayback {
  uint32_t frameCount;
  uint32_t currentFrame;
  uint16_t ledCount;
  uint16_t fps;
  uint8_t  bpp;
  bool     fileOpen;
  bool     loop;
  char     openFilename[PIXDRIVE_MAX_PATH];
};

// Global state arrays indexed by segment ID
static File pixdriveFiles[MAX_NUM_SEGMENTS];
static PixDrivePlayback pixdriveState[MAX_NUM_SEGMENTS];

// Close a pixdrive file for a given segment
static void pixdriveCloseFile(uint8_t segId) {
  if (segId < MAX_NUM_SEGMENTS && pixdriveState[segId].fileOpen) {
    pixdriveFiles[segId].close();
    pixdriveState[segId].fileOpen = false;
  }
}

// ---- CRC32 for upload validation ----
static uint32_t pixdriveCRC32(const uint8_t *data, size_t len, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static bool pixdriveValidateCRC(const char *path) {
  File f = SD_ADAPTER.open(path, FILE_READ);
  if (!f) return false;
  WledFileHeader hdr;
  if (f.read((uint8_t*)&hdr, WLED_FILE_HEADER_SIZE) != WLED_FILE_HEADER_SIZE) { f.close(); return false; }
  if (memcmp(hdr.magic, WLED_FILE_MAGIC, 4) != 0) { f.close(); return false; }
  if (hdr.crc32 == 0) { f.close(); return true; }
  uint32_t crc = 0;
  uint8_t buf[512];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    crc = pixdriveCRC32(buf, n, crc);
  }
  f.close();
  return crc == hdr.crc32;
}

// ---- Effect function ----
static uint16_t mode_pixdrive(void) {
  uint8_t segId = strip.getCurrSegmentId();
  if (segId >= MAX_NUM_SEGMENTS) return FRAMETIME;

  PixDrivePlayback &pb = pixdriveState[segId];

  // Detect when we need to (re)open a file:
  // 1. First call after effect selected (SEGENV.call == 0)
  // 2. File not open (e.g. previous open failed, or segment was reset)
  // 3. Filename changed (user switched preset)
  // 4. Data buffer was freed (segment reconfigured)
  const char *filepath = SEGMENT.name;
  bool needReopen = (SEGENV.call == 0) || !pb.fileOpen || !SEGMENT.data;
  if (!needReopen) {
    // Check if filename changed (preset switch)
    if (!filepath || filepath[0] == '\0') {
      needReopen = (pb.openFilename[0] != '\0');
    } else if (strcmp(filepath, pb.openFilename) != 0) {
      needReopen = true;
    }
  }

  if (needReopen) {
    pixdriveCloseFile(segId);
    pb = PixDrivePlayback{}; // zero-init

    const char *filepath = SEGMENT.name;
    if (!filepath || filepath[0] == '\0') {
      for (int i = 0; i < SEGLEN; i++) SEGMENT.setPixelColor(i, RGBW32(32, 0, 0, 0));
      return FRAMETIME;
    }

    File &f = pixdriveFiles[segId];
    f = SD_ADAPTER.open(filepath, FILE_READ);
    if (!f) {
      for (int i = 0; i < SEGLEN; i++) SEGMENT.setPixelColor(i, RGBW32(32, 0, 0, 0));
      return FRAMETIME;
    }

    // Read and validate header
    WledFileHeader hdr;
    if (f.read((uint8_t*)&hdr, WLED_FILE_HEADER_SIZE) != WLED_FILE_HEADER_SIZE) {
      f.close();
      return FRAMETIME;
    }
    if (memcmp(hdr.magic, WLED_FILE_MAGIC, 4) != 0 || hdr.fps == 0 || hdr.ledCount == 0 || hdr.frameCount == 0) {
      f.close();
      return FRAMETIME;
    }

    pb.frameCount = hdr.frameCount;
    pb.ledCount   = hdr.ledCount;
    pb.fps        = hdr.fps;
    pb.bpp        = (hdr.flags & WLED_FLAG_RGBW) ? 4 : 3;
    pb.currentFrame = 0;
    pb.fileOpen   = true;
    strncpy(pb.openFilename, filepath, PIXDRIVE_MAX_PATH - 1);
    pb.openFilename[PIXDRIVE_MAX_PATH - 1] = '\0';

    // Allocate frame buffer
    uint16_t frameSize = pb.ledCount * pb.bpp;
    if (!SEGMENT.allocateData(frameSize)) {
      f.close();
      pb.fileOpen = false;
      return FRAMETIME;
    }
  }

  if (!pb.fileOpen) return FRAMETIME;

  // Loop by default, check1 = "Once" (disables loop)
  pb.loop = !SEGMENT.check1;

  File &f = pixdriveFiles[segId];
  uint16_t frameSize = pb.ledCount * pb.bpp;
  uint8_t *buf = SEGMENT.data;

  // Check if we need to loop before reading
  if (pb.currentFrame >= pb.frameCount) {
    if (pb.loop) {
      f.seek(WLED_FILE_HEADER_SIZE);
      pb.currentFrame = 0;
    } else {
      // Freeze on last frame — just re-display buffer contents
      goto render;
    }
  }

  // Read one frame
  {
    size_t bytesRead = f.read(buf, frameSize);
    if (bytesRead < frameSize) {
      // Unexpected short read — close and stop
      pixdriveCloseFile(segId);
      return FRAMETIME;
    }
    pb.currentFrame++;
  }

render:
  {
    uint16_t fileLeds = pb.ledCount;
    uint16_t segLeds = SEGLEN;
    uint8_t bright = SEGMENT.intensity;
    uint8_t bpp = pb.bpp;

    for (uint16_t i = 0; i < segLeds; i++) {
      uint16_t srcIdx = (fileLeds == segLeds) ? i : (uint32_t)i * fileLeds / segLeds;
      if (srcIdx >= fileLeds) srcIdx = fileLeds - 1;
      uint16_t offset = srcIdx * bpp;
      uint8_t r = buf[offset];
      uint8_t g = buf[offset + 1];
      uint8_t b = buf[offset + 2];
      uint8_t w = (bpp == 4) ? buf[offset + 3] : 0;
      if (bright < 255) {
        r = ((uint16_t)r * bright) >> 8;
        g = ((uint16_t)g * bright) >> 8;
        b = ((uint16_t)b * bright) >> 8;
        w = ((uint16_t)w * bright) >> 8;
      }
      SEGMENT.setPixelColor(i, RGBW32(r, g, b, w));
    }
  }

  // Frame delay: base from file FPS, scaled by speed slider
  // speed=0 → 25% speed, speed=128 → 100%, speed=255 → 400%
  uint16_t baseDelay = 1000 / pb.fps;
  uint16_t speedPct = map(SEGMENT.speed, 0, 255, 25, 400);
  uint16_t delay = (uint32_t)baseDelay * 100 / speedPct;
  if (delay < 1) delay = 1;

  return delay;
}

static const char _data_FX_MODE_PIXDRIVE[] PROGMEM = "PixDrive@Speed,Brightness;!;!;Once;1";

// ---- Web UI HTML (will be populated in Phase 3) ----
static const char PIXDRIVE_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PixDrive SD Manager</title>
<style>
:root{--bg:#1e1e1e;--bg-el:#262626;--bg-srf:#303030;--bg-inp:#2a2a2a;--brd:#333;--brd2:#444;--accent:#fbbf24;--accent10:rgba(251,191,36,.1);--accent15:rgba(251,191,36,.15);--accent-s:rgba(251,191,36,.25);--txt:#ececec;--txt2:#bbb;--txt3:#999;--txt4:#555;--ok:#22c55e;--ok15:rgba(34,197,94,.15);--err:#f87171;--info:#3b82f6;--warn:#f97316;--r-sm:6px;--r-md:8px;--r-lg:12px;--r-full:100px}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Inter,-apple-system,system-ui,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh;display:flex;flex-direction:column}
.header{display:flex;align-items:center;justify-content:space-between;padding:16px 40px;background:#222;border-bottom:1px solid var(--brd)}
.logo{display:flex;align-items:center;gap:8px;font-weight:700;font-size:18px}
.logo svg{color:var(--accent)}
.nav{display:flex;gap:28px}
.nav a{color:var(--txt3);text-decoration:none;font-size:14px}
.nav a:hover{color:var(--txt)}
.main{flex:1;max-width:1312px;width:100%;margin:0 auto;padding:40px 64px}
.title-bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:32px}
.title-left h1{font-size:28px;font-weight:700;letter-spacing:-.5px}
.title-left p{color:var(--txt3);font-size:14px;margin-top:4px}
.badge{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:var(--r-full);background:var(--ok15);font-size:12px;font-weight:500;color:var(--ok)}
.badge-dot{width:8px;height:8px;border-radius:50%;background:var(--ok)}
.cards{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-bottom:32px}
.card{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);padding:20px;display:flex;flex-direction:column;gap:14px}
.card-top{display:flex;justify-content:space-between;align-items:center}
.card-label{font-size:13px;color:var(--txt3);font-weight:500}
.card-icon{width:20px;height:20px}
.card-val{font-size:24px;font-weight:700;letter-spacing:-.5px}
.card-sub{font-size:12px;color:var(--txt4);font-weight:500}
.bar-outer{height:6px;background:var(--bg-srf);border-radius:3px;width:100%}
.bar-inner{height:100%;border-radius:3px;background:var(--accent);transition:width .3s}
.action-bar{display:flex;justify-content:space-between;align-items:center;margin-bottom:24px}
.action-left{display:flex;gap:12px;align-items:center}
.search{display:flex;align-items:center;gap:10px;padding:10px 14px;border-radius:var(--r-sm);background:var(--bg-el);border:1px solid var(--brd);width:300px;color:var(--txt4)}
.search input{background:none;border:none;outline:none;color:var(--txt);font-size:13px;width:100%;font-family:inherit}
.search input::placeholder{color:var(--txt4)}
.filter-btn{display:flex;align-items:center;gap:8px;padding:10px 14px;border-radius:var(--r-sm);background:var(--bg-el);border:1px solid var(--brd);color:var(--txt2);font-size:13px;cursor:pointer;font-family:inherit}
.btn-primary{display:flex;align-items:center;gap:8px;padding:10px 20px;border-radius:var(--r-sm);background:var(--accent);border:none;color:#1a1a1a;font-size:13px;font-weight:600;cursor:pointer;font-family:inherit}
.btn-primary:hover{background:#d97706}
.table{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);overflow:hidden}
.thead{display:flex;padding:12px 20px;align-items:center;background:var(--bg-srf);border-bottom:1px solid var(--brd)}
.th{font-size:11px;font-weight:600;color:var(--txt3);letter-spacing:.5px;text-transform:uppercase}
.th-name{width:320px}.th-leds{width:80px;text-align:center}.th-frames{width:90px;text-align:center}
.th-fps{width:70px;text-align:center}.th-size{width:90px;text-align:center}.th-actions{width:160px;text-align:right}
.trow{display:flex;padding:14px 20px;align-items:center;border-bottom:1px solid var(--brd);transition:background .15s}
.trow:last-child{border-bottom:none}
.trow:hover{background:rgba(251,191,36,.03)}
.td-name{width:320px;display:flex;align-items:center;gap:12px}
.file-ico{width:32px;height:32px;border-radius:var(--r-sm);background:var(--accent10);display:flex;align-items:center;justify-content:center;flex-shrink:0}
.file-ico svg{width:16px;height:16px;color:var(--accent)}
.fname{font-size:13px;font-weight:600}.fpath{font-size:11px;color:var(--txt4);margin-top:2px}
.td-leds{width:80px;text-align:center}
.led-badge{display:inline-block;padding:3px 8px;border-radius:4px;background:var(--accent10);font-size:12px;font-weight:600;color:var(--accent)}
.td-val{font-size:13px;color:var(--txt2);text-align:center}
.td-frames{width:90px}.td-fps{width:70px}.td-size{width:90px}
.td-actions{width:160px;display:flex;gap:6px;justify-content:flex-end}
.btn-preset{padding:6px 12px;border-radius:var(--r-sm);background:var(--accent10);border:1px solid var(--accent-s);color:var(--accent);font-size:11px;font-weight:600;cursor:pointer;font-family:inherit}
.btn-preset:hover{background:var(--accent15)}
.btn-del{padding:6px 10px;border-radius:var(--r-sm);background:none;border:none;color:var(--txt4);cursor:pointer;display:flex;align-items:center}
.btn-del:hover{color:var(--err)}
.btn-del svg{width:14px;height:14px}
.drop-zone{border:2px dashed var(--brd);border-radius:var(--r-lg);background:var(--bg-el);padding:48px 0;display:flex;flex-direction:column;align-items:center;gap:12px;margin-top:24px;cursor:pointer;transition:border-color .2s,background .2s}
.drop-zone.drag-over{border-color:var(--accent);background:var(--accent10)}
.drop-zone.uploading{pointer-events:none;opacity:.7}
.dz-icon{width:56px;height:56px;border-radius:var(--r-full);background:var(--accent15);display:flex;align-items:center;justify-content:center}
.dz-icon svg{width:24px;height:24px;color:var(--accent)}
.dz-title{font-size:16px;font-weight:600}
.dz-sub{font-size:13px;color:var(--txt3)}
.dz-limit{font-size:11px;color:var(--txt4)}
.upload-progress{width:60%;height:6px;background:var(--bg-srf);border-radius:3px;margin-top:8px;display:none}
.upload-progress .bar{height:100%;background:var(--accent);border-radius:3px;width:0%;transition:width .2s}
#upload-status{font-size:12px;color:var(--txt3);margin-top:4px;min-height:16px}
.footer{display:flex;justify-content:space-between;align-items:center;padding:20px 40px;background:#202020;border-top:1px solid var(--brd)}
.footer-text{font-size:12px;color:var(--txt3)}
.footer a{color:var(--txt3);text-decoration:none;margin-left:20px}
.footer a:hover{color:var(--txt)}
.empty{padding:48px;text-align:center;color:var(--txt3);font-size:14px}
#file-input{display:none}
@media(max-width:900px){.cards{grid-template-columns:repeat(2,1fr)}.main{padding:24px 20px}
.th-frames,.td-frames,.th-fps,.td-fps{display:none}.td-name,.th-name{width:200px}
.header{padding:12px 20px}.nav{display:none}}
@media(max-width:500px){.cards{grid-template-columns:1fr}.th-size,.td-size,.th-leds,.td-leds{display:none}
.td-name,.th-name{flex:1;width:auto}.action-bar{flex-direction:column;gap:12px;align-items:stretch}
.search{width:100%}}
</style>
</head><body>
<div class="header">
  <div class="logo"><svg xmlns="http://www.w3.org/2000/svg" width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5"/><path d="M9 18h6"/><path d="M10 22h4"/></svg>PixDrive</div>
  <div style="display:flex;gap:20px;align-items:center"><a href="https://pixdrive.studio/dokumentation/" target="_blank" style="color:var(--txt3);text-decoration:none;font-size:13px">Dokumentation</a><a href="/" style="color:var(--txt2);text-decoration:none;font-size:13px">&larr; Zur&uuml;ck zu WLED</a></div>
</div>

<div class="main">
  <div class="title-bar">
    <div class="title-left">
      <h1>SD Manager</h1>
      <p>Verwalte deine .wled Animationsdateien</p>
    </div>
    <div id="sd-badge" class="badge" style="display:none"><span class="badge-dot"></span>SD Karte verbunden</div>
  </div>

  <div class="cards" id="stat-cards">
    <div class="card"><div class="card-top"><span class="card-label">Belegt</span><svg class="card-icon" style="color:var(--accent)" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 12H2"/><path d="M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/></svg></div><div class="card-val" id="used-val">--</div><div class="bar-outer"><div class="bar-inner" id="bar-fill" style="width:0%"></div></div></div>
    <div class="card"><div class="card-top"><span class="card-label">Verf&uuml;gbar</span><svg class="card-icon" style="color:var(--ok)" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M3 5v14a9 3 0 0 0 18 0V5"/><path d="M3 12a9 3 0 0 0 18 0"/></svg></div><div class="card-val" id="free-val">--</div><div class="card-sub" id="free-pct">--</div></div>
    <div class="card"><div class="card-top"><span class="card-label">Gesamt</span><svg class="card-icon" style="color:var(--info)" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="20" height="8" x="2" y="2" rx="2" ry="2"/><rect width="20" height="8" x="2" y="14" rx="2" ry="2"/><line x1="6" x2="6.01" y1="6" y2="6"/><line x1="6" x2="6.01" y1="18" y2="18"/></svg></div><div class="card-val" id="total-val">--</div><div class="card-sub" id="total-info">SD-Karte</div></div>
    <div class="card"><div class="card-top"><span class="card-label">Dateien</span><svg class="card-icon" style="color:var(--warn)" xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83Z"/><path d="m22 17.65-9.17 4.16a2 2 0 0 1-1.66 0L2 17.65"/><path d="m22 12.65-9.17 4.16a2 2 0 0 1-1.66 0L2 12.65"/></svg></div><div class="card-val" id="file-count">--</div><div class="card-sub">.wled Animationen</div></div>
  </div>

  <div class="action-bar">
    <div class="action-left">
      <div class="search"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/></svg><input type="text" placeholder="Dateien durchsuchen…" id="search-input" oninput="filterFiles()"></div>
    </div>
    <div><button class="btn-primary" onclick="document.getElementById('file-input').click()"><svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12h14"/><path d="M12 5v14"/></svg>Datei hochladen</button></div>
  </div>

  <div class="table" id="file-table">
    <div class="thead">
      <div class="th th-name">Dateiname</div>
      <div class="th th-leds">LEDs</div>
      <div class="th th-frames">Frames</div>
      <div class="th th-fps">FPS</div>
      <div class="th th-size">Gr&ouml;&szlig;e</div>
      <div class="th th-actions"></div>
    </div>
    <div id="file-list"><div class="empty">Lade Dateien…</div></div>
  </div>

  <div class="drop-zone" id="drop-zone" onclick="document.getElementById('file-input').click()">
    <div class="dz-icon"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="m18 9-6-6-6 6"/><path d="M12 3v14"/><path d="M5 21h14"/></svg></div>
    <div class="dz-title">Datei hochladen</div>
    <div class="dz-sub">Drag &amp; Drop oder klicken zum Ausw&auml;hlen &middot; .wled Format</div>
    <div class="dz-limit">Dateigr&ouml;&szlig;e nur durch SD-Karte begrenzt</div>
    <div class="upload-progress" id="progress"><div class="bar" id="progress-bar"></div></div>
    <div id="upload-status"></div>
  </div>
  <input type="file" id="file-input" accept=".wled" onchange="uploadFile(this.files[0])">
</div>

<div class="footer">
  <span class="footer-text">&copy; 2026 PixDrive</span>
  <div><a href="/">WLED</a><a href="/pixdrive">SD Manager</a></div>
</div>

<script>
var allFiles=[];
function fmtSize(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}
function fmtNum(n){return n.toLocaleString('de-DE');}

function loadFiles(){
  fetch('/pixdrive/list').then(r=>r.json()).then(d=>{
    allFiles=d.files||[];
    var used=d.used||0,total=d.total||1,free=total-used,pct=((used/total)*100);
    document.getElementById('used-val').textContent=fmtSize(used);
    document.getElementById('free-val').textContent=fmtSize(free);
    document.getElementById('total-val').textContent=fmtSize(total);
    document.getElementById('free-pct').textContent=(100-pct).toFixed(1)+'% frei';
    document.getElementById('bar-fill').style.width=pct.toFixed(1)+'%';
    document.getElementById('file-count').textContent=allFiles.length;
    document.getElementById('sd-badge').style.display=total>0?'inline-flex':'none';
    renderFiles(allFiles);
  }).catch(()=>{document.getElementById('file-list').innerHTML='<div class="empty">Fehler beim Laden</div>';});
}

function renderFiles(files){
  var el=document.getElementById('file-list');
  if(!files.length){el.innerHTML='<div class="empty">Keine Dateien auf der SD-Karte</div>';return;}
  el.innerHTML=files.map(f=>{
    var short=f.name.split('/').pop();
    return '<div class="trow" data-name="'+f.name.toLowerCase()+'">'+
      '<div class="td-name"><div class="file-ico"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect width="18" height="18" x="3" y="3" rx="2"/><path d="m10 8 4 4-4 4"/></svg></div><div><div class="fname">'+short+'</div><div class="fpath">'+f.name+'</div></div></div>'+
      '<div class="td-leds"><span class="led-badge">'+(f.leds||'?')+'</span>'+(f.rgbw?'<span style="display:block;font-size:9px;color:var(--txt4);margin-top:2px">RGBW</span>':'')+'</div>'+
      '<div class="td-val td-frames">'+(f.frames?fmtNum(f.frames):'?')+'</div>'+
      '<div class="td-val td-fps">'+(f.fps||'?')+'</div>'+
      '<div class="td-val td-size">'+fmtSize(f.size)+'</div>'+
      '<div class="td-actions">'+
        '<button class="btn-preset" onclick="makePreset(\''+f.name+'\')">Preset</button>'+
        '<button class="btn-del" onclick="delFile(\''+f.name+'\')" title="L&ouml;schen"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/></svg></button>'+
      '</div></div>';
  }).join('');
}

function filterFiles(){
  var q=document.getElementById('search-input').value.toLowerCase();
  renderFiles(q?allFiles.filter(f=>f.name.toLowerCase().includes(q)):allFiles);
}

function uploadFile(file){
  if(!file)return;
  var dz=document.getElementById('drop-zone');
  var pb=document.getElementById('progress');
  var bar=document.getElementById('progress-bar');
  var st=document.getElementById('upload-status');
  dz.classList.add('uploading');
  pb.style.display='block';bar.style.width='0%';st.textContent='Uploading…';
  var xhr=new XMLHttpRequest();
  xhr.upload.onprogress=function(e){if(e.lengthComputable)bar.style.width=(e.loaded/e.total*100)+'%';};
  xhr.onload=function(){
    st.textContent=xhr.status===200?'Upload erfolgreich!':xhr.status===422?'CRC Fehler — Datei gelöscht':'Fehler: '+xhr.statusText;
    dz.classList.remove('uploading');
    setTimeout(function(){pb.style.display='none';st.textContent='';},3000);
    loadFiles();
  };
  xhr.onerror=function(){st.textContent='Upload fehlgeschlagen';dz.classList.remove('uploading');};
  var fd=new FormData();fd.append('file',file,file.name);
  xhr.open('POST','/pixdrive/upload');xhr.send(fd);
  document.getElementById('file-input').value='';
}

function delFile(name){
  if(!confirm('Datei löschen: '+name.split('/').pop()+'?'))return;
  fetch('/pixdrive/delete?file='+encodeURIComponent(name),{method:'POST'}).then(()=>loadFiles());
}

function makePreset(name){
  var id=prompt('Preset ID (1-250):','1');
  if(!id)return;
  fetch('/json',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({psave:parseInt(id),n:'PixDrive: '+name.split('/').pop(),on:true,bri:128,
      seg:[{fx:window._pxFxId||0,n:name,sx:128,ix:255,c1b:true}]})
  }).then(r=>{alert(r.ok?'Preset '+id+' gespeichert!':'Fehler beim Speichern');});
}

// Drag & Drop
var dz=document.getElementById('drop-zone');
dz.addEventListener('dragover',function(e){e.preventDefault();dz.classList.add('drag-over');});
dz.addEventListener('dragleave',function(){dz.classList.remove('drag-over');});
dz.addEventListener('drop',function(e){e.preventDefault();dz.classList.remove('drag-over');if(e.dataTransfer.files.length)uploadFile(e.dataTransfer.files[0]);});

// Get PixDrive effect ID
fetch('/json/effects').then(r=>r.json()).then(fx=>{
  for(var i=0;i<fx.length;i++){if(fx[i].indexOf('PixDrive')===0){window._pxFxId=i;break;}}
});
loadFiles();
</script>
</body></html>
)html";

// ---- Usermod class ----
class UsermodPixDrive : public Usermod {
  private:
    static const char _name[];
    uint8_t fxId = 255;
    bool endpointsRegistered = false;

    // Upload state
    File uploadFile;
    String uploadPath;

  public:

    void setup() override {
      // Register the PixDrive effect
      fxId = strip.addEffect(255, &mode_pixdrive, _data_FX_MODE_PIXDRIVE);

      // Ensure pixdrive directory exists on SD
      if (!SD_ADAPTER.exists(PIXDRIVE_DIR)) {
        SD_ADAPTER.mkdir(PIXDRIVE_DIR);
      }
    }

    void loop() override {
      // Nothing to do in main loop
    }

    void connected() override {
      if (endpointsRegistered) return;
      endpointsRegistered = true;

      // --- List files (register before UI to avoid prefix match) ---
      server.on(F("/pixdrive/list"), HTTP_GET, [](AsyncWebServerRequest *request) {
        File root = SD_ADAPTER.open(PIXDRIVE_DIR);
        if (!root || !root.isDirectory()) {
          request->send(200, F("application/json"), F("{\"files\":[],\"used\":0,\"total\":0}"));
          return;
        }

        String json = F("{\"files\":[");
        bool first = true;
        File entry = root.openNextFile();
        while (entry) {
          if (!entry.isDirectory()) {
            // entry.name() returns full path on ESP32, just filename on ESP8266
            String ename = entry.name();
            String fname = ename.startsWith("/") ? ename : String(PIXDRIVE_DIR) + "/" + ename;
            // Read header for metadata
            WledFileHeader hdr;
            bool validHeader = false;
            if (entry.size() >= WLED_FILE_HEADER_SIZE) {
              entry.seek(0);
              if (entry.read((uint8_t*)&hdr, WLED_FILE_HEADER_SIZE) == WLED_FILE_HEADER_SIZE) {
                if (memcmp(hdr.magic, WLED_FILE_MAGIC, 4) == 0) {
                  validHeader = true;
                }
              }
            }
            if (!first) json += ",";
            first = false;
            json += F("{\"name\":\"");
            json += fname;
            json += F("\",\"size\":");
            json += String(entry.size());
            if (validHeader) {
              json += F(",\"leds\":");
              json += String(hdr.ledCount);
              json += F(",\"frames\":");
              json += String(hdr.frameCount);
              json += F(",\"fps\":");
              json += String(hdr.fps);
              json += F(",\"rgbw\":");
              json += (hdr.flags & WLED_FLAG_RGBW) ? F("true") : F("false");
            }
            json += "}";
          }
          entry = root.openNextFile();
        }
        root.close();

        json += F("],\"used\":");
        char buf64[21];
        snprintf(buf64, sizeof(buf64), "%llu", (unsigned long long)SD_ADAPTER.usedBytes());
        json += buf64;
        json += F(",\"total\":");
        snprintf(buf64, sizeof(buf64), "%llu", (unsigned long long)SD_ADAPTER.totalBytes());
        json += buf64;
        json += "}";

        request->send(200, F("application/json"), json);
      });

      // --- Upload file ---
      server.on(F("/pixdrive/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
          if (uploadPath.length() == 0) {
            request->send(500, F("text/plain"), F("Upload failed"));
            return;
          }
          if (!pixdriveValidateCRC(uploadPath.c_str())) {
            SD_ADAPTER.remove(uploadPath.c_str());
            DEBUG_PRINTF("[PixDrive] CRC mismatch, deleted %s\n", uploadPath.c_str());
            request->send(422, F("text/plain"), F("CRC validation failed — file deleted"));
            uploadPath = "";
            return;
          }
          request->send(200, F("text/plain"), F("Upload complete"));
          uploadPath = "";
        },
        [this](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
          if (!index) {
            uploadPath = String(PIXDRIVE_DIR) + "/" + filename;
            if (!SD_ADAPTER.exists(PIXDRIVE_DIR)) {
              SD_ADAPTER.mkdir(PIXDRIVE_DIR);
            }
            uploadFile = SD_ADAPTER.open(uploadPath.c_str(), FILE_WRITE);
            if (!uploadFile) {
              DEBUG_PRINTF("[PixDrive] Failed to open %s for writing\n", uploadPath.c_str());
              uploadPath = "";
              return;
            }
          }
          if (len && uploadFile) {
            uploadFile.write(data, len);
          }
          if (final && uploadFile) {
            uploadFile.close();
            DEBUG_PRINTF("[PixDrive] Upload complete: %s (%u bytes)\n", filename.c_str(), index + len);
          }
        }
      );

      // --- Delete file ---
      server.on(F("/pixdrive/delete"), HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam(F("file"))) {
          request->send(400, F("text/plain"), F("Missing file parameter"));
          return;
        }
        String path = request->getParam(F("file"))->value();
        // Security: must be within pixdrive directory
        if (!path.startsWith(PIXDRIVE_DIR)) {
          request->send(403, F("text/plain"), F("Access denied"));
          return;
        }
        if (SD_ADAPTER.exists(path.c_str())) {
          SD_ADAPTER.remove(path.c_str());
          request->send(200, F("text/plain"), F("Deleted"));
        } else {
          request->send(404, F("text/plain"), F("File not found"));
        }
      });

      // --- Serve Web UI (last, to avoid prefix-matching /pixdrive/list etc.) ---
      server.on(F("/pixdrive"), HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, "text/html", PIXDRIVE_HTML);
      });
    }

    void addToJsonInfo(JsonObject& root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray sd = user.createNestedArray(F("PixDrive SD"));
      bool cardOk = SD_ADAPTER.cardType() != CARD_NONE;
      if (cardOk) {
        // Count files
        int count = 0;
        File dir = SD_ADAPTER.open(PIXDRIVE_DIR);
        if (dir && dir.isDirectory()) {
          File f = dir.openNextFile();
          while (f) { if (!f.isDirectory()) count++; f = dir.openNextFile(); }
          dir.close();
        }
        sd.add(count);
        sd.add(F(" files"));
      } else {
        sd.add(F("no card"));
      }
    }

    uint16_t getId() override {
      return USERMOD_ID_PIXDRIVE;
    }
};

const char UsermodPixDrive::_name[] PROGMEM = "PixDrive";

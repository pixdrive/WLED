/*
 * PixDrive usermod for WLED 16
 *
 * Plays pre-rendered .wled animation files from an SD card as a WLED effect.
 * The segment name holds the file path (e.g. "/pixdrive/show.wled").
 *
 * Requires the sd_card usermod (custom_usermods = sd_card pixdrive) and
 * either WLED_USE_SD_SPI or WLED_USE_SD_MMC. The sd_card usermod owns SD
 * initialisation and pin configuration; PixDrive only reads and writes files.
 */
#include "wled.h"
#include <atomic>

// The sd_card usermod defines SD_ADAPTER only inside its own translation unit,
// so we map it here the same way. Both refer to the same global SD object.
#if defined(WLED_USE_SD_MMC)
  #include "SD_MMC.h"
  #define SD_ADAPTER SD_MMC
#elif defined(WLED_USE_SD_SPI)
  #include "SD.h"
  #define SD_ADAPTER SD
#else
  // Still compile (e.g. in the upstream "all usermods" CI build), but nothing
  // initialises the card, so PixDrive will report "no card" at runtime.
  #warning "PixDrive: define WLED_USE_SD_SPI or WLED_USE_SD_MMC and add the sd_card usermod, otherwise no SD card is available."
  #include "SD.h"
  #define SD_ADAPTER SD
#endif

// ---- .wled binary file format (20-byte header) ----
#define WLED_FLAG_RGBW 0x01   // flags bit 0: RGBW (0 = RGB/3 bpp, 1 = RGBW/4 bpp)

struct __attribute__((packed)) WledFileHeader {
  char     magic[4];      // "WLED"
  uint8_t  fps;           // 1-255
  uint8_t  flags;         // bit 0: RGBW
  uint16_t ledCount;      // little-endian
  uint32_t frameCount;    // little-endian
  uint32_t crc32;         // CRC32 over all frame data (0 = not checked)
  uint8_t  reserved[4];   // must be 0
};
static_assert(sizeof(WledFileHeader) == 20, "WledFileHeader must be 20 bytes");

#define WLED_FILE_HEADER_SIZE 20
#define WLED_FILE_MAGIC       "WLED"
#define PIXDRIVE_DIR          "/pixdrive"
#define PIXDRIVE_UPLOAD_TMP   PIXDRIVE_DIR "/.upload.tmp"

// Maximum number of animation files open at the same time. The SD library
// mounts with max_files = 5; we leave room for the web handlers (list/upload).
// A segment in a transition briefly needs a second handle for the old copy.
#ifndef PIXDRIVE_MAX_OPEN
  #define PIXDRIVE_MAX_OPEN 3
#endif
#define PIXDRIVE_SLOT_IDLE_MS   500   // close handles not used for this long
#define PIXDRIVE_RETRY_MS      1000   // wait before retrying a failed open
#define PIXDRIVE_ERROR_COLOR   RGBW32(32, 0, 0, 0)

// ---- Per-segment playback state, stored at the start of SEGENV.data ----
// Living in SEGENV.data means it belongs to exactly one Segment object, so the
// copy WLED makes for transitions gets its own independent state.
enum : uint8_t { PD_INIT = 0, PD_PLAYING = 1, PD_ERROR = 2 };

struct PixDriveState {
  char     file[WLED_MAX_SEGNAME_LEN + 1]; // path this state belongs to
  uint32_t frameCount;
  uint32_t frame;      // index of the frame currently held in the buffer
  uint32_t acc;        // sub-frame time accumulator (1 frame = 1000 units)
  uint32_t lastNow;    // strip.now at previous call
  uint32_t retryAt;    // millis() after which a failed open is retried
  uint16_t ledCount;
  uint8_t  bpp;
  uint8_t  fps;
  uint8_t  status;
};
#define PD_ACC_PER_FRAME 1000UL

// ---- Open file handles, owned by a Segment object (like WLED's image loader) ----
struct PixDriveSlot {
  const Segment *owner;
  File           file;
  uint32_t       lastUsed;  // millis()
  uint32_t       nextFrame; // frame index at the current file position (UINT32_MAX = unknown)
  char           path[WLED_MAX_SEGNAME_LEN + 1];
};
static PixDriveSlot pdSlots[PIXDRIVE_MAX_OPEN];

// ---- Playback status per segment index, written by the effect, read by /pixdrive/status ----
// Plain values only: a torn read from the web task just shows a slightly old frame.
struct PixDriveStatus {
  uint32_t frame;
  uint32_t frames;    // 0 = no file loaded
  uint32_t updated;   // millis() of last effect call
  uint8_t  fps;
  uint8_t  status;    // PD_INIT / PD_PLAYING / PD_ERROR
};
static PixDriveStatus pdStatus[MAX_NUM_SEGMENTS];

// Seek requests per segment index from the web UI (0 = none, otherwise frame + 1)
static std::atomic<uint32_t> pdSeekReq[MAX_NUM_SEGMENTS];

static void pdCloseSlot(PixDriveSlot &s) {
  if (s.file) s.file.close();
  s.owner = nullptr;
  s.path[0] = '\0';
  s.nextFrame = UINT32_MAX;
}

static PixDriveSlot *pdFindSlot(const Segment *seg) {
  for (auto &s : pdSlots) if (s.owner == seg) return &s;
  return nullptr;
}

static void pdReleaseSegment(const Segment *seg) {
  PixDriveSlot *s = pdFindSlot(seg);
  if (s) pdCloseSlot(*s);
}

// Get a free slot. Only evicts handles that are idle, so two segments never
// steal each other's handle every frame.
static PixDriveSlot *pdAcquireSlot(const Segment *seg) {
  PixDriveSlot *s = pdFindSlot(seg);
  if (s) { pdCloseSlot(*s); s->owner = seg; return s; }
  uint32_t now = millis();
  PixDriveSlot *best = nullptr;
  for (auto &c : pdSlots) {
    if (!c.owner) { best = &c; break; }
    if (now - c.lastUsed > 100 && (!best || c.lastUsed < best->lastUsed)) best = &c;
  }
  if (!best) return nullptr;
  pdCloseSlot(*best);
  best->owner = seg;
  return best;
}

// ---- CRC32 (incremental) ----
static uint32_t pixdriveCRC32(const uint8_t *data, size_t len, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++)
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
  }
  return ~crc;
}

static bool pdReadHeader(File &f, WledFileHeader &hdr) {
  if (!f.seek(0)) return false;
  if (f.read((uint8_t*)&hdr, WLED_FILE_HEADER_SIZE) != WLED_FILE_HEADER_SIZE) return false;
  return memcmp(hdr.magic, WLED_FILE_MAGIC, 4) == 0 && hdr.fps && hdr.ledCount && hdr.frameCount;
}

// ---- Effect ----
// The effect also runs for the copy WLED keeps of the old segment during a
// transition. Only the real segment (the one in the strip's list) may publish
// status or consume seek requests.
static bool pdIsMainSegment() {
  const unsigned id = strip.getCurrSegmentId();
  return id < MAX_NUM_SEGMENTS && &strip.getSegment(id) == &SEGMENT;
}

static void pdShowError(PixDriveState *st) {
  const unsigned id = strip.getCurrSegmentId();
  if (pdIsMainSegment()) {
    pdStatus[id].frames  = 0;
    pdStatus[id].status  = PD_ERROR;
    pdStatus[id].updated = millis();
  }
  if (st) {
    st->status  = PD_ERROR;
    st->retryAt = millis() + PIXDRIVE_RETRY_MS;
  }
  pdReleaseSegment(&SEGMENT);
  SEGMENT.fill(PIXDRIVE_ERROR_COLOR);
}

// Open st->file for the current segment. If resume is set, the header must
// match the stored state and the file is positioned after st->frame;
// otherwise playback starts at frame 0. Returns the (possibly moved) state.
static PixDriveState *pdOpen(PixDriveState *st, bool resume) {
  Segment &seg = SEGMENT;
  if (st->file[0] != '/') { pdShowError(st); return st; }

  PixDriveSlot *slot = pdAcquireSlot(&seg);
  if (!slot) { pdShowError(st); return st; }
  slot->lastUsed = millis();
  strlcpy(slot->path, st->file, sizeof(slot->path));
  slot->file = SD_ADAPTER.open(st->file, FILE_READ);

  WledFileHeader hdr;
  if (!slot->file || !pdReadHeader(slot->file, hdr)) { pdShowError(st); return st; }

  const uint8_t  bpp       = (hdr.flags & WLED_FLAG_RGBW) ? 4 : 3;
  const size_t   frameSize = (size_t)hdr.ledCount * bpp;

  if (resume && (hdr.frameCount != st->frameCount || hdr.ledCount != st->ledCount || bpp != st->bpp)) {
    resume = false; // file was replaced: start over
  }

  if (!resume) {
    PixDriveState copy = *st;
    if (!seg.allocateData(sizeof(PixDriveState) + frameSize)) {
      // old buffer may already be freed: keep only the small state to remember the error
      st = seg.allocateData(sizeof(PixDriveState)) ? reinterpret_cast<PixDriveState*>(seg.data) : nullptr;
      if (st) *st = copy;
      pdShowError(st);
      return st;
    }
    st = reinterpret_cast<PixDriveState*>(seg.data);
    *st = copy;
    st->frameCount = hdr.frameCount;
    st->ledCount   = hdr.ledCount;
    st->bpp        = bpp;
    st->fps        = hdr.fps;
    st->frame      = 0;
    st->acc        = 0;
    st->lastNow    = strip.now;
    uint8_t *buf = seg.data + sizeof(PixDriveState);
    if (slot->file.read(buf, frameSize) != frameSize) { pdShowError(st); return st; }
    slot->nextFrame = 1;
  }
  // when resuming, the buffer still holds st->frame; the next read seeks as needed.
  // Restart the clock so a pause (frozen segment) does not jump ahead.
  st->lastNow = strip.now;
  st->status = PD_PLAYING;
  return st;
}

static void mode_pixdrive(void) {
  Segment &seg = SEGMENT;
  const char *name = seg.name ? seg.name : "";

  PixDriveState *st = (seg.data && seg.dataSize() >= sizeof(PixDriveState))
                      ? reinterpret_cast<PixDriveState*>(seg.data) : nullptr;

  // (Re)initialise on effect start, after a segment reset or when the file changed
  if (SEGENV.call == 0 || !st || strncmp(st->file, name, WLED_MAX_SEGNAME_LEN) != 0) {
    pdReleaseSegment(&seg);
    if (!seg.allocateData(sizeof(PixDriveState))) { pdShowError(nullptr); return; }
    st = reinterpret_cast<PixDriveState*>(seg.data);
    memset(st, 0, sizeof(PixDriveState));
    strlcpy(st->file, name, sizeof(st->file));
    // a new file always starts at frame 0; drop seek requests meant for the previous one
    if (pdIsMainSegment()) pdSeekReq[strip.getCurrSegmentId()].store(0);
  }

  if (st->status == PD_ERROR) {
    if ((int32_t)(millis() - st->retryAt) < 0) { seg.fill(PIXDRIVE_ERROR_COLOR); return; }
    st->status = PD_INIT;
  }

  if (st->status == PD_INIT) {
    st = pdOpen(st, false);
    if (!st || st->status != PD_PLAYING) return;
  }

  // No handle for this Segment object: we are the copy WLED made for a
  // transition, the segment vector was reallocated, or the handle was closed
  // (file replaced/deleted). Reopen and continue where we were.
  PixDriveSlot *slot = pdFindSlot(&seg);
  if (slot && strcmp(slot->path, st->file) != 0) { pdCloseSlot(*slot); slot = nullptr; } // stale owner pointer
  if (!slot) {
    st = pdOpen(st, true);
    if (!st || st->status != PD_PLAYING) return;
    slot = pdFindSlot(&seg);
    if (!slot) { pdShowError(st); return; }
  }
  slot->lastUsed = millis();

  const size_t frameSize = (size_t)st->ledCount * st->bpp;
  uint8_t *buf = seg.data + sizeof(PixDriveState);

  const unsigned segId = strip.getCurrSegmentId();
  const bool isMain = pdIsMainSegment();

  // ---- Seek / restart requested from the web UI ----
  uint32_t seekReq = isMain ? pdSeekReq[segId].exchange(0) : 0;
  if (seekReq) {
    uint32_t target = min(seekReq - 1, st->frameCount - 1);
    if (target != slot->nextFrame && !slot->file.seek(WLED_FILE_HEADER_SIZE + target * frameSize)) { pdShowError(st); return; }
    if (slot->file.read(buf, frameSize) != frameSize) { pdShowError(st); return; }
    st->frame       = target;
    slot->nextFrame = target + 1;
    st->acc         = 0;
    st->lastNow     = strip.now;
  }

  // ---- Time-based frame advance, always at the file's FPS ----
  uint32_t dt = strip.now - st->lastNow;
  st->lastNow = strip.now;
  if (dt > 500) dt = 0;                  // resumed after pause/off or a long stall: continue, don't jump
  st->acc += dt * st->fps;               // ms * frames/s = 1/1000 frames
  uint32_t advance = seekReq ? 0 : st->acc / PD_ACC_PER_FRAME;
  st->acc %= PD_ACC_PER_FRAME;

  if (advance) {
    const bool once = seg.check1;
    uint32_t target = st->frame + advance;
    if (target >= st->frameCount) target = once ? st->frameCount - 1 : target % st->frameCount;

    if (target != st->frame) {
      if (target != slot->nextFrame) { // skipped frames, wrapped around or position unknown
        if (!slot->file.seek(WLED_FILE_HEADER_SIZE + target * frameSize)) { pdShowError(st); return; }
      }
      if (slot->file.read(buf, frameSize) != frameSize) { pdShowError(st); return; }
      st->frame = target;
      slot->nextFrame = target + 1;
    }
  }

  if (isMain) {
    PixDriveStatus &ps = pdStatus[segId];
    ps.frame   = st->frame;
    ps.frames  = st->frameCount;
    ps.fps     = st->fps;
    ps.status  = PD_PLAYING;
    ps.updated = millis();
  }

  // ---- Render buffer to segment (nearest-neighbour scaling) ----
  const unsigned fileLeds = st->ledCount;
  const unsigned segLeds  = SEGLEN;
  const unsigned bright   = seg.intensity;
  const uint8_t  bpp      = st->bpp;
  for (unsigned i = 0; i < segLeds; i++) {
    unsigned src = (fileLeds == segLeds) ? i : i * fileLeds / segLeds;
    if (src >= fileLeds) src = fileLeds - 1;
    const uint8_t *p = buf + src * bpp;
    uint8_t r = p[0], g = p[1], b = p[2], w = (bpp == 4) ? p[3] : 0;
    if (bright < 255) {
      r = (r * bright) >> 8; g = (g * bright) >> 8;
      b = (b * bright) >> 8; w = (w * bright) >> 8;
    }
    seg.setPixelColor(i, RGBW32(r, g, b, w));
  }
}

static const char _data_FX_MODE_PIXDRIVE[] PROGMEM = "PixDrive@,Brightness,,,,Once;;;1;ix=255,o1=0";

// ---- Web UI ----
static const char PIXDRIVE_HTML[] PROGMEM = R"html(
<!DOCTYPE html>
<html lang="de"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PixDrive</title>
<style>
:root{--bg:#1e1e1e;--bg-el:#262626;--bg-srf:#303030;--bg-inp:#2a2a2a;--brd:#333;--brd2:#444;--accent:#fbbf24;--accent-h:#d97706;--accent10:rgba(251,191,36,.1);--accent15:rgba(251,191,36,.15);--accent-s:rgba(251,191,36,.25);--txt:#ececec;--txt2:#bbb;--txt3:#999;--txt4:#666;--ok:#22c55e;--ok15:rgba(34,197,94,.15);--err:#f87171;--err15:rgba(248,113,113,.15);--info:#3b82f6;--warn:#f97316;--r-sm:6px;--r-md:8px;--r-lg:12px;--r-full:100px}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Inter,-apple-system,system-ui,sans-serif;background:var(--bg);color:var(--txt);min-height:100vh;display:flex;flex-direction:column}
button{font-family:inherit}
svg.i{width:16px;height:16px;flex-shrink:0}
.header{display:flex;align-items:center;justify-content:space-between;padding:16px 40px;background:#222;border-bottom:1px solid var(--brd);gap:16px}
.logo{display:flex;align-items:center;gap:8px;font-weight:700;font-size:18px}
.logo svg{color:var(--accent);width:22px;height:22px}
.hlinks{display:flex;gap:20px;align-items:center;font-size:13px}
.hlinks a{color:var(--txt3);text-decoration:none}.hlinks a:hover{color:var(--txt)}
.main{flex:1;max-width:1312px;width:100%;margin:0 auto;padding:32px 64px 48px}
h2{font-size:20px;font-weight:700;letter-spacing:-.3px}
.sec{display:flex;justify-content:space-between;align-items:flex-end;margin:40px 0 16px;gap:12px}
.sec p{color:var(--txt3);font-size:13px;margin-top:2px}
.badge{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:var(--r-full);font-size:12px;font-weight:500}
.b-ok{background:var(--ok15);color:var(--ok)}.b-acc{background:var(--accent15);color:var(--accent)}.b-off{background:var(--bg-srf);color:var(--txt3)}.b-err{background:var(--err15);color:var(--err)}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor}
/* player */
.player{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);padding:24px;display:grid;grid-template-columns:1fr 280px;gap:32px}
.np-top{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:4px}
.np-label{font-size:12px;color:var(--txt3);font-weight:500;text-transform:uppercase;letter-spacing:.5px}
.np-title{font-size:24px;font-weight:700;letter-spacing:-.5px;margin-top:6px;overflow-wrap:anywhere}
.np-sub{font-size:13px;color:var(--txt3);margin-top:4px;min-height:18px;overflow-wrap:anywhere}
.tracks{display:flex;flex-direction:column;gap:12px;margin-top:18px}
.track-h{display:flex;justify-content:space-between;font-size:12px;color:var(--txt3);margin-bottom:6px;gap:8px}
.track-h span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.bar{height:8px;background:var(--bg-srf);border-radius:4px;cursor:pointer;position:relative}
.bar:hover{height:10px;margin:-1px 0}
.bar-fill{height:100%;background:var(--accent);border-radius:4px;width:0%}
.bar.err .bar-fill{background:var(--err)}
.ctrls{display:flex;align-items:center;gap:10px;margin-top:20px;flex-wrap:wrap}
.cbtn{width:44px;height:44px;border-radius:50%;border:1px solid var(--brd2);background:var(--bg-srf);color:var(--txt);display:flex;align-items:center;justify-content:center;cursor:pointer}
.cbtn:hover{border-color:var(--accent);color:var(--accent)}
.cbtn.big{width:56px;height:56px;background:var(--accent);border:none;color:#1a1a1a}
.cbtn.big:hover{background:var(--accent-h);color:#1a1a1a}
.cbtn.big svg.i{width:22px;height:22px}
.cbtn:disabled{opacity:.35;cursor:default;border-color:var(--brd2);color:var(--txt)}
.side{display:flex;flex-direction:column;gap:20px;border-left:1px solid var(--brd);padding-left:32px}
.pw{display:flex;align-items:center;justify-content:space-between}
.pw-btn{display:flex;align-items:center;gap:8px;padding:10px 16px;border-radius:var(--r-full);border:1px solid var(--brd2);background:var(--bg-srf);color:var(--txt3);font-size:13px;font-weight:600;cursor:pointer}
.pw-btn.on{background:var(--ok15);border-color:transparent;color:var(--ok)}
.bri-h{display:flex;justify-content:space-between;align-items:center;font-size:13px;color:var(--txt3);margin-bottom:10px}
.bri-h span{display:flex;align-items:center;gap:6px}
.bri-h b{color:var(--txt);font-weight:600}
input[type=range]{width:100%;-webkit-appearance:none;appearance:none;height:8px;border-radius:4px;background:linear-gradient(to right,var(--accent) var(--p,50%),var(--bg-srf) var(--p,50%));outline:none}
input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#fff;border:3px solid var(--accent);cursor:pointer}
input[type=range]::-moz-range-thumb{width:16px;height:16px;border-radius:50%;background:#fff;border:3px solid var(--accent);cursor:pointer}
.hint{font-size:12px;color:var(--txt4);line-height:1.5}
/* cards */
.cards{display:grid;grid-template-columns:repeat(4,1fr);gap:16px;margin-top:24px}
.card{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);padding:16px 20px;display:flex;flex-direction:column;gap:10px}
.card-label{font-size:13px;color:var(--txt3);font-weight:500}
.card-val{font-size:22px;font-weight:700;letter-spacing:-.5px}
.card-sub{font-size:12px;color:var(--txt4);font-weight:500}
.bar-outer{height:6px;background:var(--bg-srf);border-radius:3px}
.bar-inner{height:100%;border-radius:3px;background:var(--accent)}
/* tables */
.search{display:flex;align-items:center;gap:10px;padding:9px 14px;border-radius:var(--r-sm);background:var(--bg-el);border:1px solid var(--brd);width:260px;color:var(--txt4)}
.search input{background:none;border:none;outline:none;color:var(--txt);font-size:13px;width:100%;font-family:inherit}
.btn-primary{display:inline-flex;align-items:center;gap:8px;padding:10px 18px;border-radius:var(--r-sm);background:var(--accent);border:none;color:#1a1a1a;font-size:13px;font-weight:600;cursor:pointer;white-space:nowrap}
.btn-primary:hover{background:var(--accent-h)}
.btn-sec{display:inline-flex;align-items:center;gap:6px;padding:9px 14px;border-radius:var(--r-sm);background:none;border:1px solid var(--brd2);color:var(--txt2);font-size:13px;cursor:pointer;white-space:nowrap}
.btn-sec:hover{border-color:var(--txt3);color:var(--txt)}
.tools{display:flex;gap:10px;align-items:center}
.table{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);overflow:hidden}
.thead,.trow{display:flex;align-items:center;padding:12px 20px;gap:12px}
.thead{background:var(--bg-srf);border-bottom:1px solid var(--brd);font-size:11px;font-weight:600;color:var(--txt3);letter-spacing:.5px;text-transform:uppercase}
.trow{border-bottom:1px solid var(--brd);font-size:13px}
.trow:last-child{border-bottom:none}
.trow:hover{background:rgba(251,191,36,.03)}
.trow.active{background:var(--accent10)}
.c-grow{flex:1;min-width:0}.c-num{width:70px;text-align:center;color:var(--txt2)}.c-id{width:44px;color:var(--txt3);font-weight:600}
.c-file{width:240px;color:var(--txt3);font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.c-act{display:flex;gap:6px;justify-content:flex-end;align-items:center;width:220px}
.nm{display:flex;align-items:center;gap:12px;min-width:0}
.nm>div{min-width:0}
.ico{width:32px;height:32px;border-radius:var(--r-sm);background:var(--accent10);display:flex;align-items:center;justify-content:center;flex-shrink:0;color:var(--accent)}
.fname{font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.fsub{font-size:11px;color:var(--txt4);margin-top:2px}
.tag{display:inline-block;padding:2px 8px;border-radius:var(--r-full);font-size:10px;font-weight:600;white-space:nowrap}
.t-ok{background:var(--ok15);color:var(--ok)}.t-off{background:var(--bg-srf);color:var(--txt3)}.t-acc{background:var(--accent10);color:var(--accent)}
.abtn{padding:6px 12px;border-radius:var(--r-sm);background:var(--accent10);border:1px solid var(--accent-s);color:var(--accent);font-size:11px;font-weight:600;cursor:pointer;display:inline-flex;align-items:center;gap:5px;white-space:nowrap}
.abtn:hover{background:var(--accent15)}
.abtn.solid{background:var(--accent);border-color:var(--accent);color:#1a1a1a}
.abtn.solid:hover{background:var(--accent-h)}
.abtn svg.i{width:12px;height:12px}
.ibtn{padding:6px;border-radius:var(--r-sm);background:none;border:none;color:var(--txt4);cursor:pointer;display:flex;align-items:center}
.ibtn:hover{color:var(--txt)}.ibtn.del:hover{color:var(--err)}
.ibtn:disabled{opacity:.3;cursor:default}
.ibtn.boot.on{color:var(--accent)}.ibtn.boot.on svg{fill:currentColor}
.empty{padding:40px 20px;text-align:center;color:var(--txt3);font-size:14px}
/* upload */
.drop-zone{border:2px dashed var(--brd);border-radius:var(--r-lg);background:var(--bg-el);padding:32px 16px;display:flex;flex-direction:column;align-items:center;gap:8px;margin-top:16px;cursor:pointer;text-align:center}
.drop-zone.drag-over{border-color:var(--accent);background:var(--accent10)}
.drop-zone.uploading{pointer-events:none;opacity:.7}
.dz-title{font-size:15px;font-weight:600}.dz-sub{font-size:13px;color:var(--txt3)}
.upload-progress{width:60%;height:6px;background:var(--bg-srf);border-radius:3px;margin-top:6px;display:none}
.upload-progress .bar-inner{width:0%}
#upload-status{font-size:12px;color:var(--txt3);min-height:16px}
#file-input{display:none}
/* modal */
.modal-bg{position:fixed;inset:0;background:rgba(0,0,0,.65);display:none;align-items:flex-start;justify-content:center;padding:40px 16px;z-index:10;overflow-y:auto}
.modal-bg.open{display:flex}
.modal{background:var(--bg-el);border:1px solid var(--brd);border-radius:var(--r-lg);padding:24px;width:100%;max-width:440px;display:flex;flex-direction:column;gap:16px}
.modal.wide{max-width:640px}
.modal h3{font-size:18px;font-weight:700}
.modal .sub{font-size:12px;color:var(--txt3);margin-top:-10px;word-break:break-all}
.field{display:flex;flex-direction:column;gap:6px;min-width:0}
.field label,.flabel{font-size:12px;color:var(--txt3);font-weight:500}
.field input,.field select,.pl-row select,.pl-row input{background:var(--bg-inp);border:1px solid var(--brd2);border-radius:var(--r-sm);color:var(--txt);padding:9px 11px;font-size:14px;font-family:inherit;outline:none;width:100%}
.field input:focus,.field select:focus,.pl-row select:focus,.pl-row input:focus{border-color:var(--accent)}
.field select,.pl-row select{-webkit-appearance:none;appearance:none;padding-right:36px;cursor:pointer;background-image:url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%23999' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'%3E%3Cpath d='m6 9 6 6 6-6'/%3E%3C/svg%3E");background-repeat:no-repeat;background-position:right 12px center;background-size:16px}
.row2{display:flex;gap:12px}.row2>.field{flex:1}
.check{display:flex;align-items:center;gap:8px;font-size:13px;color:var(--txt2);cursor:pointer}
.check input{accent-color:var(--accent);width:16px;height:16px}
.m-err{font-size:12px;color:var(--err);min-height:14px}
.m-act{display:flex;justify-content:flex-end;gap:8px}
.pl-list{display:flex;flex-direction:column;gap:8px}
.pl-row{display:flex;gap:8px;align-items:center;background:var(--bg-srf);border-radius:var(--r-md);padding:8px}
.pl-n{width:22px;text-align:center;font-size:12px;color:var(--txt3);font-weight:600}
.pl-row select{flex:1;min-width:0}
.pl-dur{display:flex;align-items:center;gap:4px;width:118px;flex-shrink:0}
.pl-dur input{width:76px;padding:9px 6px;text-align:right}
.pl-dur span{font-size:12px;color:var(--txt3)}
.pl-tools{display:flex;flex-shrink:0}
.footer{display:flex;justify-content:space-between;align-items:center;padding:20px 40px;background:#202020;border-top:1px solid var(--brd);font-size:12px;color:var(--txt3)}
.footer a{color:var(--txt3);text-decoration:none;margin-left:20px}.footer a:hover{color:var(--txt)}
@media(max-width:900px){.main{padding:24px 16px 40px}.header{padding:12px 16px}.footer{padding:16px}
.player{grid-template-columns:1fr;gap:24px}.side{border-left:none;padding-left:0;border-top:1px solid var(--brd);padding-top:20px}
.cards{grid-template-columns:repeat(2,1fr)}.c-file,.h-file{display:none}.c-num.opt{display:none}}
@media(max-width:560px){.cards{grid-template-columns:1fr 1fr}.card{padding:14px}.card-val{font-size:18px}
.c-num{display:none}.c-act{width:auto}.abtn .lbl{display:none}.sec{flex-direction:column;align-items:stretch}.tools{flex-wrap:wrap}.search{width:100%;flex:1}
.pl-row{flex-wrap:wrap}.pl-row select{flex-basis:calc(100% - 30px)}.pl-dur{flex:1}.pl-tools{margin-left:auto}.hlinks a.doc{display:none}}
</style>
</head><body>
<div class="header">
  <div class="logo"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5"/><path d="M9 18h6"/><path d="M10 22h4"/></svg>PixDrive</div>
  <div class="hlinks"><a class="doc" href="https://pixdrive.studio/dokumentation/" target="_blank">Dokumentation</a><a href="/">&larr; WLED</a></div>
</div>

<div class="main">
  <!-- Player -->
  <div class="player">
    <div>
      <div class="np-top"><span class="np-label">Wiedergabe</span><span id="np-badge" class="badge b-off"><span class="dot"></span><span id="np-state">Verbinde…</span></span></div>
      <div class="np-title" id="np-title">–</div>
      <div class="np-sub" id="np-sub"></div>
      <div class="tracks" id="tracks"></div>
      <div class="ctrls">
        <button class="cbtn" id="b-restart" onclick="restart()" title="Von vorn"></button>
        <button class="cbtn big" id="b-play" onclick="playPause()" title="Abspielen"></button>
        <button class="cbtn" id="b-stop" onclick="stopAll()" title="Stopp"></button>
        <button class="cbtn" id="b-next" onclick="nextPl()" title="N&auml;chster Eintrag" style="display:none"></button>
      </div>
    </div>
    <div class="side">
      <div class="pw"><span class="np-label">Ger&auml;t</span><button class="pw-btn" id="b-power" onclick="power()"></button></div>
      <div>
        <div class="bri-h"><span id="bri-ico"></span><span>Helligkeit <b id="bri-val">–</b></span></div>
        <input type="range" id="bri" min="1" max="255" value="128" oninput="briInput(this)" onchange="briSend(true)">
      </div>
      <div class="hint">Pause h&auml;lt das aktuelle Bild. Stopp schaltet aus und setzt die Animation auf den Anfang zur&uuml;ck. Eine laufende Playlist macht w&auml;hrend Stopp ebenfalls Pause.</div>
    </div>
  </div>

  <div class="cards">
    <div class="card"><span class="card-label">Belegt</span><div class="card-val" id="used-val">–</div><div class="bar-outer"><div class="bar-inner" id="bar-fill" style="width:0%"></div></div></div>
    <div class="card"><span class="card-label">Verf&uuml;gbar</span><div class="card-val" id="free-val">–</div><div class="card-sub" id="free-pct">–</div></div>
    <div class="card"><span class="card-label">Gesamt</span><div class="card-val" id="total-val">–</div><div class="card-sub" id="sd-state">SD-Karte</div></div>
    <div class="card"><span class="card-label">Animationen</span><div class="card-val" id="file-count">–</div><div class="card-sub">.wled Dateien</div></div>
  </div>

  <!-- Files -->
  <div class="sec">
    <div><h2>Animationen</h2><p>Dateien auf der SD-Karte</p></div>
    <div class="tools">
      <div class="search"><span id="search-ico"></span><input type="text" placeholder="Suchen…" id="search-input" oninput="renderFiles()"></div>
      <button class="btn-primary" onclick="pickFile()"><span id="up-ico"></span>Hochladen</button>
    </div>
  </div>
  <div class="table">
    <div class="thead"><div class="c-grow">Datei</div><div class="c-num">LEDs</div><div class="c-num opt">Dauer</div><div class="c-num opt">FPS</div><div class="c-num opt">Gr&ouml;&szlig;e</div><div class="c-act"></div></div>
    <div id="file-list"><div class="empty">Lade Dateien…</div></div>
  </div>
  <div class="drop-zone" id="drop-zone" onclick="pickFile()">
    <div class="dz-title">Datei hierher ziehen</div>
    <div class="dz-sub">oder klicken zum Ausw&auml;hlen &middot; .wled Format</div>
    <div class="upload-progress" id="progress"><div class="bar-inner" id="progress-bar"></div></div>
    <div id="upload-status"></div>
  </div>
  <input type="file" id="file-input" accept=".wled" onchange="uploadFile(this.files[0])">

  <!-- Presets -->
  <div class="sec"><div><h2>Presets</h2><p>Gespeicherte Szenen &ndash; anlegen &uuml;ber &bdquo;Preset&ldquo; bei einer Animation</p></div></div>
  <div class="table">
    <div class="thead"><div class="c-id">ID</div><div class="c-grow">Name</div><div class="c-file h-file">Animation</div><div class="c-act"></div></div>
    <div id="preset-list"><div class="empty">Lade…</div></div>
  </div>

  <!-- Playlists -->
  <div class="sec">
    <div><h2>Playlists</h2><p>Mehrere Presets nacheinander abspielen</p></div>
    <div class="tools"><button class="btn-primary" onclick="openPl()"><span id="pl-add-ico"></span>Neue Playlist</button></div>
  </div>
  <div class="table">
    <div class="thead"><div class="c-id">ID</div><div class="c-grow">Name</div><div class="c-num">Eintr&auml;ge</div><div class="c-num opt">Dauer</div><div class="c-act"></div></div>
    <div id="pl-list"><div class="empty">Lade…</div></div>
  </div>
</div>

<!-- Preset modal -->
<div class="modal-bg" id="pm" onclick="if(event.target===this)closeM('pm')">
  <form class="modal" onsubmit="savePm();return false">
    <h3>Preset erstellen</h3>
    <div class="sub" id="pm-file"></div>
    <div class="field"><label for="pm-name">Name</label><input id="pm-name" maxlength="32" required></div>
    <div class="row2">
      <div class="field"><label for="pm-id">Preset-ID</label><input id="pm-id" type="number" min="1" max="250" required></div>
      <div class="field" id="pm-seg-f"><label for="pm-seg">Segment</label><select id="pm-seg"></select></div>
    </div>
    <label class="check"><input type="checkbox" id="pm-once">Nur einmal abspielen (nicht wiederholen)</label>
    <label class="check"><input type="checkbox" id="pm-boot">Beim Einschalten automatisch starten</label>
    <div class="m-err" id="pm-err"></div>
    <div class="m-act"><button type="button" class="btn-sec" onclick="closeM('pm')">Abbrechen</button><button type="submit" class="btn-primary">Speichern &amp; abspielen</button></div>
  </form>
</div>

<!-- Rename modal -->
<div class="modal-bg" id="rnm" onclick="if(event.target===this)closeM('rnm')">
  <form class="modal" onsubmit="saveRn();return false">
    <h3>Animation umbenennen</h3>
    <div class="sub" id="rn-old"></div>
    <div class="field"><label for="rn-name">Neuer Name</label>
      <div style="display:flex;align-items:center;gap:8px"><input id="rn-name" maxlength="49" required style="flex:1"><span class="flabel">.wled</span></div></div>
    <div class="hint" id="rn-info"></div>
    <div class="m-err" id="rn-err"></div>
    <div class="m-act"><button type="button" class="btn-sec" onclick="closeM('rnm')">Abbrechen</button><button type="submit" class="btn-primary">Umbenennen</button></div>
  </form>
</div>

<!-- Playlist modal -->
<div class="modal-bg" id="plm" onclick="if(event.target===this)closeM('plm')">
  <form class="modal wide" onsubmit="savePl();return false">
    <h3 id="plm-title">Neue Playlist</h3>
    <div class="row2">
      <div class="field" style="flex:3"><label for="pl-name">Name</label><input id="pl-name" maxlength="32" required></div>
      <div class="field" style="flex:1"><label for="pl-id">ID</label><input id="pl-id" type="number" min="1" max="250" required></div>
    </div>
    <div class="field"><span class="flabel">Reihenfolge &amp; Dauer pro Eintrag</span><div class="pl-list" id="pl-entries"></div></div>
    <div><button type="button" class="btn-sec" onclick="addPlEntry()"><span id="pl-plus"></span>Eintrag hinzuf&uuml;gen</button></div>
    <div class="row2">
      <div class="field"><label for="pl-rep">Wiederholen</label><select id="pl-rep" onchange="plRepChg()"><option value="0">Endlos</option><option value="1">1&times; abspielen</option><option value="2">2&times;</option><option value="3">3&times;</option><option value="5">5&times;</option><option value="10">10&times;</option></select></div>
      <div class="field"><label for="pl-tr">&Uuml;bergang</label><select id="pl-tr"><option value="0">Hart (0 s)</option><option value="3">0,3 s</option><option value="7">0,7 s</option><option value="15">1,5 s</option><option value="30">3 s</option></select></div>
    </div>
    <div class="field" id="pl-end-f"><label for="pl-end">Wenn die Playlist fertig ist</label><select id="pl-end"></select></div>
    <label class="check"><input type="checkbox" id="pl-shuffle">Zuf&auml;llige Reihenfolge</label>
    <label class="check"><input type="checkbox" id="pl-boot">Beim Einschalten automatisch starten</label>
    <div class="m-err" id="pl-err"></div>
    <div class="m-act"><button type="button" class="btn-sec" onclick="closeM('plm')">Abbrechen</button><button type="submit" class="btn-primary">Speichern &amp; starten</button></div>
  </form>
</div>

<div class="footer"><span>&copy; 2026 PixDrive</span><div><a href="/">WLED</a><a href="/pixdrive">PixDrive</a></div></div>

<script>
var IC={
play:'<polygon points="6 3 20 12 6 21 6 3"/>',
pause:'<rect x="14" y="4" width="4" height="16" rx="1"/><rect x="6" y="4" width="4" height="16" rx="1"/>',
stop:'<rect width="14" height="14" x="5" y="5" rx="2"/>',
restart:'<path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"/><path d="M3 3v5h5"/>',
next:'<polygon points="5 4 15 12 5 20 5 4"/><line x1="19" x2="19" y1="5" y2="19"/>',
power:'<path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/>',
sun:'<circle cx="12" cy="12" r="4"/><path d="M12 2v2"/><path d="M12 20v2"/><path d="m4.93 4.93 1.41 1.41"/><path d="m17.66 17.66 1.41 1.41"/><path d="M2 12h2"/><path d="M20 12h2"/><path d="m6.34 17.66-1.41 1.41"/><path d="m19.07 4.93-1.41 1.41"/>',
file:'<rect width="18" height="18" x="3" y="3" rx="2"/><path d="m10 8 4 4-4 4"/>',
list:'<path d="M21 15V6"/><path d="M18.5 18a2.5 2.5 0 1 0 0-5 2.5 2.5 0 0 0 0 5Z"/><path d="M12 12H3"/><path d="M16 6H3"/><path d="M12 18H3"/>',
trash:'<path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/>',
edit:'<path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z"/>',
up:'<path d="m18 15-6-6-6 6"/>',down:'<path d="m6 9 6 6 6-6"/>',x:'<path d="M18 6 6 18"/><path d="m6 6 12 12"/>',
plus:'<path d="M5 12h14"/><path d="M12 5v14"/>',upload:'<path d="m18 9-6-6-6 6"/><path d="M12 3v14"/><path d="M5 21h14"/>',
star:'<path d="M11.525 2.295a.53.53 0 0 1 .95 0l2.31 4.679a2.123 2.123 0 0 0 1.595 1.16l5.166.756a.53.53 0 0 1 .294.904l-3.736 3.638a2.123 2.123 0 0 0-.611 1.878l.882 5.14a.53.53 0 0 1-.771.56l-4.618-2.428a2.122 2.122 0 0 0-1.973 0L6.396 21.01a.53.53 0 0 1-.77-.56l.881-5.139a2.122 2.122 0 0 0-.611-1.879L2.16 9.795a.53.53 0 0 1 .294-.906l5.165-.755a2.122 2.122 0 0 0 1.597-1.16z"/>',
search:'<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',clock:'<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>'};
function ic(n){return '<svg class="i" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">'+IC[n]+'</svg>';}
function $(id){return document.getElementById(id);}
function esc(t){return String(t).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));}
function fmtSize(b){if(b>=1073741824)return(b/1073741824).toFixed(1)+' GB';if(b>=1048576)return(b/1048576).toFixed(1)+' MB';if(b>=1024)return(b/1024).toFixed(1)+' KB';return b+' B';}
function fmtTime(s){s=Math.max(0,Math.round(s));var m=Math.floor(s/60);return m+':'+String(s%60).padStart(2,'0');}
function fmtDur(s){return s>=60?fmtTime(s)+' min':(Math.round(s*10)/10).toString().replace('.',',')+' s';}
function base(n){return String(n||'').split('/').pop();}

var bootPs=0,fxId,files=[],presets={},st={},stat={},statT=0,ws=null,wsOk=false,pmFile='',plEdit=0,plRows=[];

// ---------- API ----------
function api(o){
  o.v=true; // ask WLED to answer with the new state
  return fetch('/json/state',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)})
    .then(r=>{if(!r.ok)throw 0;return r.json();}).then(d=>{if(d&&d.seg)applyState(d);return d;});
}
function post(u){return fetch(u,{method:'POST'}).then(r=>{if(!r.ok)throw 0;});}

// ---------- live state (WebSocket, polling as fallback) ----------
function connectWs(){
  try{ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws');}catch(e){return;}
  ws.onopen=()=>{wsOk=true;};
  ws.onmessage=e=>{if(typeof e.data!=='string')return;try{var d=JSON.parse(e.data);if(d.state)applyState(d.state);}catch(x){}};
  ws.onclose=()=>{wsOk=false;setTimeout(connectWs,3000);};
}
setInterval(()=>{if(!wsOk&&!document.hidden)fetch('/json/state').then(r=>r.json()).then(applyState).catch(()=>{});},3000);

function plOn(){return typeof st.pl==='number'&&st.pl>=0;} // 0 = playlist started without preset id
function pdSegs(){return (st.seg||[]).filter(s=>s.fx===fxId&&s.stop>s.start);}
function mainSegId(){var p=pdSegs();return p.length?p[0].id:(st.mainseg||0);}

function applyState(s){
  var oldPs=st.ps,oldPl=st.pl;st=s;
  renderPlayer();
  if(s.ps!==oldPs||s.pl!==oldPl){renderPresets();renderPlaylists();}
}

// ---------- progress (from /pixdrive/status, interpolated between polls) ----------
// Only one request at a time: after the tab was in the background the ESP answers
// slowly and responses could otherwise arrive out of order.
var statBusy=false,statSeq=0,shown={};
function pollStatus(){
  if(document.hidden||statBusy)return;
  statBusy=true;
  var seq=++statSeq,t0=performance.now();
  fetch('/pixdrive/status').then(r=>r.json()).then(d=>{
    if(seq!==statSeq)return; // superseded (e.g. by a seek)
    var t1=performance.now();
    stat={};
    (d.seg||[]).forEach(x=>{x.lag=(x.age||0)+(t1-t0)/2;stat[x.id]=x;}); // value was taken this long ago
    statT=t1;renderTracks();
  }).catch(()=>{}).then(()=>{statBusy=false;});
}
setInterval(pollStatus,1000);

function curFrame(sg,x){
  if(!x||!x.frames)return 0;
  var run=st.on&&!sg.frz,f=x.frame,dt=performance.now()-statT;
  if(run&&dt<3000){ // never extrapolate from an old value (tab was in the background)
    f+=((x.lag||0)+dt)/1000*x.fps;
    f=sg.o1?Math.min(f,x.frames-1):f%x.frames;
    // smooth out measurement jitter: no small steps backwards while playing
    var p=shown[sg.id];
    if(p!==undefined&&f<p&&p-f<x.fps*0.5)f=p;
  }
  shown[sg.id]=f;
  return f;
}

function renderTracks(){
  var segs=pdSegs(),el=$('tracks');
  if(!segs.length){el.innerHTML='';return;}
  if(el.children.length!==segs.length||el.dataset.k!==segs.map(s=>s.id).join()){
    el.dataset.k=segs.map(s=>s.id).join();
    el.innerHTML=segs.map(s=>'<div><div class="track-h"><span id="tn'+s.id+'"></span><span id="tt'+s.id+'"></span></div><div class="bar" id="tb'+s.id+'" onclick="seekBar(event,'+s.id+')"><div class="bar-fill" id="tf'+s.id+'"></div></div></div>').join('');
  }
  segs.forEach(s=>{
    var x=stat[s.id],f=curFrame(s,x),tot=x&&x.frames?x.frames/x.fps:0;
    $('tn'+s.id).textContent=(segs.length>1?'Segment '+(s.id+1)+' · ':'')+base(s.n);
    $('tt'+s.id).textContent=x&&x.err?'Datei nicht lesbar':(tot?fmtTime(f/x.fps)+' / '+fmtTime(tot):'');
    $('tf'+s.id).style.width=(x&&x.frames?Math.min(100,f/x.frames*100):0)+'%';
    $('tb'+s.id).classList.toggle('err',!!(x&&x.err));
  });
}
(function anim(){if(!document.hidden&&st.on&&pdSegs().some(s=>!s.frz))renderTracks();requestAnimationFrame(anim);})();

function seekBar(e,id){
  var x=stat[id];if(!x||!x.frames)return;
  var r=e.currentTarget.getBoundingClientRect(),f=Math.floor((e.clientX-r.left)/r.width*x.frames);
  f=Math.max(0,Math.min(x.frames-1,f));
  post('/pixdrive/seek?seg='+id+'&frame='+f).then(()=>{statSeq++;x.frame=f;x.lag=0;delete shown[id];statT=performance.now();renderTracks();}).catch(()=>{});
}

// ---------- player ----------
function renderPlayer(){
  var segs=pdSegs(),on=!!st.on,paused=segs.length&&segs.every(s=>s.frz);
  var pl=plOn()?(presets[st.pl]||{n:'Playlist'}):null,ps=st.ps>0?presets[st.ps]:null;
  var state,cls;
  if(!segs.length){state=on?'Kein PixDrive aktiv':'Aus';cls='b-off';}
  else if(!on){state='Gestoppt';cls='b-off';}
  else if(paused){state='Pausiert';cls='b-acc';}
  else{state='Spielt';cls='b-ok';}
  $('np-state').textContent=state;$('np-badge').className='badge '+cls;
  $('np-title').textContent=pl?pl.n:(ps?ps.n:(segs.length?base(segs[0].n):'Nichts ausgewählt'));
  $('np-sub').textContent=pl?('Playlist'+(ps?' · '+ps.n:'')):(segs.length?(ps?base(segs[0].n):'Direkt abgespielt'):'Spiele eine Animation, ein Preset oder eine Playlist ab.');
  var playing=segs.length&&on&&!paused;
  $('b-play').innerHTML=ic(playing?'pause':'play');$('b-play').title=playing?'Pause':'Abspielen';
  $('b-play').disabled=!segs.length;$('b-restart').disabled=!segs.length;$('b-stop').disabled=!segs.length||!on;
  $('b-next').style.display=plOn()?'':'none';
  $('b-power').className='pw-btn'+(on?' on':'');$('b-power').innerHTML=ic('power')+(on?'An':'Aus');
  if(!briHold&&st.bri){$('bri').value=st.bri;briShow(st.bri);}
  renderTracks();
}
function playPause(){
  var segs=pdSegs();if(!segs.length)return;
  var playing=st.on&&!segs.every(s=>s.frz);
  if(playing)api({seg:segs.map(s=>({id:s.id,frz:true}))});
  else api({on:true,seg:segs.map(s=>({id:s.id,frz:false}))});
}
function stopAll(){
  var segs=pdSegs();
  post('/pixdrive/seek?frame=0').catch(()=>{}).then(()=>api({on:false,seg:segs.map(s=>({id:s.id,frz:false}))}));
}
function restart(){
  var segs=pdSegs();
  post('/pixdrive/seek?frame=0').then(()=>api({on:true,seg:segs.map(s=>({id:s.id,frz:false}))})).then(()=>{statSeq++;shown={};Object.values(stat).forEach(x=>{x.frame=0;x.lag=0;});statT=performance.now();}).catch(()=>{});
}
function nextPl(){api({np:true});}
function power(){api({on:'t'});}

var briHold=false,briT=null,briPend=null;
function briShow(v){$('bri-val').textContent=Math.round(v/255*100)+'%';$('bri').style.setProperty('--p',((v-1)/254*100)+'%');}
function briInput(el){briHold=true;briPend=+el.value;briShow(briPend);if(!briT)briT=setTimeout(()=>briSend(false),150);}
function briSend(fin){clearTimeout(briT);briT=null;if(briPend!==null){api({bri:briPend}).catch(()=>{});briPend=null;}if(fin)setTimeout(()=>briHold=false,500);}

// ---------- files ----------
function loadFiles(){
  return fetch('/pixdrive/list').then(r=>r.json()).then(d=>{
    files=(d.files||[]).sort((a,b)=>a.name.localeCompare(b.name,undefined,{numeric:true}));
    var used=d.used||0,total=d.total||0,pct=total?used/total*100:0;
    $('used-val').textContent=total?fmtSize(used):'–';$('free-val').textContent=total?fmtSize(total-used):'–';
    $('total-val').textContent=total?fmtSize(total):'–';$('free-pct').textContent=total?(100-pct).toFixed(1).replace('.',',')+' % frei':'–';
    $('bar-fill').style.width=pct.toFixed(1)+'%';$('file-count').textContent=files.length;
    $('sd-state').innerHTML=total?'<span class="tag t-ok">SD-Karte bereit</span>':'<span class="tag t-off">Keine SD-Karte</span>';
    renderFiles();
  }).catch(()=>{$('file-list').innerHTML='<div class="empty">Dateien konnten nicht geladen werden</div>';});
}
function fRow(b){return b.closest('.trow').dataset.f;}
function fileLen(f){return f&&f.frames&&f.fps?f.frames/f.fps:0;}
function fileByName(n){return files.find(f=>f.name===n);}
function renderFiles(){
  var q=$('search-input').value.toLowerCase(),list=q?files.filter(f=>f.name.toLowerCase().includes(q)):files,el=$('file-list');
  if(!list.length){el.innerHTML='<div class="empty">'+(q?'Keine Treffer':'Noch keine Animationen. Lade eine .wled Datei hoch.')+'</div>';return;}
  var cur=pdSegs().map(s=>s.n);
  el.innerHTML=list.map(f=>{
    var n=esc(f.name),act=cur.indexOf(f.name)>=0;
    return '<div class="trow'+(act?' active':'')+'" data-f="'+n+'"><div class="c-grow nm"><div class="ico">'+ic('file')+'</div><div><div class="fname">'+esc(base(f.name))+'</div><div class="fsub">'+(f.rgbw?'RGBW · ':'')+(act?'läuft gerade':n)+'</div></div></div>'+
      '<div class="c-num"><span class="tag t-acc">'+(f.leds||'?')+'</span></div>'+
      '<div class="c-num opt">'+(fileLen(f)?fmtDur(fileLen(f)):'?')+'</div>'+
      '<div class="c-num opt">'+(f.fps||'?')+'</div>'+
      '<div class="c-num opt">'+fmtSize(f.size)+'</div>'+
      '<div class="c-act"><button class="abtn solid" onclick="playFile(fRow(this))">'+ic('play')+'<span class="lbl">Abspielen</span></button>'+
      '<button class="abtn" onclick="makePreset(fRow(this))">'+ic('plus')+'<span class="lbl">Preset</span></button>'+
      '<button class="ibtn" onclick="openRn(fRow(this))" title="Umbenennen">'+ic('edit')+'</button>'+
      '<button class="ibtn del" onclick="delFile(fRow(this))" title="Löschen">'+ic('trash')+'</button></div></div>';
  }).join('');
}
function playFile(name){
  if(fxId===undefined)return alert('PixDrive-Effekt nicht gefunden');
  var id=mainSegId();statSeq++;shown={};
  api({on:true,seg:[{id:id,on:true,frz:false,fx:fxId,n:name}]}).then(()=>post('/pixdrive/seek?seg='+id+'&frame=0')).catch(()=>alert('Konnte nicht abspielen'));
}
var rnFile='';
function presetsUsing(path){return ids(p=>!isPl(p)&&((p.seg||[]).some?(p.seg||[]):[p.seg]).some(s=>s&&s.n===path));}
function openRn(name){
  rnFile=name;
  $('rn-old').textContent=name;
  $('rn-name').value=base(name).replace(/\.wled$/i,'');
  var n=presetsUsing(name).length;
  $('rn-info').textContent=n?(n===1?'1 Preset verwendet diese Animation und wird automatisch angepasst.':n+' Presets verwenden diese Animation und werden automatisch angepasst.'):'';
  $('rn-err').textContent='';
  $('rnm').classList.add('open');$('rn-name').select();
}
function saveRn(){
  var v=$('rn-name').value.trim().replace(/\.wled$/i,''),to=v+'.wled',err=$('rn-err');
  if(!v)return err.textContent='Bitte einen Namen eingeben';
  if(/[\/\\"]/.test(v)||v[0]==='.')return err.textContent='Nicht erlaubt: / \\ " und Punkt am Anfang';
  if(to.length>54)return err.textContent='Höchstens 49 Zeichen (ohne .wled)';
  if(to===base(rnFile))return closeM('rnm');
  fetch('/pixdrive/rename?file='+encodeURIComponent(rnFile)+'&to='+encodeURIComponent(to),{method:'POST'})
    .then(r=>r.text().then(t=>{
      if(r.ok){closeM('rnm');setTimeout(()=>{loadFiles();loadPresets();},700);return;}
      err.textContent=r.status===409?'Eine Animation mit diesem Namen gibt es schon':r.status===503?'Gerät ist beschäftigt, bitte nochmal versuchen':'Fehler: '+t;
    }))
    .catch(()=>{err.textContent='Umbenennen fehlgeschlagen';});
}
function pickFile(){$('file-input').click();}
function uploadFile(file){
  if(!file)return;
  $('file-input').value='';
  if(!/\.wled$/i.test(file.name)||file.name.length>54||/[\/\\"]/.test(file.name)){alert('Ungültiger Dateiname: .wled-Datei, max. 54 Zeichen');return;}
  var dz=$('drop-zone'),pb=$('progress'),bar=$('progress-bar'),s=$('upload-status');
  dz.classList.add('uploading');pb.style.display='block';bar.style.width='0%';s.textContent='Lade hoch…';
  var x=new XMLHttpRequest();
  x.upload.onprogress=e=>{if(e.lengthComputable)bar.style.width=(e.loaded/e.total*100)+'%';};
  x.onload=()=>{s.textContent=x.status===200?(x.responseText&&x.responseText!==file.name?'Name schon vorhanden – gespeichert als „'+x.responseText+'“':'Upload erfolgreich'):'Fehler: '+(x.responseText||x.statusText);dz.classList.remove('uploading');setTimeout(()=>{pb.style.display='none';s.textContent='';},x.responseText!==file.name?6000:3000);setTimeout(loadFiles,300);};
  x.onerror=()=>{s.textContent='Upload fehlgeschlagen';dz.classList.remove('uploading');};
  var fd=new FormData();fd.append('file',file,file.name);
  x.open('POST','/pixdrive/upload');x.send(fd);
}
function delFile(name){
  if(!confirm('Animation löschen: '+base(name)+'?'))return;
  post('/pixdrive/delete?file='+encodeURIComponent(name)).then(()=>setTimeout(loadFiles,300)).catch(()=>alert('Fehler beim Löschen'));
}

// ---------- presets ----------
function isPl(p){return p&&p.playlist&&p.playlist.ps;}
function pdFile(p){var s=(p&&p.seg)||[];for(var i=0;i<s.length;i++){if(s[i]&&s[i].fx===fxId&&s[i].n)return s[i].n;}return '';}
function loadPresets(){
  return fetch('/presets.json').then(r=>r.ok?r.json():{}).catch(()=>({})).then(p=>{
    presets={};for(var k in p){if(+k>0&&+k<=250&&p[k]&&p[k].n!==undefined)presets[+k]=p[k];}
    renderPresets();renderPlaylists();renderPlayer();
  });
}
function ids(f){return Object.keys(presets).map(Number).filter(i=>f(presets[i])).sort((a,b)=>a-b);}
function renderPresets(){
  var l=ids(p=>!isPl(p)),el=$('preset-list');
  if(!l.length){el.innerHTML='<div class="empty">Noch keine Presets. Lege eins über &bdquo;Preset&ldquo; bei einer Animation an.</div>';return;}
  el.innerHTML=l.map(id=>{
    var p=presets[id],f=pdFile(p),act=id===st.ps&&!plOn();
    return '<div class="trow'+(act?' active':'')+'"><div class="c-id">'+id+'</div>'+
      '<div class="c-grow nm"><div class="fname">'+esc(p.n||('Preset '+id))+'</div>'+(act?'<span class="tag t-ok">Aktiv</span>':'')+bootTag(id)+'</div>'+
      '<div class="c-file h-file" title="'+esc(f)+'">'+(f?esc(base(f)):'<span class="tag t-off">WLED-Effekt</span>')+'</div>'+
      '<div class="c-act"><button class="abtn solid" onclick="playPreset('+id+')">'+ic('play')+'<span class="lbl">Abspielen</span></button>'+
      bootBtn(id)+'<button class="ibtn del" onclick="delPreset('+id+')" title="Löschen">'+ic('trash')+'</button></div></div>';
  }).join('');
}
function playPreset(id){
  // via PixDrive: {"ps":id} is ignored by WLED while it considers id the current preset
  statSeq++;shown={};
  post('/pixdrive/preset?id='+id).catch(()=>alert('Konnte nicht abspielen'));
}
function delPreset(id){
  var used=ids(isPl).filter(i=>presets[i].playlist.ps.indexOf(id)>=0);
  if(!confirm('„'+(presets[id].n||id)+'“ löschen?'+(used.length?'\nWird in '+used.length+' Playlist(s) verwendet.':'')))return;
  api({pdel:id}).then(()=>{if(bootPs===id)setBoot(0);setTimeout(loadPresets,600);}).catch(()=>alert('Fehler beim Löschen'));
}
// ---------- boot preset (cfg.json def.ps) ----------
function loadBoot(){return fetch('/json/cfg').then(r=>r.json()).then(c=>{bootPs=(c.def&&c.def.ps)||0;}).catch(()=>{});}
function setBoot(id){
  return fetch('/json/cfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({def:{ps:id}})})
    .then(r=>{if(r.status===401)throw 'pin';if(!r.ok)throw 0;bootPs=id;renderPresets();renderPlaylists();})
    .catch(e=>alert(e==='pin'?'Einstellungen sind mit einer PIN geschützt. Bitte in WLED entsperren.':'Start-Preset konnte nicht gespeichert werden'));
}
function toggleBoot(id){setBoot(bootPs===id?0:id);}
function bootBtn(id){var on=bootPs===id;return '<button class="ibtn boot'+(on?' on':'')+'" onclick="toggleBoot('+id+')" title="'+(on?'Nicht mehr beim Einschalten starten':'Beim Einschalten starten')+'">'+ic('star')+'</button>';}
function bootTag(id){return bootPs===id?'<span class="tag t-acc">Beim Start</span>':'';}
function nextFreeId(){for(var i=1;i<=250;i++){if(!presets[i])return i;}return 250;}

function makePreset(name){
  if(fxId===undefined)return alert('PixDrive-Effekt nicht gefunden');
  pmFile=name;
  $('pm-file').textContent=name;$('pm-name').value=base(name).replace(/\.wled$/i,'').slice(0,32);
  $('pm-id').value=nextFreeId();$('pm-once').checked=false;$('pm-boot').checked=false;$('pm-err').textContent='';
  var segs=(st.seg||[]).filter(s=>s.stop>s.start);
  $('pm-seg').innerHTML=(segs.length?segs:[{id:0}]).map(s=>'<option value="'+s.id+'"'+(s.id===mainSegId()?' selected':'')+'>'+(s.id+1)+(s.n&&!s.n.startsWith('/')?' – '+esc(s.n):'')+'</option>').join('');
  $('pm-seg-f').style.display=segs.length>1?'':'none';
  $('pm').classList.add('open');$('pm-name').select();
}
function closeM(id){$(id).classList.remove('open');}
function savePm(){
  var n=$('pm-name').value.trim(),id=parseInt($('pm-id').value),seg=parseInt($('pm-seg').value)||0,err=$('pm-err');
  if(!n)return err.textContent='Bitte einen Namen eingeben';
  if(!(id>=1&&id<=250))return err.textContent='ID muss zwischen 1 und 250 liegen';
  if(presets[id]&&!confirm('ID '+id+' („'+(presets[id].n||'')+'“) überschreiben?'))return;
  // apply to the device, then let WLED store the resulting state as preset (same as the WLED UI)
  api({on:true,seg:[{id:seg,on:true,frz:false,fx:fxId,n:pmFile,ix:255,o1:$('pm-once').checked}]})
    .then(()=>api({psave:id,n:n,ib:false,sb:false}))
    .then(()=>{if($('pm-boot').checked)return setBoot(id);if(bootPs===id)return setBoot(0);})
    .then(()=>{closeM('pm');setTimeout(loadPresets,800);})
    .catch(()=>{err.textContent='Fehler beim Speichern';});
}

// ---------- playlists ----------
function plDur(p){var d=p.playlist.dur,n=p.playlist.ps.length,t=0;for(var i=0;i<n;i++){var v=Array.isArray(d)?(d[i]!==undefined?d[i]:d[d.length-1]):d;if(!v)return -1;t+=v/10;}return t;}
function renderPlaylists(){
  var l=ids(isPl),el=$('pl-list');
  if(!l.length){el.innerHTML='<div class="empty">Noch keine Playlists.</div>';return;}
  el.innerHTML=l.map(id=>{
    var p=presets[id],act=id===st.pl,d=plDur(p),rep=p.playlist.repeat||0;
    return '<div class="trow'+(act?' active':'')+'"><div class="c-id">'+id+'</div>'+
      '<div class="c-grow nm"><div class="ico">'+ic('list')+'</div><div><div class="fname">'+esc(p.n||('Playlist '+id))+'</div><div class="fsub">'+(rep>0?rep+'× ':'Endlos')+(p.playlist.r?' · zufällig':'')+'</div></div>'+(act?'<span class="tag t-ok">Aktiv</span>':'')+bootTag(id)+'</div>'+
      '<div class="c-num">'+p.playlist.ps.length+'</div><div class="c-num opt">'+(d<0?'manuell':fmtDur(d))+'</div>'+
      '<div class="c-act"><button class="abtn solid" onclick="playPreset('+id+')">'+ic('play')+'<span class="lbl">Abspielen</span></button>'+
      '<button class="abtn" onclick="openPl('+id+')">'+ic('edit')+'<span class="lbl">Bearbeiten</span></button>'+
      bootBtn(id)+'<button class="ibtn del" onclick="delPreset('+id+')" title="Löschen">'+ic('trash')+'</button></div></div>';
  }).join('');
}
function presetLen(id){return fileLen(fileByName(pdFile(presets[id])));}
function openPl(id){
  var choices=ids(p=>!isPl(p));
  if(!choices.length)return alert('Lege zuerst mindestens ein Preset an.');
  plEdit=id||0;
  var p=id?presets[id]:null,pl=p?p.playlist:null;
  $('plm-title').textContent=p?'Playlist bearbeiten':'Neue Playlist';
  $('pl-name').value=p?p.n:'Playlist '+ (ids(isPl).length+1);
  $('pl-id').value=id||nextFreeId();
  plRows=[];
  if(pl){var d=pl.dur,t=pl.transition;pl.ps.forEach((ps,i)=>{var v=Array.isArray(d)?(d[i]!==undefined?d[i]:d[d.length-1]):d;plRows.push({ps:ps,dur:(v||0)/10});});
    var tr=Array.isArray(t)?t[0]:t;$('pl-tr').value=[0,3,7,15,30].indexOf(tr)>=0?String(tr):'7';}
  else{plRows.push(newRow(choices[0]));$('pl-tr').value='7';}
  var rep=pl?(pl.repeat||0):0;$('pl-rep').value=['0','1','2','3','5','10'].indexOf(String(rep))>=0?String(rep):'0';
  $('pl-shuffle').checked=!!(pl&&pl.r);
  $('pl-boot').checked=!!id&&bootPs===id;
  $('pl-end').innerHTML='<option value="0">Letzte Animation weiterlaufen lassen</option><option value="255">Zurück zu dem, was vorher lief</option>'+choices.map(i=>'<option value="'+i+'">„'+esc(presets[i].n)+'“ starten</option>').join('');
  $('pl-end').value=pl&&pl.end!==undefined?String(pl.end):'0';
  $('pl-err').textContent='';
  plRepChg();renderPlRows();$('plm').classList.add('open');
}
function plRepChg(){$('pl-end-f').style.display=$('pl-rep').value==='0'?'none':'';} // never reached when endless
function newRow(ps){var l=presetLen(ps);return {ps:ps,dur:l?Math.ceil(l*10)/10:10};}
function renderPlRows(){
  var choices=ids(p=>!isPl(p)),n=plRows.length;
  $('pl-entries').innerHTML=plRows.map((r,i)=>{
    var len=presetLen(r.ps);
    return '<div class="pl-row"><span class="pl-n">'+(i+1)+'</span>'+
      '<select onchange="plSet('+i+',this.value)">'+choices.map(c=>'<option value="'+c+'"'+(c==r.ps?' selected':'')+'>'+esc(presets[c].n)+'</option>').join('')+'</select>'+
      '<div class="pl-dur"><input type="number" min="0.1" step="0.1" value="'+r.dur+'" oninput="plRows['+i+'].dur=parseFloat(this.value)||0" title="Dauer in Sekunden"><span>s</span>'+
      (len?'<button type="button" class="ibtn" title="Länge der Animation ('+fmtDur(len)+')" onclick="plFit('+i+')">'+ic('clock')+'</button>':'')+'</div>'+
      '<div class="pl-tools"><button type="button" class="ibtn" '+(i?'':'disabled')+' onclick="plMove('+i+',-1)" title="Nach oben">'+ic('up')+'</button>'+
      '<button type="button" class="ibtn" '+(i<n-1?'':'disabled')+' onclick="plMove('+i+',1)" title="Nach unten">'+ic('down')+'</button>'+
      '<button type="button" class="ibtn del" '+(n>1?'':'disabled')+' onclick="plDel('+i+')" title="Entfernen">'+ic('x')+'</button></div></div>';
  }).join('');
}
function plSet(i,v){plRows[i]=newRow(+v);renderPlRows();}
function plFit(i){var l=presetLen(plRows[i].ps);if(l){plRows[i].dur=Math.ceil(l*10)/10;renderPlRows();}}
function plMove(i,d){var j=i+d;if(j<0||j>=plRows.length)return;var t=plRows[i];plRows[i]=plRows[j];plRows[j]=t;renderPlRows();}
function plDel(i){plRows.splice(i,1);renderPlRows();}
function addPlEntry(){var c=ids(p=>!isPl(p));var last=plRows.length?plRows[plRows.length-1].ps:c[0];var k=c.indexOf(last);plRows.push(newRow(c[(k+1)%c.length]));renderPlRows();}
function savePl(){
  var n=$('pl-name').value.trim(),id=parseInt($('pl-id').value),err=$('pl-err');
  if(!n)return err.textContent='Bitte einen Namen eingeben';
  if(!(id>=1&&id<=250))return err.textContent='ID muss zwischen 1 und 250 liegen';
  if(!plRows.length)return err.textContent='Mindestens ein Eintrag nötig';
  if(plRows.some(r=>!(r.dur>0)))return err.textContent='Jeder Eintrag braucht eine Dauer';
  if(plRows.some(r=>r.ps===id))return err.textContent='Die Playlist kann sich nicht selbst enthalten – andere ID wählen';
  if(presets[id]&&id!==plEdit&&!confirm('ID '+id+' („'+(presets[id].n||'')+'“) überschreiben?'))return;
  var tr=+$('pl-tr').value;
  var pl={ps:plRows.map(r=>r.ps),dur:plRows.map(r=>Math.round(r.dur*10)),transition:plRows.map(()=>tr),repeat:+$('pl-rep').value,end:+$('pl-end').value,r:$('pl-shuffle').checked};
  var boot=$('pl-boot').checked;
  api({psave:id,n:n,on:true,o:true,playlist:pl})
    .then(()=>{if(boot)return setBoot(id);if(bootPs===id||(plEdit&&bootPs===plEdit))return setBoot(0);})
    .then(()=>{closeM('plm');setTimeout(()=>{playPreset(id);loadPresets();},800);}) // restart by id so the name shows
    .catch(()=>{err.textContent='Fehler beim Speichern';});
}

// ---------- init ----------
document.addEventListener('keydown',e=>{if(e.key==='Escape'){closeM('pm');closeM('plm');closeM('rnm');}});
document.addEventListener('visibilitychange',()=>{if(!document.hidden){shown={};statT=0;statBusy=false;statSeq++;pollStatus();}});
var dz=$('drop-zone');
dz.addEventListener('dragover',e=>{e.preventDefault();dz.classList.add('drag-over');});
dz.addEventListener('dragleave',()=>dz.classList.remove('drag-over'));
dz.addEventListener('drop',e=>{e.preventDefault();dz.classList.remove('drag-over');if(e.dataTransfer.files.length)uploadFile(e.dataTransfer.files[0]);});
$('b-restart').innerHTML=ic('restart');$('b-stop').innerHTML=ic('stop');$('b-next').innerHTML=ic('next');
$('bri-ico').innerHTML=ic('sun');$('search-ico').innerHTML=ic('search');$('up-ico').innerHTML=ic('upload');
$('pl-add-ico').innerHTML=ic('plus');$('pl-plus').innerHTML=ic('plus');
renderPlayer();
fetch('/json/effects').then(r=>r.json()).then(fx=>{for(var i=0;i<fx.length;i++){if(fx[i].indexOf('PixDrive')===0){fxId=i;break;}}}).catch(()=>{})
  .then(()=>fetch('/json/state').then(r=>r.json()).then(applyState).catch(()=>{}))
  .then(()=>Promise.all([loadFiles(),loadBoot().then(loadPresets)]))
  .then(()=>{pollStatus();connectWs();});
</script>
</body></html>
)html";

// ---- Helpers for the web handlers ----
#define PD_MAX_NAME_LEN (WLED_MAX_SEGNAME_LEN - (sizeof(PIXDRIVE_DIR) - 1) - 1) // path must fit in a segment name

static bool pdValidUploadName(const String &n) {
  const size_t maxLen = PD_MAX_NAME_LEN;
  if (n.length() < 6 || n.length() > maxLen) return false;
  if (n[0] == '.' || n.indexOf('/') >= 0 || n.indexOf('\\') >= 0 || n.indexOf('"') >= 0) return false;
  String lower = n; lower.toLowerCase();
  return lower.endsWith(".wled");
}

// Path for an uploaded file that does not overwrite an existing one:
// "show.wled" -> "show (1).wled" -> "show (2).wled" ... ("" if none is free)
static String pdUniquePath(const String &filename) {
  String path = String(F(PIXDRIVE_DIR "/")) + filename;
  if (!SD_ADAPTER.exists(path.c_str())) return path;

  const int dot = filename.lastIndexOf('.');
  String stem = filename.substring(0, dot);
  const String ext = filename.substring(dot);
  // "show (1)" -> "show", so a re-upload of a copy does not become "show (1) (1)"
  if (stem.endsWith(")")) {
    const int open = stem.lastIndexOf(" (");
    bool digits = open > 0 && (unsigned)open + 2 < stem.length() - 1;
    for (unsigned i = open + 2; digits && i < stem.length() - 1; i++) digits = isdigit(stem[i]);
    if (digits) stem = stem.substring(0, open);
  }
  for (unsigned n = 1; n < 1000; n++) {
    const String suffix = String(F(" (")) + n + ')';
    const size_t room = PD_MAX_NAME_LEN - ext.length() - suffix.length();
    path = String(F(PIXDRIVE_DIR "/")) + (stem.length() > room ? stem.substring(0, room) : stem) + suffix + ext;
    if (!SD_ADAPTER.exists(path.c_str())) return path;
  }
  return String();
}

// newlib-nano printf has no %llu, so convert 64-bit values by hand
static void pdAppendU64(String &s, uint64_t v) {
  char b[21];
  int i = sizeof(b) - 1;
  b[i] = '\0';
  do { b[--i] = '0' + (v % 10); v /= 10; } while (v);
  s += &b[i];
}

static bool pdValidStoredPath(const String &p) {
  return p.startsWith(PIXDRIVE_DIR "/") && p.indexOf("..") < 0 && p != PIXDRIVE_UPLOAD_TMP;
}

// ---- Usermod class ----
class UsermodPixDrive : public Usermod {
  private:
    static const char _name[];
    bool endpointsRegistered = false;

    // Upload state. All upload callbacks run on the AsyncTCP task; one upload at a time.
    AsyncWebServerRequest *uploadReq = nullptr;
    File     uploadFile;
    String   uploadTarget;
    uint8_t  uploadHdr[WLED_FILE_HEADER_SIZE];
    size_t   uploadBytes = 0;
    uint32_t uploadCrc   = 0;
    uint32_t uploadLast  = 0;
    uint16_t uploadErr   = 0;
    bool     uploadDone  = false;

    // Rename/delete requested by a web handler, executed in loop() on the
    // main task, where it cannot race with playback reading the same file.
    std::atomic<bool> opPending{false};
    String opFrom, opTo; // opTo empty = delete opFrom
    bool   opRename = false; // user rename: keep the target, update references

    // Preset to start from the web UI, applied in loop(). Unlike {"ps":N} this
    // also works when WLED still considers N the current preset.
    std::atomic<uint8_t> presetReq{0};

    // References to a renamed file, updated once the JSON buffer is free
    bool   refFixPending = false;
    String refFrom, refTo;

    void runPendingOp() {
      for (auto &s : pdSlots) {
        if (s.owner && (opFrom == s.path || (opTo.length() && opTo == s.path))) pdCloseSlot(s);
      }
      if (opRename) {
        if (!SD_ADAPTER.exists(opTo.c_str()) && SD_ADAPTER.rename(opFrom.c_str(), opTo.c_str())) {
          // running segments switch to the new path right away
          for (unsigned i = 0; i < strip.getSegmentsNum(); i++) {
            Segment &sg = strip.getSegment(i);
            if (sg.name && opFrom == sg.name) sg.setName(opTo);
          }
          refFrom = opFrom;
          refTo   = opTo;
          refFixPending = true;
        } else {
          DEBUG_PRINTF_P(PSTR("[PixDrive] rename %s -> %s failed\n"), opFrom.c_str(), opTo.c_str());
        }
        opRename = false;
      } else if (opTo.length()) {
        if (SD_ADAPTER.exists(opTo.c_str())) SD_ADAPTER.remove(opTo.c_str());
        if (!SD_ADAPTER.rename(opFrom.c_str(), opTo.c_str())) {
          DEBUG_PRINTF_P(PSTR("[PixDrive] rename %s -> %s failed\n"), opFrom.c_str(), opTo.c_str());
          SD_ADAPTER.remove(opFrom.c_str());
        }
      } else {
        SD_ADAPTER.remove(opFrom.c_str());
      }
      opPending.store(false);
    }

    // Point presets that use refFrom to refTo. presets.json is only rewritten
    // when it was read completely, so a too large file is never truncated.
    void fixPresetReferences() {
      if (!requestJSONBufferLock(JSON_LOCK_PRESET_SAVE)) return; // busy: try again next loop
      refFixPending = false;
      unsigned changed = 0;
      pDoc->clear();
      if (readObjectFromFile(getPresetsFileName(), nullptr, pDoc) && !pDoc->overflowed()) {
        for (JsonPair kv : pDoc->as<JsonObject>()) {
          JsonVariant segs = kv.value()["seg"];
          auto fix = [&](JsonObject so) {
            const char *n = so["n"];
            if (n && refFrom == n) { so["n"] = refTo; changed++; }
          };
          if (segs.is<JsonArray>()) for (JsonObject so : segs.as<JsonArray>()) fix(so);
          else if (segs.is<JsonObject>()) fix(segs.as<JsonObject>());
        }
        if (changed && !pDoc->overflowed()) {
          File f = WLED_FS.open(getPresetsFileName(), "w");
          if (f) {
            serializeJson(*pDoc, f);
            f.close();
            presetsModifiedTime = toki.second();
            cacheInvalidate++;
            updateFSInfo();
          }
        }
      }
      DEBUG_PRINTF_P(PSTR("[PixDrive] renamed %s -> %s, %u preset reference(s) updated\n"), refFrom.c_str(), refTo.c_str(), changed);
      pDoc->clear();
      releaseJSONBufferLock();
    }

    void resetUpload() {
      if (uploadFile) uploadFile.close();
      uploadReq   = nullptr;
      uploadBytes = 0;
      uploadCrc   = 0;
      uploadErr   = 0;
      uploadDone  = false;
    }

    void onUploadChunk(AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        // an abandoned upload (client disconnected) is taken over after 30 s
        if (uploadReq && uploadReq != request && millis() - uploadLast < 30000) return;
        resetUpload();
        uploadReq  = request;
        uploadLast = millis();
        if (opPending.load())                 { uploadErr = 503; return; }
        if (!pdValidUploadName(filename))     { uploadErr = 400; return; }
        uploadTarget = pdUniquePath(filename);
        if (!uploadTarget.length())          { uploadErr = 409; return; }
        if (!SD_ADAPTER.exists(PIXDRIVE_DIR)) SD_ADAPTER.mkdir(PIXDRIVE_DIR);
        uploadFile = SD_ADAPTER.open(PIXDRIVE_UPLOAD_TMP, FILE_WRITE);
        if (!uploadFile)                      { uploadErr = 500; return; }
      }
      if (request != uploadReq || uploadErr) return;
      uploadLast = millis();

      // keep the header, CRC everything after it (no second pass over the file needed)
      size_t off = 0;
      if (uploadBytes < WLED_FILE_HEADER_SIZE) {
        off = min(len, (size_t)(WLED_FILE_HEADER_SIZE - uploadBytes));
        memcpy(uploadHdr + uploadBytes, data, off);
      }
      if (len > off) uploadCrc = pixdriveCRC32(data + off, len - off, uploadCrc);
      uploadBytes += len;

      if (len && uploadFile.write(data, len) != len) {
        uploadErr = 507; // SD full or write error
        uploadFile.close();
        return;
      }
      if (final) {
        uploadFile.close();
        uploadDone = true;
      }
    }

    void onUploadComplete(AsyncWebServerRequest *request) {
      if (request != uploadReq) {
        request->send(409, FPSTR(CONTENT_TYPE_PLAIN), F("Another upload is in progress"));
        return;
      }
      uint16_t code = uploadErr;
      const __FlashStringHelper *msg = F("Upload failed");
      const WledFileHeader *h = reinterpret_cast<const WledFileHeader*>(uploadHdr);

      if (!code && !uploadDone) code = 500;
      if (!code && (uploadBytes < WLED_FILE_HEADER_SIZE || memcmp(h->magic, WLED_FILE_MAGIC, 4) != 0)) {
        code = 400; msg = F("Not a .wled file");
      }
      if (!code) {
        const uint64_t expected = WLED_FILE_HEADER_SIZE +
          (uint64_t)h->frameCount * h->ledCount * ((h->flags & WLED_FLAG_RGBW) ? 4 : 3);
        if (expected != uploadBytes)               { code = 422; msg = F("File size does not match header"); }
        else if (h->crc32 && h->crc32 != uploadCrc) { code = 422; msg = F("CRC validation failed"); }
      }
      if (code == 400 && uploadErr == 400) msg = F("Invalid file name (.wled, max. 54 chars, no '/')");
      if (code == 503) msg = F("Busy, please retry");
      if (code == 507) msg = F("SD card full or write error");
      if (code == 409) msg = F("Too many files with this name");

      if (!code && opPending.load()) code = 503;
      if (!code) {
        opFrom = F(PIXDRIVE_UPLOAD_TMP);
        opTo   = uploadTarget;
        opPending.store(true);
        resetUpload();
        // answer with the stored name, it differs from the upload when a file with that name existed
        request->send(200, FPSTR(CONTENT_TYPE_PLAIN), uploadTarget.substring(sizeof(PIXDRIVE_DIR)));
        return;
      }
      resetUpload();
      if (code != 503) SD_ADAPTER.remove(PIXDRIVE_UPLOAD_TMP);
      DEBUG_PRINTF_P(PSTR("[PixDrive] upload rejected (%u)\n"), code);
      request->send(code, FPSTR(CONTENT_TYPE_PLAIN), msg);
    }

    static void handleList(AsyncWebServerRequest *request) {
      File root = SD_ADAPTER.open(PIXDRIVE_DIR);
      if (!root || !root.isDirectory()) {
        request->send(200, FPSTR(CONTENT_TYPE_JSON), F("{\"files\":[],\"used\":0,\"total\":0}"));
        return;
      }
      String json = F("{\"files\":[");
      bool first = true;
      for (File entry = root.openNextFile(); entry; entry = root.openNextFile()) {
        if (entry.isDirectory()) { entry.close(); continue; }
        // entry.name() is the bare file name on current ESP32 cores, full path on older ones
        String ename = entry.name();
        int slash = ename.lastIndexOf('/');
        if (slash >= 0) ename = ename.substring(slash + 1);
        if (ename.startsWith(".") || ename.indexOf('"') >= 0 || ename.indexOf('\\') >= 0) { entry.close(); continue; }

        WledFileHeader hdr;
        bool valid = pdReadHeader(entry, hdr);
        if (!first) json += ',';
        first = false;
        json += F("{\"name\":\"" PIXDRIVE_DIR "/");
        json += ename;
        json += F("\",\"size\":");
        json += String((uint32_t)entry.size());
        if (valid) {
          json += F(",\"leds\":");   json += hdr.ledCount;
          json += F(",\"frames\":"); json += hdr.frameCount;
          json += F(",\"fps\":");    json += hdr.fps;
          json += F(",\"rgbw\":");   json += (hdr.flags & WLED_FLAG_RGBW) ? F("true") : F("false");
        }
        json += '}';
        entry.close();
      }
      root.close();

      json += F("],\"used\":");
      pdAppendU64(json, SD_ADAPTER.usedBytes());
      json += F(",\"total\":");
      pdAppendU64(json, SD_ADAPTER.totalBytes());
      json += '}';
      request->send(200, FPSTR(CONTENT_TYPE_JSON), json);
    }

    void registerEndpoints() {
      endpointsRegistered = true;

      // register sub-paths before /pixdrive (the server prefix-matches "/pixdrive/...")
      server.on(F("/pixdrive/list"), HTTP_GET, handleList);

      server.on(F("/pixdrive/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { onUploadComplete(request); },
        [this](AsyncWebServerRequest *request, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
          onUploadChunk(request, filename, index, data, len, final);
        });

      // playback progress of all segments that ran PixDrive
      server.on(F("/pixdrive/status"), HTTP_GET, [](AsyncWebServerRequest *request) {
        String json = F("{\"seg\":[");
        const uint32_t now = millis();
        bool first = true;
        for (unsigned i = 0; i < MAX_NUM_SEGMENTS; i++) {
          const PixDriveStatus ps = pdStatus[i];
          if (!ps.updated) continue;
          if (!first) json += ',';
          first = false;
          json += F("{\"id\":");     json += i;
          json += F(",\"frame\":");  json += ps.frame;
          json += F(",\"frames\":"); json += ps.frames;
          json += F(",\"fps\":");    json += ps.fps;
          json += F(",\"err\":");    json += (ps.status == PD_ERROR) ? F("true") : F("false");
          json += F(",\"age\":");    json += (uint32_t)(now - ps.updated);
          json += '}';
        }
        json += F("]}");
        request->send(200, FPSTR(CONTENT_TYPE_JSON), json);
      });

      // jump to a frame: ?frame=N (default 0 = restart), optional &seg=ID (default: all)
      server.on(F("/pixdrive/seek"), HTTP_POST, [](AsyncWebServerRequest *request) {
        uint32_t frame = request->hasParam(F("frame")) ? request->getParam(F("frame"))->value().toInt() : 0;
        int seg = request->hasParam(F("seg")) ? request->getParam(F("seg"))->value().toInt() : -1;
        for (unsigned i = 0; i < MAX_NUM_SEGMENTS; i++) {
          if (seg < 0 || (unsigned)seg == i) pdSeekReq[i].store(frame + 1);
        }
        request->send(200, FPSTR(CONTENT_TYPE_PLAIN), F("OK"));
      });

      // rename: ?file=/pixdrive/old.wled&to=new.wled (presets using the file are updated)
      server.on(F("/pixdrive/rename"), HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!request->hasParam(F("file")) || !request->hasParam(F("to"))) {
          request->send(400, FPSTR(CONTENT_TYPE_PLAIN), F("Missing parameter"));
          return;
        }
        String from = request->getParam(F("file"))->value();
        String to   = request->getParam(F("to"))->value();
        to.trim();
        if (!pdValidStoredPath(from)) {
          request->send(403, FPSTR(CONTENT_TYPE_PLAIN), F("Access denied"));
          return;
        }
        if (!pdValidUploadName(to)) {
          request->send(400, FPSTR(CONTENT_TYPE_PLAIN), F("Invalid file name (.wled, max. 54 chars, no '/')"));
          return;
        }
        const String target = String(F(PIXDRIVE_DIR "/")) + to;
        if (target == from) { request->send(200, FPSTR(CONTENT_TYPE_PLAIN), to); return; }
        if (!SD_ADAPTER.exists(from.c_str())) {
          request->send(404, FPSTR(CONTENT_TYPE_PLAIN), F("File not found"));
          return;
        }
        // FAT ignores case, so "Show.wled" exists when "show.wled" does
        if (SD_ADAPTER.exists(target.c_str())) {
          request->send(409, FPSTR(CONTENT_TYPE_PLAIN), F("A file with this name already exists"));
          return;
        }
        if (opPending.load() || refFixPending) {
          request->send(503, FPSTR(CONTENT_TYPE_PLAIN), F("Busy, please retry"));
          return;
        }
        opFrom   = from;
        opTo     = target;
        opRename = true;
        opPending.store(true);
        request->send(200, FPSTR(CONTENT_TYPE_PLAIN), to);
      });

      // start preset or playlist ?id=N from the beginning, even if it is the current one
      server.on(F("/pixdrive/preset"), HTTP_POST, [this](AsyncWebServerRequest *request) {
        int id = request->hasParam(F("id")) ? request->getParam(F("id"))->value().toInt() : 0;
        if (id < 1 || id > 250) {
          request->send(400, FPSTR(CONTENT_TYPE_PLAIN), F("Invalid preset id"));
          return;
        }
        presetReq.store(id);
        request->send(200, FPSTR(CONTENT_TYPE_PLAIN), F("OK"));
      });

      server.on(F("/pixdrive/delete"), HTTP_POST, [this](AsyncWebServerRequest *request) {
        if (!request->hasParam(F("file"))) {
          request->send(400, FPSTR(CONTENT_TYPE_PLAIN), F("Missing file parameter"));
          return;
        }
        String path = request->getParam(F("file"))->value();
        if (!pdValidStoredPath(path)) {
          request->send(403, FPSTR(CONTENT_TYPE_PLAIN), F("Access denied"));
          return;
        }
        if (!SD_ADAPTER.exists(path.c_str())) {
          request->send(404, FPSTR(CONTENT_TYPE_PLAIN), F("File not found"));
          return;
        }
        if (opPending.load()) {
          request->send(503, FPSTR(CONTENT_TYPE_PLAIN), F("Busy, please retry"));
          return;
        }
        opFrom = path;
        opTo   = "";
        opPending.store(true);
        request->send(200, FPSTR(CONTENT_TYPE_PLAIN), F("Deleted"));
      });

      server.on(F("/pixdrive"), HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send_P(200, FPSTR(CONTENT_TYPE_HTML), PIXDRIVE_HTML);
      });
    }

  public:
    void setup() override {
      strip.addEffect(255, &mode_pixdrive, _data_FX_MODE_PIXDRIVE);
      // The SD card may not be initialised yet (usermod order is not defined),
      // so the /pixdrive directory is created on first upload instead.
    }

    void loop() override {
      // The web server is started after usermod setup(); connected() is not
      // called in AP mode, so register the routes on the first loop instead.
      if (!endpointsRegistered) registerEndpoints();

      if (opPending.load()) runPendingOp();
      if (refFixPending) fixPresetReferences();

      if (uint8_t ps = presetReq.exchange(0)) {
        applyPreset(ps);                                         // also (re)starts playlists
        for (auto &req : pdSeekReq) req.store(1);                // and plays from the beginning
      }

      // close handles of segments that stopped playing (effect changed,
      // segment off/frozen/deleted, transition copy gone)
      const uint32_t now = millis();
      for (auto &s : pdSlots) {
        if (s.owner && now - s.lastUsed > PIXDRIVE_SLOT_IDLE_MS) pdCloseSlot(s);
      }
    }

    void addToJsonInfo(JsonObject &root) override {
      JsonObject user = root["u"];
      if (user.isNull()) user = root.createNestedObject("u");
      JsonArray sd = user.createNestedArray(F("PixDrive SD"));
      if (SD_ADAPTER.cardType() == CARD_NONE) {
        sd.add(F("no card"));
        return;
      }
      int count = 0;
      File dir = SD_ADAPTER.open(PIXDRIVE_DIR);
      if (dir && dir.isDirectory()) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
          String n = f.name();
          if (!f.isDirectory() && !n.endsWith(".upload.tmp")) count++;
          f.close();
        }
        dir.close();
      }
      sd.add(count);
      sd.add(F(" files"));
    }
};

const char UsermodPixDrive::_name[] PROGMEM = "PixDrive";

static UsermodPixDrive pixdrive;
REGISTER_USERMOD(pixdrive);

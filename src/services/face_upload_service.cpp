#include "face_upload_service.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_heap_caps.h>
#include <esp_spiffs.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static const char* TAG = "FaceUpload";

// ─── State ───────────────────────────────────────────────────────────────────

static httpd_handle_t       s_server    = NULL;
static bool                 s_running   = false;
static face_upload_done_cb_t s_on_done  = NULL;
static esp_netif_t*         s_ap_netif  = NULL;

static face_upload_slot_t   s_slots[FACE_UPLOAD_MAX_SLOTS] = {};
static int                  s_slot_count = 0;
static bool                 s_store_loaded = false;
static bool                 s_fs_mounted = false;

static bool mount_store_fs(void);
static bool persist_all_slots_to_store(void);
static void slot_path(int slot, char* out, size_t n);

static void remove_tail_files_from(int from_slot)
{
    if (!mount_store_fs()) return;
    if (from_slot < 0) from_slot = 0;
    if (from_slot > FACE_UPLOAD_MAX_SLOTS) from_slot = FACE_UPLOAD_MAX_SLOTS;
    for (int i = from_slot; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        char path[64];
        slot_path(i, path, sizeof(path));
        remove(path);
    }
}

static bool ensure_store_space(size_t bytes_needed)
{
    if (!mount_store_fs()) return false;

    size_t total = 0;
    size_t used = 0;
    esp_err_t err = esp_spiffs_info("assets", &total, &used);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_spiffs_info failed: %s", esp_err_to_name(err));
        return true;
    }

    size_t free_bytes = (total > used) ? (total - used) : 0;
    if (free_bytes >= bytes_needed) return true;

    size_t need_gc = bytes_needed - free_bytes;
    esp_err_t gc_err = esp_spiffs_gc("assets", need_gc);
    if (gc_err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS GC failed: %s (need=%u)", esp_err_to_name(gc_err), (unsigned)need_gc);
    }

    if (esp_spiffs_info("assets", &total, &used) != ESP_OK) return false;
    free_bytes = (total > used) ? (total - used) : 0;
    return free_bytes >= bytes_needed;
}

static bool mount_store_fs(void)
{
    if (s_fs_mounted) return true;

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = "/spiffs";
    conf.partition_label = "assets";
    conf.max_files = 8;
#ifdef BOARD_VARIANT_LCD128
    /* Yeni board'un assets partition'ı hiç formatlanmamış (mount -10025).
     * Bu kartta korunacak foto yok → ilk mount'ta formatla ki SPIFFS ve
     * Gallery çalışır hale gelsin. Fotoğraflar WiFi gelince yeniden yüklenir. */
    conf.format_if_mount_failed = true;
#else
    conf.format_if_mount_failed = false;  // never wipe user photos implicitly
#endif

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return false;
    }

    s_fs_mounted = true;
    return true;
}

static void slot_path(int slot, char* out, size_t n)
{
    snprintf(out, n, "/spiffs/face_%02d.rgb565", slot);
}

static void load_slots_from_store(void)
{
    if (s_store_loaded) return;
    if (!mount_store_fs()) return;

    s_slot_count = 0;
    bool needs_repack = false;
    for (int slot = 0; slot < FACE_UPLOAD_MAX_SLOTS; slot++) {
        char path[64];
        slot_path(slot, path, sizeof(path));
        FILE* f = fopen(path, "rb");
        if (!f) continue;

        void* buf = heap_caps_malloc(FACE_UPLOAD_BUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) buf = heap_caps_malloc(FACE_UPLOAD_BUF_BYTES, MALLOC_CAP_8BIT);
        if (!buf) {
            fclose(f);
            ESP_LOGE(TAG, "No memory while loading %s", path);
            needs_repack = true;
            break;
        }

        size_t rd = fread(buf, 1, FACE_UPLOAD_BUF_BYTES, f);
        fclose(f);
        if (rd != FACE_UPLOAD_BUF_BYTES) {
            heap_caps_free(buf);
            ESP_LOGW(TAG, "Stored file size mismatch, removing: %s (%u)", path, (unsigned)rd);
            remove(path);
            continue;
        }

        int dst = s_slot_count;
        s_slots[dst].buf = buf;
        s_slots[dst].valid = true;
        if (dst != slot) needs_repack = true;
        s_slot_count++;
    }

    if (needs_repack) {
        ESP_LOGI(TAG, "Repacking stored gallery slots");
        persist_all_slots_to_store();
    }

    ESP_LOGI(TAG, "Loaded %d gallery images from flash", s_slot_count);
    s_store_loaded = true;
}

static bool save_slot_to_store(int slot)
{
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return false;
    if (!s_slots[slot].valid || !s_slots[slot].buf) return false;
    if (!mount_store_fs()) return false;
    if (!ensure_store_space(FACE_UPLOAD_BUF_BYTES + 4096)) {
        ESP_LOGE(TAG, "Not enough SPIFFS space for slot %d", slot);
        return false;
    }

    char path[64];
    slot_path(slot, path, sizeof(path));

    // Ensure old file pages are reclaimed before creating/writing a new one.
    remove(path);
    ensure_store_space(FACE_UPLOAD_BUF_BYTES + 4096);

    FILE* f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for write", path);
        return false;
    }

    const uint8_t* src = (const uint8_t*)s_slots[slot].buf;
    size_t wr = 0;
    while (wr < FACE_UPLOAD_BUF_BYTES) {
        size_t chunk = FACE_UPLOAD_BUF_BYTES - wr;
        if (chunk > 4096u) chunk = 4096u;
        size_t w = fwrite(src + wr, 1, chunk, f);
        if (w == 0) break;
        wr += w;
    }
    fflush(f);
    fclose(f);
    if (wr != FACE_UPLOAD_BUF_BYTES) {
        ESP_LOGE(TAG, "Short write for %s (%u), errno=%d", path, (unsigned)wr, errno);
        return false;
    }
    ESP_LOGI(TAG, "Saved image slot %d to flash", slot);
    return true;
}

static bool persist_all_slots_to_store(void)
{
    if (!mount_store_fs()) return false;

    // Remove any stale tail files beyond current count first to free space.
    for (int i = s_slot_count; i < FACE_UPLOAD_MAX_SLOTS; i++) {
        char path[64];
        slot_path(i, path, sizeof(path));
        remove(path);
    }

    // Rewrite contiguous files [0..s_slot_count-1]
    for (int i = 0; i < s_slot_count; i++) {
        if (!save_slot_to_store(i)) return false;
    }

    return true;
}

static esp_err_t ensure_wifi_ready(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_mode_t mode;
    err = esp_wifi_get_mode(&mode);
    if (err == ESP_ERR_WIFI_NOT_INIT) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK) return err;
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
    } else if (err != ESP_OK) {
        return err;
    }

    return ESP_OK;
}

// ─── HTML page (browser converts image → RGB565, uploads binary) ──────────────
// JS: draws user image onto a 410×502 canvas, reads RGBA, packs RGB565
// little-endian byte order (LSB first), POSTs as octet-stream.

static const char* HTML_PAGE = R"rawhtml(<!DOCTYPE html>
<html lang="tr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Galeri Yukle</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,sans-serif;background:#111;color:#fff;padding:20px;max-width:520px;margin:0 auto}
  h1{font-size:22px;margin-bottom:10px}
  p.sub{color:#888;font-size:13px;margin-bottom:16px}
  .card{background:#1e1e1e;border-radius:16px;padding:18px;margin-bottom:16px}
  input[type=file]{width:100%;padding:10px;background:#2a2a2a;border:1px solid #333;border-radius:8px;color:#fff;font-size:14px;cursor:pointer}
  canvas{display:none;width:100%;border-radius:10px;margin-top:12px;border:1px solid #333}
  .btn{display:none;width:100%;margin-top:12px;padding:14px;border:none;border-radius:10px;font-size:16px;font-weight:600;cursor:pointer;background:#0a84ff;color:#fff}
  .btn:disabled{background:#333;color:#666;cursor:not-allowed}
  .status{margin-top:12px;font-size:14px;min-height:20px}
  .ok{color:#30d158} .err{color:#ff453a} .info{color:#0a84ff}
</style>
</head><body>
<h1>⌚ Galeriye Foto Yukle</h1>
<p class="sub">Secilen foto selector galerisinde kullanilabilir.</p>

<div class="card">
  <input type="file" accept="image/*" onchange="pick(this)">
  <canvas id="cv" width="410" height="502"></canvas>
  <button class="btn" id="b" onclick="upload()">Yukle →</button>
  <div class="status" id="s"></div>
</div>

<script>
var buf=null;

function pick(inp){
  var f=inp.files[0]; if(!f) return;
  var cv=document.getElementById('cv');
  var ctx=cv.getContext('2d');
  var img=new Image();
  img.onload=function(){
    cv.style.display='block';
    ctx.drawImage(img,0,0,410,502);
    var d=ctx.getImageData(0,0,410,502).data;
    var b=new Uint8Array(410*502*2);
    var j=0;
    for(var i=0;i<d.length;i+=4){
      var r=d[i]>>3, g=d[i+1]>>2, b2=d[i+2]>>3;
      var px=(r<<11)|(g<<5)|b2;
      // little-endian RGB565
      b[j++]=px&0xFF;
      b[j++]=(px>>8)&0xFF;
    }
    buf=b;
    document.getElementById('b').style.display='block';
    setStatus('','');
  };
  img.src=URL.createObjectURL(f);
}

function setStatus(cls,msg){
  var el=document.getElementById('s');
  el.className='status '+cls; el.textContent=msg;
}

async function upload(){
  if(!buf) return;
  var btn=document.getElementById('b');
  btn.disabled=true;
  setStatus('info','Yukleniyor… ('+(410*502*2/1024).toFixed(0)+' KB)');
  try{
    var r=await fetch('/upload',{
      method:'POST',
      headers:{'Content-Type':'application/octet-stream'},
      body:buf
    });
    if(r.ok){
      setStatus('ok','✅ Yuklendi! Saatte galeriyi kontrol edin.');
      btn.textContent='Tekrar Yukle';
      btn.disabled=false;
    } else {
      setStatus('err','❌ Hata: HTTP '+r.status);
      btn.disabled=false;
    }
  }catch(e){
    setStatus('err','❌ Baglanti hatasi: '+e.message);
    btn.disabled=false;
  }
}
</script>
</body></html>
)rawhtml";

// ─── HTTP handlers ────────────────────────────────────────────────────────────

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, HTML_PAGE);
    return ESP_OK;
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    load_slots_from_store();
    remove_tail_files_from(s_slot_count);

    if (!ensure_store_space(FACE_UPLOAD_BUF_BYTES + 4096)) {
        ESP_LOGE(TAG, "Gallery storage full (used=%d slots)", s_slot_count);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "storage full");
        return ESP_FAIL;
    }

    if (s_slot_count >= FACE_UPLOAD_MAX_SLOTS) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "gallery full");
        return ESP_FAIL;
    }

    int slot = s_slot_count;

    int expected = (int)FACE_UPLOAD_BUF_BYTES;
    int content_len = req->content_len;

    ESP_LOGI(TAG, "Upload slot=%d content_len=%d expected=%d", slot, content_len, expected);

    if (content_len != expected) {
        ESP_LOGW(TAG, "Size mismatch: got %d, expected %d", content_len, expected);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wrong size");
        return ESP_FAIL;
    }

    // Allocate or reuse PSRAM buffer
    s_slots[slot].buf = heap_caps_malloc(FACE_UPLOAD_BUF_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_slots[slot].buf) {
        s_slots[slot].buf = heap_caps_malloc(FACE_UPLOAD_BUF_BYTES,
                                             MALLOC_CAP_8BIT);
    }
    if (!s_slots[slot].buf) {
        ESP_LOGE(TAG, "alloc failed for slot %d", slot);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_FAIL;
    }

    // Receive in smaller chunks to keep HTTP task stack safe.
    uint8_t chunk[2048];
    int received = 0;
    while (received < content_len) {
        int to_read = (int)sizeof(chunk);
        if (to_read > content_len - received) to_read = content_len - received;
        int r = httpd_req_recv(req, (char*)chunk, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "recv error at byte %d", received);
            /* Heap leak fix: yeni alloc edilmiş PSRAM buffer'ı serbest bırak.
             * Aksi halde sonraki upload aynı slot'a yazarsa eski pointer
             * dangling kalır → 200KB+ PSRAM her başarısız upload'da kaybolur. */
            heap_caps_free(s_slots[slot].buf);
            s_slots[slot].buf = NULL;
            s_slots[slot].valid = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv error");
            return ESP_FAIL;
        }
        memcpy((uint8_t*)s_slots[slot].buf + received, chunk, r);
        received += r;
    }

    s_slots[slot].valid = true;
    if (!save_slot_to_store(slot)) {
        ESP_LOGW(TAG, "Persist failed for slot %d, running SPIFFS check + retry", slot);
        esp_spiffs_check("assets");
        if (!save_slot_to_store(slot)) {
            ESP_LOGE(TAG, "Persist failed for slot %d", slot);
            heap_caps_free(s_slots[slot].buf);
            s_slots[slot].buf = NULL;
            s_slots[slot].valid = false;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist failed");
            return ESP_FAIL;
        }
    }

    s_slot_count++;
    ESP_LOGI(TAG, "Slot %d uploaded OK (%d bytes)", slot, received);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");

    // Notify watchface (called from HTTP server task — user must use lv_async_call)
    if (s_on_done) s_on_done(slot);

    return ESP_OK;
}

// ─── WiFi AP setup ────────────────────────────────────────────────────────────

static esp_err_t start_wifi_ap(void)
{
    esp_err_t err = ensure_wifi_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return err;
    }

    // We do a minimal AP init; existing STA (if any) is stopped first.
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_NULL);

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "Failed to create AP netif");
            return ESP_FAIL;
        }
    }

    wifi_config_t ap_cfg = {};
    strncpy((char*)ap_cfg.ap.ssid, "EmbedWatch", sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len      = (uint8_t)strlen("EmbedWatch");
    ap_cfg.ap.channel       = 6;
    ap_cfg.ap.authmode      = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = 4;

    err = esp_wifi_set_mode(WIFI_MODE_AP); if (err) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg); if (err) return err;
    err = esp_wifi_start(); if (err) return err;

    ESP_LOGI(TAG, "AP started: SSID=EmbedWatch, IP=192.168.4.1");
    return ESP_OK;
}

// ─── HTTP server start/stop ───────────────────────────────────────────────────

static esp_err_t start_http_server(void)
{
    httpd_config_t cfg      = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers    = 4;
    cfg.recv_wait_timeout   = 60;   // 60s for large upload
    cfg.send_wait_timeout   = 30;
    cfg.stack_size          = 16384;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t root = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = root_get_handler,
        .user_ctx = NULL
    };
    static const httpd_uri_t upload = {
        .uri      = "/upload",
        .method   = HTTP_POST,
        .handler  = upload_post_handler,
        .user_ctx = NULL
    };
    /* Handler register fail durumunda server kalkmamış sayılır — endpoint
     * cevap vermez, kullanıcı upload deneyince 404 alır. Server'ı kapat ve
     * hata döndür ki caller doğru fallback yapabilsin. */
    esp_err_t ru = httpd_register_uri_handler(s_server, &root);
    if (ru != ESP_OK) {
        ESP_LOGE(TAG, "register / handler fail: %s", esp_err_to_name(ru));
        httpd_stop(s_server); s_server = NULL;
        return ru;
    }
    ru = httpd_register_uri_handler(s_server, &upload);
    if (ru != ESP_OK) {
        ESP_LOGE(TAG, "register /upload handler fail: %s", esp_err_to_name(ru));
        httpd_stop(s_server); s_server = NULL;
        return ru;
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return ESP_OK;
}

// ─── Public API ──────────────────────────────────────────────────────────────

esp_err_t face_upload_load_persisted(void)
{
    load_slots_from_store();
    return ESP_OK;
}

esp_err_t face_upload_service_start(face_upload_done_cb_t on_done)
{
    load_slots_from_store();

    if (s_running) return ESP_OK;
    s_on_done = on_done;

    esp_err_t err = start_wifi_ap();
    if (err != ESP_OK) return err;

    err = start_http_server();
    if (err != ESP_OK) {
        esp_wifi_stop();
        return err;
    }

    s_running = true;
    return ESP_OK;
}

void face_upload_service_stop(void)
{
    if (!s_running) return;
    /* Stop sırasını sırasıyla yönet:
     * 1) s_on_done = NULL — server task'ı in-flight bir upload'u bitirirken
     *    callback'i tetiklemesin (gallery on_stop'tan sonra çağrılırsa
     *    s_grid zaten NULL → callback uçar; refresh_ui'deki erken-exit
     *    ikinci güvenlik hattı).
     * 2) httpd_stop — server task'ını sonlandır, açık socket'leri kapat.
     * 3) Kısa drain — handler task'ları temizlensin.
     * 4) esp_wifi_stop — AP kapanır. */
    s_on_done = NULL;
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_wifi_stop();
    s_running = false;
    ESP_LOGI(TAG, "Upload service stopped");
}

bool face_upload_service_is_running(void)
{
    return s_running;
}

const face_upload_slot_t* face_upload_get_slot(int slot)
{
    load_slots_from_store();
    if (slot < 0 || slot >= FACE_UPLOAD_MAX_SLOTS) return NULL;
    return &s_slots[slot];
}

int face_upload_slot_count(void)
{
    load_slots_from_store();
    return s_slot_count;
}

esp_err_t face_upload_delete_slot(int slot)
{
    load_slots_from_store();
    if (slot < 0 || slot >= s_slot_count) return ESP_ERR_INVALID_ARG;
    if (!s_slots[slot].valid) return ESP_ERR_INVALID_STATE;

    if (s_slots[slot].buf) {
        heap_caps_free(s_slots[slot].buf);
        s_slots[slot].buf = NULL;
    }

    for (int i = slot; i < s_slot_count - 1; i++) {
        s_slots[i] = s_slots[i + 1];
    }

    s_slot_count--;
    if (s_slot_count >= 0) {
        s_slots[s_slot_count].buf = NULL;
        s_slots[s_slot_count].valid = false;
    }

    persist_all_slots_to_store();
    ESP_LOGI(TAG, "Deleted gallery slot %d, remaining=%d", slot, s_slot_count);
    return ESP_OK;
}

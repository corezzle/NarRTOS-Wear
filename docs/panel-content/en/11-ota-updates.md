NarRTOS devices can download new software **over the air** and update themselves — no physical intervention needed.

## The flow

```
1. Manifest sorgusu  ->  sunucudaki güncel sürüm nedir?
2. Karşılaştırma     ->  OS_VERSION_CODE'dan büyük mü?
3. İndirme           ->  yeni imaj WiFi üzerinden yedek bölüme yazılır
4. Doğrulama         ->  bütünlük kontrolü
5. Yeniden başlatma  ->  yeni sürüm aktif olur
```

User interface: **Settings → General → Software Updates**. With automatic checking enabled, the manifest is queried periodically in the background.

## Version comparison

Version information is reduced to a single integer — to avoid the pitfalls of string parsing:

```
#define OS_VERSION_MAJOR 1
#define OS_VERSION_MINOR 4
#define OS_VERSION_PATCH 81
#define OS_VERSION_STR   "1.4.81"
// OS_VERSION_CODE = MAJOR*10000 + MINOR*100 + PATCH   -> 10481
```

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit">A string comparison yields <code>"0.10" < "0.2"</code> — comparing integers avoids that mistake.</div></div>

## Manifest format

```
{
  "version":      "1.5.0",
  "version_code": 10500,
  "url":          "https://sunucu/firmware/narrtos-1.5.0.bin",
  "size":         2480000,
  "notes":        "Hata düzeltmeleri ve yeni özellikler"
}
```

The manifest address is supplied through a build flag; the device identity and the current version are appended as query parameters — so the server can pick a suitable version, or return `204 No Content` if the device is already up to date.

## A/B partition layout

```
app0  →  çalışan sürüm
app1  →  yeni imaj buraya yazılır
```

If the update fails, or the new version crashes at boot, the device **falls back to the old version** (anti-crash-loop protection). The partition table is defined in `partitions_custom.csv`: two application partitions + NVS + an assets partition.

## Configuration

```
# platformio.ini
build_flags =
    '-DOTA_BACKEND_HOST="https://sunucunuz.example.com"'
```

Every OTA address in `os_config.h` is `#ifndef`-guarded; it can be overridden from the env.

## Update lifecycle telemetry

The device reports update events (started / succeeded / failed) to the server. This is best-effort: if the server is unreachable the OTA flow is not disturbed, the event is silently skipped.

## Security note

In production, manifest and image distribution must go over **HTTPS**. During development a configuration that permits HTTP is used; certificate pinning is on the roadmap.

<div style="display:flex;gap:22px;align-items:flex-start;flex-wrap:wrap;margin:18px 0"><figure style="flex:0 0 190px;margin:0;text-align:center"><img src="GORSEL:ota.png" style="width:100%;display:block;border-radius:10px" /><b style="display:block;font-size:14px;font-weight:700;margin-top:12px;color:inherit">Software update</b><span style="display:block;font-size:12.5px;color:inherit;opacity:.72;margin-top:2px">Settings → Software Update</span></figure><div style="flex:1 1 320px;min-width:280px"><p>The device queries the manifest and, if a new version exists, shows it together with its size. The download is written to the spare partition; if verification succeeds the new image takes over at the next boot.</p><img src="GORSEL:02-admin-devices.png" style="width:100%;display:block;border-radius:10px;margin:14px 0" /><div style="font-size:13px;color:inherit;opacity:.72;margin:-6px 0 12px">Assigning a target version from the panel — <em>Push Update</em> pins a required version onto the device</div></div></div>

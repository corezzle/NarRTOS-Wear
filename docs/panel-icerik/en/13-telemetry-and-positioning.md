NarRTOS devices periodically report health data and a scan of the surrounding WiFi. The server side resolves the device's location from that scan — **without GPS hardware**, to roughly **±19 metres**.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">This page covers the device side</div><div style="line-height:1.6;color:inherit">To set up the server, choose a location provider and connect the message queue, continue on the <a href="/en/document-content/narrtos-sunucu-kurulumu-8">Server Setup</a> page.</div></div>

## How it works

The whole chain is five steps. The device is responsible for only the first two: measuring and sending. Resolving the location and publishing to external systems happen on the server side.

![Telemetry pipeline animation](GORSEL: boru-hatti.gif)

*The path a report takes from the device to the message queue*

<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px;margin:18px 0"><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Period</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">60 seconds</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">While WiFi is connected, one report is attempted each cycle.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Scan</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">Every 5th cycle</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">The list of surrounding BSSIDs is refreshed and cached.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Failure behaviour</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">Fire-and-forget</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">With no network or no server the report is skipped; the device is unaffected.</div></div></div>

## Why are the scan and the report on separate cycles?

An active WiFi scan interrupts the connection for **roughly 20 seconds**. If a cycle both scanned and POSTed, the report would always fail. So the scan cycle only refreshes the list, and sending is left to the following cycles.

![Scan and report cycle animation](GORSEL: tarama-dongusu.gif)

*A six-cycle loop and the serial output produced on each cycle*

```
#define TELEMETRY_PERIOD_MS         (60 * 1000)  // rapor periyodu
#define TELEMETRY_SCAN_EVERY_CYCLES 5            // 5 turda bir tarama

static void telemetry_task(void* arg) {
    uint32_t cycle = 0;
    for (;;) {
        if (wifi_service_is_connected()) {
            if (cycle % TELEMETRY_SCAN_EVERY_CYCLES == 0)
                refresh_wifi_scan();   // BSSID listesini önbelleğe al
            else                       //   (bu tur rapor YOK — bağlantı toparlansın)
                send_report();         // önbellekli listeyle temiz HTTP POST
        }
        cycle++;
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
```

## Location: by BSSID, not by SSID

The device collects **not the network names (SSID) but the hardware MAC addresses (BSSID)** of the access points around it. The reason is simple: an SSID is not unique, a MAC is.

![Resolving location from BSSIDs](GORSEL: 02-bssid-ile-konum.png)

*Intersection weighted by signal strength — roughly ±19 m*

- An SSID is not unique; thousands of routers carry the same name.

- Geolocation providers keep a database mapping *"the router with this MAC is at these coordinates"*.

- Scanning **does not require connecting** to the networks — hearing their beacons is enough. No password is used, and no SSID is sent.

```
typedef struct { uint8_t bssid[6]; int8_t rssi; uint8_t channel; } wifi_bssid_ap_t;

esp_err_t wifi_service_scan_bssids(wifi_bssid_ap_t* out, uint8_t max, uint8_t* out_count);
```

### The scanning trap with Bluetooth enabled

Under BT+WiFi coexistence, giving the driver custom scan durations produces an error. The `scan_time` field **must be left at 0** — the driver then picks a coex-safe default itself.

### Why does the network list in Settings use a different API?

The network list on the Settings screen uses an asynchronous scan and returns **SSIDs** — for the user to read. Geolocation, on the other hand, uses the synchronous API and wants **BSSIDs**. Two different purposes, two different calls.

## Anatomy of a report

The device sends raw data; the server embeds the location resolution into the same record. The two blocks below are two stages of the same report.

```
{
  "device_id":        "E4B063A764CC",
  "wifi_rssi":        -55,
  "wifi":             [ {"mac": "A4:B1:C2:D3:E4:F5", "rssi": -53}, ... ],
  "uptime_s":         887,
  "firmware_version": "1.4.81",
  "heap_free":        970288
}
```

```
"geo": {
  "lat": 41.0289898, "lng": 29.2229154,
  "accuracy_m": 18.9,
  "fallback": null,              // null = GERÇEK WiFi çözümü; "ipf" = IP tahmini
  "provider": "www.googleapis.com"
}
```

## The data honesty rule

When the provider does not recognise the access points, it can fall back to a **coarse IP-based estimate**. Its deviation reaches ±25 km — in testing it placed a device in Istanbul in a different city entirely. That is why a result carrying a fallback does not count as a location.

<div style="background:rgba(128,128,128,.10);border:1px solid rgba(128,128,128,.28);border-radius:12px;padding:16px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:10px;color:inherit">What reaches the consumers when the result changes</div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;font-size:11.5px;font-weight:800;letter-spacing:.6px;text-transform:uppercase;color:inherit;opacity:.72"><div>Consumer</div><div style="text-align:center">fallback: null</div><div style="text-align:center">fallback: "ipf"</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">Coordinate fields</div><div style="font-size:12.5px;color:inherit;opacity:.72">latitude / longitude</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">location-data-queue</div><div style="font-size:12.5px;color:inherit;opacity:.72">systems that consume location</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div><div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)"><div><div style="font-weight:700;color:inherit">Admin panel</div><div style="font-size:12.5px;color:inherit;opacity:.72">as a location, with an OSM link</div></div><div style="text-align:center;color:#12A0AD;font-weight:800">✓</div><div style="text-align:center;color:inherit;opacity:.72;font-weight:800">✕</div></div></div>

<div style="border-left:4px solid #D95757;background:rgba(217,87,87,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The rule is baked into the code</div><div style="line-height:1.6;color:inherit">A result carrying a fallback is <strong>never written</strong> into the coordinate fields and is <strong>never</strong> published to the location queue. It is architecturally impossible for a coarse estimate to reach systems that consume location — it is not an option in the panel.</div></div>

## Cost protection

Without a safeguard every report would be one API call: roughly **43,000 requests** per device per month. The cache reuses the result for the same set of access points.

![Location cache cost comparison](GORSEL: 05-maliyet-korumasi.png)

*Sorted MAC-set key, 10-minute TTL — a few hundred calls a month for a stationary device*

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Telemetry does not depend on location</div><div style="line-height:1.6;color:inherit">Even if the location API errors out or times out, the report proceeds without a location. The telemetry stream is under no circumstances dependent on the location service.</div></div>

## How it looks in the panel

Reports are listed in the Telemetry tab of the admin panel. The location column shows the data honesty rule directly: real resolutions are clickable coordinates, IP estimates are grey text.

![Admin panel telemetry tab](GORSEL: 01-admin-telemetri.png)

*Blue links are real WiFi resolutions; the grey row is an IP estimate (±25 km). Delivery badges: sent dry-run dropped*

| State | Meaning |
|---|---|
| dry_run | The publishing switch is off — the message is prepared and stored but not sent (a safe rehearsal mode) |
| sent | The message was delivered to the queue |
| dropped | Publishing was attempted and failed — the persistent record is kept regardless |

## It has no screen on the device

Telemetry and positioning are **headless background services**. There is no application or settings page on the watch that shows them: `telemetry_service.h` exposes only `telemetry_service_init()`, and no file under `src/apps/` references telemetry.

The only visible output on the device side is the serial log:

```
I (12034) telemetry: init OK (device_id=E4B063A764CC)
I (12040) telemetry: telemetry loop started (period=60 s, url=...)
I (72118) telemetry: wifi scan refreshed: 7 AP
I (132204) telemetry: report sent (HTTP 200, rssi=-53)
```

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The city in Weather is not this</div><div style="line-height:1.6;color:inherit">The only place the watch shows a location is the city name in the Weather application — but that is <code>weather_service</code>'s <strong>IP geolocation</strong> (ip-api.com) result, cached in NVS. It has nothing to do with the WiFi BSSID positioning described here, and its accuracy is in kilometres.</div></div>

<div style="margin:40px 0 0"><a href="/en/document-content/narrtos-sunucu-kurulumu-8" style="display:flex;align-items:center;gap:22px;flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);border-radius:14px;padding:26px 28px;text-decoration:none;color:inherit"><div><div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;text-transform:uppercase;color:#12A0AD">Next step</div><div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">Server Setup</div><div style="font-size:15.5px;color:inherit;opacity:.72;margin-top:4px">Bring the backend up, choose the location provider, connect the queue</div></div><div style="margin-left:auto;font-size:32px;color:#12A0AD">→</div></a></div>

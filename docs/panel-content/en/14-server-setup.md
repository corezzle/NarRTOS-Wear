Setting up, from scratch, the server program that collects the telemetry reports sent by the watches, resolves their location and publishes to external systems. The same program also distributes software (OTA) updates.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Looking for the device side?</div><div style="line-height:1.6;color:inherit">For how the report is produced, the BSSID scan and the data honesty rule, see the <a href="/en/document-content/narrtos-telemetri-ve-konumlandirma-7">Telemetry and Positioning</a> page.</div></div>

This page was written for someone who has never set up a server before. Follow it in order and the server will be running on your own computer, with the admin panel open in your browser. The first time takes 30-40 minutes, most of it the one-off installation of two programs (Node.js and PostgreSQL). The second time it starts with a single command.

## What does the server do?

The watches send this program regular reports over the internet: battery level, signal, nearby WiFi networks, the software version they are running. The program writes those reports to a database, resolves the device's location from the WiFi networks and shows everything in an admin panel. When you release a new software version, it is also distributed to the watches through this program.

That is why two pieces are needed: **Node.js**, which runs the program itself, and the **PostgreSQL** database where the reports are stored. Both are free and are installed on your computer.

## What you need

<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:14px;margin:18px 0"><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Runtime</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">Node.js (LTS release)</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">Runs the server program. Downloaded from nodejs.org; you will install it in step 2.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Database</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">PostgreSQL</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">Where the reports are stored. Works in its own <code>wear_os</code> schema and touches no other data. You will install it in step 3.</div></div><div style="background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);border-radius:14px;padding:20px"><div style="font-size:11.5px;font-weight:800;letter-spacing:1.2px;text-transform:uppercase;color:#12A0AD">Not needed yet</div><div style="font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit">RabbitMQ</div><div style="font-size:14px;color:inherit;opacity:.72;line-height:1.5">For forwarding reports to external systems. Without it the program still runs; only forwarding stays off.</div></div></div>

## Installation

### 1. Open a terminal

The installation is done from a window where you type commands; that window is called a terminal.

- **macOS:** open Spotlight (⌘ + space), type `Terminal` and press Enter

- **Windows:** type `PowerShell` into the Start menu and press Enter

- **Linux:** Ctrl + Alt + T

You will type the commands one at a time, pressing Enter after each. While a command is running, wait; when it finishes the prompt comes back.

### 2. Install Node.js

Go to [nodejs.org](https://nodejs.org) and download the release marked **LTS**. Go through the installer without changing any settings. On Windows, close and reopen the terminal once the installation finishes; otherwise it will not know about the new program.

Verify it is installed:

```
node --version
npm --version
```

The first should print `v20` or a higher version number, the second a number as well. `npm` comes with Node.js; you will use it to download the packages the program needs.

### 3. Install PostgreSQL

PostgreSQL is the database the reports are written to. The installation differs by operating system; follow only the section for yours. In all three you will end up with a **username** and a **password**. Write them down, you will enter them in step 6.

**Windows**

Download and run the installer from [postgresql.org/download/windows](https://www.postgresql.org/download/windows/). During the wizard:

- It asks for a **password** for the `postgres` user. Choose one and write it down.

- Leave the port question at `5432`.

- If a "Stack Builder" window opens at the end, close it; it is not needed.

Write down: username `postgres`, password the one you just chose.

**macOS**

Download Postgres.app from [postgresapp.com](https://postgresapp.com), drag it into Applications and open it. Press the **Initialize** button in the window; after a few seconds a green "Running" label appears. The database runs as long as the app is open; after restarting the computer you need to open the app again.

Write down: the username is your Mac login name (what `whoami` prints in the terminal), and there is no password.

**Linux (Ubuntu / Debian)**

```
sudo apt update
sudo apt install postgresql
```

When the installation finishes the database is already running. You need to give the `postgres` user a password:

```
sudo -u postgres psql -c "ALTER USER postgres PASSWORD 'a-password-here';"
```

Write down: username `postgres`, password the one you just typed.

### 4. Create the database

PostgreSQL is installed, but our database does not exist inside it yet. The program creates its tables itself, but it expects a database named `corezzle` to already exist. Open PostgreSQL's command line (psql) for that:

- **Windows:** open **SQL Shell (psql)** from the Start menu. Press Enter without typing anything at the Server, Database, Port and Username questions; at the password question type the password from step 3 (it is not shown while typing, that is normal).

- **macOS:** in the Postgres.app window, double-click the database icon carrying your name; psql opens in Terminal.

- **Linux:** type `sudo -u postgres psql` in the terminal.

The prompt that opens ends in `=#`. Type this command (including the semicolon at the end):

```
CREATE DATABASE corezzle;
```

If it answers `CREATE DATABASE`, it worked. Type `\q` and press Enter to leave.

### 5. Download the server files

```
git clone REPOSITORY-URL
cd corezzle-wearos-backend
```

The first line copies the files onto your computer, the second enters that folder. Every command from here on is run from inside this folder.

If you get `git: command not found`, git is not installed. If you would rather not install it, use the **Code → Download ZIP** link on the repository page, unpack the folder, then enter it in the terminal with `cd`.

### 6. Create the settings file

The program reads its settings from a file named `.env` in the folder: which user and password to connect to the database with, the admin panel's password, and so on. The file does not exist yet; create it by copying the sample:

```
# macOS / Linux
cp .env.sample .env

# Windows
copy .env.sample .env
```

Now open the file in a text editor. Because its name starts with a dot, file managers usually hide it; opening it from the terminal is easier:

```
# Windows
notepad .env

# macOS
open -e .env

# Linux
nano .env
```

You will change only three lines in it:

| Line | What to enter |
|---|---|
| `DB_USER=` | The username you wrote down in step 3 (`postgres` on Windows and Linux, your Mac login name on macOS) |
| `DB_PASSWORD=` | The password you wrote down in step 3. On macOS Postgres.app uses no password; leave everything after the equals sign empty |
| `ADMIN_TOKEN=` | The admin panel's password. Generate it with the command below and paste it in |

Leave the other lines alone; the defaults are correct for an installation on your own computer.

`ADMIN_TOKEN` needs a long value that cannot be guessed. Do not make one up yourself; run this in the terminal and copy the 64-character string it prints:

```
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

Save the file. The top of a finished `.env` looks like this (the vehicle-tracking bridge lines below it, VTS_*, stay as they are):

```
PORT=3001
DB_HOST=localhost
DB_PORT=5432
DB_NAME=corezzle
DB_USER=postgres
DB_PASSWORD=the-password-you-chose-in-step-3
DB_SCHEMA=wear_os
ADMIN_TOKEN=9f2c...(64 characters)...a71e
```

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">The file name trap on Windows</div><div style="line-height:1.6;color:inherit">When saving with "Save as", Notepad may append <code>.txt</code> to the name; the file becomes <code>.env.txt</code> and the program does not see it. After saving, type <code>dir</code> in the terminal and check that the file is named exactly <code>.env</code>.</div></div>

### 7. Download the program's packages

```
npm install
```

The server program uses ready-made libraries; this command downloads them from the internet and puts them in the folder under the name `node_modules`. It takes a minute or two. At the end you see a summary such as "added ... packages". Yellow `warn` lines along the way are normal; as long as no red `ERR!` appeared, all is well.

### 8. Create the database tables

```
npm run migration:up
```

This command connects to the `corezzle` database you created in step 4, creates a compartment (schema) named `wear_os` inside it and builds the tables the program needs. You should see `schema "wear_os" ensured` on the first line, followed by a few migration lines. If it finished without an error, the tables are ready.

If you get an error here, the cause is almost always in step 3, 4 or 6; see the "where people get stuck" table below.

### 9. Start the server

```
npm start
```

When you see this line, the server is running:

```
corezzle-wearos-backend running on port 3001
```

Leave this terminal window open; if you close it, the server stops too. Press Ctrl + C to stop it. From the second time on, all you need is to enter the folder and run `npm start` (provided PostgreSQL is running).

### 10. See that it works

Open [http://localhost:3001/admin/](http://localhost:3001/admin/) in the browser; the admin panel appears. Paste the `ADMIN_TOKEN` value you wrote into `.env` in the field at the top and press *Connect*. Empty lists are normal; no watch has sent a report yet.

To send a fake report and see the pipeline work end to end, use the commands in the **Verifying the installation** section at the bottom of the page.

## Where people get stuck on the first try

| What you see | Cause and fix |
|---|---|
| `node: command not found` | Node.js is not installed, or the terminal was not reopened after installing it (step 2) |
| `database "corezzle" does not exist` | Step 4 was skipped; create the database |
| `password authentication failed for user "postgres"` | `DB_PASSWORD` in `.env` is not the one you set during the PostgreSQL installation (step 6) |
| `role "postgres" does not exist` | You are using Postgres.app on macOS; `DB_USER` must be your Mac login name |
| `connect ECONNREFUSED 127.0.0.1:5432` | PostgreSQL is not running. On macOS open Postgres.app; on Windows start the `postgresql` service from Services |
| `EADDRINUSE ... :3001` | Another program is using port 3001, most likely an earlier copy of the server. Close the other terminal with Ctrl + C, or change `PORT` in `.env` |
| The panel opens but "Connect" says unauthorised | The token you pasted is not the one in `.env`. A space may have been left at the start or end while copying |
| I changed `.env` and nothing happened | The file is read only at startup. Stop the server with Ctrl + C and start it again with `npm start` |

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">If you are installing into a shared database</div><div style="line-height:1.6;color:inherit">The service only operates in the schema given by <code>DB_SCHEMA</code> (default <code>wear_os</code>) and creates its tables there. If you share the database with other systems, make sure that schema is yours; other schemas are not touched.</div></div>

For Docker users: `docker compose up` brings up only the server program, not PostgreSQL. The database must be running separately, and inside the container `localhost` is not your computer; on Mac and Windows use `DB_HOST=host.docker.internal`.

## Environment variables

In step 6 you changed only three variables. The full list of variables the program recognises is below; every default is suitable for an installation on your own computer.

| Variable | Default | Description |
|---|---|---|
| `PORT` | 3001 | HTTP port |
| `NODE_ENV` | development | Outside `production`, SQL debug logging is on |
| `ADMIN_TOKEN` | (none) | Bearer key for the admin endpoints. **Must be long and random.** |
| `DB_HOST` / `DB_PORT` | localhost / 5432 | PostgreSQL address |
| `DB_NAME` / `DB_USER` / `DB_PASSWORD` | corezzle / postgres / postgres | Connection details |
| `DB_SCHEMA` | wear_os | The schema the service uses |

| Variable | Default | Description |
|---|---|---|
| `GEOLOCATE_URL` | api.beacondb.net/v1/geolocate | Ichnaea-compatible endpoint |
| `GEOLOCATE_KEY` | empty | If set, appended as `?key=` |
| `GEOLOCATE_TIMEOUT_MS` | 4000 | Timeout |
| `GEOLOCATE_CACHE_TTL_MS` | 600000 | Cache lifetime (10 minutes) |

The request/response format is the **Ichnaea** standard, identical to the Google Geolocation API. Switching providers means changing nothing but these two variables:

```
GEOLOCATE_URL=https://www.googleapis.com/geolocation/v1/geolocate
GEOLOCATE_KEY=<api-key>
```

| Provider | Result |
|---|---|
| beaconDB (open, no key) | The pipeline works; where regional coverage is thin it returns an IP fallback |
| Google Geolocation API | Real resolution: **±19 m** (verified) |

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Pricing</div><div style="line-height:1.6;color:inherit">On Google's side, 10,000 requests a month are free, after which it is 5 USD per 1,000 requests. The location cache brings that cost down to a few hundred calls per device per month; see <a href="/en/document-content/narrtos-telemetri-ve-konumlandirma-7">cost protection</a> for the details.</div></div>

| Variable | Description |
|---|---|
| `OTA_SERVE_DIR` | The folder where binaries uploaded from the admin panel are written |
| `BINARY_BASE_URL` | `binary_url` is derived from this base: `{BINARY_BASE_URL}/firmware-{version}.bin` |

| Variable | Default | Description |
|---|---|---|
| `VTS_ENABLED` | false | Until it is `true`, publishing runs in dry-run |
| `VTS_TRANSPORT` | http | `rabbitmq` is the real transport; `http`/`tcp` are the legacy protobuf path |
| `RABBITMQ_URI` | amqp://127.0.0.1 | Broker address |

## Message queues

The backend publishes the report to durable queues over the `amq.direct` exchange. There are two queues and **their conditions differ**:

| Queue | When |
|---|---|
| `live-data-queue` | On every report (device health data) |
| `location-data-queue` | **Only** when there is a real location resolution (no fallback) |

<div style="border-left:4px solid #D95757;background:rgba(217,87,87,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">This distinction is a rule, not a preference</div><div style="line-height:1.6;color:inherit">A coarse IP-based estimate is never published to the location queue. You can see why, along with an interactive example, in the <a href="/en/document-content/narrtos-telemetri-ve-konumlandirma-7">data honesty rule</a> section.</div></div>

### Published message format

```
{
  "deviceType": "TRACKER",              // generic tracking device type
  "imei": "E4B063A764CC",
  "dateTime": "2026-07-21T01:46:48Z",
  "latitude": 41.0289898,               // real resolution only; otherwise 0
  "longitude": 29.2229154,
  "locationAccuracy": 18.9,
  "gsmSignalQuality": 20,
  "wifi": [ {"mac": "...", "dbms": -53}, ... ],
  "softwareVersion": "1.4.81",
  "battery": null
}
```

Vehicle-specific fields (CAN, tyre pressure, ignition, fuel) are deliberately **absent**. This format is shared by every GPS-free scenario: wearables, animals, elderly care and asset tracking.

## Admin panel

While the service is running, the panel opens at `/admin/`. Enter the `ADMIN_TOKEN` value in the field at the top and press *Connect*; the token is stored in the browser.

![Admin panel releases tab](GORSEL: 03-admin-releases.png)

*The Releases tab: channel, size, active flag and publication state*

![Admin panel devices tab](GORSEL: 02-admin-devices.png)

*The Devices tab: last seen, current version and target version assignment*

## API endpoints

| Endpoint | Access | Description |
|---|---|---|
| `/api/wear-os/manifest` | Public | Device version query |
| `/api/wear-os/events` | Public | Update events |
| `/api/wear-os/telemetry` | POST public · GET admin | Report submission and listing |
| `/api/wear-os/releases` | Admin | Release management |
| `/api/wear-os/devices` | Admin | Device management |
| `/admin/` | Static | The panel UI (its endpoints require a token) |

## Verifying the installation

Once the service is up, test the telemetry stream end to end:

```
# 1) Does the panel open
curl -s -o /dev/null -w "%{http_code}\n" http://localhost:3001/admin/

# 2) Does admin auth work (should return 200 even with an empty list)
curl -s -H "Authorization: Bearer $ADMIN_TOKEN" \
     http://localhost:3001/api/wear-os/telemetry

# 3) Send a fake report; tests the pipeline without waiting for a device
curl -s -X POST http://localhost:3001/api/wear-os/telemetry \
  -H "Content-Type: application/json" \
  -d '{"device_id":"E4B063A764CC","wifi_rssi":-53,
       "wifi":[{"mac":"A4:B1:C2:D3:E4:F5","rssi":-53}],
       "uptime_s":887,"firmware_version":"1.4.81","heap_free":970288}'
```

After the third step, a new row should appear in the panel's **Telemetry** tab. If the location column is empty, the provider did not recognise the access points; if you see a grey coordinate, an IP fallback was returned.

<div style="margin:40px 0 0"><a href="/en/document-content/narrtos-telemetri-ve-konumlandirma-7" style="display:flex;align-items:center;gap:22px;flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);border-radius:14px;padding:26px 28px;text-decoration:none;color:inherit"><div><div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;text-transform:uppercase;color:#12A0AD">Back</div><div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">Telemetry and Positioning</div><div style="font-size:15.5px;color:inherit;opacity:.72;margin-top:4px">Device side: the scan cycle, BSSID, the data honesty rule</div></div><div style="margin-left:auto;font-size:32px;color:#12A0AD">→</div></a></div>

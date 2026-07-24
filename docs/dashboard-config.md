# Dashboard configuration schema v2

Runtime path:

```text
/etc/mdvwb/dashboard.json
```

The file stores a collection of independent user-facing fan-coil panels. It is
independent from `/etc/mdvwb/buses.json`; saving a panel does not restart any
RS-485 driver process.

## Root object

```json
{
  "version": 2,
  "revision": 12,
  "defaultPanel": "main",
  "panels": []
}
```

| Field | Type | Rule |
|---|---|---|
| `version` | integer | exactly `2` |
| `revision` | integer | `0..INT_MAX`; optimistic concurrency for the complete collection |
| `defaultPanel` | string | ID of an existing panel |
| `panels` | array | 1–64 independent panels |

Unknown fields are rejected. Panel IDs are unique and use 1–48 ASCII letters,
digits, `_` or `-`.

A legacy version-1 single-panel file is accepted and migrated in memory to a
version-2 collection containing panel `main`. The next successful save writes
the canonical version-2 format.

## Panel object

```json
{
  "id": "floor-2",
  "title": "Второй этаж",
  "background": {
    "file": "background-a1b2c3.webp",
    "naturalWidth": 2400,
    "naturalHeight": 1600,
    "defaultScale": 1.25,
    "fit": "custom"
  },
  "fans": []
}
```

Each panel has its own:

- public URL ID;
- title;
- background image;
- initial image scale and fit mode;
- selected fan coils;
- user numbers, labels and marker positions.

The same physical fan coil may appear on more than one panel. User numbers need
to be unique only inside one panel.

## Public URLs

The default panel opens without a query parameter:

```text
/fancoils/
```

A specific panel opens by ID:

```text
/fancoils/?panel=main
/fancoils/?panel=floor-2
/fancoils/?panel=production
```

The user-facing page has no panel selector and no link to the engineering UI.
Different links may therefore be distributed to different user groups.

If a requested ID does not exist, the browser falls back to `defaultPanel` and
shows a warning.

## Background object

```json
{
  "file": "background-a1b2c3.webp",
  "naturalWidth": 2400,
  "naturalHeight": 1600,
  "defaultScale": 1.25,
  "fit": "custom"
}
```

Rules:

- `file` is empty before the first upload, otherwise it is a safe file name
  without directories;
- accepted formats: PNG, JPEG and WebP;
- dimensions are `0/0` without an image and `1..8192` with an image;
- `defaultScale`: `0.25..4.0`;
- `fit`: `contain`, `width`, `actual` or `custom`.

Background upload is inside the selected panel's **Parameters** drawer. There
is no permanent image-upload block in the editor.

## Fan placement

```json
{
  "id": "fan-2-18",
  "number": 101,
  "bus": 2,
  "address": 18,
  "label": "Кабинет 201",
  "x": 0.73,
  "y": 0.18,
  "markerScale": 1,
  "rotation": 0,
  "visible": true
}
```

Rules:

- `id`: unique inside the panel, 1–64 characters, `[A-Za-z0-9_-]`;
- `number`: unique user number `1..200` inside the panel;
- `bus`: `1..999`;
- `address`: `0..63`;
- one placement per `bus/address` inside the panel;
- `label`: 1–120 UTF-8 bytes;
- `x`, `y`: normalized coordinates `0..1`;
- `markerScale`: `0.5..3.0`;
- `rotation`: legacy compatibility field; the editor always writes `0` and does not render rotation;
- `visible`: controls whether the marker is rendered.

The editor snaps coordinates to a visible 1% X/Y grid. Marker size is edited
once in panel Parameters and written uniformly to every placement for schema
compatibility.

## References to buses.json

Panels derive their available bus and address catalog from `/mdvwb/config`.
They do not assume one, two or any fixed number of buses.

A panel may retain a marker whose bus or address was later removed. Such
references are reported as repairable warnings and include the owning panel ID;
they are not deleted automatically.

## Manager MQTT API

```text
/mdvwb/dashboard/config          retained canonical collection JSON
/mdvwb/dashboard/config/set      non-retained complete collection request
/mdvwb/dashboard/config/result   non-retained save result
/mdvwb/dashboard/status          retained backend status
```

The `/set` request contains the complete collection and the latest received
`revision`. On success the manager increments the global collection revision,
atomically writes `dashboard.json` and republishes canonical version-2 JSON.
A stale revision is rejected without changing the file.

## Panel-specific background upload

```text
/mdvwb/dashboard/background/upload/start
/mdvwb/dashboard/background/upload/chunk/<uploadId>/<index>
/mdvwb/dashboard/background/upload/finish/<uploadId>
/mdvwb/dashboard/background/upload/cancel/<uploadId>
/mdvwb/dashboard/background/upload/status
/mdvwb/dashboard/background/upload/result
```

The start payload identifies the selected panel:

```json
{
  "version": 1,
  "uploadId": "web-abc123",
  "fileName": "floor-2.png",
  "panelId": "floor-2",
  "size": 123456,
  "sha256": "...64 lowercase hexadecimal characters...",
  "revision": 12
}
```

The manager rejects an unknown `panelId`. At finish it checks the collection
revision again and updates only that panel's `background`. If several panels
reference the same content-addressed asset, replacing one panel's image does
not delete the asset while another panel still uses it.

Chunks remain binary, non-retained and sequential, with a maximum chunk size of
48 KiB. The complete image is limited to 10 MiB and validated from its actual
PNG/JPEG/WebP header before it is committed atomically.

## Administrative editor

The engineering editor at `/mdvwb/#dashboard` provides:

- panel selector;
- create, copy and delete operations;
- editable panel ID and title;
- default-panel selection;
- generated public URL;
- independent image upload in Parameters;
- independent fan selection, numbering and placement.

All panel changes are one revisioned transaction. Unsaved edits remain local
until **Save** is pressed.


### Step 9.8 editor interaction correction

The dashboard editor keeps the 1% grid switch inside **Panel settings**. The editor header contains no zoom controls; wheel input over the map changes only the temporary editor preview scale. The saved opening scale remains the explicit setting in the drawer. Marker rotation is no longer exposed or rendered; legacy `rotation` values are accepted for compatibility and normalized to zero on the next save.

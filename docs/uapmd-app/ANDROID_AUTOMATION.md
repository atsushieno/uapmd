# Android Project Load Automation (AI slop)

This note documents how to load a `uapmd` project on Android without driving the system document picker.

It is primarily intended for automation agents that need to reproduce or investigate project-load issues.

## Summary

There are two project-loading APIs:

- filesystem path:
  - JS: `uapmd.project.load(path)`
  - MCP: `load_project { "path": "..." }`
- persisted document handle token:
  - JS: `uapmd.project.loadHandle(token)`
  - MCP: `load_project_handle { "token": "..." }`

On Android, the preferred route is `loadHandle()`, because it matches the document-provider path that the picker uses.

## Why handle-based loading matters

The Android document picker does not give the app a plain filesystem path. It gives a `DocumentHandle`, backed by a persisted `content://...` URI permission.

`uapmd-app` now supports restoring that handle token directly and loading through the same document-provider resolution path that the picker uses.

Relevant implementation:

- JS binding: `source/tools/uapmd-app/UapmdJSRuntime.cpp`
- JS API surface: `source/tools/uapmd-app/scripts/uapmd-api.js`
- app loader: `source/tools/uapmd-app/AppModel.cpp`
- Android token persistence: `source/uapmd-file/src/DocumentProviderAndroid.cpp`
- MCP tool: `source/tools/uapmd-app/mcp/McpServer.cpp`

Today, Android `persistHandle()` simply serializes the URI string itself. In practice, the “token” is the `content://...` URI.

## Preferred automation path on Android

The easiest transport on Android is the broadcast-based JS runner:

- action: `dev.atsushieno.uapmd.RUN_JS`
- payload extra: `code` or `code_base64`
- receiver: `dev.atsushieno.uapmd/.AutomationReceiver`

The app must already be running.

## Minimal recipe

1. Launch `uapmd-app` on the device.
2. Prepare a handle token.
3. Send a JS payload that calls `uapmd.project.loadHandle(token)`.

Example JS payload:

```js
JSON.stringify(
  uapmd.project.loadHandle(
    "content://com.android.externalstorage.documents/document/primary%3ADownload%2Faugene2-samples%2Fmugene-fantasy-suite%2Fproject.uapmdz"
  )
)
```

Example shell flow:

```sh
CODE='JSON.stringify(uapmd.project.loadHandle("content://com.android.externalstorage.documents/document/primary%3ADownload%2Faugene2-samples%2Fmugene-fantasy-suite%2Fproject.uapmdz"))'
ENCODED=$(printf '%s' "$CODE" | base64)
adb shell am broadcast \
  -a dev.atsushieno.uapmd.RUN_JS \
  -n dev.atsushieno.uapmd/.AutomationReceiver \
  --es code_base64 "$ENCODED"
```

The broadcast result contains the JS return value.

Expected successful result shape:

```json
{"success":true,"error":""}
```

## Obtaining the token

### If you already know the `content://` URI

Use it directly. That is the token.

### If the project was already opened through the picker once

Reuse the same `content://` URI that the picker granted. The Android document provider restores from that URI string.

### If you only know a standard external-storage path

For paths served by Android External Storage Documents, the token can usually be derived from the file path.

For example:

- filesystem path:
  - `/sdcard/Download/augene2-samples/mugene-fantasy-suite/project.uapmdz`
- corresponding token:
  - `content://com.android.externalstorage.documents/document/primary%3ADownload%2Faugene2-samples%2Fmugene-fantasy-suite%2Fproject.uapmdz`

This mapping is specific to that provider shape. Do not assume every document source can be converted this way.

## MCP route

The same handle-based loader is also exposed through MCP:

```json
{
  "name": "load_project_handle",
  "arguments": {
    "token": "content://com.android.externalstorage.documents/document/primary%3ADownload%2Faugene2-samples%2Fmugene-fantasy-suite%2Fproject.uapmdz"
  }
}
```

Use this when you already have an MCP transport connected to the app. On Android, the JS broadcast path is usually simpler.

## Notes for crash reproduction

- `project.uapmdz` archives are valid load targets.
- A long black screen does not necessarily mean a crash. The current loader is still synchronous and may stall rendering while plugin creation finishes.
- If you are trying to reproduce an intermittent load crash, call `loadHandle()` repeatedly with the same token instead of re-driving the picker every time.

## Practical guidance for agents

- Prefer `uapmd.project.loadHandle(token)` over picker automation.
- Prefer base64 payloads over raw `--es code '...'` when the JS string is long.
- Keep the app running between attempts.
- If the app reports that the token is invalid or inaccessible, re-acquire the URI permission through the picker once and then reuse that URI.

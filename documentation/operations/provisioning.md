# Provisioning

A device needs three values to find its server: URL, token, and device id.

## Build-time defaults

`sdkconfig` bakes the common case so a build flashes-and-goes:

| Config | Meaning |
|--------|---------|
| `CONFIG_APOLLO_URL` | Websocket base, e.g. `wss://<worker>.workers.dev` |
| `CONFIG_APOLLO_TOKEN` | Shared secret, checked by the server |
| `CONFIG_APOLLO_DEVICE_ID` | Instance name; falls back to the MAC when empty |

## Per-device override

NVS namespace `apollo` (keys `url`, `token`, `device_id`) wins over the build-time values, so a provisioned device can be repointed without a rebuild. Full NVS provisioning rewrites the partition — taking the WiFi credentials with it — which is why the build-time defaults are the friendlier path.

## WiFi

Standard xiaozhi hotspot provisioning: on first boot (or after credentials fail) the device opens an access point and serves a config page.

## Navigation

Prev: [Flash](flash.md) · Next: [Upstream](../reference/upstream.md)

# SQLite Sync API (DDNet Server)

This fork can route server SQL traffic through a shared TCP SQLite sync service.

## Server Config

Add to `autoexec.cfg` / server config:

```cfg
sv_sqlite_sync_enable 1
sv_sqlite_sync_host 127.0.0.1
sv_sqlite_sync_bindaddr ""
sv_sqlite_sync_port 3901
sv_sqlite_sync_admin_key "change-me"
sv_sqlite_sync_timeout_ms 2000
sv_sqlite_sync_max_pending 512
sv_sqlite_sync_mode 0
```

- `sv_sqlite_sync_mode 0`: read-write (normal)
- `sv_sqlite_sync_mode 1`: read-only (writes are acknowledged with 0 affected rows)

## Behavior

- Existing DDNet async SQL worker threads stay unchanged.
- When enabled, DDNet creates a remote SQLite connection for READ/WRITE DB roles.
- SQL jobs are still asynchronous and do not block game ticks.
- If API is unavailable, jobs fail gracefully, server keeps running, and errors are logged.
- SQL queue has backpressure (`sv_sqlite_sync_max_pending`) to avoid silent queue overwrite under overload.

## Multi-server topology

Multiple DDNet servers can point to one sqlite-sync service and therefore one shared SQLite file:

`DDNet Server A/B/C -> TCP sqlite-sync-service -> shared SQLite DB`

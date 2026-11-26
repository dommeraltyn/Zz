# DDNet SQLite Sync Service

A standalone TCP service that serializes writes to a shared SQLite DB and serves read queries for multiple DDNet servers.

## Requirements

- Node.js 20+
- `npm install`

## Install

```bash
cd tools/sqlite-sync-service
npm install
```

## Run

```bash
SQLITE_SYNC_HOST=0.0.0.0 \
SQLITE_SYNC_PORT=3901 \
SQLITE_SYNC_DB_PATH=./ddnet-shared.sqlite \
SQLITE_SYNC_ADMIN_KEY=change-me \
SQLITE_SYNC_WRITE_QUEUE_LIMIT=4096 \
SQLITE_SYNC_BUSY_TIMEOUT_MS=5000 \
node server.js
```

## Protocol

- Transport: plain TCP
- Framing: one line per request/response (`\n`)

Requests:

- `READ <admin-key> <sql>`
- `WRITE <admin-key> <sql>`

`<sql>` may be raw SQL text (single-line) or `B64:<base64-sql>`.

Responses:

- `OK <request_id> <payload>`
- `ERR <request_id> <message>`

Payload formats:

- READ success: `READBIN <base64-binary-table>`
- WRITE success: `WRITE <affected_rows>`

`READBIN` binary layout (little-endian):

1. `u32 row_count`
2. `u32 col_count`
3. For each cell in row-major order:
   - `u8 cell_type` (`0=null`, `1=int64-text`, `2=float-text`, `3=utf8-text`, `4=blob-bytes`)
   - If type != 0: `u32 byte_len` + `byte_len` bytes

## Notes

- Service enables SQLite pragmas on startup:
  - `journal_mode=WAL`
  - `synchronous=NORMAL`
  - `busy_timeout`
- WRITE requests are processed strictly sequentially using an in-process queue.
- Queue overflow returns `ERR ... write queue full ...`.
- All requests, queue size changes, and errors are logged.

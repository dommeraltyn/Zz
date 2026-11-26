'use strict';

const net = require('node:net');
const crypto = require('node:crypto');
const Database = require('better-sqlite3');

const CONFIG = {
  host: process.env.SQLITE_SYNC_HOST || '127.0.0.1',
  port: Number(process.env.SQLITE_SYNC_PORT || 3901),
  dbPath: process.env.SQLITE_SYNC_DB_PATH || './ddnet-shared.sqlite',
  adminKey: process.env.SQLITE_SYNC_ADMIN_KEY || '',
  writeQueueLimit: Number(process.env.SQLITE_SYNC_WRITE_QUEUE_LIMIT || 4096),
  busyTimeoutMs: Number(process.env.SQLITE_SYNC_BUSY_TIMEOUT_MS || 5000),
};

if(!CONFIG.adminKey)
{
  console.error('[sqlite-sync] SQLITE_SYNC_ADMIN_KEY is required');
  process.exit(1);
}

if(!Number.isFinite(CONFIG.port) || CONFIG.port <= 0 || CONFIG.port > 65535)
{
  console.error('[sqlite-sync] SQLITE_SYNC_PORT must be in range 1..65535');
  process.exit(1);
}

if(!Number.isFinite(CONFIG.writeQueueLimit) || CONFIG.writeQueueLimit <= 0)
{
  console.error('[sqlite-sync] SQLITE_SYNC_WRITE_QUEUE_LIMIT must be > 0');
  process.exit(1);
}

if(!Number.isFinite(CONFIG.busyTimeoutMs) || CONFIG.busyTimeoutMs < 0)
{
  console.error('[sqlite-sync] SQLITE_SYNC_BUSY_TIMEOUT_MS must be >= 0');
  process.exit(1);
}

const db = new Database(CONFIG.dbPath, {timeout: CONFIG.busyTimeoutMs});
db.pragma('journal_mode = WAL');
db.pragma('synchronous = NORMAL');
db.pragma(`busy_timeout = ${CONFIG.busyTimeoutMs}`);

let requestSeq = 1;
const writeQueue = [];
let writeWorkerBusy = false;

function nextRequestId()
{
  const id = `${Date.now().toString(36)}-${requestSeq.toString(36)}`;
  requestSeq += 1;
  if(requestSeq > 0x7fffffff)
    requestSeq = 1;
  return id;
}

function oneLine(str)
{
  return String(str).replace(/[\r\n]+/g, ' ').trim();
}

function sendOk(socket, requestId, payload)
{
  socket.write(`OK ${requestId} ${payload}\n`);
}

function sendErr(socket, requestId, message)
{
  socket.write(`ERR ${requestId} ${oneLine(message)}\n`);
}

function parseSqlToken(token)
{
  if(token.startsWith('B64:'))
  {
    const encoded = token.slice(4);
    return Buffer.from(encoded, 'base64').toString('utf8');
  }
  return token;
}

function writeU32LE(parts, value)
{
  const b = Buffer.allocUnsafe(4);
  b.writeUInt32LE(value >>> 0, 0);
  parts.push(b);
}

function writeCell(parts, type, bytes)
{
  const t = Buffer.allocUnsafe(1);
  t.writeUInt8(type, 0);
  parts.push(t);
  if(type !== 0)
  {
    writeU32LE(parts, bytes.length);
    parts.push(bytes);
  }
}

function encodeReadRows(rawRows, colCount)
{
  const parts = [];
  writeU32LE(parts, rawRows.length);
  writeU32LE(parts, colCount);

  for(const row of rawRows)
  {
    for(let i = 0; i < colCount; i += 1)
    {
      const value = row[i];
      if(value === null || value === undefined)
      {
        writeCell(parts, 0, Buffer.alloc(0));
      }
      else if(Buffer.isBuffer(value))
      {
        writeCell(parts, 4, value);
      }
      else if(typeof value === 'bigint')
      {
        writeCell(parts, 1, Buffer.from(value.toString(), 'utf8'));
      }
      else if(typeof value === 'number')
      {
        if(Number.isInteger(value))
          writeCell(parts, 1, Buffer.from(String(value), 'utf8'));
        else
          writeCell(parts, 2, Buffer.from(String(value), 'utf8'));
      }
      else
      {
        writeCell(parts, 3, Buffer.from(String(value), 'utf8'));
      }
    }
  }

  return Buffer.concat(parts).toString('base64');
}

function handleRead(socket, requestId, sql)
{
  try
  {
    const stmt = db.prepare(sql).safeIntegers(true);
    const cols = stmt.columns().length;
    const rows = stmt.raw(true).all();
    const payload = encodeReadRows(rows, cols);
    sendOk(socket, requestId, `READBIN ${payload}`);
  }
  catch(err)
  {
    sendErr(socket, requestId, `read failed: ${err.message}`);
  }
}

function processWriteQueue()
{
  if(writeWorkerBusy)
    return;

  writeWorkerBusy = true;
  while(writeQueue.length > 0)
  {
    const item = writeQueue.shift();
    console.log(`[sqlite-sync] dequeue write id=${item.requestId} queue=${writeQueue.length}`);
    try
    {
      const info = db.prepare(item.sql).run();
      const changes = Number(info.changes || 0);
      sendOk(item.socket, item.requestId, `WRITE ${changes}`);
    }
    catch(err)
    {
      sendErr(item.socket, item.requestId, `write failed: ${err.message}`);
    }
  }
  writeWorkerBusy = false;
}

function enqueueWrite(socket, requestId, sql)
{
  if(writeQueue.length >= CONFIG.writeQueueLimit)
  {
    sendErr(socket, requestId, `write queue full (${writeQueue.length}/${CONFIG.writeQueueLimit})`);
    return;
  }

  writeQueue.push({socket, requestId, sql});
  console.log(`[sqlite-sync] enqueue write id=${requestId} queue=${writeQueue.length}`);
  setImmediate(processWriteQueue);
}

function handleCommand(socket, line)
{
  const requestId = nextRequestId();
  const trimmed = line.trim();
  if(!trimmed)
    return;

  const firstSpace = trimmed.indexOf(' ');
  if(firstSpace <= 0)
  {
    sendErr(socket, requestId, 'invalid request');
    return;
  }

  const cmd = trimmed.slice(0, firstSpace).toUpperCase();
  const rest = trimmed.slice(firstSpace + 1);
  const secondSpace = rest.indexOf(' ');
  if(secondSpace <= 0)
  {
    sendErr(socket, requestId, 'invalid request, expected admin-key and sql');
    return;
  }

  const adminKey = rest.slice(0, secondSpace);
  const sqlToken = rest.slice(secondSpace + 1);
  const sql = parseSqlToken(sqlToken);

  if(adminKey !== CONFIG.adminKey)
  {
    sendErr(socket, requestId, 'unauthorized');
    return;
  }

  if(!sql || !sql.trim())
  {
    sendErr(socket, requestId, 'sql is empty');
    return;
  }

  if(cmd !== 'READ' && cmd !== 'WRITE')
  {
    sendErr(socket, requestId, 'unknown command');
    return;
  }

  console.log(`[sqlite-sync] ${cmd} id=${requestId} len=${sql.length}`);
  if(cmd === 'READ')
    handleRead(socket, requestId, sql);
  else
    enqueueWrite(socket, requestId, sql);
}

const server = net.createServer((socket) => {
  const peer = `${socket.remoteAddress || '?'}:${socket.remotePort || 0}`;
  console.log(`[sqlite-sync] client connected ${peer}`);

  let buffer = '';
  socket.setEncoding('utf8');

  socket.on('data', (chunk) => {
    buffer += chunk;
    for(;;)
    {
      const nl = buffer.indexOf('\n');
      if(nl === -1)
        break;
      const line = buffer.slice(0, nl);
      buffer = buffer.slice(nl + 1);
      handleCommand(socket, line);
    }
  });

  socket.on('error', (err) => {
    console.error(`[sqlite-sync] client error ${peer}: ${err.message}`);
  });

  socket.on('close', () => {
    console.log(`[sqlite-sync] client closed ${peer}`);
  });
});

server.on('error', (err) => {
  console.error(`[sqlite-sync] server error: ${err.message}`);
  process.exit(1);
});

server.listen(CONFIG.port, CONFIG.host, () => {
  console.log(`[sqlite-sync] listening on ${CONFIG.host}:${CONFIG.port}`);
  console.log(`[sqlite-sync] db=${CONFIG.dbPath} queue_limit=${CONFIG.writeQueueLimit} busy_timeout=${CONFIG.busyTimeoutMs}ms`);
});

process.on('SIGINT', () => {
  console.log('[sqlite-sync] shutting down');
  try
  {
    db.close();
  }
  finally
  {
    process.exit(0);
  }
});

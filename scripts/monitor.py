#!/usr/bin/env python3
"""Serve a local voice and screen monitor for the device."""

import base64
import http.server
import json
import os
import re
import socketserver
import threading
import urllib.request
from pathlib import Path


DEFAULT_STATE_DIRECTORY = Path.home() / '.voicemode'
LOG = Path(
    os.environ.get(
        'VOICEMODE_MONITOR_LOG',
        str(DEFAULT_STATE_DIRECTORY / 'voicemode_live.log'),
    )
).expanduser()
ROTATED_LOG = LOG.with_name(LOG.name + '.1')
PORT = int(os.environ.get('VOICEMODE_MONITOR_PORT', '8787'))
MIN_VOICE_AUDIO_BYTES = 4
MAX_READ_BYTES = 2_000_000
TIMESTAMP_ROLLBACK_TOLERANCE_MS = 100
STRIP = re.compile(r'\x1b\[[0-9;]*m')
TS = re.compile(r'\((\d{4,})\)')
SCREEN_LOCK = threading.Lock()
SCREEN_REQUEST_ID = 1


def screen_endpoint():
    """The listener on this Mac, which holds the device connection.

    This used to be a Cloudflare Worker reached over the internet. The device
    no longer talks to one, so the screen now comes from the same local
    process that carries the call. The endpoint is loopback-only by design.
    """
    return os.environ.get('ESP32_VOICE_MCP_URL', 'http://127.0.0.1:8790/mcp')


def capture_screen():
    """Ask the Mac listener's screen tool for one JPEG frame."""
    global SCREEN_REQUEST_ID
    with SCREEN_LOCK:
        request_id = SCREEN_REQUEST_ID
        SCREEN_REQUEST_ID += 1
    body = json.dumps({
        'jsonrpc': '2.0',
        'id': request_id,
        'method': 'tools/call',
        'params': {
            'name': 'capture_screen',
            'arguments': {'quality': 70},
        },
    }).encode()
    request = urllib.request.Request(
        screen_endpoint(),
        data=body,
        headers={
            'Accept': 'application/json, text/event-stream',
            'Content-Type': 'application/json',
        },
        method='POST',
    )
    try:
        with urllib.request.urlopen(request, timeout=12) as response:
            payload = json.loads(response.read())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError('The device screen is unavailable') from error
    if 'error' in payload:
        raise RuntimeError('The device screen request failed')
    for item in payload.get('result', {}).get('content', []):
        if item.get('type') == 'image' and item.get('data'):
            return base64.b64decode(item['data'])
    raise RuntimeError('The device returned no screen frame')


def number(pattern, line):
    match = re.search(pattern, line)
    return int(match.group(1)) if match else None


def read_log_tail(path, max_bytes):
    if path.is_symlink():
        return b''
    try:
        with path.open('rb') as log_file:
            log_file.seek(0, os.SEEK_END)
            start = max(0, log_file.tell() - max_bytes)
            log_file.seek(start)
            if start:
                log_file.readline()
            return log_file.read()
    except OSError:
        return b''


def read_log_lines():
    log_chunk_list = [
        chunk
        for path in (ROTATED_LOG, LOG)
        if (chunk := read_log_tail(path, MAX_READ_BYTES // 2))
    ]
    log_bytes = b'\n'.join(log_chunk_list)
    if not log_bytes:
        return []
    return log_bytes.decode('utf-8', 'ignore').splitlines()


def read_events():
    lines = [STRIP.sub('', line).rstrip() for line in read_log_lines()]

    start, previous_timestamp = 0, None
    for index, line in enumerate(lines):
        match = TS.search(line)
        if not match:
            continue
        timestamp = int(match.group(1))
        if (
            previous_timestamp is not None
            and timestamp < previous_timestamp - TIMESTAMP_ROLLBACK_TOLERANCE_MS
        ):
            start = index
        previous_timestamp = timestamp

    event_list = []
    last_audio_frames = None
    last_received_frames = None
    for line in lines[start:]:
        match = TS.search(line)
        if not match:
            continue
        timestamp = int(match.group(1))
        played = number(r'played=(\d+)', line)
        if played is not None:
            event_list.append((timestamp, 'played', played))
        peak = number(r'peak=(\d+)', line)
        if peak is not None:
            event_list.append((timestamp, 'peak', peak))
        audio_total = number(r'audio_frames=(\d+)', line)
        received_total = number(r'received=(\d+)', line)
        received_bytes = number(r'bytes=(\d+)', line) if received_total is not None else None
        if audio_total is not None:
            audio_delta = (
                audio_total
                if last_audio_frames is None or audio_total < last_audio_frames
                else audio_total - last_audio_frames
            )
            last_audio_frames = audio_total
            last_received_frames = None
            event_list.append((timestamp, 'rx', audio_delta))
            if audio_delta > 0:
                event_list.append((timestamp, 'audio_rx', audio_delta))
        elif received_total is not None and received_bytes is not None:
            received_delta = (
                received_total
                if last_received_frames is None or received_total < last_received_frames
                else received_total - last_received_frames
            )
            last_received_frames = received_total
            event_list.append((timestamp, 'rx', received_delta))
            if received_bytes >= MIN_VOICE_AUDIO_BYTES and received_delta > 0:
                event_list.append((timestamp, 'audio_rx', received_delta))
        backlog = number(r'backlog=(\d+)', line)
        if backlog is not None:
            event_list.append((timestamp, 'backlog', backlog))
        if '<< ' in line:
            event_list.append((timestamp, 'reply', line.split('<< ', 1)[1]))
        if '>> ' in line:
            event_list.append((timestamp, 'you', line.split('>> ', 1)[1]))
        if 'No reply audio' in line:
            event_list.append((timestamp, 'recovered', 'audio stalled - call restarted'))
        if 'Encode queue is full' in line:
            event_list.append((timestamp, 'micdrop', ''))
    return event_list


def build():
    event_list = read_events()
    replies = [event for event in event_list if event[1] == 'reply']
    turns = []
    for index, (timestamp, _, text) in enumerate(replies):
        start = replies[index - 1][0] if index else timestamp - 15_000
        window = [event for event in event_list if start < event[0] <= timestamp + 1_500]
        audio_frame_values = [size for _, kind, size in window if kind == 'audio_rx']
        audio_frame_count = sum(audio_frame_values)
        loud_peaks = [peak for _, kind, peak in window if kind == 'peak' and peak > 500]
        turns.append({
            't': timestamp,
            'text': text.strip(),
            'spoken': audio_frame_count > 0,
            'audio_frames': audio_frame_count,
            'bursts': len(loud_peaks) if audio_frame_count else 0,
            'peak': max(loud_peaks) if audio_frame_count and loud_peaks else 0,
        })

    first_audio = next((timestamp for timestamp, kind, _ in event_list if kind == 'audio_rx'), None)
    backlog = [
        value for timestamp, kind, value in event_list
        if kind == 'backlog' and first_audio is not None and timestamp >= first_audio
    ]

    said = [event for event in event_list if event[1] == 'you']
    heard = []
    for index, (timestamp, _, text) in enumerate(said):
        start = said[index - 1][0] if index else timestamp - 15_000
        drops = [
            event for event in event_list
            if event[1] == 'micdrop' and start < event[0] <= timestamp + 500
        ]
        heard.append({
            't': timestamp,
            'text': text.strip(),
            'clean': not drops,
            'drops': len(drops),
        })

    conversation = sorted(
        [{'who': 'device', **turn} for turn in turns]
        + [{'who': 'you', **line} for line in heard],
        key=lambda item: item['t'],
    )
    return {
        'turns': turns[-40:],
        'conversation': conversation[-60:],
        'recovered': [
            {'t': timestamp, 'text': value}
            for timestamp, kind, value in event_list
            if kind == 'recovered'
        ],
        'stats': {
            'audio_frames': sum(size for _, kind, size in event_list if kind == 'audio_rx'),
            'mic_drops': sum(1 for _, kind, _ in event_list if kind == 'micdrop'),
            'max_backlog': max(backlog) if backlog else 0,
            'silent': sum(1 for turn in turns if not turn['spoken']),
            'total': len(turns),
        },
    }


PAGE = """<!doctype html><meta charset=utf-8><title>Voice monitor</title>
<style>
 body{background:#0d0d0f;color:#e8e8ea;font:15px/1.5 -apple-system,system-ui,sans-serif;margin:0;padding:24px}
 h1{font-size:17px;font-weight:600;margin:0 0 4px}
 .sub{color:#8a8a91;font-size:13px;margin-bottom:18px}
 .cards{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:20px}
 .card{background:#17171a;border:1px solid #26262b;border-radius:10px;padding:12px 16px;min-width:104px}
 .n{font-size:22px;font-weight:600}.k{color:#8a8a91;font-size:12px;margin-top:2px}
 .row{display:flex;gap:10px;align-items:flex-start;padding:10px 12px;border-radius:9px;background:#17171a;margin-bottom:7px;border:1px solid #26262b}
 .you{background:#121218;border-color:#20202a}
 .who{font-size:11px;color:#6e6e76;letter-spacing:.04em;margin-bottom:2px}
 .ok{color:#4ade80}.bad{color:#f87171}
 .tag{font-size:11px;font-weight:600;padding:3px 8px;border-radius:20px;white-space:nowrap;margin-top:1px}
 .tok{background:#14331f;color:#4ade80}.tbad{background:#3a1717;color:#f87171}
 .tyou{background:#152331;color:#60a5fa}.tcut{background:#3a2a17;color:#fbbf24}
 .txt{flex:1}.meta{color:#6e6e76;font-size:12px;margin-top:3px}
 .warn{background:#2a1f0d;border-color:#4a3410;color:#fbbf24;padding:10px 14px;border-radius:9px;margin-bottom:14px;font-size:13px}
</style>
<h1>Voice monitor</h1>
<div class=sub>Live from the device. Updates every 2s. Green = real voice-audio frames reached the device's playback path; this cannot prove the speaker is audible. Blue = your words reached the Mac intact.</div>
<div id=app>Waiting for the device…</div>
<script>
async function tick(){
 try{
  const d = await (await fetch('/data')).json();
  const s = d.stats;
  let h = '';
  if(!s.total) h += '<div class=warn>No replies captured yet. Is the device plugged in over USB?</div>';
  if(d.recovered.length) h += '<div class=warn>Audio stall auto-recovered '+d.recovered.length+'x — the watchdog restarted the call.</div>';
  h += '<div class=cards>'
    +'<div class=card><div class="n '+(s.silent?'bad':'ok')+'">'+(s.total-s.silent)+'/'+s.total+'</div><div class=k>replies with audio frames</div></div>'
    +'<div class=card><div class=n>'+s.audio_frames+'</div><div class=k>audio frames</div></div>'
    +'<div class=card><div class="n '+(s.max_backlog>6?'bad':'ok')+'">'+s.max_backlog+'</div><div class=k>peak backlog</div></div>'
    +'<div class=card><div class=n>'+s.mic_drops+'</div><div class=k>mic drops</div></div>'
    +'</div>';
  const esc = s => s.replace(/[<>&]/g, c => ({'<':'&lt;','>':'&gt;','&':'&amp;'}[c]));
  for(const t of d.conversation.slice().reverse()){
    const mine = t.who === 'you';
    const tag  = mine ? (t.clean ? 'HEARD' : 'CUT') : (t.spoken ? 'AUDIO RX' : 'NO AUDIO');
    const cls  = mine ? (t.clean ? 'tyou' : 'tcut') : (t.spoken ? 'tok' : 'tbad');
    const meta = mine
      ? (t.clean ? 'reached the device intact' : t.drops + ' dropped fragment(s) — the device may have misheard')
      : t.audio_frames + ' audio frame(s) · playback peak ' + t.peak;
    h += '<div class="row'+(mine?' you':'')+'"><span class="tag '+cls+'">'+tag+'</span>'
      +'<div class=txt><div class=who>'+(mine?'YOU':'DEVICE')+'</div>'+esc(t.text)
      +'<div class=meta>'+meta+'</div></div></div>';
  }
  document.getElementById('app').innerHTML = h;
 }catch(e){}
}
tick(); setInterval(tick, 2000);
</script>"""


class MonitorHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split('?', 1)[0]
        status = 200
        if path == '/data':
            body = json.dumps(build()).encode()
            content_type = 'application/json'
        elif path == '/screen':
            try:
                body = capture_screen()
                content_type = 'image/jpeg'
            except RuntimeError as error:
                body = str(error).encode()
                content_type = 'text/plain; charset=utf-8'
                status = 503
        else:
            body = PAGE.encode()
            content_type = 'text/html; charset=utf-8'
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class MonitorServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True


if __name__ == '__main__':
    MonitorServer(('127.0.0.1', PORT), MonitorHandler).serve_forever()

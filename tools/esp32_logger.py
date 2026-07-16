#!/usr/bin/env python3
import argparse
import base64
import csv
import hashlib
import os
import shlex
import socket
import sqlite3
import ssl
import struct
import sys
import threading
import time
from datetime import datetime
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin, urlparse
from urllib.request import Request, urlopen

DEFAULT_DB = os.path.join(os.path.dirname(__file__), 'esp32_data.db')
DEFAULT_HOST = 'http://192.168.1.29'

CREATE_SESSIONS_SQL = '''
CREATE TABLE IF NOT EXISTS sessions (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  start_time TEXT NOT NULL,
  stop_time TEXT,
  status TEXT NOT NULL CHECK(status IN ('active','complete'))
);
'''

CREATE_READINGS_SQL = '''
CREATE TABLE IF NOT EXISTS readings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id INTEGER NOT NULL,
  timestamp TEXT NOT NULL,
  temperature REAL NOT NULL,
  target REAL NOT NULL,
  heating_power REAL NOT NULL,
  pterm REAL,
  dterm REAL,
  iterm REAL,
  FOREIGN KEY(session_id) REFERENCES sessions(id)
);
'''

# Columns added after the initial schema; applied to pre-existing databases.
READINGS_MIGRATIONS = (
    ('pterm', 'REAL'),
    ('dterm', 'REAL'),
    ('iterm', 'REAL'),
)


def normalize_host(host):
    if not host.startswith('http://') and not host.startswith('https://'):
        host = 'http://' + host
    return host.rstrip('/')


def http_get(url, timeout=10):
    req = Request(url, method='GET')
    with urlopen(req, timeout=timeout) as resp:
        return resp.read().decode('utf-8')


def http_post(url, data, timeout=10):
    body = data.encode('utf-8')
    req = Request(url, data=body, method='POST')
    req.add_header('Content-Type', 'application/x-www-form-urlencoded')
    with urlopen(req, timeout=timeout) as resp:
        return resp.read().decode('utf-8')


def ensure_db(path):
    conn = sqlite3.connect(path, check_same_thread=False)
    conn.execute('PRAGMA foreign_keys = ON')
    conn.execute(CREATE_SESSIONS_SQL)
    conn.execute(CREATE_READINGS_SQL)
    existing = {row[1] for row in conn.execute('PRAGMA table_info(readings)')}
    for column, coltype in READINGS_MIGRATIONS:
        if column not in existing:
            conn.execute(f'ALTER TABLE readings ADD COLUMN {column} {coltype}')
    conn.commit()
    return conn


def parse_data_payload(payload):
    parts = payload.strip().split(',')
    if len(parts) != 3:
        raise ValueError(f'Expected 3 values from /data, got: {payload!r}')
    return float(parts[0]), float(parts[1]), float(parts[2])


def parse_ws_payload(payload):
    import json
    try:
        data = json.loads(payload)
    except ValueError as exc:
        raise ValueError(f'Invalid websocket JSON payload: {exc}')

    try:
        temperature = float(data['temperature'])
        target = float(data['target'])
        heating_power = float(data['heatingPower'])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError(f'Invalid websocket payload structure: {exc}')

    def _optional(key):
        value = data.get(key)
        return float(value) if value is not None else None

    pterm = _optional('pTerm')
    dterm = _optional('dTerm')
    iterm = _optional('iTerm')

    return temperature, target, heating_power, pterm, dterm, iterm


def make_ws_url(host):
    parsed = urlparse(host)
    scheme = 'ws' if parsed.scheme in ('http', '') else 'wss'
    netloc = parsed.netloc or parsed.path
    return f'{scheme}://{netloc}/ws'


class WebSocketClient:
    def __init__(self, url, timeout=10):
        parsed = urlparse(url)
        if parsed.scheme not in ('ws', 'wss'):
            raise ValueError('WebSocket URL must start with ws:// or wss://')

        hostname = parsed.hostname
        port = parsed.port or (443 if parsed.scheme == 'wss' else 80)
        resource = parsed.path or '/'
        if parsed.query:
            resource += '?' + parsed.query

        self.sock = socket.create_connection((hostname, port), timeout=timeout)
        self.sock.settimeout(timeout)
        if parsed.scheme == 'wss':
            context = ssl.create_default_context()
            self.sock = context.wrap_socket(self.sock, server_hostname=hostname)

        key = base64.b64encode(os.urandom(16)).decode('ascii')
        headers = [
            f'GET {resource} HTTP/1.1',
            f'Host: {hostname}:{port}',
            'Upgrade: websocket',
            'Connection: Upgrade',
            f'Sec-WebSocket-Key: {key}',
            'Sec-WebSocket-Version: 13',
            '\r\n',
        ]
        self.sock.sendall('\r\n'.join(headers).encode('ascii'))
        response = self._read_http_response()
        if '101' not in response.split('\r\n')[0]:
            raise ConnectionError('WebSocket handshake failed: ' + response.split('\r\n')[0])

        accept = None
        for line in response.split('\r\n'):
            if line.lower().startswith('sec-websocket-accept:'):
                accept = line.split(':', 1)[1].strip()
                break
        if accept is None:
            raise ConnectionError('Missing Sec-WebSocket-Accept in handshake response')

        expected = base64.b64encode(hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode('ascii')).digest()).decode('ascii')
        if accept != expected:
            raise ConnectionError('WebSocket handshake accept mismatch')

    def _read_http_response(self):
        data = b''
        while b'\r\n\r\n' not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data.decode('ascii', errors='ignore')

    def recv(self):
        header = self._read_exact(2)
        if not header:
            raise ConnectionError('Socket closed')
        first, second = header
        fin = (first >> 7) & 1
        opcode = first & 0x0F
        masked = (second >> 7) & 1
        length = second & 0x7F

        if length == 126:
            length = struct.unpack('>H', self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack('>Q', self._read_exact(8))[0]

        mask = b''
        if masked:
            mask = self._read_exact(4)

        payload = self._read_exact(length) if length else b''
        if masked and payload:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

        if opcode == 0x8:
            self.close()
            raise ConnectionError('WebSocket closed by server')
        if opcode == 0x9:
            self._send_pong(payload)
            return None
        if opcode == 0xA:
            return None
        if opcode != 0x1:
            return None

        return payload.decode('utf-8', errors='replace')

    def _read_exact(self, count):
        data = b''
        while len(data) < count:
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                raise ConnectionError('Socket closed while reading')
            data += chunk
        return data

    def _send_pong(self, payload=b''):
        frame = bytearray([0x8A, len(payload)])
        frame.extend(payload)
        self.sock.sendall(frame)

    def close(self):
        try:
            self.sock.close()
        except Exception:
            pass


def make_session_name():
    now = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
    return f'ESP32 log {now}'


class Recorder:
    def __init__(self, conn, host, interval=1.0):
        self.conn = conn
        self.lock = threading.Lock()
        self.host = host
        self.interval = interval
        self.session_id = None
        self.session_name = None
        self.thread = None
        self.stop_event = threading.Event()
        self.stop_requested = False
        self.verbose = True

    def _create_session(self, name):
        now = datetime.now().isoformat()
        with self.lock:
            cursor = self.conn.execute(
                'INSERT INTO sessions (name, start_time, status) VALUES (?, ?, ?)',
                (name, now, 'active')
            )
            self.conn.commit()
            return cursor.lastrowid

    def _log_reading(self, temperature, target, heating_power, pterm=None, dterm=None, iterm=None):
        timestamp = datetime.now().isoformat()
        with self.lock:
            self.conn.execute(
                'INSERT INTO readings (session_id, timestamp, temperature, target, heating_power, pterm, dterm, iterm) '
                'VALUES (?, ?, ?, ?, ?, ?, ?, ?)',
                (self.session_id, timestamp, temperature, target, heating_power, pterm, dterm, iterm),
            )
            self.conn.commit()

    def _close_db_session(self):
        if self.session_id is None:
            return False
        now = datetime.now().isoformat()
        with self.lock:
            cursor = self.conn.execute(
                'UPDATE sessions SET stop_time = ?, status = ? WHERE id = ? AND status = ?',
                (now, 'complete', self.session_id, 'active')
            )
            if cursor.rowcount:
                self.conn.commit()
                return True
        return False

    def _finalize_session(self):
        if self.session_id is None:
            return
        closed = self._close_db_session()
        if closed and self.verbose:
            print(f'Session {self.session_id} closed safely.')
        self.session_id = None
        self.session_name = None

    def set_verbose(self, enabled):
        self.verbose = bool(enabled)

    def _run(self):
        ws_url = make_ws_url(self.host)
        try:
            ws = WebSocketClient(ws_url, timeout=max(5, int(self.interval) + 5))
        except Exception as exc:
            print(f'WebSocket connect failed: {exc}')
            self._finalize_session()
            return

        print(f'Connected to websocket {ws_url}')

        try:
            while not self.stop_event.is_set():
                try:
                    message = ws.recv()
                    if message is None:
                        continue
                    temperature, target, heating_power, pterm, dterm, iterm = parse_ws_payload(message)
                    self._log_reading(temperature, target, heating_power, pterm, dterm, iterm)
                    if self.verbose:
                        extra = ''
                        if pterm is not None and dterm is not None and iterm is not None:
                            extra = f' P={pterm:.3f} D={dterm:.3f} I={iterm:.3f}'
                        print(f'[{datetime.now().isoformat()}] T={temperature:.2f}°C target={target:.2f}°C power={heating_power:.3f}{extra}')
                except socket.timeout:
                    continue
                except ConnectionError as exc:
                    print(f'WebSocket connection error: {exc}')
                    break
                except ValueError as exc:
                    print(f'Failed to parse websocket payload: {exc}')
                    continue
                except Exception as exc:
                    print(f'WebSocket error: {exc}')
                    break
        finally:
            ws.close()
            if not self.stop_requested:
                self._finalize_session()

    def start(self, interval=None, name=None):
        if self.is_active():
            print('Recording is already active.')
            return
        if interval is not None:
            self.interval = interval
        self.session_name = name or make_session_name()
        self.session_id = self._create_session(self.session_name)
        self.stop_requested = False
        self.stop_event.clear()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        print(f'Started session {self.session_id} ({self.session_name})')

    def stop(self):
        if not self.is_active():
            print('No active recording session.')
            return
        self.stop_requested = True
        self.stop_event.set()
        self.thread.join(timeout=self.interval + 2)
        if self.session_id is not None:
            stop_session(self.conn, self.session_id)
            print(f'Stopped recording session {self.session_id}.')
        self.session_id = None
        self.session_name = None
        self.thread = None

    def is_active(self):
        return self.thread is not None and self.thread.is_alive()


def print_help():
    print('Available commands:')
    print('  start [interval] [name]   Start recording data. Interval defaults to 1 second.')
    print('                            If the first argument is not a number, it is treated as the session name.')
    print('  stop                      Stop the active recording session.')
    print('  set-target <temperature>  Send a target temperature to the ESP32.')
    print('  verbose on|off            Toggle terminal readouts while recording.')
    print('  list                      Show all recorded sessions.')
    print('  status                    Show the currently active session, if any.')
    print('  export <session_id> [path]  Export a recording session to CSV. Use - for stdout.')
    print('  help                      Show this help text.')
    print('  exit                      Quit the application.')


def list_sessions(conn):
    cursor = conn.execute(
        'SELECT id, name, start_time, stop_time, status, '
        '(SELECT COUNT(*) FROM readings WHERE session_id=sessions.id) AS count '
        'FROM sessions ORDER BY start_time DESC'
    )
    rows = cursor.fetchall()
    if not rows:
        print('No sessions found.')
        return
    print(f"{'ID':>3}  {'STATUS':>8}  {'START':<19}  {'STOP':<19}  {'COUNT':>6}  NAME")
    for row in rows:
        sid, name, start_time, stop_time, status, count = row
        stop_time = stop_time or '---'
        print(f'{sid:>3}  {status:>8}  {start_time:<19}  {stop_time:<19}  {count:>6}  {name}')


def get_active_session(conn):
    cursor = conn.execute('SELECT id, name FROM sessions WHERE status = ? ORDER BY start_time DESC LIMIT 1', ('active',))
    return cursor.fetchone()


def stop_session(conn, session_id=None):
    if session_id is None:
        row = get_active_session(conn)
        if row is None:
            print('No active session to stop.')
            return
        session_id = row[0]
    now = datetime.now().isoformat()
    conn.execute('UPDATE sessions SET stop_time = ?, status = ? WHERE id = ? AND status = ?', (now, 'complete', session_id, 'active'))
    if conn.total_changes == 0:
        print(f'No active session found with id {session_id}.')
    else:
        conn.commit()
        print(f'Stopped session {session_id}.')


def export_session(conn, session_id, output_path):
    cursor = conn.execute('SELECT name, start_time, stop_time, status FROM sessions WHERE id = ?', (session_id,))
    session = cursor.fetchone()
    if session is None:
        print(f'No session found with id {session_id}.')
        return
    cursor = conn.execute(
        'SELECT id, timestamp, temperature, target, heating_power, pterm, dterm, iterm FROM readings '
        'WHERE session_id = ? ORDER BY timestamp ASC',
        (session_id,)
    )
    rows = cursor.fetchall()
    if not rows:
        print(f'Session {session_id} has no readings.')
        return
    header = ['id', 'session_id', 'timestamp', 'temperature', 'target', 'heating_power', 'pterm', 'dterm', 'iterm']
    if output_path == '-':
        writer = csv.writer(sys.stdout)
        writer.writerow(header)
        for row in rows:
            writer.writerow([row[0], session_id, row[1], row[2], row[3], row[4], row[5], row[6], row[7]])
    else:
        with open(output_path, 'w', newline='') as csvfile:
            writer = csv.writer(csvfile)
            writer.writerow(header)
            for row in rows:
                writer.writerow([row[0], session_id, row[1], row[2], row[3], row[4], row[5], row[6], row[7]])
        print(f'Exported session {session_id} to {output_path}')


def set_target(host, target):
    url = urljoin(host + '/', 'setTargetTemperature')
    payload = f'target={target}'
    response = http_post(url, payload)
    print(f'Set target temperature to {response}')


def fetch_data(host):
    url = urljoin(host + '/', 'data')
    response = http_get(url)
    return parse_data_payload(response)


def start_logging(conn, host, interval, session_name=None):
    now = datetime.now().isoformat()
    name = session_name or make_session_name()
    cursor = conn.execute('INSERT INTO sessions (name, start_time, status) VALUES (?, ?, ?)', (name, now, 'active'))
    session_id = cursor.lastrowid
    conn.commit()
    print(f'Started session {session_id} ({name})')
    print('Press Ctrl+C to stop recording.')
    try:
        while True:
            try:
                temperature, target, heating_power = fetch_data(host)
                timestamp = datetime.now().isoformat()
                conn.execute(
                    'INSERT INTO readings (session_id, timestamp, temperature, target, heating_power) VALUES (?, ?, ?, ?, ?)',
                    (session_id, timestamp, temperature, target, heating_power),
                )
                conn.commit()
                print(f'[{timestamp}] T={temperature:.2f}°C target={target:.2f}°C power={heating_power:.3f}')
            except (ValueError, HTTPError, URLError) as exc:
                print(f'Failed to fetch data: {exc}')
            time.sleep(interval)
    except KeyboardInterrupt:
        print('\nStopping recording...')
        stop_session(conn, session_id)


def parse_args():
    parser = argparse.ArgumentParser(description='ESP32 temperature logger for Rotary Crystalizer.')
    parser.add_argument('host', nargs='?', default=DEFAULT_HOST,
                        help='ESP32 host or base URL (default: http://192.168.1.29)')
    parser.add_argument('--db', default=DEFAULT_DB, help='Path to sqlite database file.')
    return parser.parse_args()


def parse_command(line):
    try:
        return shlex.split(line)
    except ValueError as exc:
        print(f'Could not parse command: {exc}')
        return []


def repl(conn, host):
    recorder = Recorder(conn, host)
    print('ESP32 logger interactive console. Type "help" for commands.')

    while True:
        try:
            line = input('> ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue

        parts = parse_command(line)
        if not parts:
            continue

        command = parts[0].lower()
        args = parts[1:]

        if command == 'help':
            print_help()
        elif command == 'exit':
            break
        elif command == 'start':
            interval = None
            name = None
            if len(args) >= 1:
                try:
                    interval = float(args[0])
                    if len(args) > 1:
                        name = ' '.join(args[1:])
                except ValueError:
                    name = ' '.join(args)
            recorder.start(interval=interval, name=name)
        elif command == 'stop':
            recorder.stop()
        elif command == 'set-target':
            if len(args) != 1:
                print('Usage: set-target <temperature>')
                continue
            try:
                target = float(args[0])
                set_target(host, target)
            except ValueError:
                print('Temperature must be a number.')
        elif command == 'verbose':
            if len(args) != 1 or args[0].lower() not in ('on', 'off'):
                print('Usage: verbose on|off')
                continue
            enabled = args[0].lower() == 'on'
            recorder.set_verbose(enabled)
            print(f'Verbose readouts {'enabled' if enabled else 'disabled'}')
        elif command == 'list':
            if recorder.is_active():
                print(f'Active recorder session {recorder.session_id}: {recorder.session_name}')
            else:
                row = get_active_session(conn)
                if row:
                    print(f'Active database session {row[0]}: {row[1]} (not currently recording in this app)')
                else:
                    print('No active session.')
        elif command == 'export':
            if len(args) == 0:
                print('Usage: export <session_id> [path]')
                continue
            try:
                session_id = int(args[0])
            except ValueError:
                print('Session ID must be a number.')
                continue
            output_path = args[1] if len(args) > 1 else f'session_{session_id}.csv'
            export_session(conn, session_id, output_path)
        else:
            print('Unknown command. Type "help" for a command list.')

    if recorder.is_active():
        recorder.stop()


def main():
    args = parse_args()
    host = normalize_host(args.host)
    conn = ensure_db(args.db)
    repl(conn, host)


if __name__ == '__main__':
    main()

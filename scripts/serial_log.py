#!/usr/bin/env python3
"""Capture the device's USB serial output for the local monitor."""

import os
import stat
import sys
import time
from pathlib import Path

import serial


DEVICE = os.environ.get('VOICEMODE_SERIAL_DEVICE', '/dev/cu.usbmodem1101')
DEFAULT_STATE_DIRECTORY = Path.home() / '.voicemode'
STATE_DIRECTORY = Path(
    os.environ.get('VOICEMODE_MONITOR_STATE_DIR', str(DEFAULT_STATE_DIRECTORY))
).expanduser()
LOG = Path(
    os.environ.get('VOICEMODE_MONITOR_LOG', str(STATE_DIRECTORY / 'voicemode_live.log'))
).expanduser()
ROTATED_LOG = LOG.with_name(LOG.name + '.1')
MAX_LOG_BYTES = 2_000_000


def ensure_private_parent():
    LOG.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    parent_stat = LOG.parent.stat()
    if parent_stat.st_uid != os.getuid():
        raise RuntimeError(f'Log directory is not owned by this user: {LOG.parent}')
    if parent_stat.st_mode & 0o077:
        if LOG.parent == DEFAULT_STATE_DIRECTORY:
            os.chmod(LOG.parent, 0o700)
        else:
            raise RuntimeError(f'Log directory is not private: {LOG.parent}')


def open_log():
    ensure_private_parent()
    try:
        log_stat = LOG.lstat()
    except FileNotFoundError:
        pass
    else:
        if stat.S_ISLNK(log_stat.st_mode):
            raise RuntimeError(f'Refusing to follow log symlink: {LOG}')
        if log_stat.st_uid != os.getuid():
            raise RuntimeError(f'Log file is not owned by this user: {LOG}')
        os.chmod(LOG, 0o600)
    flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND | getattr(os, 'O_NOFOLLOW', 0)
    descriptor = os.open(LOG, flags, 0o600)
    os.fchmod(descriptor, 0o600)
    return os.fdopen(descriptor, 'a', buffering=1, encoding='utf-8')


def rotate_log(log_file):
    log_file.flush()
    if LOG.stat().st_size <= MAX_LOG_BYTES:
        return log_file
    log_file.close()
    try:
        ROTATED_LOG.unlink()
    except FileNotFoundError:
        pass
    LOG.replace(ROTATED_LOG)
    return open_log()


def main():
    log_file = open_log()
    with log_file:
        connection = None
        while True:
            if connection is None:
                try:
                    connection = serial.Serial(DEVICE, 115200, timeout=1)
                    log_file.write('--- reader attached ---\n')
                except (serial.SerialException, OSError) as error:
                    print(f'Could not open {DEVICE}: {error}', file=sys.stderr)
                    time.sleep(2)
                    continue
            try:
                line = connection.readline().decode('utf-8', 'replace').rstrip()
            except (serial.SerialException, OSError) as error:
                print(f'Lost connection to {DEVICE}: {error}', file=sys.stderr)
                try:
                    connection.close()
                except (serial.SerialException, OSError):
                    pass
                connection = None
                time.sleep(2)
                continue
            if line:
                log_file.write(line + '\n')
                log_file = rotate_log(log_file)


if __name__ == '__main__':
    main()

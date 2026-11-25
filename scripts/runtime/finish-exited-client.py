#!/usr/bin/env python3
"""Finish only this session's client that outlived its own Unreal shutdown."""
import argparse
import json
import os
from pathlib import Path
import re
import select
import signal
import time

POLL_SECONDS = 2
GRACE_SECONDS = 30
TP = bytes(i ^ (0xa5 if i & 1 else 0xef) for i in range(256))
FINAL = re.compile(rb'\[[ 0-9]+\]\[GameThread\]LogExit: Exiting\.\r?\n'
                   rb'\[[0-9.:\-]+\]\[[ 0-9]+\]Log file closed, '
                   rb'\d{4}\.\d{2}\.\d{2}-\d{2}\.\d{2}\.\d{2}\r?\n\Z')

def ident(st):
    return st.st_dev, st.st_ino

def final_close(path, inode):
    try:
        with path.open('rb') as f:
            st = os.fstat(f.fileno())
            if ident(st) != inode:
                return False
            header = f.read(3)
            f.seek(max(0, st.st_size - 2048))
            tail = f.read(2048)
    except FileNotFoundError:
        return False
    if header == b'\x00TP':
        tail = tail.translate(TP)
    elif header == b'\xef\xbb\xbf' and not FINAL.search(tail):
        # 3.8 keeps a plain UTF-8 BOM before the TP-transformed body.
        # Plain BOM logs still use their original strict terminal match.
        tail = tail.translate(TP)
    return bool(FINAL.search(tail))

def descendant(pid, parent):
    while pid > 1:
        if pid == parent:
            return True
        stat = Path(f'/proc/{pid}/stat').read_text()
        pid = int(stat[stat.rfind(')')+2:].split()[1])
    return False

def find_client(parent, game):
    for p in Path('/proc').glob('[0-9]*'):
        try:
            if (p/'comm').read_bytes().strip() != b'Client-Win64-Sh':
                continue
            name = (p/'cmdline').read_bytes().split(b'\0',1)[0].strip().replace(b'\\',b'/')
            if re.sub(br'^[A-Za-z]:', b'', name) != os.fsencode(game):
                continue
            fd = os.pidfd_open(int(p.name))
            try:
                if descendant(int(p.name), parent):
                    return p, fd
            except OSError:
                os.close(fd)
                raise
            os.close(fd)
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return None

def was_writer(process, inode):
    for fd in (process/'fd').iterdir():
        try:
            if ident(fd.stat()) != inode:
                continue
            info = (process/'fdinfo'/fd.name).read_text()
            flags = int(re.search(r'^flags:\s*([0-7]+)', info, re.M)[1], 8)
            if (flags & os.O_ACCMODE) in (os.O_WRONLY, os.O_RDWR):
                return True
        except (FileNotFoundError, PermissionError):
            continue
    return False

def watch(parent, game, log, events):
    poll = select.poll()
    poll.register(os.pidfd_open(parent), select.POLLIN)
    target = bound = None
    while not poll.poll(int(POLL_SECONDS*1000)):
        if target is None:
            target = find_client(parent, game)
            if not target:
                continue
            process, pidfd = target
            poll.register(pidfd, select.POLLIN)
        try:
            inode = ident(log.stat())
        except FileNotFoundError:
            continue
        if bound != inode:
            # Prove the writer. A stale terminal log or a log rotation is not exit.
            if not was_writer(process, inode) or final_close(log, inode):
                continue
            bound = inode
        if not final_close(log, bound):
            continue
        if poll.poll(int(GRACE_SECONDS*1000)):
            return
        if not final_close(log, bound) or not descendant(int(process.name), parent):
            continue
        signal.pidfd_send_signal(pidfd, signal.SIGTERM)
        with events.open('a') as f:
            f.write(json.dumps(dict(client=int(process.name), marker='LogExit: Exiting. + Log file closed',
                                    grace_seconds=GRACE_SECONDS, at=time.time(), signal='SIGTERM'))+'\n')
        return

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--session-pid', required=True, type=int)
    ap.add_argument('--game-exe', required=True, type=Path)
    ap.add_argument('--log', required=True, type=Path)
    ap.add_argument('--events', required=True, type=Path)
    a = ap.parse_args()
    try:
        watch(a.session_pid, a.game_exe, a.log, a.events)
    except (FileNotFoundError, ProcessLookupError):
        pass  # Session or client exited during observation.


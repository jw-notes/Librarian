#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
# This file is part of Librarian. Additional terms under GPLv3 section 7
# and third-party notices: see the NOTICE file.
"""Librarian's Kiwix download helper.

Runs beside a Kiwix Server (kiwix-serve). Librarian on the PS4 asks it to
download collections (ZIM files) from the Kiwix online library straight into
the server's folder, so the console does not have to fetch and copy them.

Only Python 3's standard library is used. Example:

    python3 librarian-kiwix-helper.py --folder /srv/zim
    python3 librarian-kiwix-helper.py --folder /srv/zim --library /srv/zim/library.xml
    python3 librarian-kiwix-helper.py --folder /srv/zim --after "docker restart kiwix"

On its first start it makes a key and prints it; type it into Librarian
(Settings > Servers > Kiwix > Helper key). See README.md beside this file.
"""

import argparse
import hmac
import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = 1
CHUNK = 1024 * 1024
KEY_ALPHABET = "abcdefghjkmnpqrstuvwxyz23456789"  # nothing that is easy to misread


def log(message):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


class Job:
    def __init__(self, job_id, url, title, file_name, size):
        self.id = job_id
        self.url = url
        self.title = title
        self.file = file_name
        self.total = size
        self.done = 0
        self.state = "queued"  # queued, running, done, failed, cancelled
        self.error = ""
        self.cancel = False

    def as_json(self):
        return {"id": self.id, "title": self.title, "file": self.file, "url": self.url, "state": self.state,
                "done": self.done, "total": self.total, "error": self.error}


class Helper:
    def __init__(self, args):
        self.args = args
        self.folder = os.path.abspath(args.folder)
        self.jobs = []
        self.lock = threading.Lock()
        self.wake = threading.Condition(self.lock)
        self.next_id = 1
        threading.Thread(target=self.worker, daemon=True).start()

    # ---- requests ----------------------------------------------------------------

    def allowed_source(self, url):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme not in ("http", "https") or not parts.hostname:
            return False
        if self.args.any_source:
            return True
        host = parts.hostname.lower()
        return host == "kiwix.org" or host.endswith(".kiwix.org")

    def add(self, url, title, size):
        if not self.allowed_source(url):
            raise ValueError("Only files from the Kiwix online library (kiwix.org) are downloaded.")
        name = urllib.parse.unquote(os.path.basename(urllib.parse.urlsplit(url).path))
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*\.zim", name):
            raise ValueError("That address is not a ZIM file.")
        if os.path.exists(os.path.join(self.folder, name)):
            raise FileExistsError(name + " is already in the server's folder.")
        free = shutil.disk_usage(self.folder).free
        part = os.path.join(self.folder, name + ".part")
        have = os.path.getsize(part) if os.path.exists(part) else 0
        if size and size - have > free:
            raise OSError("Not enough space on the server: it needs %d MB, %d MB is free."
                          % ((size - have) // 2**20, free // 2**20))
        with self.lock:
            for job in self.jobs:
                if job.file == name and job.state in ("queued", "running"):
                    raise FileExistsError(name + " is already downloading.")
            job = Job(str(self.next_id), url, title or name, name, size)
            job.done = have
            self.next_id += 1
            self.jobs.append(job)
            # Keep the list short: finished jobs beyond the last 30 go.
            while len(self.jobs) > 30 and self.jobs[0].state not in ("queued", "running"):
                self.jobs.pop(0)
            self.wake.notify()
        log("queued %s (%s)" % (name, url))
        return job

    def cancel(self, job_id):
        with self.lock:
            for job in self.jobs:
                if job.id == job_id:
                    if job.state == "queued":
                        job.state = "cancelled"
                    job.cancel = True
                    return True
        return False

    def status(self):
        return {"app": "librarian-kiwix-helper", "version": VERSION, "folder": self.folder,
                "free_bytes": shutil.disk_usage(self.folder).free,
                "updates_library": bool(self.args.library or self.args.after)}

    # ---- downloading -------------------------------------------------------------

    def worker(self):
        while True:
            with self.lock:
                job = None
                while job is None:
                    job = next((j for j in self.jobs if j.state == "queued"), None)
                    if job is None:
                        self.wake.wait()
                job.state = "running"
            try:
                self.download(job)
                if job.cancel:
                    job.state = "cancelled"
                    part = os.path.join(self.folder, job.file + ".part")
                    if os.path.exists(part):
                        os.remove(part)
                    log("cancelled %s" % job.file)
                else:
                    self.finish(job)
                    job.state = "done"
                    log("downloaded %s" % job.file)
            except Exception as e:  # noqa: BLE001 - any failure is reported to Librarian
                job.state = "failed"
                job.error = str(e) or e.__class__.__name__
                log("failed %s: %s" % (job.file, job.error))

    def download(self, job):
        part = os.path.join(self.folder, job.file + ".part")
        have = os.path.getsize(part) if os.path.exists(part) else 0
        request = urllib.request.Request(job.url, headers={"User-Agent": "librarian-kiwix-helper/%d" % VERSION})
        if have:
            request.add_header("Range", "bytes=%d-" % have)
        with urllib.request.urlopen(request, timeout=60) as response:
            length = int(response.headers.get("Content-Length") or 0)
            if have and response.status == 206:
                mode, job.done = "ab", have
                job.total = have + length if length else job.total
            else:
                mode, job.done = "wb", 0
                job.total = length or job.total
            with open(part, mode) as out:
                while not job.cancel:
                    data = response.read(CHUNK)
                    if not data:
                        break
                    out.write(data)
                    job.done += len(data)
        if job.cancel:
            return
        if job.total and job.done != job.total:
            raise IOError("The download ended early (%d of %d bytes); try again to continue it." % (job.done, job.total))
        os.replace(part, os.path.join(self.folder, job.file))

    def finish(self, job):
        """Makes kiwix-serve see the new file."""
        path = os.path.join(self.folder, job.file)
        if self.args.library:
            result = subprocess.run([self.args.kiwix_manage, self.args.library, "add", path],
                                    capture_output=True, text=True)
            if result.returncode != 0:
                raise RuntimeError("Downloaded, but kiwix-manage failed: " + (result.stderr or result.stdout).strip())
        if self.args.after:
            result = subprocess.run(self.args.after, shell=True, capture_output=True, text=True)
            if result.returncode != 0:
                raise RuntimeError("Downloaded, but the --after command failed: " + (result.stderr or result.stdout).strip())


def make_handler(helper, key):
    class Handler(BaseHTTPRequestHandler):
        server_version = "librarian-kiwix-helper/%d" % VERSION

        def log_message(self, fmt, *args):
            pass  # requests are not logged: Librarian polls every few seconds

        def reply(self, status, body):
            data = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def authorised(self):
            given = self.headers.get("Authorization", "")
            if given.startswith("Bearer ") and hmac.compare_digest(given[7:].strip().encode(), key.encode()):
                return True
            self.reply(401, {"error": "The helper key is missing or wrong."})
            return False

        def do_GET(self):
            if not self.authorised():
                return
            if self.path == "/api/status":
                self.reply(200, helper.status())
            elif self.path == "/api/downloads":
                with helper.lock:
                    jobs = [j.as_json() for j in reversed(helper.jobs)]
                self.reply(200, {"downloads": jobs})
            else:
                self.reply(404, {"error": "No such page."})

        def do_POST(self):
            if not self.authorised():
                return
            if self.path != "/api/downloads":
                self.reply(404, {"error": "No such page."})
                return
            try:
                length = min(int(self.headers.get("Content-Length") or 0), 64 * 1024)
                body = json.loads(self.rfile.read(length) or b"{}")
                job = helper.add(str(body.get("url", "")), str(body.get("title", "")), int(body.get("size") or 0))
                self.reply(202, {"id": job.id})
            except FileExistsError as e:
                self.reply(409, {"error": str(e)})
            except (ValueError, OSError) as e:
                self.reply(400, {"error": str(e)})

        def do_DELETE(self):
            if not self.authorised():
                return
            prefix = "/api/downloads/"
            if self.path.startswith(prefix) and helper.cancel(urllib.parse.unquote(self.path[len(prefix):])):
                self.reply(200, {"cancelled": True})
            else:
                self.reply(404, {"error": "No such download."})

    return Handler


def load_key(args):
    if args.key:
        return args.key
    path = args.key_file or os.path.join(os.path.abspath(args.folder), ".librarian-helper-key")
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            key = f.read().strip()
        if key:
            return key
    key = "".join(secrets.choice(KEY_ALPHABET) for _ in range(10))
    with open(path, "w", encoding="utf-8") as f:
        f.write(key + "\n")
    try:
        os.chmod(path, 0o600)
    except OSError:
        pass
    log("made a new key in %s" % path)
    return key


def main():
    parser = argparse.ArgumentParser(description="Downloads Kiwix collections into a Kiwix Server's folder for Librarian.")
    parser.add_argument("--folder", required=True, help="the folder kiwix-serve serves ZIM files from")
    parser.add_argument("--port", type=int, default=8091, help="port to listen on (default 8091)")
    parser.add_argument("--bind", default="0.0.0.0", help="address to listen on (default: all)")
    parser.add_argument("--key", help="the key Librarian must send (default: made once, kept in the key file)")
    parser.add_argument("--key-file", help="where the key is kept (default: <folder>/.librarian-helper-key)")
    parser.add_argument("--library", help="kiwix-serve's library.xml: finished files are added with kiwix-manage "
                                          "(run kiwix-serve with --library --monitorLibrary to pick them up)")
    parser.add_argument("--kiwix-manage", default="kiwix-manage", help="the kiwix-manage program (default: on PATH)")
    parser.add_argument("--after", help="a shell command run after each download, e.g. \"docker restart kiwix\"")
    parser.add_argument("--any-source", action="store_true", help="also download from addresses outside kiwix.org")
    args = parser.parse_args()

    if not os.path.isdir(args.folder):
        sys.exit("No such folder: " + args.folder)
    if args.library and not shutil.which(args.kiwix_manage):
        sys.exit("kiwix-manage was not found (needed for --library): install kiwix-tools or pass --kiwix-manage")
    key = load_key(args)
    helper = Helper(args)
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(helper, key))
    log("Librarian's Kiwix download helper on port %d, saving to %s" % (args.port, helper.folder))
    log("key: %s  (Librarian: Settings > Servers > Kiwix > Helper key)" % key)
    if not (args.library or args.after):
        log("note: without --library or --after, restart kiwix-serve to see new collections")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

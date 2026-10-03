"""
Scripted stand-in for the Android companion app, for exercising the watchapp in the PebbleOS QEMU emulator.

It speaks the Notification Sync AppMessage protocol (see protocol.md) over libpebble2's QEMU transport,
so whole flows (new notification, phone-side dismiss, scrolling, actions) can be replayed and screenshotted
without a phone.

Usage sketch:

    phone = FakePhone(qmp_socket="/path/qmp.sock")
    phone.connect()
    phone.install("build/watch.pbw")
    phone.add("WhatsApp", "Alice", "Hello there", icon=2, color=2)
    phone.launch(for_notification=True)
    phone.press("down")
    phone.screenshot("shot.png")
"""

import json
import os
import socket
import struct
import threading
import time
import uuid as uuidlib

from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.qemu import QemuTransport
from libpebble2.protocol.apps import AppRunState, AppRunStateStart, AppRunStateStop
from libpebble2.services.appmessage import AppMessageService, ByteArray, Uint8, Uint16
from libpebble2.services.install import AppInstaller

APP_UUID = uuidlib.UUID("1c5f3908-e3ea-419b-ae55-7f167ea8fafa")
PROTOCOL_VERSION = 10

QMP_KEYS = {"back": "left", "select": "right", "up": "up", "down": "down"}


class Notification:
    def __init__(self, bucket_id, app, title, body, icon=0, color=0, timestamp=None, actions=None,
                 unread=True, image=None):
        self.bucket_id = bucket_id
        self.app = app
        self.title = title
        self.body = body
        self.icon = icon
        self.color = color
        self.timestamp = int(timestamp if timestamp is not None else time.time())
        self.actions = actions or ["Dismiss", "Snooze", "Reply"]
        self.unread = unread
        self.version = 0
        # Optional PIL image attached to the notification (shown inline on Emery).
        self.image = image

    def image_aspect(self):
        if self.image is None:
            return 0
        w, h = self.image.size
        return max(4, min(24, (h * 16 + w // 2) // w))

    def bucket_bytes(self, limit=220):
        tag = (id(self.image) & 0xFF) or 1 if self.image is not None else 0
        data = struct.pack(">IBBBB", int(self.timestamp), self.icon, self.color, self.image_aspect(), tag)
        data += self.app.encode()[:24] + b"\0"
        data += self.title.encode()[:40] + b"\0"
        remaining = limit - len(data)
        data += self.body.encode()[:max(remaining, 0)]
        return data

    def flags(self):
        return 0x01 if self.unread else 0x00


class FakePhone:
    def __init__(self, host="localhost", port=12344, qmp_socket=None, log=print):
        self.host = host
        self.port = port
        self.qmp_socket = qmp_socket
        self.log = log
        self.pebble = None
        self.appmessage = None
        self.notifications = {}
        self.settings = bytes([0x00]) + struct.pack(">HH", 0, 10) + bytes([1, 1, 1])
        self.settings_version = 1
        self.version = 1
        self.watch_version = None
        self.watch_buffer = 0
        self.connected_to_app = False
        self.next_launch_bucket = None
        self.details_requests = []
        self.image_requests = []
        self.actions_received = []
        self.ignore_detail_requests = False
        self.detail_chunk_size = None
        self._send_lock = threading.Lock()
        self._acks = {}
        self._ack_event = threading.Condition()
        self._next_bucket = 2

    # ----------------------------------------------------------------- connection

    def connect(self):
        self.pebble = PebbleConnection(QemuTransport(self.host, self.port))
        self.pebble.connect()
        self.pebble.run_async()
        self.appmessage = AppMessageService(self.pebble)
        self.appmessage.register_handler("appmessage", self._on_appmessage)
        self.appmessage.register_handler("ack", self._on_ack)
        self.appmessage.register_handler("nack", self._on_nack)

    def install(self, pbw_path):
        AppInstaller(self.pebble, pbw_path).install()

    def launch(self, for_notification=None):
        """Start the watchapp. for_notification=bucket id (or True for newest) mimics a phone-triggered open."""
        if for_notification is True:
            for_notification = self.newest_bucket()
        self.next_launch_bucket = for_notification
        self.pebble.send_packet(AppRunState(data=AppRunStateStart(uuid=APP_UUID)))

    def stop(self):
        self.pebble.send_packet(AppRunState(data=AppRunStateStop(uuid=APP_UUID)))
        self.connected_to_app = False

    # ----------------------------------------------------------------- QMP input

    def _qmp(self, command, arguments=None):
        s = socket.socket(socket.AF_UNIX)
        s.connect(self.qmp_socket)
        f = s.makefile("rw")
        f.readline()
        f.write(json.dumps({"execute": "qmp_capabilities"}) + "\n")
        f.flush()
        f.readline()
        payload = {"execute": command}
        if arguments:
            payload["arguments"] = arguments
        f.write(json.dumps(payload) + "\n")
        f.flush()
        while True:
            line = f.readline()
            if not line:
                break
            response = json.loads(line)
            if "return" in response or "error" in response:
                break
        s.close()
        return response

    def press(self, button, hold_ms=60, times=1, gap=0.25):
        for _ in range(times):
            self._qmp("send-key", {"keys": [{"type": "qcode", "data": QMP_KEYS[button]}], "hold-time": hold_ms})
            time.sleep(gap + hold_ms / 1000.0)

    def key(self, button, down):
        """Raw key down/up, for press-and-hold."""
        self._qmp("input-send-event", {"events": [
            {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": QMP_KEYS[button]}}}]})

    # Touch (Emery/Gabbro QEMU only). Coordinates are in screen pixels.
    def _touch_move(self, x, y, size=(200, 228)):
        self._qmp("input-send-event", {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / size[0])}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / size[1])}},
        ]})

    def _touch_button(self, down):
        self._qmp("input-send-event", {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]})

    def tap(self, x, y):
        self._touch_move(x, y)
        self._touch_button(True)
        time.sleep(0.08)
        self._touch_button(False)

    def drag(self, x0, y0, x1, y1, duration=0.3, steps=10, hold_end=0.0):
        """Drag from (x0, y0) to (x1, y1). hold_end pauses before lifting (kills the fling)."""
        self._touch_move(x0, y0)
        self._touch_button(True)
        for i in range(1, steps + 1):
            time.sleep(duration / steps)
            self._touch_move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
        if hold_end:
            time.sleep(hold_end)
        self._touch_button(False)

    def screenshot(self, path):
        ppm = path + ".ppm"
        self._qmp("screendump", {"filename": ppm})
        time.sleep(0.2)
        from PIL import Image
        Image.open(ppm).save(path)
        os.remove(ppm)
        return path

    # ----------------------------------------------------------------- phone model

    def newest_bucket(self):
        if not self.notifications:
            return None
        return max(self.notifications.values(), key=lambda n: (n.timestamp, n.bucket_id)).bucket_id

    def add(self, app, title, body, sync=True, **kwargs):  # image=PIL.Image attaches a photo
        bucket_id = self._next_bucket
        self._next_bucket += 1
        if self._next_bucket > 15:
            self._next_bucket = 2
        notification = Notification(bucket_id, app, title, body, **kwargs)
        self.version += 1
        notification.version = self.version
        self.notifications[bucket_id] = notification
        if sync:
            self.sync()
        return bucket_id

    def update(self, bucket_id, sync=True, **changes):
        notification = self.notifications[bucket_id]
        for key, value in changes.items():
            setattr(notification, key, value)
        self.version += 1
        notification.version = self.version
        if sync:
            self.sync()

    def remove(self, bucket_id, sync=True):
        self.notifications.pop(bucket_id, None)
        self.version += 1
        if sync:
            self.sync()

    def set_text_size(self, size, sync=True):
        """0 = small, 1 = default, 2 = large."""
        self.settings = self.settings[:5] + bytes([size]) + self.settings[6:]
        self.version += 1
        self.settings_version = self.version
        if sync:
            self.sync()

    def active_buckets(self):
        ordered = sorted(self.notifications.values(), key=lambda n: (-n.timestamp, n.bucket_id))[:14]
        return [(1, 0)] + [(n.bucket_id, n.flags()) for n in ordered]

    def _bucket_payloads(self, since_version):
        payloads = []
        if since_version < self.settings_version:
            payloads.append((1, self.settings))
        active_ids = {bucket_id for bucket_id, _ in self.active_buckets()}
        for n in self.notifications.values():
            if n.bucket_id in active_ids and n.version > since_version:
                payloads.append((n.bucket_id, n.bucket_bytes()))
        return payloads

    def _sync_packets(self, first_packet_status_prefix, since_version, budget):
        """Builds the sync byte arrays: first packet carries metadata, followers carry leftover buckets."""
        active = self.active_buckets()
        payloads = self._bucket_payloads(since_version)
        header = struct.pack(">HB", self.version, len(active))
        for bucket_id, flags in active:
            header += bytes([bucket_id, flags])

        packets = []
        current = bytearray(header)
        for bucket_id, data in payloads:
            chunk = bytes([bucket_id, len(data)]) + data
            if len(current) + len(chunk) + 1 > budget and len(current) > len(header):
                packets.append(current)
                current = bytearray()
            current += chunk
        packets.append(current)

        result = []
        for i, body in enumerate(packets):
            status = 1 if i == len(packets) - 1 else 0
            result.append(bytes([status]) + bytes(body))
        return result

    def sync(self):
        if not self.connected_to_app or self.watch_version is None:
            return
        budget = (self.watch_buffer or 2000) - 40
        packets = self._sync_packets(None, self.watch_version, budget)
        self._send({0: Uint8(2), 1: ByteArray(packets[0])})
        for extra in packets[1:]:
            self._send({0: Uint8(3), 1: ByteArray(extra)})
        self.watch_version = self.version

    def vibrate(self, pattern=(200, 100, 200)):
        data = b"".join(struct.pack(">H", p) for p in pattern)
        self._send({0: Uint8(7), 1: ByteArray(data)})

    # ----------------------------------------------------------------- AppMessage plumbing

    def _on_ack(self, txid, uuid):
        with self._ack_event:
            self._acks[txid] = True
            self._ack_event.notify_all()

    def _on_nack(self, txid, uuid):
        with self._ack_event:
            self._acks[txid] = False
            self._ack_event.notify_all()

    def _send(self, dictionary, retries=8):
        with self._send_lock:
            delay = 0.1
            for _ in range(retries):
                txid = self.appmessage.send_message(APP_UUID, dictionary)
                with self._ack_event:
                    deadline = time.time() + 3
                    while txid not in self._acks and time.time() < deadline:
                        self._ack_event.wait(0.1)
                    result = self._acks.pop(txid, None)
                if result:
                    return True
                time.sleep(delay)
                delay *= 2
            self.log("send failed: %r" % (list(dictionary.keys()),))
            return False

    def _on_appmessage(self, txid, uuid, data):
        packet_id = data.get(0)
        threading.Thread(target=self._handle_packet, args=(packet_id, data), daemon=True).start()

    def _handle_packet(self, packet_id, data):
        self.log("watch -> phone packet %s %s" % (packet_id, {k: v for k, v in data.items() if k != 0}))
        if packet_id == 0:
            self._on_welcome(data)
        elif packet_id == 4:
            self.details_requests.append(data[1])
            if not self.ignore_detail_requests:
                self.send_details(data[1])
        elif packet_id == 6:
            self.actions_received.append(data)
            bucket_id, action_id = data[1], data[2]
            if data.get(3, 0) == 0 and action_id == 0:
                self.remove(bucket_id)
        elif packet_id == 8:
            self.connected_to_app = False
            self.stop()
        elif packet_id == 16:
            self.image_requests.append((data[1], data[2], data[3]))
            self.send_image(data[1], data[2], data[3])

    def send_image(self, bucket_id, width, height):
        """Serve a notification photo the way the phone app does: centre-crop, 16 colours, 4 bpp."""
        from PIL import Image
        n = self.notifications.get(bucket_id)
        if n is None or n.image is None:
            self._send({0: Uint8(16), 1: ByteArray(bytes([bucket_id, 0x04, 0, 0]))})
            return
        src = n.image.convert("RGB")
        sw, sh = src.size
        if sw * height > sh * width:
            cw = sh * width // height
            src = src.crop(((sw - cw) // 2, 0, (sw - cw) // 2 + cw, sh))
        else:
            ch = sw * height // width
            src = src.crop((0, (sh - ch) // 2, sw, (sh - ch) // 2 + ch))
        src = src.resize((width, height), Image.BILINEAR)
        q = src.quantize(16, dither=Image.FLOYDSTEINBERG)
        pal = q.getpalette()[:48]
        colors = len(set(q.getdata()))
        palette = bytes(0xC0 | ((round(pal[i * 3] / 85)) << 4) | ((round(pal[i * 3 + 1] / 85)) << 2)
                        | round(pal[i * 3 + 2] / 85) for i in range(16))
        idx = list(q.getdata())
        stride = (width + 1) // 2
        pixels = bytearray(stride * height)
        for y in range(height):
            for x in range(width):
                v = idx[y * width + x] & 0x0F
                b = y * stride + x // 2
                pixels[b] |= (v << 4) if x % 2 == 0 else v
        budget = (self.watch_buffer or 2000) - 40
        header = struct.pack(">HHB", width, height, 16) + palette
        offset = 0
        first = True
        while offset < len(pixels) or first:
            room = budget - 4 - (len(header) if first else 0)
            chunk = bytes(pixels[offset:offset + room])
            flags = (0x01 if first else 0) | (0x02 if offset + len(chunk) >= len(pixels) else 0)
            payload = bytes([bucket_id, flags]) + struct.pack(">H", offset) + (header if first else b"") + chunk
            self._send({0: Uint8(16), 1: ByteArray(payload)})
            offset += len(chunk)
            first = False

    def _on_welcome(self, data):
        self.watch_buffer = data.get(3, 0)
        watch_version = data.get(2, 0)
        hello = {0: Uint8(1), 1: Uint16(PROTOCOL_VERSION)}
        if self.next_launch_bucket:
            hello[4] = Uint8(self.next_launch_bucket)
        self.next_launch_bucket = None

        active_on_watch = list(data.get(7, b""))
        expected = [bucket_id for bucket_id, _ in self.active_buckets()]
        if watch_version == self.version and active_on_watch == expected:
            hello[2] = ByteArray(bytes([2]))
            self._send(hello)
        else:
            since = watch_version if 0 < watch_version <= self.version else 0
            budget = (self.watch_buffer or 2000) - 60
            packets = self._sync_packets(None, since, budget)
            hello[2] = ByteArray(packets[0])
            self._send(hello)
            for extra in packets[1:]:
                self._send({0: Uint8(3), 1: ByteArray(extra)})
        self.watch_version = self.version
        self.connected_to_app = True

    def send_details(self, bucket_id):
        notification = self.notifications.get(bucket_id)
        if notification is None:
            return
        notification.unread = False
        header = bytearray([bucket_id, 1, len(notification.actions)])
        for i, action in enumerate(notification.actions):
            header += bytes([i]) + action.encode()[:20] + b"\0"
        header += struct.pack(">H", 0)
        body = notification.body.encode()[:3500]
        chunk = self.detail_chunk_size or max(200, (self.watch_buffer or 2000) - 80 - len(header))
        chunks = [body[i:i + chunk] for i in range(0, len(body), chunk)] or [b""]
        header[1] = len(chunks)
        self._send({0: Uint8(13), 1: ByteArray(bytes(header) + chunks[0])})
        for index, part in enumerate(chunks[1:], start=1):
            self._send({0: Uint8(14), 1: ByteArray(bytes([bucket_id, index, len(chunks)]) + part)})

"""Public-release screenshots with made-up content.

1. EMU_DIR=<dir with <platform>/qemu images> QEMU=<qemu-system-arm> ./run_emulator.sh <platform>
   (images: the "emulator-images" release, built by .github/workflows/emulator-images.yaml)
2. python screenshots.py <pbw> <platform> <outdir>     (platform: basalt, diorite, emery, flint, gabbro)
"""
import os, sys, time, random
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fakephone import FakePhone
from PIL import Image, ImageDraw, ImageFilter

pbw, platform, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
D = os.environ.get('EMU_DIR', '.')
SIZE = {'emery': (200, 228), 'gabbro': (260, 260)}.get(platform, (144, 168))


def photo():
    """A made-up sunset landscape (no real place or person)."""
    w, h = 900, 600
    img = Image.new('RGB', (w, h))
    d = ImageDraw.Draw(img)
    for y in range(h):
        t = y / h
        d.line([(0, y), (w, y)], fill=(int(250 - 60 * t), int(150 - 80 * t + 40 * (1 - t)), int(90 + 110 * t)))
    d.ellipse([560, 250, 700, 390], fill=(255, 214, 120))
    random.seed(4)
    pts = [(0, 420)] + [(x, 400 - random.randint(0, 120) * (1 if x % 180 else 0.4)) for x in range(0, w + 60, 60)] + [(w, h), (0, h)]
    d.polygon(pts, fill=(70, 50, 90))
    pts2 = [(0, 500)] + [(x, 470 - random.randint(0, 60)) for x in range(0, w + 45, 45)] + [(w, h), (0, h)]
    d.polygon(pts2, fill=(35, 30, 55))
    return img.filter(ImageFilter.GaussianBlur(1.2))


p = FakePhone(qmp_socket=D + '/qmp.sock')
p.connect()
p.install(pbw)
time.sleep(4)  # let a launch triggered by the install settle before navigating
actions = ["Dismiss", "Reply", "Snooze", "Pause convo", "Pause app", "Open on phone"]
now = time.time()
p.add('Telegram', 'Book Club', 'The next meeting moved to Thursday at 7. Bring your notes on chapters 4 to 6!',
      icon=6, color=3, timestamp=now - 7200, actions=actions)
p.add('Weather', 'Rain this afternoon', 'Light rain expected from 3 PM. Grab an umbrella on your way out.',
      icon=30, color=13, timestamp=now - 3600, actions=actions)
p.add('WhatsApp', 'Family', 'Dinner at 7 tonight? I\'ll bring dessert \U0001F370',
      icon=2, color=2, timestamp=now - 1500, actions=actions)
p.add('Calendar', 'Lunch with Jordan', 'Today, 12:30 PM\nCorner Café, Main Street',
      icon=18, color=13, timestamp=now - 900, actions=actions)
p.add('Gmail', 'Weekly project update', 'Hi team, here is where things stand this week. The new design is in '
      'review, testing starts Monday, and the launch is still on track for the end of the month. Thanks, Alex',
      icon=1, color=1, timestamp=now - 420, actions=actions)
p.add('Messages', 'Sam', 'Made it to the top just in time for sunset. Worth the climb!',
      icon=9, color=6, timestamp=now - 60, actions=actions, image=photo())


def shot(name):
    raw = os.path.join(out, '_raw.png')
    p.screenshot(raw)
    img = Image.open(raw).convert('RGB')
    w, h = img.size
    x, y = (w - SIZE[0]) // 2, (h - SIZE[1]) // 2
    img = img.crop((x, y, x + SIZE[0], y + SIZE[1]))
    if platform in ('diorite', 'flint', 'aplite'):
        # Black and white screen: the emulator draws "white" as light grey.
        img = img.convert('L').point(lambda v: 255 if v > 100 else 0).convert('RGB')
    img.save(os.path.join(out, f'{platform}_{name}.png'))
    os.remove(raw)


def to_watchface():
    for _ in range(4):
        p.press('back')
        time.sleep(0.4)


# Open the app from the watch's own app menu (a phone launch would jump straight into a notification).
opened = False
for downs in range(0, 20):
    to_watchface()
    p.connected_to_app = False
    p.press('select'); time.sleep(1.0)
    for _ in range(downs):
        p.press('down', gap=0.15)
    time.sleep(0.4)
    p.press('select')
    t0 = time.time()
    while time.time() - t0 < 2.5 and not p.connected_to_app:
        time.sleep(0.1)
    if p.connected_to_app:
        opened = True
        break
print('opened app:', opened)
time.sleep(3)
p.press('up'); time.sleep(0.6)  # wakes the backlight; the list is already at the top
shot('1_list')

p.press('select'); time.sleep(1.4)  # the newest notification (with the photo on Emery)
if platform in ('emery', 'gabbro'):
    time.sleep(4)                    # let the photo arrive
    p.press('up'); time.sleep(0.6)   # already at the top: only wakes the backlight
shot('2_notification')
if platform in ('emery', 'gabbro'):
    p.press('down'); time.sleep(0.8)
    shot('3_notification_scrolled')
p.press('select'); time.sleep(1.0)
shot('4_actions')
p.press('back'); time.sleep(0.6)   # close the actions
p.press('back'); time.sleep(0.8)   # back to the list
for _ in range(4):
    p.press('up', gap=0.2)
time.sleep(0.4)
p.press('down'); time.sleep(0.4)
p.press('select'); time.sleep(1.2)
shot('5_email')
print('image requests', p.image_requests)

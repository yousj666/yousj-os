import socket, subprocess, sys, time, re, os, struct, threading

# Interactive M72 acceptance: boot the Multiboot chain with a
# legacy virtio-net-pci on a QEMU socket netdev whose other
# end is this script. The script plays the peer: it asks ARP
# for the guest (10.0.2.15) and expects a correct reply, asks
# for a stranger and expects silence, then has nettest ask
# ARP for the gateway (10.0.2.2) and checks the request on
# the wire. ALL OK or exit 1.

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUEST_MAC = bytes([0x52, 0x54, 0x00, 0x12, 0x34, 0x56])
PEER_MAC = bytes([0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc])
GUEST_IP = bytes([10, 0, 2, 15])
PEER_IP = bytes([10, 0, 2, 2])

for p in ('/tmp/serNET',):
    try: os.unlink(p)
    except FileNotFoundError: pass

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind(('127.0.0.1', 0))
server.listen(1)
port = server.getsockname()[1]

qemu = subprocess.Popen(['qemu-system-x86_64','-m','128','-kernel','build/yousj-stub.elf',
    '-initrd','build/yousj-kernel.elf','-display','none',
    '-serial','unix:/tmp/serNET,server,nowait','-monitor','none','-no-reboot',
    '-netdev',f'socket,id=n0,connect=127.0.0.1:{port}',
    '-device','virtio-net-pci,netdev=n0,disable-modern=on,mac=52:54:00:12:34:56'],
    cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

conn, _ = server.accept()
conn.settimeout(0.3)
frames = []
wirebuf = b''
def wire_reader():
    global wirebuf
    while True:
        try:
            d = conn.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return
        if not d:
            return
        wirebuf += d
        while len(wirebuf) >= 4:
            (n,) = struct.unpack('>I', wirebuf[:4])
            if len(wirebuf) < 4 + n:
                break
            frames.append(wirebuf[4:4 + n])
            wirebuf = wirebuf[4 + n:]
threading.Thread(target=wire_reader, daemon=True).start()

def send_frame(frame):
    conn.sendall(struct.pack('>I', len(frame)) + frame)

for _ in range(50):
    if os.path.exists('/tmp/serNET'): break
    time.sleep(0.2)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.connect('/tmp/serNET'); s.settimeout(0.5)
buf = b''
def until(needle, timeout=30, start=0):
    global buf
    end = time.time() + timeout
    while needle.encode() not in buf[start:]:
        if time.time() > end: return False
        try:
            d = s.recv(65536)
            if d: buf += d
        except socket.timeout: pass
    return True
def send(line):
    s.send((line + '\n').encode()); time.sleep(0.3)
def free_pages(mark):
    send('free')
    if not until('free pages: ', 8, mark): return None
    seg = buf[mark:].decode(errors='replace')
    m = re.search(r'free pages: (\d+)', seg)
    return int(m.group(1)) if m else None

fails = []
def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name)
    if not cond: fails.append(name)

def arp_packet(oper, sha, spa, tha, tpa):
    return (b'\x00\x01' + b'\x08\x00' + bytes([6, 4]) +
            struct.pack('>H', oper) + sha + spa + tha + tpa)

def eth(dst, src, etype, payload):
    return dst + src + struct.pack('>H', etype) + payload

def wait_frame(pred, timeout=12):
    end = time.time() + timeout
    while time.time() < end:
        for f in list(frames):
            if pred(f):
                return f
        time.sleep(0.1)
    return None

check('link up with our MAC', until('net: link up mac=52:54:00:12:34:56', 45))
check('shell prompt', until('sh> ', 45))
f0 = free_pages(len(buf))

# 1. ARP for the guest: expect a correct reply.
frames.clear()
send_frame(eth(b'\xff' * 6, PEER_MAC, 0x0806,
               arp_packet(1, PEER_MAC, PEER_IP, b'\x00' * 6, GUEST_IP)))
rep = wait_frame(lambda f: len(f) >= 42 and f[12:14] == b'\x08\x06')
ok = rep is not None
if ok:
    a = rep[14:42]
    ok = (rep[0:6] == PEER_MAC and rep[6:12] == GUEST_MAC and
          a[6:8] == b'\x00\x02' and a[8:14] == GUEST_MAC and
          a[14:18] == GUEST_IP and a[18:24] == PEER_MAC and
          a[24:28] == PEER_IP)
check('ARP request for guest answered correctly', ok)

# 2. ARP for a stranger: expect silence.
frames.clear()
send_frame(eth(b'\xff' * 6, PEER_MAC, 0x0806,
               arp_packet(1, PEER_MAC, PEER_IP, b'\x00' * 6,
                          bytes([10, 0, 2, 99]))))
time.sleep(2.5)
check('ARP for a stranger ignored', len(frames) == 0)

# 3. The guest asks for the gateway (nettest arp): expect its
# request on the wire and counters moved in netinfo.
frames.clear()
mark = len(buf)
send('nettest arp')
got = wait_frame(lambda f: len(f) >= 42 and f[12:14] == b'\x08\x06', 15)
ok = got is not None
if ok:
    a = got[14:42]
    ok = (got[0:6] == b'\xff' * 6 and got[6:12] == GUEST_MAC and
          a[6:8] == b'\x00\x01' and a[8:14] == GUEST_MAC and
          a[14:18] == GUEST_IP and a[24:28] == PEER_IP)
check('guest ARP request for gateway on the wire', ok)
check('nettest done', until('NETTEST DONE', 30, mark))
seg = buf[mark:].decode(errors='replace')
m = re.search(r'arp_replied=(\d+)', seg)
check('netinfo counts the reply', m is not None and int(m.group(1)) >= 1)

f1 = free_pages(len(buf))
check('PMM steady state', f0 is not None and f0 == f1)

print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)

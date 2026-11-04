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
# M79: the frames above must have raised the NIC's own IRQ
# at least once (the tick poll alone would leave this 0).
m = re.search(r'net_irq=(\d+)', seg)
check('NIC interrupt fired', m is not None and int(m.group(1)) >= 1)

# 4. (M73) The peer pings the guest: expect a correct echo
# reply, both checksums verifiable.
def csum(data):
    if len(data) % 2: data += b'\x00'
    s = sum(struct.unpack('>%dH' % (len(data) // 2), data))
    while s >> 16: s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF

def ip_packet(src, dst, proto, payload, corrupt=False):
    hdr = struct.pack('>BBHHHBBH', 0x45, 0, 20 + len(payload),
                      0x2222, 0, 64, proto, 0) + src + dst
    c = 0 if corrupt else csum(hdr)
    hdr = hdr[:10] + struct.pack('>H', c) + hdr[12:]
    return hdr + payload

def icmp_echo(type_, id_, seq, data):
    msg = struct.pack('>BBHHH', type_, 0, 0, id_, seq) + data
    return msg[:2] + struct.pack('>H', csum(msg)) + msg[4:]

frames.clear()
ping_data = b'yousj-ping-4'
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               ip_packet(PEER_IP, GUEST_IP, 1,
                         icmp_echo(8, 0x1234, 7, ping_data))))
rep = wait_frame(lambda f: len(f) >= 14 and f[12:14] == b'\x08\x00', 12)
ok = rep is not None
if ok:
    ip = rep[14:]
    ic = ip[20:]
    ok = (rep[0:6] == PEER_MAC and rep[6:12] == GUEST_MAC and
          ip[12:16] == GUEST_IP and ip[16:20] == PEER_IP and
          csum(ip[:20]) == 0 and ic[0] == 0 and
          ic[4:8] == struct.pack('>HH', 0x1234, 7) and
          ic[8:] == ping_data and csum(ic) == 0)
check('guest answers a ping correctly', ok)

# 5. A packet with a corrupted IP checksum is dropped.
frames.clear()
mark = len(buf)
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               ip_packet(PEER_IP, GUEST_IP, 1,
                         icmp_echo(8, 0x1234, 8, ping_data),
                         corrupt=True)))
time.sleep(2.5)
check('corrupt-checksum packet gets no reply', len(frames) == 0)
send('nettest')
check('nettest done (corrupt check)', until('NETTEST DONE', 30, mark))
seg = buf[mark:].decode(errors='replace')
m = re.search(r'dropped=(\d+)', seg)
check('netinfo counts the drop', m is not None and int(m.group(1)) >= 1)

# 6. The guest pings the gateway: watch for the echo request,
# answer it, and expect netinfo to count the reply.
frames.clear()
mark = len(buf)
send('nettest ping')
req = wait_frame(lambda f: len(f) >= 14 and f[12:14] == b'\x08\x00', 15)
ok = req is not None
if ok:
    ip = req[14:]
    ic = ip[20:]
    ok = (ip[12:16] == GUEST_IP and ip[16:20] == PEER_IP and
          ic[0] == 8 and ic[4:6] == struct.pack('>H', 0x5973) and
          csum(ip[:20]) == 0 and csum(ic) == 0)
check('guest ping request on the wire', ok)
if ok:
    seq = ic[6:8]
    send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
                   ip_packet(PEER_IP, GUEST_IP, 1,
                             icmp_echo(0, 0x5973,
                                       struct.unpack('>H', seq)[0],
                                       ic[8:]))))
check('nettest done (ping)', until('NETTEST DONE', 40, mark))
seg = buf[mark:].decode(errors='replace')
check('nettest ping sent', 'ping-sent' in seg)
m = re.search(r'ping_replied=(\d+)', seg)
check('netinfo counts the ping reply', m is not None and int(m.group(1)) >= 1)

# 7. (M74) UDP on the wire: the harness sends a datagram to
# the guest's bound port; nettest answers it.
def udp_packet(src_ip, dst_ip, sport, dport, data):
    seg = struct.pack('>HHHH', sport, dport, 8 + len(data), 0) + data
    pseudo = src_ip + dst_ip + struct.pack('>BBH', 0, 17, len(seg))
    seg = seg[:6] + struct.pack('>H', csum(pseudo + seg)) + seg[8:]
    return ip_packet(src_ip, dst_ip, 17, seg)

frames.clear()
mark = len(buf)
send('nettest udp')
check('nettest udp loopback', until('NETTEST udp-loop PASS', 30, mark))
check('nettest udp ready', until('NETTEST udp-ready', 30, mark))
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               udp_packet(PEER_IP, GUEST_IP, 9999, 7777,
                          b'wire-hello')))
rep = wait_frame(lambda f: len(f) >= 14 and f[12:14] == b'\x08\x00', 12)
ok = rep is not None
if ok:
    ip = rep[14:]
    u = ip[20:]
    ulen = struct.unpack('>H', u[4:6])[0]
    pseudo = ip[12:16] + ip[16:20] + struct.pack('>BBH', 0, 17, ulen)
    ok = (ip[16:20] == PEER_IP and
          struct.unpack('>HH', u[0:4]) == (7777, 9999) and
          u[8:ulen] == b'yousj-udp-pong' and
          csum(pseudo + u[:ulen]) == 0 and csum(ip[:20]) == 0)
check('guest answers a UDP datagram correctly', ok)
check('nettest done (udp)', until('NETTEST DONE', 30, mark))

# 8. A datagram to an unbound port earns ICMP port
# unreachable with the original packet quoted.
frames.clear()
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               udp_packet(PEER_IP, GUEST_IP, 9999, 7999,
                          b'anyone?')))
rep = wait_frame(lambda f: len(f) >= 14 and f[12:14] == b'\x08\x00', 12)
ok = rep is not None
if ok:
    ip = rep[14:]
    ic = ip[20:]
    ok = (ip[9] == 1 and ic[0] == 3 and ic[1] == 3 and
          ic[8 + 9] == 17 and
          ic[8 + 20 + 2:8 + 20 + 4] == struct.pack('>H', 7999))
check('unbound port earns ICMP unreachable', ok)

# 9. (M75) TCP: the harness plays the server for the guest's
# outbound connect -- handshake, data both ways, FIN close.
def tcp_seg(sport, dport, seq, ack, flags, data=b''):
    hdr = struct.pack('>HHII', sport, dport, seq, ack) + \
        bytes([0x50, flags]) + struct.pack('>HHH', 64240, 0, 0)
    pseudo = PEER_IP + GUEST_IP + struct.pack('>BBH', 0, 6,
                                              len(hdr) + len(data))
    hdr = hdr[:16] + struct.pack('>H', csum(pseudo + hdr + data)) \
        + hdr[18:]
    return hdr + data

def wait_tcp(pred, timeout=20):
    end = time.time() + timeout
    while time.time() < end:
        for f in list(frames):
            if len(f) >= 54 and f[12:14] == b'\x08\x00' and \
                    f[23] == 6 and pred(f):
                return f
        time.sleep(0.1)
    return None

mark = len(buf)
frames.clear()  # before dialing: the SYN may beat the prints
send('nettest tcp')
check('nettest tcp loopback', until('NETTEST tcp-loop PASS', 30, mark))
check('nettest tcp refused', until('NETTEST tcp-refused PASS', 30,
                                    mark))
check('nettest tcp dialing', until('NETTEST tcp-dialing', 30, mark))
syn = wait_tcp(lambda f: f[47] & 0x02 and not (f[47] & 0x10))
ok = syn is not None
g_sport, g_seq = 0, 0
if ok:
    seg = syn[34:]
    g_sport = struct.unpack('>H', seg[0:2])[0]
    g_seq = struct.unpack('>I', seg[4:8])[0]
    ok = struct.unpack('>H', seg[2:4])[0] == 8888 and \
        syn[14 + 16:14 + 20] == PEER_IP
check('guest SYN on the wire', ok)
MY_ISS = 0x10000
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               ip_packet(PEER_IP, GUEST_IP, 6,
                         tcp_seg(8888, g_sport, MY_ISS, g_seq + 1,
                                 0x12))))
ackf = wait_tcp(lambda f: f[47] & 0x10 and
                struct.unpack('>I', f[42:46])[0] == MY_ISS + 1)
check('guest completes the handshake', ackf is not None)
check('nettest tcp connected', until('NETTEST tcp-conn', 30, mark))
send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
               ip_packet(PEER_IP, GUEST_IP, 6,
                         tcp_seg(8888, g_sport, MY_ISS + 1,
                                 g_seq + 1, 0x18, b'srv42'))))
got = wait_tcp(lambda f: len(f) >= 59 and f[54:] == b'cli42' and
               struct.unpack('>I', f[38:42])[0] == g_seq + 1)
check('guest data arrives intact', got is not None)
if got is not None:
    send_frame(eth(GUEST_MAC, PEER_MAC, 0x0800,
                   ip_packet(PEER_IP, GUEST_IP, 6,
                             tcp_seg(8888, g_sport, MY_ISS + 6,
                                     g_seq + 6, 0x11))))
fin = wait_tcp(lambda f: f[47] & 0x01)
check('guest FINs after close', fin is not None)
check('nettest done (tcp)', until('NETTEST DONE', 40, mark))

f1 = free_pages(len(buf))
check('PMM steady state', f0 is not None and f0 == f1)

print('ALL OK' if not fails else 'FAILURES PRESENT')
qemu.terminate()
sys.exit(0 if not fails else 1)

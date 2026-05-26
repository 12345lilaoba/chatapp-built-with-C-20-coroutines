#!/usr/bin/env python3
"""WebSocket 完整流程测试"""

import socket, struct, hashlib, base64, os, json, sys, time, threading

HOST = '127.0.0.1'
PORT = 9090

def ws_connect(token):
    """建立 WebSocket 连接，返回 (socket, remainder_buffer)"""
    s = socket.socket()
    s.connect((HOST, PORT))
    s.settimeout(3)

    key = base64.b64encode(os.urandom(16)).decode()
    expected = base64.b64encode(
        hashlib.sha1((key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()
    ).decode()

    req = (
        f'GET /ws?token={token} HTTP/1.1\r\n'
        f'Host: {HOST}:{PORT}\r\n'
        f'Upgrade: websocket\r\n'
        f'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Key: {key}\r\n'
        f'Sec-WebSocket-Version: 13\r\n'
        f'\r\n'
    ).encode()
    s.send(req)

    data = b''
    while b'\r\n\r\n' not in data:
        data += s.recv(4096)

    hdr_end = data.index(b'\r\n\r\n') + 4
    header = data[:hdr_end].decode()
    status = header.split('\r\n')[0]

    if '101' not in status:
        s.close()
        return None, None, status

    accept_lines = [l.split(': ',1)[1] for l in header.split('\r\n')
                    if l.startswith('Sec-WebSocket-Accept')]
    if accept_lines:
        accept = accept_lines[0]
        assert accept == expected, f"Accept mismatch: {accept} != {expected}"

    return s, data[hdr_end:], status


def ws_read_frame(sock, buf):
    """读取一个 WebSocket 帧"""
    while len(buf) < 2:
        buf += sock.recv(4096)

    opcode = buf[0] & 0x0F
    masked = (buf[1] & 0x80) != 0
    length = buf[1] & 0x7F
    off = 2

    if length == 126:
        while len(buf) < off + 2: buf += sock.recv(4096)
        length = struct.unpack('!H', buf[off:off+2])[0]
        off += 2
    elif length == 127:
        while len(buf) < off + 8: buf += sock.recv(4096)
        length = struct.unpack('!Q', buf[off:off+8])[0]
        off += 8

    if masked:
        while len(buf) < off + 4: buf += sock.recv(4096)
        mask = buf[off:off+4]
        off += 4

    while len(buf) < off + length: buf += sock.recv(4096)
    payload = buf[off:off+length]
    if masked:
        payload = bytes(b ^ mask[i%4] for i, b in enumerate(payload))

    return opcode, payload.decode('utf-8', errors='replace'), buf[off+length:]


def ws_send_text(sock, text):
    """发送文本帧（带 mask）"""
    payload = text.encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i%4] for i, b in enumerate(payload))
    if len(payload) < 126:
        frame = bytes([0x81, 0x80 | len(payload)]) + mask + masked
    else:
        frame = bytes([0x81, 0x80 | 126]) + struct.pack('!H', len(payload)) + mask + masked
    sock.send(frame)


def ws_close(sock):
    try:
        sock.send(bytes([0x88, 0x00]))
        sock.close()
    except:
        pass


def http_post(path, body):
    """简单 HTTP POST"""
    s = socket.socket()
    s.connect((HOST, PORT))
    s.settimeout(3)
    data = body.encode()
    req = (
        f'POST {path} HTTP/1.1\r\n'
        f'Host: {HOST}:{PORT}\r\n'
        f'Content-Type: application/json\r\n'
        f'Content-Length: {len(data)}\r\n'
        f'Connection: close\r\n'
        f'\r\n'
    ).encode() + data
    s.send(req)
    resp = b''
    while True:
        try:
            chunk = s.recv(4096)
            if not chunk: break
            resp += chunk
        except: break
    s.close()
    body_start = resp.find(b'\r\n\r\n')
    if body_start >= 0:
        return json.loads(resp[body_start+4:])
    return {}


# ============ 测试 ============

print('='*50)
print(' WebSocket 完整流程测试')
print('='*50)
print()

# 1. 注册+登录
print('1. 注册+登录')
r = http_post('/api/register', '{"username":"ws_alice","password":"pass1"}')
print(f'   注册 alice: {r}')
r = http_post('/api/register', '{"username":"ws_bob","password":"pass2"}')
print(f'   注册 bob: {r}')

r1 = http_post('/api/login', '{"username":"ws_alice","password":"pass1"}')
token1 = r1.get('token', '')
print(f'   alice token: {token1[:8]}...')
r2 = http_post('/api/login', '{"username":"ws_bob","password":"pass2"}')
token2 = r2.get('token', '')
print(f'   bob token: {token2[:8]}...')
print()

# 2. Alice 建立 WebSocket 连接
print('2. Alice 建立 WebSocket')
ws1, buf1, status1 = ws_connect(token1)
print(f'   Status: {status1}')
assert ws1, "Alice WebSocket connect failed"

# 读取所有历史消息
hist_count = 0
while True:
    try:
        opcode, payload, buf1 = ws_read_frame(ws1, buf1)
        if opcode == 1:
            msg = json.loads(payload)
            print(f'   历史: [{msg["time"]}] {msg["user"]}: {msg["text"]}')
            hist_count += 1
    except:
        break
print(f'   共 {hist_count} 条历史消息')
print()

# 3. Bob 建立 WebSocket 连接
print('3. Bob 建立 WebSocket')
ws2, buf2, status2 = ws_connect(token2)
print(f'   Status: {status2}')
assert ws2, "Bob WebSocket connect failed"

# 读取 Bob 的历史消息
bob_hist = 0
while True:
    try:
        opcode, payload, buf2 = ws_read_frame(ws2, buf2)
        if opcode == 1: bob_hist += 1
    except:
        break
print(f'   Bob 收到 {bob_hist} 条历史消息')
print()

# 4. Alice 发送消息，Bob 应该收到广播
print('4. Alice 发送消息')
ws_send_text(ws1, 'Hello Bob, this is Alice via WebSocket!')
print('   Sent: Hello Bob, this is Alice via WebSocket!')

# Alice 自己也应该收到广播
try:
    buf1 = b''
    opcode, payload, buf1 = ws_read_frame(ws1, buf1)
    msg = json.loads(payload)
    print(f'   Alice 收到: [{msg["time"]}] {msg["user"]}: {msg["text"]}')
except Exception as e:
    print(f'   Alice 读取失败: {e}')

# Bob 收到广播
try:
    buf2 = b''
    opcode, payload, buf2 = ws_read_frame(ws2, buf2)
    msg = json.loads(payload)
    print(f'   Bob 收到:   [{msg["time"]}] {msg["user"]}: {msg["text"]}')
except Exception as e:
    print(f'   Bob 读取失败: {e}')
print()

# 5. Bob 回复
print('5. Bob 回复消息')
ws_send_text(ws2, '你好 Alice！我是 Bob')
print('   Sent: 你好 Alice！我是 Bob')

try:
    opcode, payload, buf2 = ws_read_frame(ws2, buf2)
    msg = json.loads(payload)
    print(f'   Bob 收到:   [{msg["time"]}] {msg["user"]}: {msg["text"]}')
except Exception as e:
    print(f'   Bob 读取失败: {e}')

try:
    opcode, payload, buf1 = ws_read_frame(ws1, buf1)
    msg = json.loads(payload)
    print(f'   Alice 收到: [{msg["time"]}] {msg["user"]}: {msg["text"]}')
except Exception as e:
    print(f'   Alice 读取失败: {e}')
print()

# 6. 关闭
print('6. 关闭连接')
ws_close(ws1)
ws_close(ws2)
print('   Done')
print()

print('='*50)
print(' WebSocket 测试完成！')
print('='*50)

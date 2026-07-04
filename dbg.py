import socket, time

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('mon2.sock')
time.sleep(0.5)
try:
    s.recv(65536)
except Exception:
    pass


def cmd(c):
    s.sendall((c + '\n').encode())
    time.sleep(0.5)


for k in ('l', 'w', 'k', 't', 'ret'):
    cmd('sendkey ' + k)
    time.sleep(0.2)
time.sleep(0.8)
for _ in range(4):
    cmd('sendkey spc')
    time.sleep(0.4)
for _ in range(3):
    cmd('sendkey y')
    time.sleep(0.4)
cmd('sendkey c')
time.sleep(0.4)
for i in range(3):
    time.sleep(0.35)
    cmd('screendump cap4_%d.ppm' % i)
s.close()
print('done')

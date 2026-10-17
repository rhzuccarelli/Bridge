import serial,time,sys
s=serial.Serial('/dev/cu.usbmodem5B5F0908891',115200,timeout=0.1)
buf=b''; mark=0
def pump(t):
    global buf
    e=time.time()+t
    while time.time()<e: buf+=s.read(65536)
def keys(k, gap=0.4):
    for c in k: s.write(c.encode()); pump(gap)
def wait_for(*marks, limit=60):
    global mark
    t0=time.time()
    while time.time()-t0<limit:
        pump(0.3)
        hit=[m for m in marks if m in buf[mark:]]
        if hit:
            mark=buf.index(hit[0], mark)+len(hit[0]); return hit[0].decode(), round(time.time()-t0,1)
    return None, limit
def step(name, k, *marks, limit=60, after=0.5):
    global mark
    mark=len(buf); keys(k)
    r=wait_for(*marks, limit=limit); pump(after)
    print(f"{name}: {r}", flush=True)
    if r[0] is None: dump(); sys.exit(1)
    return r
def dump():
    for l in buf.decode(errors='replace').splitlines():
        if (l.startswith('[') and not l.startswith('[display]')) or 'E (' in l: print(l[:220])
pump(12)  # boot settles
step('projects', 'kkkkkkkkjjjjjj\n', b'screen ProjectBrowser')
step('open project 2', 'kkkkkjj\n', b'screen TrackView', limit=10, after=1)
step('import screen', 'i', b'MTP session open', b'session failed', limit=25, after=2)
step('recordings', 'kkkkjj\n', b'_never_', limit=6) if False else (keys('kkkkjj\n'), pump(6))
step('import', 'kkkkkkkk\n\n', b'import completed', b'failed', limit=300)
step('leave', '\n', b'screen ', limit=10)
step('save', 'w', b'project saved', b'save failed', limit=15, after=2)
step('export screen', 'e', b'MTP session open', b'session failed', limit=25, after=3)
step('export', '\n', b'export completed', b'export failed', limit=1500, after=10)
dump()

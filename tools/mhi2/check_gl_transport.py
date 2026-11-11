#!/usr/bin/env python3
"""Check bridge bulk RX, acknowledgement bounds, and cross-chunk ordering."""
import os
import argparse
import select
import tempfile
import subprocess
import sys
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('media',type=Path)
parser.add_argument('--machine',choices=('mhi2-harman','mh2p'),default='mhi2-harman')
parser.add_argument('--qemu',type=Path,
                    default=Path(__file__).resolve().parents[2]/'build/qemu-system-arm')
args=parser.parse_args()
media=args.media.resolve()
qemu=args.qemu.resolve()
scratch=tempfile.TemporaryDirectory(prefix="mhi2-gltest-")
pipe_path=Path(scratch.name)/'bridge'
os.mkfifo(str(pipe_path)+'.in');os.mkfifo(str(pipe_path)+'.out')
host=os.open(str(pipe_path)+'.in',os.O_RDWR|os.O_NONBLOCK)
tx=os.open(str(pipe_path)+'.out',os.O_RDWR|os.O_NONBLOCK)
machine_args=(['-M','mhi2-harman,iram='+str(media/'iram.bin'),'-bios',str(media/'nor.bin')]
              if args.machine=='mhi2-harman' else
              ['-M','mh2p','-kernel',str(media/'startup.bin')])
errors=tempfile.TemporaryFile()
p=subprocess.Popen([str(qemu),*machine_args,
    '-run-with','exit-with-parent=on',
    '-display','none','-serial','null','-monitor','none',
    '-accel','qtest','-qtest','stdio','-qtest-log','/dev/null',
    '-chardev',f'pipe,id=glbridge,path={pipe_path}'],
    stdin=subprocess.PIPE,stdout=subprocess.PIPE,
    stderr=errors,bufsize=0,env=dict(os.environ,TMPDIR=str(media/'tmp')))
pending=bytearray()
def q(cmd):
    p.stdin.write((cmd+'\n').encode())
    while b'\n' not in pending:
        assert select.select([p.stdout],[],[],5)[0],cmd
        data=os.read(p.stdout.fileno(),4096);assert data;pending.extend(data)
    line,_,rest=pending.partition(b'\n');pending[:]=rest
    assert line.startswith(b'OK'),line
    return line.split()[1:] if len(line)>2 else []
def read(addr):return int(q(f'readl {addr:#x}')[0],16)
def write(addr,value):q(f'writel {addr:#x} {value:#x}')
base=0x5f000000
try:
    assert read(base)==0x474c4252
    payload=bytes(range(256))*32
    assert os.write(host,payload)==len(payload)
    got=bytearray()
    for _ in range(10000):
        available=read(base+8)
        if not available:continue
        write(base+20,4097)
        assert read(base+8)>=available,'Oversized acknowledgement consumed bytes'
        chunk=min(available,508)
        for off in range(0,chunk,4):got+=read(base+0x2000+off).to_bytes(4,'little')
        write(base+20,chunk)
        if len(got)==len(payload):break
    assert bytes(got)==payload,(len(got),len(payload))
    assert read(base+8)==0
    assert read(base+0x2000)==0,'Read beyond available RX data'
    assert read(base+16)==0 and read(base+16)==1
    write(base+16,0);assert read(base+16)==0
    bulk_size=read(base+24)
    assert bulk_size==1024*1024
    bulk=0x5e000000
    q(f'write {bulk:#x} {len(payload)} 0x{payload.hex()}')
    write(base+28,bulk_size+1)
    assert not select.select([tx],[],[],0)[0], 'Oversized bulk TX accepted'
    write(base+28,len(payload))
    sent=bytearray()
    while len(sent)<len(payload):
        assert select.select([tx],[],[],5)[0]
        sent.extend(os.read(tx,len(payload)-len(sent)))
    assert bytes(sent)==payload
    os.write(host,payload)
    for _ in range(10000):
        if read(base+32)==len(payload):break
    assert read(base+32)==len(payload)
    write(base+36,len(payload)+1)
    assert read(base+32)==len(payload)
    result=q(f'read {bulk+bulk_size:#x} {len(payload)}')[0]
    assert bytes.fromhex(result.decode().removeprefix('0x'))==payload
    write(base+36,len(payload))
    assert read(base+32)==0
    # A stalled renderer must not hold QEMU's global lock. Fill beyond the
    # host pipe capacity without reading it, then run an unrelated DMA timer.
    large=bytes(range(256))*4096
    for off in range(0,len(large),65536):
        part=large[off:off+65536]
        q(f'write {bulk+off:#x} {len(part)} 0x{part.hex()}')
    write(base+28,len(large))
    q(f'write {bulk:#x} 4 0xdeadbeef')  # queued TX must own a copy
    if args.machine=='mhi2-harman':
        write(0x6000a000,1<<31)
        write(0x70080000,0x80070000)
        write(0x70080210,1)
        write(0x70080300,0xc0001207)
        write(0x6000b010,0x81000000)
        write(0x6000b018,0x7008000c)
        write(0x6000b000,0xd800001c)
        q('clock_step 1000000')
        assert read(0x6000b004)&(1<<30), 'Blocked renderer stalled audio DMA'
    else:
        before=read(0x60005010)
        q('clock_step 1000000')
        assert (read(0x60005010)-before)&0xffffffff==1000, 'Blocked renderer stalled timer'
    sent=bytearray()
    while len(sent)<len(large):
        assert select.select([tx],[],[],5)[0],len(sent)
        sent.extend(os.read(tx,len(large)-len(sent)))
    assert bytes(sent)==large, 'Queued graphics data changed with staging RAM'
    write(base+16,0)
    q(f'write {bulk:#x} 4 0x00010203')
    # The host pipe absorbs some records, so count until the lock is refused.
    queued=0
    while read(base+16)==0:
        write(base+28,len(large))
        write(base+16,0)
        queued+=1
        assert queued<=20, 'Queue accepted a lock without reserving record space'
    assert queued>=16, 'Queue refused the lock before it was full'
    remaining=queued*len(large)
    while remaining:
        assert select.select([tx],[],[],5)[0],remaining
        part=os.read(tx,min(remaining,len(large)))
        remaining-=len(part)
    assert read(base+16)==0, 'Backpressure left the guest lock held'
    write(base+16,0)
    print('PASS: bulk graphics ordering, bounded acknowledgements, lock, queued TX ownership, device progress under renderer backpressure')
finally:
    os.close(host);os.close(tx);p.terminate()
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:p.kill();p.wait()
    p.stdin.close();p.stdout.close()
    errors.seek(0)
    sys.stderr.buffer.write(errors.read())
    errors.close()
    scratch.cleanup()

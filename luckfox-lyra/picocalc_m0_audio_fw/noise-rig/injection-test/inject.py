#!/usr/bin/env python3
"""inject.py LOOPS MASK MODE: control words of the injection test firmware (SRAM 0xFFF83100): delay = LOOPS*4 cycles
before the pin write on plain ticks where (lfsr & MASK)==0; MODE 0 every write, 1 only writes with B2 high, 2 only low.
inject.py count: number of delays injected by the last stream (published at its end)."""
import mmap,os,struct,sys
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC); m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=0xFFF83000)
if sys.argv[1]=="count": print(struct.unpack("<I",m[0x110:0x114])[0])
else: m[0x100:0x10c]=struct.pack("<III",int(sys.argv[1]),int(sys.argv[2],0),int(sys.argv[3])); print("loops %s mask %s mode %s"%tuple(sys.argv[1:4]))

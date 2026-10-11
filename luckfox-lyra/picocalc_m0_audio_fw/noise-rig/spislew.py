import mmap,os,struct,sys
# SPI1 pad slew (GPIO1_IOC 0xFF660000): MOSI gpio1c3 (C_SL 0x618 bits 7:6), CLK gpio1d1 (D_SL 0x61C bits 3:2),
# CS gpio1d2 (D_SL bits 5:4). 3 = fastest (reset), 0 = slowest. usage: spislew.py [LEVEL]  (no arg: show)
wr=len(sys.argv)>1
fd=os.open("/dev/mem",os.O_RDWR if wr else os.O_RDONLY|os.O_SYNC)
m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ|(mmap.PROT_WRITE if wr else 0),offset=0xFF660000)
def r(o): return struct.unpack("<I",m[o:o+4])[0]
def show():
    c=r(0x618); d=r(0x61C)
    print("slew: mosi=%d clk=%d cs=%d miso=%d"%((c>>6)&3,(d>>2)&3,(d>>4)&3,(d>>6)&3))
show()
if wr:
    l=int(sys.argv[1])&3
    m[0x618:0x61C]=struct.pack("<I",(3<<6)<<16|(l<<6))                 # mosi
    m[0x61C:0x620]=struct.pack("<I",((3<<2)|(3<<4))<<16|(l<<2)|(l<<4)) # clk, cs
    show()

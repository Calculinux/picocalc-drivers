import mmap,os,struct,sys
# SPI1 pads: mosi gpio1c3 (0x30 bits 15:12), clk gpio1d1 (0x38 bits 7:4), cs gpio1d2 (11:8), miso gpio1d3 (15:12).
# usage: padmux.py MODE   MODE: restore | clk | all | nonclk   (disconnected pads become GPIO inputs, function 0)
mode=sys.argv[1]
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=0xFF660000)
def rd(o): return struct.unpack("<I",m[o:o+4])[0]
def wr(o,mask,val): m[o:o+4]=struct.pack("<I",(mask<<16)|val)
fn={"restore":7}.get(mode,0)
def sel(c,d1,d2,d3):
    wr(0x30,0xF000,c<<12); wr(0x38,0x00F0,d1<<4); wr(0x38,0x0F00,d2<<8); wr(0x38,0xF000,d3<<12)
if mode=="restore": sel(7,7,7,7)
elif mode=="clk":   sel(7,0,7,7)
elif mode=="all":   sel(0,0,0,0)
elif mode=="nonclk":sel(0,7,0,0)
print("iomux: mosi=%d clk=%d cs=%d miso=%d"%((rd(0x30)>>12)&15,(rd(0x38)>>4)&15,(rd(0x38)>>8)&15,(rd(0x38)>>12)&15))

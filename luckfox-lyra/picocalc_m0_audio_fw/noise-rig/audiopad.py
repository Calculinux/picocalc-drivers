import mmap,os,struct,sys
fd=os.open("/dev/mem",os.O_RDWR if len(sys.argv)>1 else os.O_RDONLY|os.O_SYNC)
prot=mmap.PROT_READ|(mmap.PROT_WRITE if len(sys.argv)>1 else 0)
m=mmap.mmap(fd,4096,mmap.MAP_SHARED,prot,offset=0xFF4D8000)
def rd(): return struct.unpack("<I",m[0x840:0x844])[0]
def show(v): print("GPIO4_IOC_SARADC_CON=%#06x  ps(up)=%d pe=%d slew_limit=%d drive=%d smt=%#x ie=%#x he=%#x"%(v&0xFFFF,(v>>14)&1,(v>>13)&1,(v>>12)&1,(v>>10)&3,(v>>8)&3,(v>>4)&15,v&15))
show(rd())
if len(sys.argv)>1:
    ds=int(sys.argv[1]); sl=int(sys.argv[2])   # drive level 0..3, slew limit 0/1
    mask=(1<<12)|(3<<10)
    val=(sl<<12)|(ds<<10)
    m[0x840:0x844]=struct.pack("<I",(mask<<16)|val)
    show(rd())

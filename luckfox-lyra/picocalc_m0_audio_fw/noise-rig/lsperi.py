import mmap,os,struct,sys
div=int(sys.argv[1])       # div_con; bus = 375 MHz / (div+1)
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
m=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=0xFF9A0000)
old=struct.unpack("<I",m[0x374:0x378])[0]
m[0x374:0x378]=struct.pack("<I",(0x1F<<16)|(div&0x1F))   # only bits 4:0, via the write-enable half
new=struct.unpack("<I",m[0x374:0x378])[0]
print("CRU_CLKSEL_CON29 %#x -> %#x  hclk_lsperi_root = 375 MHz / %d = %.2f MHz"%(old,new,(new&31)+1,375.0/((new&31)+1)))

import os,sys,fcntl,struct
hz=int(sys.argv[1]); chunk=int(sys.argv[2]) if len(sys.argv)>2 else 4096
fd=os.open("/dev/spidev1.0",os.O_RDWR)
fcntl.ioctl(fd,0x40046b04,struct.pack("I",hz))   # SPI_IOC_WR_MAX_SPEED_HZ
fcntl.ioctl(fd,0x40016b01,struct.pack("B",0))    # SPI_IOC_WR_MODE 0
buf=b"\xff"*chunk
while True:
    os.write(fd,buf)

import usb.core, usb.util, struct, time
class Scope:
    def __init__(s, vid=0x1ab1, pid=0x04ce, timeout=15000):
        s.dev=usb.core.find(idVendor=vid,idProduct=pid)
        if s.dev is None: raise RuntimeError("scope not found")
        s.timeout=timeout
        try:
            if s.dev.is_kernel_driver_active(0): s.dev.detach_kernel_driver(0)
        except (usb.core.USBError, NotImplementedError): pass
        try: s.dev.set_configuration()
        except usb.core.USBError: pass
        cfg=s.dev.get_active_configuration(); intf=cfg[(0,0)]
        s.ep_out=usb.util.find_descriptor(intf,custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)==usb.util.ENDPOINT_OUT and usb.util.endpoint_type(e.bmAttributes)==usb.util.ENDPOINT_TYPE_BULK)
        s.ep_in=usb.util.find_descriptor(intf,custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)==usb.util.ENDPOINT_IN and usb.util.endpoint_type(e.bmAttributes)==usb.util.ENDPOINT_TYPE_BULK)
        usb.util.claim_interface(s.dev,0)
        s.tag=0
        s.clear()
    def clear(s):
        # drain anything pending
        try:
            while True: s.ep_in.read(65536,timeout=100)
        except usb.core.USBError: pass
    def _tag(s):
        s.tag=(s.tag%255)+1; return s.tag
    def w(s,cmd):
        data=(cmd+"\n").encode()
        t=s._tag()
        hdr=struct.pack("<BBBBIBBBB",1,t,(~t)&0xff,0,len(data),1,0,0,0)
        pad=(-len(data))%4
        s.ep_out.write(hdr+data+b"\0"*pad,timeout=s.timeout)
    def r(s,maxlen=1<<22):
        out=b""
        while True:
            t=s._tag()
            req=struct.pack("<BBBBIBBBB",2,t,(~t)&0xff,0,maxlen,0,0,0,0)
            s.ep_out.write(req,timeout=s.timeout)
            first=bytes(s.ep_in.read(65536,timeout=s.timeout))
            while len(first)<12:
                first+=bytes(s.ep_in.read(65536,timeout=s.timeout))
            msgid,tg,tgi,_,size,attrs=struct.unpack("<BBBBIB",first[:9])
            body=first[12:]
            while len(body)<size:
                body+=bytes(s.ep_in.read(65536,timeout=s.timeout))
            out+=body[:size]
            if attrs&1: return out
    def q(s,cmd,maxlen=1<<22):
        s.w(cmd); return s.r(maxlen).decode(errors="replace").strip()
    def qb(s,cmd,maxlen=1<<22):
        s.w(cmd); d=s.r(maxlen)
        while d[:1]!=b"#": d=d[1:]
        nd=int(d[1:2]); ln=int(d[2:2+nd])
        return d[2+nd:2+nd+ln]
    def close(s):
        usb.util.release_interface(s.dev,0)
        usb.util.dispose_resources(s.dev)

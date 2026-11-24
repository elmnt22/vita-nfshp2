# gcov_stream_split.py <stream> <outdir>: split a GCC merge-stream (gcfn+gcda records) into .gcda files.
import struct,sys,os
data=open(sys.argv[1],'rb').read(); out=sys.argv[2]; pos=0; n=0
def u32(p): return struct.unpack_from('<I',data,p)[0]
GCFN=0x6763666e; GCDA=0x67636461
while pos+8<=len(data):
    assert u32(pos)==GCFN,(hex(u32(pos)),pos); pos+=8   # magic, version
    words=u32(pos); pos+=4
    name=data[pos:pos+words].split(bytes([0]))[0].decode().replace(chr(92),"/"); pos+=words
    start=pos; assert u32(pos)==GCDA; pos+=16
    while pos+8<=len(data) and u32(pos)!=GCFN:
        if u32(pos)==0: pos+=4; break  # end-of-file tag
        tag=u32(pos); length=struct.unpack_from('<i',data,pos+4)[0]; pos+=8
        if length>0: pos+=length
    path=os.path.join(out,name); os.makedirs(os.path.dirname(path) or '.',exist_ok=True)
    open(path,'wb').write(data[start:pos]); n+=1
print('files',n,'end',pos,len(data))

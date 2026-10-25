import capstone,sys
from elfinfo import *
img=Image('SCES_508.85')
md=capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64|capstone.CS_MODE_LITTLE_ENDIAN)
def dis(a,n):
    out=[]
    for i in range(n):
        x=a+i*4; w=img.word(x)
        r=list(md.disasm(w.to_bytes(4,'little'),x))
        out.append('%08x: %08x  %s'%(x,w,(r[0].mnemonic+' '+r[0].op_str) if r else '???'))
    return '\n'.join(out)
if __name__=='__main__':
    print(dis(int(sys.argv[1],16),int(sys.argv[2])))

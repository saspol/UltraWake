# Apply the ACPI/Patch entries of an OpenCore config.plist to one dumped ACPI
# table, the way OpenCore does (TableSignature, OemTableId, TableLength guards,
# Find/Replace with Mask, Skip, Count, Limit), print each patch's replacement
# count and offsets, and fix the table checksum.
#
#   python3 ocpatch.py config.plist dsdt.aml dsdt-patched.aml [extra-patches.plist]
#
# Not emulated: Base/BaseSkip and ReplaceMask (the script stops on them).
import plistlib,sys
def nz(v): return v if v and any(v) else None
def find(b,f,m,i,end):
    if not m: return b.find(f,i,end)
    fm=bytes(x&y for x,y in zip(f,m))
    for j in range(i,end-len(f)+1):
        if all((b[j+k]&m[k])==fm[k] for k in range(len(f))): return j
    return -1
def apply(table,patches,log=True):
    b=bytearray(table)
    sig=bytes(b[0:4]); oem=bytes(b[16:24]); ln=len(b); res=[]
    for p in patches:
        if not p.get('Enabled'): continue
        if nz(p.get('TableSignature')) and p['TableSignature']!=sig: continue
        if nz(p.get('OemTableId')) and p['OemTableId']!=oem: continue
        if p.get('TableLength') and p['TableLength']!=ln: continue
        if p.get('Base'): raise SystemExit('Base not emulated: '+p['Comment'])
        if nz(p.get('ReplaceMask')): raise SystemExit('ReplaceMask not emulated')
        f,r=p['Find'],p['Replace']; m=nz(p.get('Mask')); assert len(f)==len(r)
        skip=p.get('Skip',0); count=p.get('Count',0); limit=p.get('Limit',0)
        end=len(b) if not limit else min(len(b),limit)
        i=0; done=0; offs=[]
        while True:
            j=find(b,f,m,i,end)
            if j<0: break
            if skip>0: skip-=1; i=j+1; continue
            b[j:j+len(f)]=r; offs.append(hex(j)); done+=1; i=j+len(f)
            if count and done>=count: break
        res.append((p.get('Comment',''),done,offs))
        if log: print(f"{p.get('Comment','')[:60]:60} replaced {done} at {offs[:6]}")
    b[9]=(b[9]-sum(b))&0xff
    return bytes(b),res
if __name__=='__main__':
    cfg=plistlib.load(open(sys.argv[1],'rb'))
    pats=list(cfg['ACPI']['Patch'])
    if len(sys.argv)>4: pats+=plistlib.load(open(sys.argv[4],'rb'))
    out,_=apply(open(sys.argv[2],'rb').read(),pats)
    open(sys.argv[3],'wb').write(out); print('sum',sum(out)&0xff,'len',len(out))

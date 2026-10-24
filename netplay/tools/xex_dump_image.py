# Decrypt + decompress a retail XEX2 (basic compression) and dump the title SPA resource.
import struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
RETAIL = bytes([0x20,0xB1,0x85,0xA5,0x9D,0x28,0xFD,0xC3,0x40,0x58,0x3F,0xBB,0x08,0x96,0xBF,0x91])
def aes_cbc_dec(key, data):
    d = Cipher(algorithms.AES(key), modes.CBC(b'\0'*16)).decryptor()
    return d.update(data) + d.finalize()
xex = open(sys.argv[1],'rb').read()
assert xex[:4] == b'XEX2'
mod_flags, pe_off, _res, sec_off, nopt = struct.unpack('>IIIII', xex[4:24])
opts = {}
for i in range(nopt):
    k, v = struct.unpack('>II', xex[24+8*i:32+8*i]); opts[k] = v
ffi = opts[0x000003FF]
info_size, enc, comp = struct.unpack('>IHH', xex[ffi:ffi+8])
print('pe_off', hex(pe_off), 'enc', enc, 'comp', comp)
aes_key_enc = xex[sec_off+0x150:sec_off+0x160]
load_addr = struct.unpack('>I', xex[sec_off+0x110:sec_off+0x114])[0]
session = Cipher(algorithms.AES(RETAIL), modes.ECB()).decryptor().update(aes_key_enc)
assert comp == 1, 'only basic compression supported'
blocks = [struct.unpack('>II', xex[ffi+8+8*i:ffi+16+8*i]) for i in range((info_size-8)//8)]
src = xex[pe_off:]
total_data = sum(b[0] for b in blocks)
plain = aes_cbc_dec(session, src[:total_data - (total_data % 16)]) if enc == 1 else src[:total_data]
img = bytearray(); p = 0
for ds, zs in blocks:
    img += plain[p:p+ds]; p += ds; img += b'\0'*zs
print('load', hex(load_addr), 'image', hex(len(img)), img[:2])
open(sys.argv[2],'wb').write(img)
# resource info opt header 0x000002FF
if 0x2FF in opts:
    ro = opts[0x2FF]; size = struct.unpack('>I', xex[ro:ro+4])[0]
    n = (size-4)//16
    for i in range(n):
        name, addr, sz = struct.unpack('>8sII', xex[ro+4+16*i:ro+20+16*i])
        name = name.rstrip(b'\0').decode()
        print('resource', name, hex(addr), hex(sz))
        open(sys.argv[2]+'.'+name+'.bin','wb').write(img[addr-load_addr:addr-load_addr+sz])

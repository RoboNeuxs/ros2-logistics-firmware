"""Check HEX record checksums, C8 flash bounds, vector table, and matching BIN."""
from pathlib import Path
import struct

root=Path(__file__).resolve().parents[1]
memory={}
base=0
eof=False
for text in (root/'Build/STM32F103C8T6.hex').read_text().splitlines():
    assert text.startswith(':')
    record=bytes.fromhex(text[1:])
    assert sum(record)&255==0 and len(record)==record[0]+5
    count=record[0]
    offset=int.from_bytes(record[1:3],'big')
    kind=record[3]
    payload=record[4:4+count]
    if kind==0:
        for i,value in enumerate(payload):
            address=base+offset+i
            assert 0x08000000<=address<0x08010000
            assert address not in memory
            memory[address]=value
    elif kind==4: base=int.from_bytes(payload,'big')<<16
    elif kind==1: eof=True
assert eof and memory
binary=(root/'Build/STM32F103C8T6.bin').read_bytes()
assert len(binary)<=65536
for address,value in memory.items(): assert binary[address-0x08000000]==value
stack,reset=struct.unpack_from('<II',binary)
assert 0x20000000<stack<=0x20005000 and stack%8==0
assert reset&1 and 0x08000000<=reset<0x08010000
for vector in (2,3,4,5,6,15,16+28,16+29,16+30,16+38,16+40):
    handler=struct.unpack_from('<I',binary,vector*4)[0]
    assert handler&1 and 0x08000000<=handler<0x08010000
print(f'HEX/BIN verified: {len(binary)} bytes, stack=0x{stack:08X}, reset=0x{reset:08X}; C8 memory bounds and IRQ vectors valid.')

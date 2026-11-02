import {test} from 'node:test';import assert from 'node:assert/strict';import {readFileSync} from 'node:fs';import {plan,crc,firmware} from './core.mjs';
function table(){const b=new Uint8Array(4096).fill(255),d=new DataView(b.buffer);[[1,2,0x9000,0x5000,'nvs'],[1,0,0xe000,0x2000,'otadata'],[0,16,0x10000,0x600000,'ota_0'],[0,17,0x610000,0x600000,'ota_1'],[1,3,0xc10000,0x10000,'coredump']].forEach((r,i)=>{let p=i*32;d.setUint16(p,0x50aa,true);b[p+2]=r[0];b[p+3]=r[1];d.setUint32(p+4,r[2],true);d.setUint32(p+8,r[3],true);b.fill(0,p+12,p+32);b.set(new TextEncoder().encode(r[4]),p+12);});return b;}
function ota(seq=1,state=2){const b=new Uint8Array(8192).fill(255),d=new DataView(b.buffer);d.setUint32(0,seq,true);d.setUint32(24,state,true);d.setUint32(28,crc(b.slice(0,4)),true);return b;}
test('CRC matches ESP-IDF fixture',()=>assert.equal(crc(Uint8Array.of(1,0,0,0)),0x4743989a));
test('both active slots resolved without altering metadata',()=>{for(let n=1;n<=8;n++){let o=ota(n),copy=o.slice();assert.equal(plan(table(),o).address,n%2?0x10000:0x610000);assert.deepEqual(o,copy);}});
test('legacy valid state accepted',()=>assert.equal(plan(table(),ota(1,0xffffffff)).address,0x10000));
test('pending and invalid boot states fail closed',()=>{for(const s of [0,1,3,4])assert.throws(()=>plan(table(),ota(1,s)));});
test('corrupt boot checksum fails',()=>{const o=ota();o[28]^=1;assert.throws(()=>plan(table(),o));});
test('blank boot metadata fails',()=>assert.throws(()=>plan(table(),new Uint8Array(8192).fill(255))));
test('small or overlapping layout fails',()=>{const t=table();new DataView(t.buffer).setUint32(2*32+8,0x1f0000,true);assert.throws(()=>plan(t,ota()));});
test('unknown partition fails',()=>{const t=table();t[12]=120;assert.throws(()=>plan(t,ota()));});
test('pinned firmware passes; modified or short image rejected',async()=>{const b=new Uint8Array(readFileSync('../firmware.bin'));await firmware(b);b[99]^=1;await assert.rejects(firmware(b));await assert.rejects(firmware(b.slice(0,100)));});

test('custom-apps layout retains same application slot',()=>{const t=table(),d=new DataView(t.buffer);[[1,2,0xc20000,0x3000,'shipping'],[1,130,0xc30000,0x3d0000,'customapps']].forEach((r,j)=>{const p=(j+5)*32;d.setUint16(p,0x50aa,true);t[p+2]=r[0];t[p+3]=r[1];d.setUint32(p+4,r[2],true);d.setUint32(p+8,r[3],true);t.fill(0,p+12,p+32);t.set(new TextEncoder().encode(r[4]),p+12);});assert.deepEqual(plan(t,ota()),{address:0x10000,protectedEnd:0x1000000});});

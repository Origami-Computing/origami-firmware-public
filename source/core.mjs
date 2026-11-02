export const RELEASE={version:'v1.0.5',size:5250576,sha256:'0dd99bb81231a9fc89d205a43629a14fb80a0c7c6a8c3134ea3d8aa7b2bee96f'};
export const sha=async b=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b)),v=>v.toString(16).padStart(2,'0')).join('');
export function crc(bytes){let c=0;for(const b of bytes){c^=b;for(let j=0;j<8;j++)c=(c>>>1)^((c&1)?0xedb88320:0);}return (c^0xffffffff)>>>0;}
export function plan(table,ota){
 if(table.length!==4096||ota.length!==8192)throw Error('Incomplete device information. Reconnect and try again.');
 const d=new DataView(table.buffer,table.byteOffset,table.byteLength),parts=[];
 for(let i=0;i<4096;i+=32){let m=d.getUint16(i,true);if(m===0xebeb||m===0xffff)break;if(m!==0x50aa)throw Error('Unrecognized memory layout. Nothing was changed.');parts.push([table[i+2],table[i+3],d.getUint32(i+4,true),d.getUint32(i+8,true),new TextDecoder().decode(table.slice(i+12,i+28)).replace(/\0.*$/s,''),d.getUint32(i+28,true)]);}
 const expected=[[1,2,0x9000,0x5000,'nvs',0],[1,0,0xe000,0x2000,'otadata',0],[0,16,0x10000,0x600000,'ota_0',0],[0,17,0x610000,0x600000,'ota_1',0],[1,3,0xc10000,0x10000,'coredump',0],[1,2,0xc20000,0x3000,'shipping',0],[1,130,0xc30000,0x3d0000,'customapps',0]];
 if(![5,6,7].includes(parts.length)||JSON.stringify(parts)!==JSON.stringify(expected.slice(0,parts.length)))throw Error('This device needs individual recovery. Its memory layout is not supported; nothing was changed.');
 const o=new DataView(ota.buffer,ota.byteOffset,ota.byteLength),entries=[];
 for(const p of [0,4096]){const seq=o.getUint32(p,true),state=o.getUint32(p+24,true),sum=o.getUint32(p+28,true);if(seq===0xffffffff||seq===0)continue;if(sum!==crc(ota.slice(p,p+4)))throw Error('Boot information is damaged. Nothing was changed.');if(![2,0xffffffff].includes(state))throw Error('A previous update has not settled. Restart the device normally before recovery.');entries.push(seq);}
 if(!entries.length)throw Error('No confirmed installed system found. This page does not set up blank devices.');
 const address=(Math.max(...entries)-1)%2===0?0x10000:0x610000;
 return {address,protectedEnd:0x1000000};
}
export async function firmware(bytes){if(bytes.length!==RELEASE.size||await sha(bytes)!==RELEASE.sha256)throw Error('Firmware integrity check failed. Nothing was changed.');}

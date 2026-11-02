import {ESPLoader,Transport} from 'esptool-js';
import {plan,firmware,sha,RELEASE} from './core.mjs';
const $=id=>document.getElementById(id);let loader,transport,port,selected,binary,before,busy=false;const diagnostic={release:RELEASE.version,stage:'not started',device:null};
function status(text){$('status').textContent=text;diagnostic.stage=text;}
async function protectedHash(){const regions=[[0,0x10000],[0xc10000,selected.protectedEnd-0xc10000]];const hashes=[];for(const [a,n] of regions)hashes.push(await loader.flashMd5sum(a,n));return hashes;}
async function close(){try{await transport?.disconnect();}catch{}loader=null;}
function lock(value){busy=value;$('connect').disabled=value||!!selected;$('install').disabled=value||!selected;}
window.addEventListener('beforeunload',e=>{if(busy){e.preventDefault();e.returnValue='';}});
$('connect').onclick=async()=>{if(!$('kami').checked){status('Please confirm this is an Origami / Kami device first.');return;}lock(true);selected=null;$('install').hidden=true;try{
 port=await navigator.serial.requestPort({filters:[{usbVendorId:0x303a}]});status('Connecting to your Kami…');
 transport=new Transport(port,true);loader=new ESPLoader({transport,baudrate:460800,terminal:{clean(){},write(){},writeLine(){}}});
 await loader.main();
 if(loader.chip.CHIP_NAME!=='ESP32-S3'||await loader.detectFlashSize()!=='16MB'||!(await loader.chip.getChipFeatures(loader)).some(x=>x.includes('PSRAM 8MB')))throw Error('This hardware is not supported. Nothing was changed.');
 const security=await loader.getSecurityInfo();if(security.flashCryptCnt!==0||security.parsedFlags.SECURE_BOOT_EN||security.parsedFlags.SECURE_DOWNLOAD_ENABLE)throw Error('This device uses protected firmware. Please contact support. Nothing was changed.');
 selected=plan(await loader.readFlash(0x8000,4096),await loader.readFlash(0xe000,8192));
 status('Checking the Public download and saving a settings fingerprint…');
 const response=await fetch('./firmware.bin');if(!response.ok)throw Error('Public firmware could not be downloaded. Try again.');binary=new Uint8Array(await response.arrayBuffer());await firmware(binary);before=await protectedHash();
 const mac=await loader.chip.readMac(loader);diagnostic.device=mac.slice(-8).toUpperCase();$('device').textContent='Kami • '+diagnostic.device+' • Public '+RELEASE.version;
 status('Ready. Your account, Wi-Fi, SD card and settings will be kept.');$('install').hidden=false;
 }catch(e){selected=null;status(e.name==='NotFoundError'?'No device selected. Click Connect when you’re ready.':e.message);await close();}finally{lock(false);}};
$('install').onclick=async()=>{lock(true);$('progress').hidden=false;try{
 status('Installing Public. Keep your Kami plugged in and this tab open.');
 await loader.writeFlash({fileArray:[{data:binary,address:selected.address}],flashSize:'keep',flashMode:'keep',flashFreq:'keep',eraseAll:false,compress:true,reportProgress:(_,n,total)=>{$('progress').value=n/total*85;}});
 status('Checking every byte of the installed firmware…');const digest=await loader.flashMd5sum(selected.address,binary.length);if(digest.toLowerCase()!=='55e756f174ced05e17dc4bde39b555b4')throw Error('Installed firmware verification failed.');$('progress').value=95;
 const after=await protectedHash();if(JSON.stringify(after)!==JSON.stringify(before))throw Error('Firmware was written, but the settings check failed. Keep the device connected and contact support.');
 status('Public is installed and verified. Restarting your Kami…');await loader.after('hard_reset');await close();$('progress').value=100;
 status('Public is installed. Unplug and reconnect your Kami if the screen stays blank. On the device, check Settings → About for v1.0.5, then try Chat.');$('install').hidden=true;$('done').hidden=false;selected=null;
 }catch(e){status('Recovery did not finish: '+e.message+' Keep this page open. Reconnect and retry the same recovery; do not erase the device.');selected=null;await close();}finally{lock(false);}};
if(!isSecureContext||!('serial'in navigator)){ $('connect').disabled=true;status('Open this page in desktop Chrome or Edge on a computer. Safari, Firefox and in-app browsers cannot connect to your Kami.');}

$('debug').onclick=async()=>{const text='Help me debug Kami browser recovery. Do not erase the device, replace account keys, or change partitions. The page installs Public v1.0.5 application-only on a validated ESP32-S3 16 MB / 8 MB PSRAM layout. Diagnose the failure and suggest the smallest safe next step.\n'+JSON.stringify(diagnostic,null,2);$('debugtext').value=text;$('debugtext').hidden=false;try{await navigator.clipboard.writeText(text);$('debugresult').textContent='Copied. Paste this into Claude or Codex.';}catch{$('debugresult').textContent='Select and copy the text below, then paste it into Claude or Codex.';}};

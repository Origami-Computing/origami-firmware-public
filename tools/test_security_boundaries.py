"""Host checks for widget path confinement and Wi-Fi portal DOM rendering.
Requires a C++ compiler and Node.js; does not contact or flash a device.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    source = temp / 'paths.cpp'
    source.write_text(r'''
#include "widgets/path_validation.h"
#include <cassert>
#include <string>
int main() {
 using widgets::validPathSegment;
 assert(validPathSegment("weather_today-2",31));
 assert(validPathSegment("12345678-abcd-1234-abcd-123456789abc",64));
 assert(validPathSegment(std::string(31,'a').c_str(),31));
 assert(!validPathSegment(std::string(32,'a').c_str(),31));
 assert(!validPathSegment(nullptr,31));
 for(auto s : {"", ".", "..", "../boot", "a/b", "a\\b", "%2e%2e", "a?b", "a#b", "a\nb"})
   assert(!validPathSegment(s,31));
}
''')
    subprocess.run(['c++', '-std=c++17', '-I', str(root/'include'), str(source), '-o', str(temp/'paths')], check=True)
    subprocess.run([str(temp/'paths')], check=True)
    portal = (root/'src/services/wifi_portal.cpp').read_text().split('<script>',1)[1].split('</script>',1)[0]
    (temp/'portal.js').write_text(portal)
    test = temp/'test.js'
    test.write_text(r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const hostile='<img src=x onerror=alert(1)>';let unsafeHTML=false;
class Element {
 constructor(){this.children=[];this.handlers={};this.classList={remove(){}};}
 set innerHTML(value){if(value.includes(hostile))unsafeHTML=true;this.children=[];}
 set textContent(value){this.text=String(value);this.children=[];}
 replaceChildren(){this.children=[];}
 appendChild(child){this.children.push(child);}
 addEventListener(name,fn){this.handlers[name]=fn;}
}
const elements={};let forgotten='';
const context={document:{getElementById:id=>elements[id]??=(new Element()),createElement:()=>new Element()},
 fetch:async()=>({json:async()=>({networks:[{ssid:hostile,rssi:-42}],max:4})}),
 setInterval(){},setTimeout(){},confirm:()=>false};
vm.createContext(context);vm.runInContext(fs.readFileSync(process.argv[2],'utf8'),context);
context.forget=(i,ssid)=>{assert.equal(i,0);forgotten=ssid;};
(async()=>{
 context.scan();context.refreshSaved();await new Promise(resolve=>setImmediate(resolve));
 assert(!unsafeHTML,'SSID interpreted as HTML');
 assert.equal(elements.networks.children.length,1);
 const row=elements.networks.children[0];assert(row.text.includes(hostile));
 row.handlers.click();assert.equal(elements.ssid.value,hostile);
 const saved=elements.saved.children[0];assert.equal(saved.children[0].text,hostile);
 saved.children[1].handlers.click();assert.equal(forgotten,hostile);
 console.log('PASS hostile SSIDs remain text; selection and forget callbacks preserve values');
})().catch(e=>{console.error(e);process.exitCode=1;});
''')
    subprocess.run(['node', str(test), str(temp/'portal.js')], check=True)
print('PASS path segments reject traversal, URL metacharacters and overlength input')

"""Read menu labels from staging; preview routing never calls device services."""
import json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
def pages():
 source=(ROOT/'src/apps/menu_app.cpp').read_text(encoding='utf-8')
 ids=['PAGE_MAIN','PAGE_AUDIO','PAGE_TIME','PAGE_GAMES']
 result=[]
 for name in ['Main','Audio','Time','Game']:
  body=re.search(r'k'+name+r'Entries\[\] = \{(.*?)\n\};',source,re.S)[1]
  body=re.sub(r'#ifdef.*?#endif','',body,flags=re.S)
  result.append([dict(label=label,child=ids.index(child) if child in ids else -1,icon=icon,opens=True) for label,child,icon in re.findall(r'\{"([^"]+)",.*?, (PAGE_\w+), icons::(k\w+)\}',body)])
 settings=(ROOT/'src/apps/settings_menu_app.cpp').read_text(encoding='utf-8')
 for array in ['kLabels','kWifiLabels','kSoundLabels','kSensorLabels','kMusicOutLabels']:
  body=re.search(array+r'\[[^]]+\] = \{(.*?)\};',settings,re.S)[1]
  result.append([dict(label=s,child=-1,icon=None,opens=True) for s in re.findall(r'"([^"]+)"',body)])
 result.append([dict(label=s,child=-1,icon=None,opens=i==1) for i,s in enumerate(['Sleep: on','Timer: 60s','Default: 60s','Sleep Now'])])
 for e in result[4]:
  e['child']={'WiFi':5,'Sound':6,'Sensors':7,'Sleep':9}.get(e['label'],-1)
  e['opens']=e['label'] in ['WiFi','Connect to app','Sound','Sleep','Sensors','About','Redo Tutorial','Upload Queue','Manage apps','App Downloader']
 result[6][0]['child']=8
 for e in result[8]:e['opens']=False
 return result
NAMES=['Launcher','Audio','Clock','Games','Settings','Wi-Fi','Sound','Sensors','Music Output','Sleep']
def generate(destination):
 data=pages()
 source=(ROOT/'src/apps/settings_menu_app.cpp').read_text(encoding='utf-8')
 callbacks=['Main','WifiMenu','SoundMenu','SensorsMenu','Music','SleepMenu']
 mappings={}
 for page,fn in zip(range(4,10),callbacks):
  body=source.split('static ui::ListItem settings'+fn+'ItemFn(',1)[1].split('static ui::ListItem',1)[0]
  mappings[page]=re.findall(r'&settings_icons::(k\w+)',body)
 s='#pragma once\n#include "assets/menu_icons.h"\n#include "assets/menu_motion.h"\n#include "assets/settings_icons.h"\nnamespace menu_cleanup_data {\nstruct Entry{const char* label;int child;const uint8_t* icon;bool opens;const uint8_t* motion;int size;uint16_t color;};\n'
 for i,rows in enumerate(data):
  values=[]
  for j,e in enumerate(rows):
   icon='assets::menu_icons::'+e['icon'] if e['icon'] else 'nullptr'
   motion='nullptr';size=20;color='0'
   if i==0:
    motion=f'assets::menu_motion::kMasks[{j}]';color=f'assets::menu_motion::kColors[{j}]'
   if i in mappings:
    asset='assets::settings_icons::'+mappings[i][j]
    icon=asset+'.still';motion=asset+'.motion';color=asset+'.color';size=20
   values.append('{'+json.dumps(e['label'])+','+str(e['child'])+','+icon+','+str(e['opens']).lower()+','+motion+','+str(size)+','+color+'}')
  s+='static const Entry entries'+str(i)+'[]={'+','.join(values)+'};\n'
 s+='static const Entry* pages[]={'+','.join('entries'+str(i) for i in range(len(data)))+'};\n'
 s+='static const int counts[]={'+','.join(str(len(rows)) for rows in data)+'};\n}\n'
 destination.write_text(s)

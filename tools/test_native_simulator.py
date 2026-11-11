"""Run production C++ games in the host simulator, without devices."""
import sys,time,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/lua_simulator'))
import server
class NativeTests(unittest.TestCase):
 def setUp(self):
  self.runtime=server.Runtime(server.NATIVE_BINARY);self.now=1000;self.logs=''
 def send(self,op='frame',**data):
  self.now+=34;self.runtime.started=time.monotonic()-self.now/1000
  result=self.runtime.request(dict(data,op=op));self.logs+=result.get('logs','')
  self.assertFalse(result.get('error'),result)
  return result
 def frames(self,n):
  for _ in range(n):result=self.send()
  return result
 def button(self,name,edge='tap'):return self.send('input',type=name,edge=edge)
 def tearDown(self):
  try:
   if self.runtime.error_file:
    self.runtime.error_file.seek(0);errors=self.runtime.error_file.read().decode(errors='replace')
    self.assertNotIn('runtime error:',errors);self.assertNotIn('ERROR: AddressSanitizer',errors)
  finally:self.runtime.close()
 def test_all_catalog_apps_start_input_stop_and_reenter_without_retained_resources(self):
  entries=[e for e in server.store.catalog(native_preview=True) if e.get('preview')]
  self.assertEqual(len(entries),15)
  for entry in entries:
   for repeat in range(2):
    self.send('environment',tilt=0,noise=35,failure=False)
    loaded=self.send('load',id=entry['id']);self.assertTrue(loaded['running'],entry['id'])
    self.frames(75)
    self.button('C');self.send('input',type='scroll',delta=1)
    self.button('B','down');self.frames(5);self.button('B','up');self.frames(5)
    stopped=self.send('stop');self.assertFalse(stopped['running'])
    self.assertEqual(stopped['resources'],{'gyro':0,'microphone':0},entry['id'])
 def test_tennis_and_bowling_use_actual_calibration_and_swing_detection(self):
  self.send('load',id='native-tennis');self.frames(80)
  self.assertIn('[tennis] calibrated',self.logs)
  self.button('C');self.frames(11);self.send('environment',gesture=1);self.frames(35)
  self.assertIn('[tennis] swing',self.logs)
  self.send('stop');self.logs=''
  self.send('load',id='native-bowling');self.frames(80)
  self.send('environment',gesture=3);self.frames(45)
  self.assertIn('[bowling] throw',self.logs)
  self.assertNotIn('no throw',self.logs)
 def test_golf_original_course_and_swing_produce_a_shot(self):
  self.send('load',id='native-golf');self.frames(80)
  self.button('C');self.button('C');self.frames(150)
  self.button('C');self.frames(20)
  self.send('environment',gesture=3);self.frames(60)
  self.assertIn('[golf] shot ',self.logs)
  self.assertNotIn('no stroke',self.logs)
 def test_microphone_calibrates_and_sensor_failure_recovers(self):
  self.send('load',id='native-dumb-flappy-bird');self.frames(80)
  self.assertIn('room baseline=',self.logs)
  self.button('C');self.send('environment',noise=65);self.frames(10)
  self.send('stop');self.send('environment',failure=True)
  self.send('load',id='native-tennis');self.frames(3)
  stopped=self.send('stop');self.assertEqual(stopped['resources'],{'gyro':0,'microphone':0})
  self.send('environment',failure=False);self.send('load',id='native-tennis');self.frames(80)
  self.assertIn('[tennis] calibrated',self.logs)
 def test_supplied_pcm_reaches_other_microphone_apps(self):
  # Actual Flappy capture/calibration must see supplied silence, not synthetic noise.
  self.send('environment',noise=80)
  self.send('load',id='native-dumb-flappy-bird')
  for _ in range(80):self.send('environment',micSamples=[0]*512)
  self.assertIn('room baseline=18.0',self.logs)
  self.assertEqual(self.send('stop')['resources']['microphone'],0)
 def test_unknown_native_app_and_install_path_rejected(self):
  sim=server.Simulator(ROOT/'examples/lua_apps/mac_counter.lua','/not-a-device')
  try:
   with self.assertRaises(ValueError):sim.request('native-load',{'id':'../../bin/sh'})
   sim.request('native-load',{'id':'native-pong'})
   with self.assertRaises(ValueError):sim.request('package',{'digest':None})
   with self.assertRaises(ValueError):sim.request('install',{'digest':None,'medium':'flash'})
  finally:sim.runtime.close();sim.native.close()
if __name__=='__main__':unittest.main()

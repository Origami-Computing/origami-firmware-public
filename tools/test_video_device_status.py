"""Check the actual device harness against shared-USB log corruption."""
import unittest
from video_av_device_check import parse_status,read_status

RECORD='[videos] active=1 ready=1 playing=0 ended=0 error=0 index=1 count=1 frame=1 total=100 audio=1 audio_ms=0 skipped=0 elapsed_ms=0 decode_max_ms=55 present_max_ms=31 path=/videos/test-av-sync-1793039676.avi'
class StatusTest(unittest.TestCase):
 def test_complete_prefixed_reply(self):
  # The prior startswith waiter discarded a real reply with this log prefix.
  self.assertEqual(parse_status('[ 36756][E][WiFiClientSecure.cpp:144] connect(): start_ssl_clien'+RECORD),parse_status(RECORD))
 def test_incomplete_or_interleaved_reply(self):
  for line in [RECORD[:100],RECORD.replace('total=100','[health] total=100'),RECORD[:-4],RECORD.split('path=')[0]+'path=','unrelated log']:
   self.assertIsNone(parse_status(line),line)
 def test_empty_catalog(self):
  self.assertEqual(parse_status(RECORD.replace('count=1','count=0').split('path=')[0]+'path=')['path'],'')
 def test_lost_reply_retried_without_accepting_partial(self):
  class Device:
   sent=0
   def send(self,cmd):self.sent+=1;assert cmd=='videos status'
   def line(self,deadline):
    if self.sent==1:raise TimeoutError()
    return '[other] '+RECORD
  d=Device()
  self.assertEqual(read_status(d)['frame'],'1');self.assertEqual(d.sent,2)
 def test_unresponsive_device_still_fails(self):
  class Device:
   sent=0
   def send(self,cmd):self.sent+=1
   def line(self,deadline):raise TimeoutError()
  d=Device()
  with self.assertRaises(TimeoutError):read_status(d)
  self.assertEqual(d.sent,3)
 def test_crash_is_not_retried(self):
  class Device:
   def send(self,cmd):pass
   def line(self,deadline):return 'Guru Meditation Error: Core 1 panic'
  with self.assertRaises(RuntimeError):read_status(Device())
if __name__=='__main__':unittest.main()

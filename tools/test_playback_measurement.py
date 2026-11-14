"""Synthetic checks of noise rejection; these are not physical speaker evidence."""
import tempfile
from pathlib import Path
import unittest
import wave
import numpy as np
from analyze_recording_playback import analyze


class MeasurementTest(unittest.TestCase):
    def measure(self, pre=0.001, post=0.001, amplitude=0.03):
        rate=48000
        rng=np.random.default_rng(42)
        x=np.concatenate((rng.normal(0,pre,rate),rng.normal(0,pre,rate),rng.normal(0,post,rate)))
        x[rate:2*rate]+=amplitude*np.sin(2*np.pi*1000*np.arange(rate)/rate)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'synthetic.wav'
            with wave.open(str(path),'wb') as f:
                f.setparams((1,2,rate,0,'NONE','not compressed'))
                f.writeframes(np.rint(x*32768).astype('<i2').tobytes())
            return analyze(path,(0,1),(1,2),(2,3))

    def test_stationary_noise_power_subtraction(self):
        r=self.measure()
        self.assertTrue(r['valid_for_noise_subtraction'])
        self.assertAlmostEqual(r['noise_corrected_dbfs'],20*np.log10(.03/np.sqrt(2)),delta=.1)

    def test_changed_noise_rejected(self):
        r=self.measure(post=.01)
        self.assertFalse(r['valid_for_noise_subtraction'])
        self.assertIsNone(r['noise_corrected_dbfs'])

    def test_quiet_playback_rejected(self):
        self.assertFalse(self.measure(amplitude=.001)['valid_for_noise_subtraction'])

    def test_gated_capture_rejected(self):
        self.assertFalse(self.measure(pre=0,post=0)['valid_for_noise_subtraction'])


if __name__=='__main__':unittest.main()

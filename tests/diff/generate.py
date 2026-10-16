#!/usr/bin/env python3
"""Recreate the small LC fixtures with FAAC, without network access."""
import math, pathlib, struct, subprocess, tempfile, wave
encoder = '/Users/nschimme/gitprojects/faac/build/frontend/faac'
out = pathlib.Path(__file__).resolve().parents[1] / 'data'
with tempfile.TemporaryDirectory(dir=out) as tmp:
    for name, rate, channels in [('stereo22_lc',22050,2),('stereo24_lc',24000,2),('stereo48_lc',48000,2),('surround71_lc',48000,8)]:
        wav = pathlib.Path(tmp) / (name+'.wav')
        with wave.open(str(wav),'wb') as f:
            f.setparams((channels,2,rate,0,'NONE','not compressed'))
            f.writeframes(b''.join(struct.pack('<h', round(6000*math.sin(2*math.pi*(220+73*c)*i/rate))) for i in range(10240) for c in range(channels)))
        subprocess.run([encoder,'-v0','--overwrite','--object-type','lc','-a','-b','96','-o',str(out/(name+'.aac')),str(wav)],check=True)

        # Exercise FAAC's raw mode too, and verify that transport removal is
        # exactly the raw packet data used by Init2 in the tests/harness.
        raw = pathlib.Path(tmp) / (name+'.raw')
        subprocess.run([encoder,'-v0','--object-type','lc','-r','-b','96','-o',str(raw),str(wav)],check=True)
        adts=(out/(name+'.aac')).read_bytes()
        packets=[]
        pos=0
        while pos<len(adts):
            size=((adts[pos+3]&3)<<11)|(adts[pos+4]<<3)|(adts[pos+5]>>5)
            header=7 if adts[pos+1]&1 else 9
            packets.append(adts[pos+header:pos+size])
            pos+=size
        assert b''.join(packets)==raw.read_bytes(), name
        assert len(adts)<20000, name

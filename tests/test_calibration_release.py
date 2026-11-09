"""Independent numeric checks against the published calibration; no devices."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

import cv2
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'assets/camera/calibration_20261109'


class CalibrationRelease(unittest.TestCase):
    def test_release_files_are_a_complete_pinned_set(self):
        contract = json.loads((ASSETS/'host_contract.json').read_text())
        for name, expected in contract['files'].items():
            self.assertEqual(hashlib.sha256((ASSETS/name).read_bytes()).hexdigest(), expected, name)
        self.assertEqual(contract['calibration_sha256'], 'c7836f67c2d4645389634b03e7e93518b13c6ecda3292c8d90319d24136d3dfa')

    def test_geo2_and_native16_apply_mapping_once(self):
        model = json.loads((ASSETS/'calibration.json').read_text())
        e = model['extrinsics']
        transform = np.array(e['R']) @ np.array(e['M'])
        data = (ASSETS/'M48_geometry.bin').read_bytes()
        actual = np.frombuffer(data, '<f8', 1536*3, 124).reshape(32,48,3)
        self.assertAlmostEqual(np.linalg.det(transform), -1, places=12)
        np.testing.assert_allclose(np.frombuffer(data,'<f8',3,84), e['t_m'], atol=1e-14)
        for rows, cols in [(32,48),(16,16)]:
            y, x = np.indices((rows,cols), dtype=float)
            q = np.stack(((x+.5-cols/2)/(.75*cols)-.0125,
                          (y+.5-rows/2)/rows+.0125, np.ones_like(x)), -1)
            q /= np.linalg.norm(q,axis=-1,keepdims=True)
            expected = q @ transform.T
            if cols == 48:
                np.testing.assert_allclose(actual, expected, atol=1e-14)
            else:
                x0,y0=-23.5/36-.0125,-15.5/32+.0125
                a=actual[0,0]*np.linalg.norm([x0,y0,1])
                b=actual[0,1]*np.linalg.norm([x0+1/36,y0,1])
                c=actual[1,0]*np.linalg.norm([x0,y0+1/32,1])
                col0=(b-a)*36;col1=(c-a)*32;col2=a-col0*x0-col1*y0
                derived=q @ np.column_stack((col0,col1,col2)).T
                np.testing.assert_allclose(derived, expected, atol=2e-14)

    def test_h_remap_uses_new_raw_intrinsics_and_unchanged_virtual_camera(self):
        model=json.loads((ASSETS/'calibration.json').read_text())
        data=(ASSETS/'rgb672_rectify.bin').read_bytes()
        self.assertEqual(struct.unpack('<8s2I',data[:16]),(b'NVMAP1\0\0',672,504))
        ids=np.frombuffer(data,'<u4',672*504,16).reshape(504,672)
        fractions=np.frombuffer(data,'<u2',672*504,16+672*504*4).reshape(504,672)
        decoded=np.stack((ids%2304+(fractions%32)/32,ids//2304+(fractions//32)/32),-1)
        y,x=np.indices((504,672),dtype=float);f=260*480/324
        rays=np.stack(((x-335.5)/f,(y-251.5)/f,np.ones_like(x)),-1)
        expected=cv2.projectPoints(rays.reshape(-1,3),np.zeros(3),np.zeros(3),np.array(model['camera']['K']),np.array(model['camera']['D']))[0].reshape(504,672,2)
        self.assertLess(np.max(np.abs(decoded-expected)), .016)
        identity=json.loads((ROOT/'UniDepthRuntimeRgb672VgaFP16RTX/model.json').read_text())
        self.assertEqual(identity['onnx_sha256'],'50d22cd33e126832793d905788a1efadeb4c9298de72cbfa634d49fdcdbc2dc1')
        self.assertEqual(identity['rectify_sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(identity['rectified_intrinsics_integer_centres'],[f,f,319.5,239.5])


if __name__ == '__main__':
    unittest.main()

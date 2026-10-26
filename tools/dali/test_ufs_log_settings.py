from pathlib import Path
import struct
import tempfile
import unittest
from ufs_log_settings import log_settings


class LogSettingsTests(unittest.TestCase):
    def fixture(self,folder,userdata=False):
        wire=bytearray(240 if userdata else 236);wire[:8]=b'DALIUFSW';struct.pack_into('<II',wire,8,4 if userdata else 3,4 if userdata else 2)
        wire[16:48]=b'a'*32;wire[48:160]=bytes([1])*112
        if userdata:
            wire[144:160]=bytes.fromhex('cde9ab3218d847778ced691e7cd5c636')
            struct.pack_into('<II8QII',wire,160,2,4096,64577536,64565240,8192,0,64565240,1024,7168,1,2,4194304)
        else:
            struct.pack_into('<II8QI',wire,160,2,4096,64577536,2184,32768,0,2440,256,32256,1,1)
        path=Path(folder)/'wire.bin';path.write_bytes(wire)
        env=dict(DALI_UFS_LOG_ENABLED='true',DALI_UFS_LOG_WIRE=str(path),DALI_UFS_PROFILE='userdata-logs-diagnostic' if userdata else 'expdb-log-diagnostic',
                 DALI_UFS_READ_ONLY='true',DALI_UFS_MEASURE_PAYLOAD='true',DALI_UFS_PROBE='false',
                 DALI_AUTO_FASTBOOT='false',DALI_RUN_TOKEN='a'*32)
        return wire,path,env

    def test_default_off_never_reads_media_or_wire_and_enabled_preserves_exact_bytes(self):
        self.assertEqual(log_settings({}),(False,'{0x00}'))
        with tempfile.TemporaryDirectory() as tmp:
            wire,path,env=self.fixture(tmp);enabled,encoded=log_settings(env)
            self.assertTrue(enabled);self.assertEqual(bytes(int(n,16) for n in encoded[1:-1].split(',')),wire)
            wire,path,env=self.fixture(tmp,True);enabled,encoded=log_settings(env)
            self.assertTrue(enabled);self.assertEqual(bytes(int(n,16) for n in encoded[1:-1].split(',')),wire)

    def test_explicit_profile_and_all_reset_or_measurement_controls_are_required(self):
        with tempfile.TemporaryDirectory() as tmp:
            _,_,env=self.fixture(tmp)
            for key,value in [('DALI_UFS_LOG_ENABLED','1'),('DALI_UFS_LOG_ENABLED','false'),
                ('DALI_UFS_LOG_WIRE',''),('DALI_UFS_PROFILE','read-only-blockio'),('DALI_UFS_READ_ONLY','false'),
                ('DALI_UFS_MEASURE_PAYLOAD','false'),('DALI_UFS_PROBE','true'),('DALI_AUTO_FASTBOOT','true'),
                ('DALI_RUN_TOKEN','bad'),('DALI_RESET_METHOD','psci')]:
                with self.assertRaises(ValueError):log_settings(dict(env,**{key:value}))

    def test_legacy_or_wrong_token_target_extent_and_nonregular_wire_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            wire,path,env=self.fixture(tmp)
            for at in (0,8,12,16,160,164,168,176,184,192,200,208,216,224,232):
                broken=bytearray(wire);broken[at]^=0xff;path.write_bytes(broken)
                if at==168:
                    # A different plausible capacity is a runtime observation
                    # check, not a reason to hardcode this device's old size.
                    struct.pack_into('<Q',broken,168,0);path.write_bytes(broken)
                with self.assertRaises(ValueError,msg=str(at)):log_settings(env)
            path.write_bytes(wire[:-1])
            with self.assertRaises(ValueError):log_settings(env)
            path.write_bytes(wire);link=Path(tmp)/'link';link.symlink_to(path)
            with self.assertRaises(OSError):log_settings(dict(env,DALI_UFS_LOG_WIRE=str(link)))


if __name__=='__main__':unittest.main()

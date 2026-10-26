"""Firmware record derives from clean actual repositories and their board/feed pins."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from ufs_source_record import source_record, measurement_settings


class UfsSourceRecordTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        base = Path(self.temp.name)
        self.uefi, self.unixnu, self.device = (base/name for name in ('uefi','unixnu','device'))
        self.env = dict(os.environ, GIT_AUTHOR_NAME='Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
                        GIT_COMMITTER_NAME='Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid')
        for path in (self.uefi, self.unixnu, self.device):
            path.mkdir(); self.git(path, 'init', '-q'); (path/'seed').write_text('fixture')
        self.commit(self.uefi)
        (self.unixnu/'feeds.toml').write_text('[[source]]\nname="xnu"\ncommit="'+'4'*40+'"\n')
        self.commit(self.unixnu)
        self.board()
        self.commit(self.device)

    def tearDown(self):
        self.temp.cleanup()

    def git(self, path, *args):
        return subprocess.check_output(['git','-C',str(path),*args],env=self.env,text=True).strip()

    def commit(self, path):
        self.git(path,'add','.'); self.git(path,'commit','-qm','fixture')

    def board(self):
        (self.device/'board.toml').write_text('[firmware]\nuefi_commit="'+self.git(self.uefi,'rev-parse','HEAD')+
            '"\nunixnu_commit="'+self.git(self.unixnu,'rev-parse','HEAD')+'"\n')

    def settings(self):
        return dict(DALI_UFS_MEASURE_PAYLOAD='true',DALI_UFS_READ_ONLY='true',DALI_UFS_PROBE='false',
            DALI_AUTO_FASTBOOT='false',DALI_RUN_TOKEN='1'*32,DALI_UFS_SOURCE_DEVICE=str(self.device),
            DALI_UNIXNU_PACKAGE=str(self.unixnu/'package'))

    def test_record_is_actual_four_pins_in_kernel_abi_order(self):
        expected=b'DALIUFSSRC1'+bytes(5)+b''.join(self.git(p,'rev-parse','HEAD').encode()+b'\0'
            for p in (self.device,self.unixnu,self.uefi))+b'4'*40+b'\0'
        self.assertEqual(source_record(self.uefi,self.unixnu,self.device), expected)
        enabled, array=measurement_settings(self.settings(),self.uefi)
        self.assertTrue(enabled)
        self.assertEqual(bytes(int(v,16) for v in array[1:-1].split(',')),expected)

    def test_disabled_default_never_reads_a_repository(self):
        self.assertEqual(measurement_settings({},'/does/not/exist'),(False,'{0x00}'))

    def test_dirty_and_untracked_source_cannot_supply_a_record(self):
        for repo in (self.device,self.unixnu,self.uefi):
            extra=repo/'extra'; extra.write_text('unexpected')
            with self.assertRaisesRegex(ValueError,'clean'):
                source_record(self.uefi,self.unixnu,self.device)
            extra.unlink()
            seed=repo/'seed'; seed.write_text('changed')
            with self.assertRaisesRegex(ValueError,'clean'):
                source_record(self.uefi,self.unixnu,self.device)
            seed.write_text('fixture')

    def test_board_and_feed_mismatch_fail_closed(self):
        (self.device/'board.toml').write_text('[firmware]\nuefi_commit="'+'0'*40+'"\n')
        self.commit(self.device)
        with self.assertRaisesRegex(ValueError,'board'):
            source_record(self.uefi,self.unixnu,self.device)
        self.board();self.commit(self.device)
        (self.unixnu/'feeds.toml').write_text('[[source]]\nname="xnu"\ncommit="latest"\n')
        self.commit(self.unixnu);self.board();self.commit(self.device)
        with self.assertRaisesRegex(ValueError,'exact commits'):
            source_record(self.uefi,self.unixnu,self.device)

    def test_measurement_requires_complete_explicit_mode_and_source_inputs(self):
        for key,value in [('DALI_UFS_READ_ONLY','false'),('DALI_UFS_PROBE','true'),
            ('DALI_AUTO_FASTBOOT','true'),('DALI_RUN_TOKEN','none'),('DALI_UFS_SOURCE_DEVICE',''),
            ('DALI_UNIXNU_PACKAGE',''),('DALI_RESET_METHOD','watchdog'),('DALI_UFS_MEASURE_PAYLOAD','1')]:
            env=self.settings();env[key]=value
            with self.assertRaises(ValueError,msg=key):
                measurement_settings(env,self.uefi)


if __name__ == '__main__':
    unittest.main()

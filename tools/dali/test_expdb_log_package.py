import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import uuid

import test_ufs_package as physical
from package import ufs_expdb_log_metadata,ufs_read_only_metadata,validate_expdb_log_kernel


class ExpdbLogPackagingTests(unittest.TestCase):
    token='a1'*16

    def fixture(self,folder):
        physical.PhysicalPackagingTests().fixture(folder)
        record=b'DALIUFSSRC1'+bytes(5)+b''.join((str(i)*40).encode()+b'\0' for i in range(1,5))
        kernel=b'whole kernel'+b'DALIUFSSRC1'+bytes(5)+b'ordinary string\0'+record+b'unloaded trailer'
        pins={key:str(i)*40 for i,key in enumerate(('device_commit','unixnu_commit','uefi_commit','xnu_commit'),1)}
        pins['kernel_sha256']=hashlib.sha256(kernel).hexdigest()
        source=hashlib.sha256(b'DALI-UFS-SOURCE-V1\0'+json.dumps(pins,sort_keys=True,separators=(',',':')).encode()).digest()
        wire=bytearray(236);wire[:8]=b'DALIUFSW';struct.pack_into('<II',wire,8,3,2)
        wire[16:48]=self.token.encode();wire[48:80]=bytes([1])*32
        wire[80:112]=hashlib.sha256(kernel).digest();wire[112:144]=source;wire[144:160]=bytes([1])*16
        struct.pack_into('<II8QI',wire,160,2,4096,64577536,2184,32768,0,2440,256,32256,1,1)
        wire=bytes(wire);path=folder/'grant.bin';path.write_bytes(wire)
        host=folder/'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG'
        bds=folder/'daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG'
        for auto in (host,bds):
            p=auto/'AutoGen.h';p.write_text(p.read_text().replace('ScratchEnabled 0U','ScratchEnabled 1U')+
                '#define _PCD_VALUE_PcdDaliUfsLogEnabled 1U\n'+
                ('#define _PCD_VALUE_PcdDaliUfsMeasurePayload 1U\n' if auto==host else ''))
        p=host/'AutoGen.c';p.write_text(p.read_text()+''.join(
            f'const UINT8 _gPcd_FixedAtBuild_PcdDaliUfs{name}[{len(data)}] = {{'+','.join(str(v) for v in data)+'};\n'
            for name,data in (('ScratchWire',wire),('SourceRecord',record))))
        protocol=uuid.UUID('6e54ce7f-374c-47ec-a028-62d95c858475').bytes_le
        for name in ('DaliUfsHostDxe','DaliDiagnosticBds'):
            p=folder/(name+'.efi');p.write_bytes(p.read_bytes()+protocol+(wire+record if name=='DaliUfsHostDxe' else b''))
        fd=physical.combined_firmware([(guid,(folder/(name+'.efi')).read_bytes()) for name,guid in physical.PHYSICAL_UFS_MODULES.items()])
        return path,wire,record,kernel,fd

    def test_dedicated_profile_binds_final_fd_wire_measurement_and_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp);path,wire,record,kernel,fd=self.fixture(folder)
            probe,host,log=ufs_expdb_log_metadata(folder,self.token,path,fd=fd,expected_source_record=record)
            self.assertFalse(probe['enabled']);self.assertTrue(host['media_writes'])
            self.assertTrue(host['runtime_authorization_required']);self.assertFalse(host['write_authorized'])
            self.assertEqual(log['wire_sha256'],hashlib.sha256(wire).hexdigest());self.assertTrue(log['ramoops_retained'])
            self.assertFalse(log['automatic_fastboot_enabled']);self.assertFalse(log['persistence_qualified'])
            validate_expdb_log_kernel(kernel,wire,record)
            with self.assertRaises(ValueError):ufs_read_only_metadata(folder,self.token,fd=fd)

    def test_disabled_missing_duplicate_flags_stale_wire_source_and_final_fd_refuse(self):
        for fault in ('host-log','bds-log','scratch','measurement','duplicate-log','wire','record','fd'):
            with self.subTest(fault=fault),tempfile.TemporaryDirectory() as tmp:
                folder=Path(tmp);path,wire,record,_,fd=self.fixture(folder)
                auto=folder/('daliPkg/Drivers/DiagnosticBdsDxe/DiagnosticBdsDxe/DEBUG' if fault=='bds-log' else 'MT6991Pkg/Drivers/UfsHostDxe/UfsHostDxe/DEBUG')
                if fault in ('host-log','bds-log','scratch','measurement'):
                    flag={'host-log':'LogEnabled','bds-log':'LogEnabled','scratch':'ScratchEnabled','measurement':'MeasurePayload'}[fault]
                    p=auto/'AutoGen.h';p.write_text(p.read_text().replace(flag+' 1U',flag+' 0U'))
                elif fault=='duplicate-log':
                    p=auto/'AutoGen.h';p.write_text(p.read_text()+'#define _PCD_VALUE_PcdDaliUfsLogEnabled 1U\n')
                elif fault=='wire':
                    changed=bytearray(wire);changed[80]^=1;path.write_bytes(changed)
                elif fault=='record':record=record[:-2]+b'9\0'
                else:fd=fd[:-8]
                with self.assertRaises(ValueError):ufs_expdb_log_metadata(folder,self.token,path,fd=fd,expected_source_record=record)

    def test_actual_kernel_tail_digest_stale_or_multiple_valid_records_refuse(self):
        with tempfile.TemporaryDirectory() as tmp:
            _,wire,record,kernel,_=self.fixture(Path(tmp))
            for value in (kernel[:-1],kernel+b'X',kernel+record,kernel.replace(record,b'X'*180)):
                with self.assertRaises(ValueError):validate_expdb_log_kernel(value,wire,record)
            # Even if kernel SHA is edited, duplicate records and a changed
            # canonical source digest still cannot pass the independent gate.
            changed=bytearray(wire);changed[80:112]=hashlib.sha256(kernel+record).digest()
            with self.assertRaisesRegex(ValueError,'ambiguous'):validate_expdb_log_kernel(kernel+record,bytes(changed),record)
            changed=bytearray(wire);changed[112]^=1
            with self.assertRaisesRegex(ValueError,'source digest'):validate_expdb_log_kernel(kernel,bytes(changed),record)


if __name__=='__main__':unittest.main()

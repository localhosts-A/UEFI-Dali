"""Compile-only expdb log inputs. Runtime measurements issue the actual grant."""
import os
from pathlib import Path
import re
import stat
import struct


def log_settings(environment):
    enabled=environment.get('DALI_UFS_LOG_ENABLED','false')
    if enabled not in ('true','false'):
        raise ValueError('DALI_UFS_LOG_ENABLED must be true or false')
    path=environment.get('DALI_UFS_LOG_WIRE','')
    if enabled=='false':
        if path:raise ValueError('a log wire requires explicit DALI_UFS_LOG_ENABLED=true')
        return False,'{0x00}'
    token=environment.get('DALI_RUN_TOKEN','')
    if (environment.get('DALI_UFS_READ_ONLY')!='true' or
            environment.get('DALI_UFS_PROFILE')!='expdb-log-diagnostic' or
            environment.get('DALI_UFS_MEASURE_PAYLOAD')!='true' or
            environment.get('DALI_UFS_PROBE','false')!='false' or
            environment.get('DALI_AUTO_FASTBOOT')!='false' or environment.get('DALI_RESET_METHOD') or
            not re.fullmatch('[0-9a-f]{32}',token) or not path):
        raise ValueError('expdb log needs its own explicit measured/manual profile, token and wire')
    fd=os.open(Path(path),os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK)
    try:
        info=os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_size!=236:
            raise ValueError('log wire must be a regular exact236-byte expdbv3 record')
        with os.fdopen(fd,'rb',closefd=False) as source:wire=source.read(237)
        if len(wire)!=236:raise ValueError('log wire changed length')
    finally:os.close(fd)
    if (wire[:8]!=b'DALIUFSW' or struct.unpack_from('<II',wire,8)!=(3,2) or
            wire[16:48]!=token.encode() or struct.unpack_from('<I',wire,232)[0]!=1 or
            any(not any(wire[at:at+size]) for at,size in ((48,32),(80,32),(112,32),(144,16)))):
        raise ValueError('wrong expdb wire schema, token, target or empty identity binding')
    lun,block=struct.unpack_from('<II',wire,160)
    capacity,start,blocks,payload,scratch,count,guard,sequence=struct.unpack_from('<8Q',wire,168)
    if block<512 or block>65536 or block&(block-1) or lun>=8:
        raise ValueError('invalid wire LUN or logical block geometry')
    one=1048576//block
    if (not start or start+blocks>capacity or blocks*block>(1<<64)-1 or blocks<6*one or
            payload or scratch!=start+one or count!=one or guard!=blocks-2*one or
            not 0<sequence<=count//2):
        raise ValueError('wire is not the fixed expdb one-MiB window with preserved guards')
    return True,'{'+','.join('0x%02x'%byte for byte in wire)+'}'

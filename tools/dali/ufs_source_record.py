"""Derive firmware's compiled four-repository record from actual build inputs.

This checks local input consistency, not compiler/network attestation. It does
not accept a grant, a kernel manifest or caller-supplied digest as an observation.
"""
from pathlib import Path
import re
import subprocess
import tomllib

FIELDS = ('device_commit', 'unixnu_commit', 'uefi_commit', 'xnu_commit')
MAGIC = b'DALIUFSSRC1' + bytes(5)


def source_record(uefi, unixnu, device):
    repos = {key: Path(path).resolve() for key, path in (
        ('uefi_commit', uefi), ('unixnu_commit', unixnu), ('device_commit', device))}
    pins = {}
    def git(repo, *args):
        return subprocess.check_output(['git', '-C', str(repo), *args], text=True).strip()
    for name, repo in repos.items():
        if Path(git(repo, 'rev-parse', '--show-toplevel')).resolve() != repo:
            raise ValueError('source record requires exact repository roots')
        # Mu's owned patchset is checked separately by the normal build/flash
        # verifier. Do not mistake generated ignored build outputs for sources.
        if git(repo, 'status', '--porcelain', '--untracked-files=all', '--ignore-submodules=all'):
            raise ValueError('source record requires clean top-level sources: ' + str(repo))
        pins[name] = git(repo, 'rev-parse', 'HEAD')
    board = tomllib.loads((repos['device_commit']/'board.toml').read_text())['firmware']
    if any(board.get(k) != pins[k] for k in ('unixnu_commit', 'uefi_commit')):
        raise ValueError('source record firmware revisions differ from board pins')
    sources = tomllib.loads((repos['unixnu_commit']/'feeds.toml').read_text())['source']
    xnu = [entry for entry in sources if entry.get('name') == 'xnu']
    if len(xnu) != 1:
        raise ValueError('source record requires one fixed XNU feed')
    pins['xnu_commit'] = xnu[0].get('commit')
    if any(not isinstance(pins.get(k), str) or not re.fullmatch('[0-9a-f]{40}', pins[k]) for k in FIELDS):
        raise ValueError('source record requires four exact commits')
    return MAGIC + b''.join(pins[k].encode('ascii') + b'\0' for k in FIELDS)


def measurement_settings(environment, uefi):
    enabled = environment.get('DALI_UFS_MEASURE_PAYLOAD', 'false')
    if enabled not in ('true', 'false'):
        raise ValueError('DALI_UFS_MEASURE_PAYLOAD must be true or false')
    if enabled == 'false':
        return False, '{0x00}'
    if (environment.get('DALI_UFS_READ_ONLY') != 'true' or
            environment.get('DALI_UFS_PROBE', 'false') != 'false' or
            environment.get('DALI_AUTO_FASTBOOT') != 'false' or environment.get('DALI_RESET_METHOD') or
            not re.fullmatch('[0-9a-f]{32}', environment.get('DALI_RUN_TOKEN', '')) or
            not environment.get('DALI_UFS_SOURCE_DEVICE') or not environment.get('DALI_UNIXNU_PACKAGE')):
        raise ValueError('payload measurement requires explicit source device/package, RO token and manual recovery')
    record = source_record(uefi, Path(environment['DALI_UNIXNU_PACKAGE']).resolve().parent,
                           environment['DALI_UFS_SOURCE_DEVICE'])
    return True, '{' + ','.join('0x%02x' % b for b in record) + '}'

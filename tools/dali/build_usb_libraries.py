#!/usr/bin/env python3
"""Build actual portable/device/UEFI USB libraries with EDK2; never run a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generic', type=Path, default=root.parent / 'UniXNU')
    parser.add_argument('--device', type=Path, default=root.parent / 'UniXNU-Dali')
    parser.add_argument('--edk2', type=Path, default=root / 'Mu_Basecore')
    parser.add_argument('--output', type=Path, required=True, help='independent output workspace')
    args = parser.parse_args()
    edk, generic, device, output = (p.resolve() for p in (args.edk2, args.generic, args.device, args.output))
    for source in (root, generic, device, edk):
        if output.is_relative_to(source) or source.is_relative_to(output):
            parser.error(f'output must be separate from the source/dependency tree: {source}')
    clang = shutil.which('clang')
    archiver, nm = shutil.which('llvm-ar'), shutil.which('llvm-nm')
    if not clang or not archiver or not nm:
        parser.error('clang, llvm-ar and llvm-nm are required')
    tools = edk / 'BaseTools'
    if not (tools / 'Source/C/bin/GenFw').is_file():
        parser.error('initialize/build the isolated EDK2 BaseTools first')
    conf = output / 'Conf'
    conf.mkdir(parents=True, exist_ok=True)
    (output / 'usb-library-build.json').unlink(missing_ok=True)
    for name in ('target', 'tools_def', 'build_rule'):
        shutil.copyfile(tools / 'Conf' / (name + '.template'), conf / (name + '.txt'))
    env = os.environ.copy()
    env.update(WORKSPACE=str(output), CONF_PATH=str(conf), EDK_TOOLS_PATH=str(tools),
               PACKAGES_PATH=os.pathsep.join(map(str, (root / 'Platforms/Xiaomi',
                   root / 'Silicon/MediaTek', root / 'Silicon/Silicium',
                   generic / 'package', generic / 'drivers', device / 'drivers', edk))),
               CLANG_BIN=str(Path(clang).parent) + '/', PYTHON_COMMAND=sys.executable)
    env['PATH'] = os.pathsep.join((str(tools / 'BinWrappers/PosixLike'), str(tools / 'Source/C/bin'), env.get('PATH', '')))
    command = [str(tools / 'BinWrappers/PosixLike/build'), '-p', 'daliPkg/UsbLibraries.dsc',
               '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB', '-n', '4']
    subprocess.run(command, cwd=output, env=env, check=True)
    artifacts, members, definitions = {}, {}, {}
    required = {
        'UsbCoreLib': (('UsbCore', 'UsbHost', 'UsbStorage', 'UsbMtu3Qmu', 'UsbMtu3Ep0', 'UsbMtu3Endpoints', 'UsbMtu3Device', 'UsbXhci', 'UsbXhciController'),
                       ('usb_cdc_control', 'usb_host_enumerate', 'usb_host_start_storage',
                        'usb_host_poll_input', 'usb_host_disconnect', 'usb_host_hub_start',
                        'usb_host_hub_poll', 'usb_host_hub_child_route', 'usb_host_hub_stop',
                        'usb_bot_command', 'usb_bot_read_only', 'usb_block_io_init', 'usb_block_io_transfer',
                        'usb_block_io_sync', 'usb_block_io_quiesce', 'usb_block_io_detach',
                        'usb_qmu_init', 'usb_mtu3_ep0_init', 'usb_mtu3_endpoints_init',
                        'usb_mtu3_device_init', 'usb_mtu3_device_log',
                        'usb_xhci_read_capabilities', 'usb_xhci_controller_start', 'usb_xhci_controller_stop',
                        'usb_xhci_controller_submit_command', 'usb_xhci_controller_next_event',
                        'usb_xhci_control_submit', 'usb_xhci_control_event',
                        'usb_xhci_control_finish', 'usb_xhci_control_release_stopped',
                        'usb_xhci_controller_command', 'usb_xhci_device_prepare',
                        'usb_xhci_device_address', 'usb_xhci_device_configure_hub',
                        'usb_xhci_device_control', 'usb_xhci_device_disable',
                        'usb_xhci_device_release_stopped',
                        'usb_xhci_transfer_ring_init', 'usb_xhci_transfer_ring_mark_active',
                        'usb_xhci_transfer_ring_enqueue', 'usb_xhci_transfer_ring_complete_through',
                        'usb_xhci_transfer_ring_halted', 'usb_xhci_transfer_ring_destroy',
                        'usb_xhci_event_ring_init', 'usb_xhci_event_ring_mark_active',
                        'usb_xhci_event_ring_read', 'usb_xhci_event_ring_dequeue_address',
                        'usb_xhci_event_ring_erst_address', 'usb_xhci_event_ring_halted',
                        'usb_xhci_event_ring_destroy')),
        'DaliUsbCdcLogLib': (('DaliUsbCdcLog',),
                             ('dali_usb_cdc_log_bind', 'dali_usb_cdc_log_unbind',
                              'dali_usb_cdc_log_write', 'dali_usb_cdc_log_dropped')),
        'DaliUsbLib': (('DaliUsbPmif', 'DaliUsbVusb', 'DaliUsbTypec', 'DaliUsbMt6379Repeater', 'DaliUsbNxpRepeater', 'DaliUsbGpio', 'DaliUsbI2c', 'DaliUsbI2cPins', 'DaliUsbI2cClocks', 'DaliUsbPhy', 'DaliUsbDomains', 'DaliUsbResources', 'DaliUsbIppc', 'DaliUsbCalibration', 'DaliUsbClockRates'),
                       ('dali_usb_pmif_init', 'dali_mt6379_read', 'dali_usb_typec_start', 'dali_usb_vbus_enable',
                        'dali_mt6379_repeater_power_on', 'dali_nxp_repeater_probe', 'dali_usb_gpio72_set',
                        'dali_usb_i2c6_init', 'dali_usb_i2c6_check_idle', 'dali_usb_i2c6_pins_read', 'dali_i2c_clocks_init', 'dali_i2c_clocks_enable', 'dali_i2c_clocks_destroy', 'dali_nxp_io_bind', 'dali_usb_phy_power_on', 'dali_usb_domains_power_on',
                        'dali_usb_clocks_enable', 'dali_usb_hwrsc_enable', 'dali_usb_ippc_enable', 'dali_usb_ippc_stop', 'dali_usb_calibration_decode', 'dali_usb_clock_rates_read',
                        'dali_usb_pmif_init_vusb', 'dali_mt6373_vusb_identify', 'dali_usb_vusb_acquire', 'dali_usb_vusb_release')),
        'DaliUsbPmicLib': (('UsbPmicLib',), ('DaliUsbPmicOpen', 'DaliUsbVusbPmicOpen', 'DaliUsbPmicClose')),
        'DaliUsbI2cLib': (('UsbI2cLib',), ('DaliUsbNativeI2cOpen', 'DaliUsbNativeI2cRead', 'DaliUsbNativeI2cWrite', 'DaliUsbNativeI2cReset', 'DaliUsbNativeI2cRecover', 'DaliUsbNativeI2cClose')),
        'DaliUsbXhciIoLib': (('UsbXhciIoLib',), ('DaliUsbXhciIoOpen', 'DaliUsbXhciIoQuiesce', 'DaliUsbXhciIoRearm', 'DaliUsbXhciIoClose')),
        'DaliUsbNativeHostLib': (('UsbNativeHostLib', 'MuEngine', 'MuSchedule', 'MuRegister', 'MuMemory'),
            ('DaliUsbNativeHostStart', 'DaliUsbNativeHostStop', 'DaliUsbNativeHostPoll',
             'DaliUsbNativeHostPrepareHandoff', 'DaliNativeXhciCommandFailed', 'DaliNativeXhciCheck',
             'DaliNativeXhciRecordShortCompletion', 'DaliNativeXhciIgnoreSpuriousSuccess',
             'DaliNativeXhciClearShortCompletions', 'DaliNativeXhciBeginEndpointConfiguration',
             'DaliNativeXhciCommitEndpointConfiguration', 'DaliNativeXhciRollbackEndpointConfiguration',
             'DaliNativeXhciRemoveSlotReservations', 'DaliNativeXhciScheduleEndpoint',
             'XhcControlTransfer', 'XhcBulkTransfer', 'XhcCreateTransferTrb', 'UsbHcInitMemPool')),
        'DaliUsbPlatformLib': (('UsbCalibration', 'UsbClockResources', 'UsbVusbResources', 'UsbI2cResources'), ('DaliUsbReadCalibration', 'DaliUsbReadClockResources', 'DaliUsbReadXhciResources', 'DaliUsbReadVusbResources', 'DaliUsbReadI2cResources')),
        'BaseFdtLib': (('fdt_check', 'FdtLib'), ('fdt_check_full', 'FdtGetProp')),
    }
    required_undefined = {'DaliUsbCdcLogLib': ('usb_mtu3_device_log',)}
    checked_undefined = {}
    for name, (objects, symbols) in required.items():
        matches = sorted((output / 'Build').rglob(name + '.lib'))
        if len(matches) != 1:
            raise RuntimeError(f'expected one actual {name} archive, got {matches}')
        archive = matches[0]
        inventory = subprocess.check_output([archiver, 't', str(archive)], text=True).splitlines()
        if not {item + '.obj' for item in objects} <= {Path(item).name for item in inventory}:
            raise RuntimeError(f'missing production objects in {archive}: {inventory}')
        defined = subprocess.check_output([nm, '--defined-only', str(archive)], text=True)
        if not set(symbols) <= {line.split()[-1] for line in defined.splitlines() if len(line.split()) >= 3 and line.split()[-2] == 'T'}:
            raise RuntimeError(f'missing production definitions in {archive}')
        if name in required_undefined:
            unresolved = subprocess.check_output([nm, '--undefined-only', str(archive)], text=True)
            unresolved_symbols = {line.split()[-1] for line in unresolved.splitlines() if line.split()}
            missing = set(required_undefined[name]) - unresolved_symbols
            if missing:
                raise RuntimeError(f'{archive} does not reference the portable MTU3 log API: {sorted(missing)}')
            checked_undefined[name] = sorted(required_undefined[name])
        artifacts[str(archive.relative_to(output))] = hashlib.sha256(archive.read_bytes()).hexdigest()
        members[name], definitions[name] = inventory, symbols
    evidence = {'scope': 'actual EDK2 AArch64 library compilation/archive only; no OS binding or hardware qualification',
                'command': command, 'artifacts': artifacts, 'members': members,
                'checked_definitions': definitions, 'checked_undefined': checked_undefined,
                'hardware_qualified': False}
    (output / 'usb-library-build.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(evidence, indent=2))


if __name__ == '__main__':
    main()

import unittest

from collect import parse_evidence


class EvidenceTests(unittest.TestCase):
    def test_qualification_requires_selected_timed_smc_without_watchdogs(self):
        token = '0123456789abcdef0123456789abcdef'
        prefix = ('DALI: SEC diagnostics v1\nDALI: UEFI_DIAGNOSTIC_PASS\n'
                  f'DALI: reset qualification run={token} marker=40002003 mode=17c watchdog=off\n'
                  f'DALI: reset policy run={token} method=psci-cold delay=5 firmware-watchdogs=off status=Success\n'
                  f'DALI-XNU: darwin=23.6.0 build=10063.141.1 run={token} bootarg=1 registered=1 '
                  'platform=1 gic=1 timer=1 mpidr=0x81000000 checkpoint=PASS\n')
        def line(stage, ticks):
            return (f'DALI-RESET: run={token} method=psci-system-reset stage={stage} '
                    f'marker=0x40002003 mode=0x17c qualification=1 delay_ms=5000 elapsed_ticks={ticks} freq=13000000 type=0x0\n')
        good = prefix + line('wait', 0) + line('issue', 65000000) + line('smc', 65000000)
        def parse(text, method='psci-cold'):
            return parse_evidence(text.encode(), expected_run_token=token, expected_reset_method=method)
        self.assertTrue(parse(good)['recovery']['software_reset_requested'])
        self.assertIn('unverified', parse(good)['recovery']['automatic_fastboot'])
        for text in (good.replace('elapsed_ticks=65000000', 'elapsed_ticks=64999999'),
                     good.replace('mode=0x17c', 'mode=0x17d'), good.replace('mode=17c', 'mode=17d'),
                     good.replace('type=0x0', 'type=0x80000201'), good.replace('firmware-watchdogs=off', 'firmware-watchdogs=on'),
                     good.replace(line('smc', 65000000), ''), good + line('returned', 65000000),
                     good + line('features-refused', 65000000), good + line('issue', 65000000),
                     line('wait', 0) + prefix + line('issue', 65000000) + line('smc', 65000000)):
            with self.subTest(text=text):
                self.assertFalse(parse(text)['recovery']['software_reset_requested'])
        self.assertFalse(parse(good, 'psci-warm')['recovery']['software_reset_requested'])
        self.assertFalse(parse_evidence(good.encode(), expected_run_token='f' * 32,
                                      expected_reset_method='psci-cold')['recovery']['software_reset_requested'])
        for bad in (good.replace(f'run={token} bootarg', f'run={"f" * 32} bootarg'),
                    good.replace('checkpoint=PASS', 'checkpoint=FAIL'), good.replace('gic=1', 'gic=0')):
            self.assertFalse(parse(bad)['recovery']['qualification']['pass'])
        legacy = (prefix + f'DALI: auto-fastboot armed run={token} marker=40002003 timeout=31 seconds\n' +
                  f'DALI-RESET: run={token} method=psci-aee-reset2 stage=issue marker=0x40002003 mode=0x135\n')
        self.assertFalse(parse(legacy)['recovery']['software_reset_requested'])

    def test_reset_requires_matching_post_checkpoint_request_and_does_not_prove_no_keys(self):
        token = '0123456789abcdef0123456789abcdef'
        prefix = (f'DALI: SEC diagnostics v1\nDALI: UEFI_DIAGNOSTIC_PASS\n'
                  f'DALI: auto-fastboot armed run={token} marker=40002003 timeout=31 seconds\n'
                  f'DALI-XNU: darwin=23.6.0 build=10063.141.1 run={token} bootarg=1 registered=1 '
                  'platform=1 gic=1 timer=1 mpidr=0x81000000 checkpoint=PASS\n')
        issue = f'DALI-RESET: run={token} method=rgu-swrst stage=issue marker=0x40002003 mode=0x135\n'
        result = parse_evidence((prefix + issue).encode(), expected_run_token=token)
        self.assertTrue(result['recovery']['software_reset_requested'])
        self.assertIn('unverified', result['recovery']['automatic_fastboot'])
        controls = [prefix, issue + prefix,
                    prefix + issue.replace(token, 'f' * 32),
                    prefix + issue.replace('mode=0x135', 'mode=0x13d'),
                    prefix + issue.replace('marker=0x40002003', 'marker=0x40002002'),
                    prefix + issue + issue.replace('stage=issue', 'stage=returned')]
        for control in controls:
            with self.subTest(control=control):
                self.assertFalse(parse_evidence(control.encode(), expected_run_token=token)['recovery']['software_reset_requested'])
        for method in ('psci-system-reset', 'psci-system-reset2', 'psci-aee-reset2'):
            psci = issue.replace('method=rgu-swrst', 'method=' + method)
            returned = psci.replace('stage=issue', 'stage=returned').replace('\n', ' features=0xffffffff result=0xffffffff\n')
            self.assertTrue(parse_evidence((prefix + psci).encode(), expected_run_token=token)['recovery']['software_reset_requested'])
            self.assertFalse(parse_evidence((prefix + psci + returned).encode(), expected_run_token=token)['recovery']['software_reset_requested'])

    def test_core_pass_does_not_prove_unixnu(self):
        result = parse_evidence(b"DALI: UEFI_DIAGNOSTIC_PASS\nDALI: UEFI_STAGE_FAIL consoles\n")
        self.assertTrue(result["diagnostic_pass"])
        self.assertFalse(result["unixnu"]["menu_shown"])
        self.assertTrue(result["failures"])

    def test_complete_unixnu_handoff(self):
        result = parse_evidence(b"DALI: payload filesystem=PASS\nDALI: UEFI_UNIXNU_READY\n"
                                b"UNIXNU: UniXNU 0.1 starting as firmware boot option\n"
                                b"UNIXNU: config: loaded config\nUNIXNU: menu: shown 2 entries\n"
                                b"UNIXNU: driver SampleDriver.efi (config): started\n"
                                b"UNIXNU: boot: HelloPayload.efi returned Success\n"
                                b"UNIXNU: action: exit\nDALI: UniXNU returned status=Aborted\n")
        self.assertTrue(all(result["unixnu"].values()))

    def test_menu_does_not_hide_driver_load_failure(self):
        result = parse_evidence(b"UNIXNU: menu: shown 2 entries\n"
                                b"UNIXNU: driver SampleDriver.efi (config): load failed: Access Denied\n")
        self.assertTrue(result["unixnu"]["menu_shown"])
        self.assertFalse(result["unixnu"]["sample_driver_started"])
        self.assertTrue(result["failures"])

    def test_keypad_and_event_wait(self):
        result = parse_evidence(b"DALI: keypad event scan=1 unicode=0\n"
                                b"DALI: keypad event scan=0 unicode=D\n"
                                b"DALI: WaitForEvent timer status=Success\n")
        self.assertEqual(result["io"]["keypad_events"], [{"scan": 1, "unicode": 0}, {"scan": 0, "unicode": 13}])
        self.assertTrue(result["io"]["event_wait_pass"])

    def test_watchdog_and_pmic_sampling_are_distinct_from_menu_success(self):
        result = parse_evidence(b"DALI: inherited RGU WDT mode=23->22 length=F808 disabled=1\n"
                                b"DALI: MT6363 TOPSTATUS=AB time-ms=12345\n"
                                b"DALI: app heartbeat=3 time-ms=15000\n")
        self.assertTrue(result["io"]["inherited_watchdog_disabled"])
        self.assertEqual(result["io"]["pmic_samples"], [{"raw": 0xAB, "time_ms": 12345}])
        self.assertEqual(result["io"]["app_heartbeats"], [{"number": 3, "time_ms": 15000}])
        self.assertFalse(result["unixnu"]["hello_returned_success"])

    def test_pmic_key_proof_requires_each_correct_source_and_code(self):
        result = parse_evidence(b"DALI: keypad event scan=0 unicode=D time-ms=1 source=volume-up\n"
                                b"DALI: keypad event scan=1 unicode=0 time-ms=2 source=volume-down\n")
        self.assertFalse(any(result["io"]["pmic_key_events"].values()))
        result = parse_evidence(b"DALI: keypad event scan=2 unicode=0 time-ms=3 source=volume-down\n"
                                b"DALI: keypad event scan=0 unicode=D time-ms=4 source=power\n")
        self.assertTrue(all(result["io"]["pmic_key_events"].values()))

    def test_readback_does_not_prove_visibility_or_submission(self):
        result = parse_evidence(b"DALI: SEC diagnostics v1\nDALI: UEFI_DIAGNOSTIC_PASS\n"
                                b"DALI: GOP_BLT_PASS (CPU readback)\n")
        self.assertTrue(result["diagnostic_pass"])
        self.assertTrue(result["io"]["gop_cpu_readback_pass"])
        self.assertEqual(result["io"]["display_visibility"], "unverified")
        self.assertFalse(result["io"]["frame_submission_enabled"])

    def test_new_boot_does_not_inherit_old_pass(self):
        result = parse_evidence(b"DALI: SEC diagnostics v1\nDALI: UEFI_DIAGNOSTIC_PASS\n"
                                b"DALI: SEC diagnostics v1\nASSERT failed\n")
        self.assertFalse(result["diagnostic_pass"])
        self.assertEqual(result["failures"], ["ASSERT failed"])

    def test_input_and_frame_evidence(self):
        result = parse_evidence(b"DALI: INPUT key=2 scan=0 unicode=D\n"
                                b"DALI: frame submission enabled; mutex=0\n"
                                b"DALI: frame submit=1 mutex=0\n"
                                b"DALI: frame completion sample submit=1 DSI=4\n")
        self.assertEqual(result["io"]["input_events"], [{"number": 2, "scan": 0, "unicode": 13}])
        self.assertEqual(result["io"]["frame_submissions"], [1])
        self.assertEqual(len(result["io"]["frame_completion_samples"]), 1)

    def test_xnu_checkpoint_requires_forwarded_bootarg_single_cpu_gic_and_timer(self):
        token = "0123456789abcdef0123456789abcdef"
        result = parse_evidence(
            ("DALI: SEC diagnostics v1\nDALI: UEFI_DIAGNOSTIC_PASS\n"
             f"DALI: auto-fastboot armed run={token} marker=13 timeout=31 seconds; waiting for XNU to stop\n"
             f"DALI-XNU: darwin=23.6.0 build=10063.141.1 run={token} bootarg=1 registered=1 "
             "platform=1 gic=1 timer=5 mpidr=0x80000000 checkpoint=PASS\n").encode(),
            expected_run_token=token,
            expected_watchdog_timeout=31,
        )
        self.assertTrue(result["xnu"]["pass"])
        self.assertEqual(result["xnu"]["darwin_version"], "23.6.0")
        self.assertTrue(result["xnu"]["single_boot_cpu"])
        self.assertTrue(result["new_boot_marker"])
        self.assertTrue(result["recovery"]["timeout_matches_expected"])
        self.assertTrue(result["recovery"]["run_token_matches_expected"])

    def test_old_or_unmarked_xnu_evidence_cannot_satisfy_a_cycle(self):
        old_token = "0123456789abcdef0123456789abcdef"
        expected = "fedcba9876543210fedcba9876543210"
        text = ("DALI-XNU: darwin=23.6.0 build=10063.141.1 run=" + old_token +
                " bootarg=1 registered=1 platform=1 gic=1 timer=5 mpidr=0x80000000 checkpoint=PASS\n")
        result = parse_evidence(text.encode(), expected_run_token=expected,
                                expected_watchdog_timeout=31)
        self.assertFalse(result["new_boot_marker"])
        self.assertFalse(result["xnu"]["run_token_matches_expected"])
        self.assertFalse(result["xnu"]["pass"])

    def test_recovery_timeout_must_match_the_built_candidate(self):
        result = parse_evidence(
            b"DALI: SEC diagnostics v1\n"
            b"DALI: auto-fastboot armed run=0123456789abcdef0123456789abcdef marker=13 timeout=30 seconds; waiting for XNU to stop\n",
            expected_watchdog_timeout=31,
        )
        self.assertFalse(result["recovery"]["timeout_matches_expected"])

    def test_stale_fastboot_marker_cannot_satisfy_another_cycle(self):
        expected = "fedcba9876543210fedcba9876543210"
        result = parse_evidence(
            b"DALI: SEC diagnostics v1\n"
            b"DALI: auto-fastboot armed run=0123456789abcdef0123456789abcdef marker=13 timeout=31 seconds; waiting for XNU to stop\n",
            expected_run_token=expected,
            expected_watchdog_timeout=31,
        )
        self.assertFalse(result["recovery"]["run_token_matches_expected"])

    def test_xnu_failure_and_recovery_marker_are_distinct(self):
        result = parse_evidence(
            b"DALI: SEC diagnostics v1\n"
            b"DALI: auto-fastboot armed run=0123456789abcdef0123456789abcdef marker=13 timeout=31 seconds; waiting for XNU to stop\n"
            b"DALI-XNU: darwin=23.6.0 build=10063.141.1 run=0123456789abcdef0123456789abcdef bootarg=0 registered=1 "
            b"platform=1 gic=1 timer=0 mpidr=0x80000000 checkpoint=FAIL\n")
        self.assertTrue(result["recovery"]["auto_fastboot_armed"])
        self.assertEqual(result["recovery"]["bootloader_marker"] & 0xF, 3)
        self.assertFalse(result["xnu"]["pass"])
        self.assertTrue(result["failures"])


if __name__ == "__main__":
    unittest.main()

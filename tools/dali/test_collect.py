import unittest

from collect import parse_evidence


class EvidenceTests(unittest.TestCase):
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


if __name__ == "__main__":
    unittest.main()

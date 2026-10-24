"""Offline regression for the saved-record startup acceptance policy."""
import copy
import json
import unittest
from usb2_characterize import assess, FAULTS

class AssessmentTests(unittest.TestCase):
    def evidence(self, times=(0.02, 0.03, 0.04)):
        stats = {key:0 for key in FAULTS}
        stats.update(usb_MBps=42, channels=1, elapsed_seconds=300,
                     device_overflows=1, resyncs=1)
        payload = {'config':{'sample_rate':14000000}, 'rx':stats, 'interrupted':False}
        messages = [
            {'type':'event', 'event_type':1, 'message':'EVT type=rx_overflow action=waiting_rx_resync status=0'},
            {'type':'event', 'event_type':2, 'epoch':2, 'message':'RX preamble complete'},
            {'type':'event', 'event_type':1, 'message':'EVT type=rx_overflow action=rx_restarted status=0'}]
        for event, seconds in zip(messages, times):
            if seconds is not None: event['observed_seconds'] = seconds
        return payload, {'exit_code':0, 'stderr':'\n'.join(map(json.dumps, messages))}

    def check_run(self, payload, run):
        return assess(payload, run, 'rx1', 14000000, 300)

    def test_startup_is_allowed_without_changing_raw_counters(self):
        payload, run = self.evidence()
        original = copy.deepcopy(payload)
        reasons, _, startup = self.check_run(payload, run)
        self.assertFalse(reasons)
        self.assertEqual(startup['accepted'], 1)
        self.assertEqual(payload, original)

    def test_late_or_untimed_or_incomplete_recovery_fails(self):
        for times in [(2, 2.01, 2.02), (None, None, None), (0.02, 0.03, 1.01)]:
            with self.subTest(times=times):
                payload, run = self.evidence(times)
                reasons, _, startup = self.check_run(payload, run)
                self.assertTrue(reasons)
                self.assertEqual(startup['accepted'], 0)

    def test_additional_late_faults_are_not_hidden(self):
        payload, run = self.evidence()
        payload['rx']['device_overflows'] = payload['rx']['resyncs'] = 2
        self.assertTrue(self.check_run(payload, run)[0])
        payload, run = self.evidence()
        payload['rx']['device_underflows'] = 1
        self.assertTrue(self.check_run(payload, run)[0])

    def test_all_completed_startup_recoveries_can_pass(self):
        payload, run = self.evidence()
        payload['rx']['device_overflows'] = payload['rx']['resyncs'] = 2
        run['stderr'] += '\n' + run['stderr']
        reasons, _, startup = self.check_run(payload, run)
        self.assertFalse(reasons)
        self.assertEqual(startup['accepted'], 2)

    def test_early_stop_cannot_pass(self):
        payload, run = self.evidence()
        payload['stopped_early'] = True
        payload['rx']['elapsed_seconds'] = 2
        run['exit_code'] = 3
        reasons, _, _ = self.check_run(payload, run)
        self.assertIn('early stop: repeated RX overflows after startup', reasons)
        self.assertIn('rx channels / duration mismatch', reasons)

if __name__ == '__main__':
    unittest.main()

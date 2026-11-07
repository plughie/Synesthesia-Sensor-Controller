import unittest

from protocol import ControllerConfig, hue_from_request, hue_message, osc_float_message


class ProtocolTests(unittest.TestCase):
    def test_raw_range(self):
        self.assertEqual(hue_from_request(1.0, "raw"), (1.0, 0.5))
        self.assertEqual(hue_from_request(3.0, "raw"), (2.0, 1.0))

    def test_normalized_range(self):
        self.assertEqual(hue_from_request(0.25, "normalized"), (0.5, 0.25))

    def test_serial_message(self):
        self.assertEqual(hue_message(1.0, 0.5), b'{"type":"hue","raw":1.0,"normalized":0.5}\n')

    def test_osc_message(self):
        packet = osc_float_message("/controls/meta/hue", 0.5)
        self.assertEqual(packet[:20], b"/controls/meta/hue\0\0")
        self.assertEqual(packet[20:24], b",f\0\0")
        self.assertAlmostEqual(__import__("struct").unpack(">f", packet[24:28])[0], 0.5)

    def test_config_validation(self):
        self.assertEqual(ControllerConfig().update({"hold_timeout_ms": 250}).hold_timeout_ms, 250)
        with self.assertRaises(ValueError):
            ControllerConfig().update({"deadband": 2})


if __name__ == "__main__":
    unittest.main()

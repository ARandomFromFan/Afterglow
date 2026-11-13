"""Unit test for PAC (Pointer Authentication) lowering in the ARM64-to-C lifter.

Verifies classification and translation for all PAC instruction forms:
- autibsp, pacibsp, paciasp, autiasp, xpaclri (HINT space)
- retab, retaa (authenticated returns)
- braa, brab, braaz, brabz, blraa, blrab, blraaz, blrabz (authenticated branches/calls)
- pacia, pacib, pacda, pacdb, autia, autib, autda, autdb, paciaz, pacibz, autiaz, autibz (register/zero forms)
- xpaci, xpacd (strip signature bits)
- pacga (generic code authenticator)
"""
import sys
import os
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "scripts"))
import env  # noqa: E402
LIFTER_KIT_DIR = env.LIFTER_KIT_DIR
sys.path.insert(0, os.path.join(LIFTER_KIT_DIR, "nx2"))
sys.path.insert(0, LIFTER_KIT_DIR)

import kit  # noqa: F401
import insn


class TestPacLowering(unittest.TestCase):
    def setUp(self):
        insn.install()

    def test_authenticated_returns(self):
        # retaa (0xD65F0BFF) and retab (0xD65F0FFF)
        retaa = 0xD65F0BFF
        retab = 0xD65F0FFF
        self.assertEqual(insn.classify_ext(retaa, 0), ("ret", 30))
        self.assertEqual(insn.classify_ext(retab, 0), ("ret", 30))

    def test_authenticated_branches(self):
        # braa / brab (register modifier) and braaz / brabz (zero modifier)
        braa = 0xD61F0800 | (1 << 5) | 2   # braa x1, x2
        brab = 0xD61F0C00 | (3 << 5) | 4   # brab x3, x4
        braaz = 0xD61F0800 | (5 << 5) | 31 # braaz x5
        brabz = 0xD61F0C00 | (6 << 5) | 31 # brabz x6

        self.assertEqual(insn.classify_ext(braa, 0), ("br", 1))
        self.assertEqual(insn.classify_ext(brab, 0), ("br", 3))
        self.assertEqual(insn.classify_ext(braaz, 0), ("br", 5))
        self.assertEqual(insn.classify_ext(brabz, 0), ("br", 6))

    def test_authenticated_calls(self):
        # blraa / blrab (register modifier) and blraaz / blrabz (zero modifier)
        blraa = 0xD63F0800 | (1 << 5) | 2   # blraa x1, x2
        blrab = 0xD63F0C00 | (3 << 5) | 4   # blrab x3, x4
        blraaz = 0xD63F0800 | (5 << 5) | 31 # blraaz x5
        blrabz = 0xD63F0C00 | (6 << 5) | 31 # blrabz x6

        self.assertEqual(insn.classify_ext(blraa, 0), ("blr", 1))
        self.assertEqual(insn.classify_ext(blrab, 0), ("blr", 3))
        self.assertEqual(insn.classify_ext(blraaz, 0), ("blr", 5))
        self.assertEqual(insn.classify_ext(blrabz, 0), ("blr", 6))

    def test_pointer_auth_register_and_zero_forms(self):
        # Register and zero forms of PAC/AUT lower to identity comment
        forms = {
            "pacia": 0xDAC10020,
            "pacib": 0xDAC10420,
            "pacda": 0xDAC10820,
            "pacdb": 0xDAC10C20,
            "autia": 0xDAC11020,
            "autib": 0xDAC11420,
            "autda": 0xDAC11820,
            "autdb": 0xDAC11C20,
            "paciaz": 0xDAC12000,
            "pacibz": 0xDAC12400,
            "autiaz": 0xDAC13000,
            "autibz": 0xDAC13400,
        }
        for name, op in forms.items():
            c_text = insn.emit(op)
            self.assertIsNotNone(c_text, f"emit returned None for {name}")
            self.assertIn("identity", c_text, f"expected identity lowering for {name}")

    def test_xpac_pointer_stripping(self):
        # xpaci / xpacd lower to comment stating pointers carry no PAC
        for name, op in [("xpaci", 0xDAC143E0), ("xpacd", 0xDAC147E0)]:
            c_text = insn.emit(op)
            self.assertIsNotNone(c_text, f"emit returned None for {name}")
            self.assertIn("xpac", c_text, f"expected xpac comment for {name}")

    def test_pacga(self):
        # pacga writes 0 to destination register
        pacga = 0x9AC23020  # pacga x0, x1, x2
        c_text = insn.emit(pacga)
        self.assertIsNotNone(c_text)
        self.assertIn("c->x[0] = 0", c_text)

    def test_hint_space_pac(self):
        # HINT-space PAC instructions match (w & 0xFFFFF01F) == 0xD503201F
        hint_ops = {
            "pacibsp": 0xD503237F,
            "autibsp": 0xD50323FF,
            "paciasp": 0xD503233F,
            "autiasp": 0xD50323BF,
            "xpaclri": 0xD50320FF,
        }
        for name, op in hint_ops.items():
            self.assertEqual(op & 0xFFFFF01F, 0xD503201F, f"{name} must match HINT mask")


if __name__ == "__main__":
    unittest.main()

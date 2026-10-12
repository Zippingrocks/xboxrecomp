import unittest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


class StringCompareLifterTest(unittest.TestCase):
    def test_repne_scasb_scans_until_equal(self):
        scan = Instruction(
            0, 2, "repne scasb", "al, byte ptr es:[edi]", "f2ae")

        lifted = Lifter().lift_instruction(scan)
        generated = "\n".join(lifted)

        self.assertIn("while (ecx != 0)", generated)
        self.assertIn("_flags = (LO8(eax) == MEM8(edi));", generated)
        self.assertIn("edi += (g_df ? (uint32_t)-1 : 1u); ecx--;", generated)
        self.assertIn("if (_flags) break;", generated)
        self.assertLess(
            generated.index("edi += (g_df ? (uint32_t)-1 : 1u); ecx--;"),
            generated.index("if (_flags) break;"),
        )

    def test_repe_cmpsb_compares_bytes_and_feeds_equal_jump(self):
        compare = Instruction(
            0, 2, "repe cmpsb", "byte ptr [esi], byte ptr es:[edi]",
            "f3a6")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=1),
            Operand(type="mem", mem_base="edi", mem_size=1),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = (MEM8(esi) == MEM8(edi));", generated)
        self.assertIn(
            "esi += (g_df ? (uint32_t)-1 : 1u); edi += (g_df ? (uint32_t)-1 : 1u); ecx--;",
            generated)
        self.assertIn("if (!_flags) break;", generated)
        self.assertIn("if ((_flags != 0)) goto loc_00000010;", generated)

    def test_repe_cmpsd_compares_dwords_and_feeds_equal_jump(self):
        # cmpsd used to be left as a comment and not counted as a flag
        # setter, so the je after it tested whatever set flags before it.
        compare = Instruction(
            0, 2, "repe cmpsd", "dword ptr [esi], dword ptr es:[edi]",
            "f3a7")
        compare.operands = [
            Operand(type="mem", mem_base="esi", mem_size=4),
            Operand(type="mem", mem_base="edi", mem_size=4),
        ]
        jump = Instruction(2, 2, "je", "0x10", "740c")
        jump.jump_target = 0x10
        lifter = Lifter()
        lifter.func_start = 0
        lifter.func_end = 0x20

        lifted, _ = lift_basic_block(
            lifter, BasicBlock(start=0, instructions=[compare, jump]))
        generated = "\n".join(lifted)

        self.assertIn("_flags = (MEM32(esi) == MEM32(edi));", generated)
        self.assertIn("esi += (g_df ? (uint32_t)-4 : 4u);", generated)
        self.assertIn("if ((_flags != 0)) goto loc_00000010;", generated)

    def test_std_scasb_steps_backwards(self):
        lifter = Lifter()
        std = lifter.lift_instruction(Instruction(0, 1, "std", "", "fd"))
        cld = lifter.lift_instruction(Instruction(0, 1, "cld", "", "fc"))
        self.assertEqual(std, ["g_df = 1; /* std */"])
        self.assertEqual(cld, ["g_df = 0; /* cld */"])

if __name__ == "__main__":
    unittest.main()

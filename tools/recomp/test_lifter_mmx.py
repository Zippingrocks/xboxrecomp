import unittest

from .disasm import Instruction, Operand
from .lifter import Lifter


def _reg(name):
    return Operand(type="reg", reg=name)


class MmxLifterTest(unittest.TestCase):
    def lift(self, mnemonic, op_str, operands):
        insn = Instruction(0, 3, mnemonic, op_str, "0f")
        insn.operands = operands
        return "\n".join(Lifter().lift_instruction(insn))

    def test_binary_op_uses_lane_helper(self):
        out = self.lift("paddw", "mm0, mm1", [_reg("mm0"), _reg("mm1")])
        self.assertEqual(out, "mm0 = mmx_paddw(mm0, mm1); /* paddw */")

    def test_shift_by_immediate(self):
        out = self.lift("psrlq", "mm2, 0x38",
                        [_reg("mm2"), Operand(type="imm", imm=0x38)])
        self.assertIn("mm2 = mmx_psrlq(mm2, (uint64_t)", out)

    def test_movq_load_and_store(self):
        load = self.lift("movq", "mm0, qword ptr [esi]",
                         [_reg("mm0"), Operand(type="mem", mem_base="esi", mem_size=8)])
        store = self.lift("movq", "qword ptr [edi], mm1",
                          [Operand(type="mem", mem_base="edi", mem_size=8), _reg("mm1")])
        self.assertIn("mm0 = MEM64(", load)
        self.assertIn("MEM64(", store)
        self.assertIn("= mm1;", store)

    def test_movd_zero_extends_into_mmx(self):
        out = self.lift("movd", "mm7, ebx", [_reg("mm7"), _reg("ebx")])
        self.assertIn("mm7 = (uint64_t)(uint32_t)(", out)

    def test_pshufw_immediate(self):
        out = self.lift("pshufw", "mm2, mm2, 0",
                        [_reg("mm2"), _reg("mm2"), Operand(type="imm", imm=0)])
        self.assertIn("mm2 = mmx_pshufw(mm2,", out)


if __name__ == "__main__":
    unittest.main()

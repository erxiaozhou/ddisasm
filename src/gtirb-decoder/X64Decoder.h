//===- X64Decoder.h ---------------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2020 GrammaTech, Inc.
//
//  This code is licensed under the GNU Affero General Public License
//  as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. See the
//  LICENSE.txt file in the project root for license terms or visit
//  https://www.gnu.org/licenses/agpl.txt.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//  GNU Affero General Public License for more details.
//
//  This project is sponsored by the Office of Naval Research, One Liberty
//  Center, 875 N. Randolph Street, Arlington, VA 22203 under contract #
//  N68335-17-C-0700.  The content of the information does not necessarily
//  reflect the position or policy of the Government and no official
//  endorsement should be inferred.
//
//===----------------------------------------------------------------------===//
#ifndef SRC_X64_DECODER_H_
#define SRC_X64_DECODER_H_

#include "DatalogLoader.h"

#include <capstone/capstone.h>

class X64Decoder : public InstructionDecoder
{
public:
    X64Decoder()
    {
        [[maybe_unused]] cs_err Err = cs_open(CS_ARCH_X86, CS_MODE_64, &CsHandle);
        assert(Err == CS_ERR_OK && "Failed to initialize X64 disassembler.");
        cs_option(CsHandle, CS_OPT_DETAIL, CS_OPT_ON);
    }
    ~X64Decoder()
    {
        cs_close(&CsHandle);
    }

    using Instruction = InstructionDecoder::Instruction;
    using Operand = std::variant<InstructionDecoder::ImmOp, InstructionDecoder::RegOp,
                                 InstructionDecoder::IndirectOp>;

    std::optional<Instruction> disasm(const uint8_t* Bytes, uint64_t Size, uint64_t Addr) override;

private:
    std::optional<Operand> build(const cs_x86_op& CsOp);
    std::optional<Instruction> build(const cs_insn& CsInstruction);
    std::tuple<std::string, std::string> splitMnemonic(const cs_insn& CsInstruction);

    csh CsHandle = CS_ERR_ARCH;
};

#endif /* SRC_X64_DECODER_H_ */
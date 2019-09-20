//===- GtirbToDatalog.cpp ---------------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2019 GrammaTech, Inc.
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

#include "DatalogUtils.h"

void writeFacts(souffle::SouffleProgram* prog, const std::string& directory)
{
    std::ios_base::openmode filemask = std::ios::out;
    for(souffle::Relation* relation : prog->getInputRelations())
    {
        std::ofstream file(directory + relation->getName() + ".facts", filemask);
        souffle::SymbolTable symbolTable = relation->getSymbolTable();
        for(souffle::tuple tuple : *relation)
        {
            for(size_t i = 0; i < tuple.size(); i++)
            {
                if(i > 0)
                    file << "\t";
                if(relation->getAttrType(i)[0] == 's')
                    file << symbolTable.resolve(tuple[i]);
                else
                    file << tuple[i];
            }
            file << std::endl;
        }
        file.close();
    }
}

void GtirbToDatalog::populateEdgeProperties(souffle::tuple& T, const gtirb::EdgeLabel& Label)
{
    assert(Label.has_value() && "Found edge without a label");
    if(std::get<gtirb::ConditionalEdge>(*Label) == gtirb::ConditionalEdge::OnTrue)
        T << "true";
    else
        T << "false";
    if(std::get<gtirb::DirectEdge>(*Label) == gtirb::DirectEdge::IsIndirect)
        T << "true";
    else
        T << "false";
    switch(std::get<gtirb::EdgeType>(*Label))
    {
        case gtirb::EdgeType::Branch:
            T << "branch";
            break;
        case gtirb::EdgeType::Call:
            T << "call";
            break;
        case gtirb::EdgeType::Fallthrough:
            T << "fallthrough";
            break;
        case gtirb::EdgeType::Return:
            T << "return";
            break;
        case gtirb::EdgeType::Syscall:
            T << "syscall";
            break;
        case gtirb::EdgeType::Sysret:
            T << "sysret";
            break;
    }
}

std::string str_toupper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string GtirbToDatalog::getRegisterName(csh& CsHandle, unsigned int reg)
{
    if(reg == X86_REG_INVALID)
        return "NONE";
    std::string name = str_toupper(cs_reg_name(CsHandle, reg));
    return name;
}

DlInstruction GtirbToDatalog::transformInstruction(csh& CsHandle, DlOperandTable& OpDict,
                                                   cs_insn& insn)
{
    std::vector<uint64_t> op_codes;
    std::string prefix_name = str_toupper(insn.mnemonic);
    std::string prefix, name;
    size_t pos = prefix_name.find(' ');
    if(pos != std::string::npos)
    {
        prefix = prefix_name.substr(0, pos);
        name = prefix_name.substr(pos + 1);
    }
    else
    {
        prefix = "";
        name = prefix_name;
    }

    auto &detail = insn.detail->x86;
    if(name != "NOP")
    {
        auto opCount = detail.op_count;
        // skip the destination operand
        for(int i = 0; i < opCount; i++)
        {
            const auto &op = detail.operands[i];
            uint64_t index = OpDict.add(buildOperand(CsHandle, op));
            op_codes.push_back(index);
        }
        // we put the destination operand at the end
        if(opCount > 0)
            std::rotate(op_codes.begin(), op_codes.begin() + 1, op_codes.end());
    }
    return {insn.address,
            insn.size,
            prefix,
            name,
            op_codes,
            detail.encoding.imm_offset,
            detail.encoding.disp_offset};
}

std::variant<ImmOp, RegOp, IndirectOp> GtirbToDatalog::buildOperand(csh& CsHandle,
                                                                    const cs_x86_op& op)
{
    switch(op.type)
    {
        case X86_OP_REG:
            return getRegisterName(CsHandle, op.reg);
        case X86_OP_IMM:
            return op.imm;
        case X86_OP_MEM:
        {
            IndirectOp I = {getRegisterName(CsHandle, op.mem.segment),
                            getRegisterName(CsHandle, op.mem.base),
                            getRegisterName(CsHandle, op.mem.index),
                            op.mem.scale,
                            op.mem.disp,
                            op.size * 8};
            return I;
        }
        case X86_OP_INVALID:
        default:
            std::cerr << "invalid operand\n";
            exit(1);
    }
}

souffle::tuple& operator<<(souffle::tuple& t, const DlInstruction& inst)
{
    t << inst.address << inst.size << inst.prefix << inst.name;
    for(size_t i = 0; i < 4; ++i)
    {
        if(i < inst.op_codes.size())
            t << inst.op_codes[i];
        else
            t << 0;
    }
    t << inst.immediateOffset << inst.displacementOffset;
    return t;
}

void GtirbToDatalog::populateBlocks(gtirb::Module& M)
{
    if(M.blocks().empty())
        return;
    auto* BlocksRel = Prog->getRelation("block");
    auto* NextBlockRel = Prog->getRelation("next_block");
    gtirb::Addr PrevBlockAddr = M.blocks().begin()->getAddress();
    for(auto& Block : M.blocks())
    {
        souffle::tuple T(BlocksRel);
        T << static_cast<uint64_t>(Block.getAddress()) << Block.getSize();
        BlocksRel->insert(T);

        if(PrevBlockAddr < Block.getAddress())
        {
            souffle::tuple TupleNext(NextBlockRel);
            TupleNext << static_cast<uint64_t>(PrevBlockAddr)
                      << static_cast<uint64_t>(Block.getAddress());
            NextBlockRel->insert(TupleNext);
        }
        PrevBlockAddr = Block.getAddress();
    }
}

void GtirbToDatalog::populateInstructions(gtirb::Module& M)
{
    csh csHandle;
    cs_open(CS_ARCH_X86, CS_MODE_64, &csHandle); // == CS_ERR_OK
    cs_option(csHandle, CS_OPT_DETAIL, CS_OPT_ON);
    std::vector<DlInstruction> Insns;
    DlOperandTable OpDict;
    for(auto& Block : M.blocks())
    {
        cs_insn* Insn;
        gtirb::ImageByteMap::const_range Bytes = getBytes(M.getImageByteMap(), Block);
        size_t Count = cs_disasm(csHandle, reinterpret_cast<const uint8_t*>(&Bytes[0]),
                                 Bytes.size(), static_cast<uint64_t>(Block.getAddress()), 0, &Insn);

        // Exception-safe cleanup of instructions
        std::unique_ptr<cs_insn, std::function<void(cs_insn*)>> freeInsn(
            Insn, [Count](cs_insn* i) { cs_free(i, Count); });
        for(size_t i = 0; i < Count; ++i)
        {
            Insns.push_back(GtirbToDatalog::transformInstruction(csHandle, OpDict, Insn[i]));
        }
    }
    GtirbToDatalog::addToRelation(&*Prog, "instruction", Insns);
    GtirbToDatalog::addToRelation(&*Prog, "op_regdirect", OpDict.regTable);
    GtirbToDatalog::addToRelation(&*Prog, "op_immediate", OpDict.immTable);
    GtirbToDatalog::addToRelation(&*Prog, "op_indirect", OpDict.indirectTable);
}

void GtirbToDatalog::populateCfgEdges(gtirb::Module& M)
{
    std::map<gtirb::ProxyBlock*, std::string> InvSymbolMap;
    for(auto& Symbol : M.symbols())
    {
        if(gtirb::ProxyBlock* Proxy = Symbol.getReferent<gtirb::ProxyBlock>())
            InvSymbolMap[Proxy] = Symbol.getName();
    }
    gtirb::CFG& Cfg = M.getCFG();
    auto* EdgeRel = Prog->getRelation("cfg_edge");
    auto* TopEdgeRel = Prog->getRelation("cfg_edge_to_top");
    auto* SymbolEdgeRel = Prog->getRelation("cfg_edge_to_symbol");
    for(auto& Edge : Cfg.m_edges)
    {
        if(gtirb::Block* Src = dyn_cast<gtirb::Block>(Cfg[Edge.m_source]))
        {
            if(gtirb::Block* Dest = dyn_cast<gtirb::Block>(Cfg[Edge.m_target]))
            {
                souffle::tuple T(EdgeRel);
                T << static_cast<uint64_t>(Src->getAddress())
                  << static_cast<uint64_t>(Dest->getAddress());
                populateEdgeProperties(T, Edge.get_property());
                EdgeRel->insert(T);
            }

            if(gtirb::ProxyBlock* Dest = dyn_cast<gtirb::ProxyBlock>(Cfg[Edge.m_target]))
            {
                auto foundSymbol = InvSymbolMap.find(Dest);
                if(foundSymbol != InvSymbolMap.end())
                {
                    souffle::tuple T(SymbolEdgeRel);
                    T << static_cast<uint64_t>(Src->getAddress()) << foundSymbol->second;
                    populateEdgeProperties(T, Edge.get_property());
                    SymbolEdgeRel->insert(T);
                }
                else
                {
                    souffle::tuple T(TopEdgeRel);
                    T << static_cast<uint64_t>(Src->getAddress());
                    populateEdgeProperties(T, Edge.get_property());
                    TopEdgeRel->insert(T);
                }
            }
        }
    }
}

void GtirbToDatalog::populateSccs(gtirb::Module& M)
{
    auto* InSccRel = Prog->getRelation("in_scc");
    auto* SccTable = M.getAuxData<std::map<gtirb::UUID, int>>("SCCs");
    assert(SccTable && "SCCs AuxData table missing from GTIRB module");
    std::vector<int> SccBlockIndex;
    for(auto& Block : M.blocks())
    {
        auto Found = SccTable->find(Block.getUUID());
        assert(Found != SccTable->end() && "Block missing from SCCs table");
        if(SccBlockIndex.size() <= Found->second)
            SccBlockIndex.resize(Found->second + 1);
        souffle::tuple T(InSccRel);
        T << Found->second << SccBlockIndex[Found->second]++
          << static_cast<uint64_t>(Block.getAddress());
        InSccRel->insert(T);
    }
}

void GtirbToDatalog::populateSymbolicExpressions(gtirb::Module& M)
{
    auto* SymExprRel = Prog->getRelation("symbolic_expression");
    auto* SymMinusSymRel = Prog->getRelation("symbol_minus_symbol");
    for(auto& SymExpr : M.symbolic_exprs())
    {
        auto Addrs = M.getAddrsForSymbolicExpression(SymExpr);
        assert(!Addrs.empty() && "Symbolic expression without address");
        gtirb::Addr Addr(*Addrs.begin());
        switch(SymExpr.index())
        {
            case 0:
                break;
            case 1:
            {
                auto AddrConst = std::get<gtirb::SymAddrConst>(SymExpr);
                if(AddrConst.Sym->getAddress())
                {
                    souffle::tuple T(SymExprRel);
                    T << static_cast<uint64_t>(Addr)
                      << static_cast<uint64_t>(*AddrConst.Sym->getAddress()) << AddrConst.Offset;
                    SymExprRel->insert(T);
                }
                break;
            }
            case 2:
            {
                auto AddrAddr = std::get<gtirb::SymAddrAddr>(SymExpr);
                if(AddrAddr.Sym1->getAddress() && AddrAddr.Sym2->getAddress())
                {
                    souffle::tuple T(SymMinusSymRel);
                    T << static_cast<uint64_t>(Addr)
                      << static_cast<uint64_t>(*AddrAddr.Sym1->getAddress())
                      << static_cast<uint64_t>(*AddrAddr.Sym2->getAddress()) << AddrAddr.Offset;
                    SymMinusSymRel->insert(T);
                }
                break;
            }
        }
    }
}

void GtirbToDatalog::populateFdeEntries(gtirb::Context& Ctx, gtirb::Module& M)
{
    std::set<gtirb::Addr> FdeStart;
    std::set<gtirb::Addr> FdeEnd;
    auto* CfiDirectives = M.getAuxData<std::map<
        gtirb::Offset, std::vector<std::tuple<std::string, std::vector<int64_t>, gtirb::UUID>>>>(
        "cfiDirectives");
    if(!CfiDirectives)
        return;
    for(auto& Pair : *CfiDirectives)
    {
        gtirb::Block* Block =
            dyn_cast<gtirb::Block>(gtirb::Node::getByUUID(Ctx, Pair.first.ElementId));
        assert(Block && "Found CFI directive that does no belong to a block");
        for(auto& Directive : Pair.second)
        {
            if(std::get<0>(Directive) == ".cfi_startproc")
                FdeStart.insert(Block->getAddress() + Pair.first.Displacement);
            if(std::get<0>(Directive) == ".cfi_endproc")
                FdeEnd.insert(Block->getAddress() + Pair.first.Displacement);
        }
    }
    assert(FdeStart.size() == FdeEnd.size() && "Malformed CFI directives");
    auto StartIt = FdeStart.begin();
    auto EndIt = FdeEnd.begin();
    auto* FdeAddresses = Prog->getRelation("fde_addresses");
    for(; StartIt != FdeStart.end(); ++StartIt, ++EndIt)
    {
        souffle::tuple T(FdeAddresses);
        T << static_cast<uint64_t>(*StartIt) << static_cast<uint64_t>(*EndIt);
        FdeAddresses->insert(T);
    }
}

void GtirbToDatalog::populateFunctionEntries(gtirb::Context& Ctx, gtirb::Module& M)
{
    auto* FunctionEntries =
        M.getAuxData<std::map<gtirb::UUID, std::set<gtirb::UUID>>>("functionEntries");
    if(!FunctionEntries)
        return;
    auto* FunctionEntryRel = Prog->getRelation("function_entry");
    for(auto& Pair : *FunctionEntries)
    {
        for(auto& UUID : Pair.second)
        {
            if(auto* Block = dyn_cast<gtirb::Block>(gtirb::Node::getByUUID(Ctx, UUID)))
            {
                souffle::tuple T(FunctionEntryRel);
                T << static_cast<uint64_t>(Block->getAddress());
                FunctionEntryRel->insert(T);
            }
        }
    }
}
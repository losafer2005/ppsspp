// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/logging.h"
#include "profiler/profiler.h"
#include "Common/ChunkFile.h"
#include "Common/CPUDetect.h"
#include "Common/StringUtils.h"

#include "Core/Reporting.h"
#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/Debugger/SymbolMap.h"
#include "Core/MemMap.h"

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSCodeUtils.h"
#include "Core/MIPS/MIPSInt.h"
#include "Core/MIPS/MIPSTables.h"
#include "Core/HLE/ReplaceTables.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/MIPS/IR/IRRegCache.h"
#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/JitCommon/JitCommon.h"

void DisassembleArm64Print(const u8 *data, int size) {
	std::vector<std::string> lines = DisassembleArm64(data, size);
	for (auto s : lines) {
		ILOG("%s", s.c_str());
	}
	/*
	ILOG("+++");
	// A format friendly to Online Disassembler which gets endianness wrong
	for (size_t i = 0; i < lines.size(); i++) {
		uint32_t opcode = ((const uint32_t *)data)[i];
		ILOG("%d/%d: %08x", (int)(i+1), (int)lines.size(), swap32(opcode));
	}
	ILOG("===");
	ILOG("===");*/
}

namespace MIPSComp
{

IRJit::IRJit(MIPSState *mips) : gpr(), mips_(mips) { 
	logBlocks = 0;
	dontLogBlocks = 0;
	js.startDefaultPrefix = mips_->HasDefaultPrefix();
	js.currentRoundingFunc = convertS0ToSCRATCH1[0];
	u32 size = 128 * 1024;
	blTrampolines_ = kernelMemory.Alloc(size, true, "trampoline");
}

IRJit::~IRJit() {
}

void IRJit::DoState(PointerWrap &p) {
	auto s = p.Section("Jit", 1, 2);
	if (!s)
		return;

	p.Do(js.startDefaultPrefix);
	if (s >= 2) {
		p.Do(js.hasSetRounding);
		js.lastSetRounding = 0;
	} else {
		js.hasSetRounding = 1;
	}

	if (p.GetMode() == PointerWrap::MODE_READ) {
		js.currentRoundingFunc = convertS0ToSCRATCH1[(mips_->fcr31) & 3];
	}
}

// This is here so the savestate matches between jit and non-jit.
void IRJit::DoDummyState(PointerWrap &p) {
	auto s = p.Section("Jit", 1, 2);
	if (!s)
		return;

	bool dummy = false;
	p.Do(dummy);
	if (s >= 2) {
		dummy = true;
		p.Do(dummy);
	}
}

void IRJit::FlushAll() {
	FlushPrefixV();
}

void IRJit::FlushPrefixV() {
	if ((js.prefixSFlag & JitState::PREFIX_DIRTY) != 0) {
		ir.Write(IROp::SetCtrlVFPU, VFPU_CTRL_SPREFIX, ir.AddConstant(js.prefixS));
		js.prefixSFlag = (JitState::PrefixState) (js.prefixSFlag & ~JitState::PREFIX_DIRTY);
	}

	if ((js.prefixTFlag & JitState::PREFIX_DIRTY) != 0) {
		ir.Write(IROp::SetCtrlVFPU, VFPU_CTRL_TPREFIX, ir.AddConstant(js.prefixT));
		js.prefixTFlag = (JitState::PrefixState) (js.prefixTFlag & ~JitState::PREFIX_DIRTY);
	}

	if ((js.prefixDFlag & JitState::PREFIX_DIRTY) != 0) {
		ir.Write(IROp::SetCtrlVFPU, VFPU_CTRL_DPREFIX, ir.AddConstant(js.prefixD));
		js.prefixDFlag = (JitState::PrefixState) (js.prefixDFlag & ~JitState::PREFIX_DIRTY);
	}
}

void IRJit::ClearCache() {
	ILOG("ARM64Jit: Clearing the cache!");
	blocks_.Clear();
}

void IRJit::InvalidateCache() {
	blocks_.Clear();
}

void IRJit::InvalidateCacheAt(u32 em_address, int length) {
	blocks_.InvalidateICache(em_address, length);
}

void IRJit::EatInstruction(MIPSOpcode op) {
	MIPSInfo info = MIPSGetInfo(op);
	if (info & DELAYSLOT) {
		ERROR_LOG_REPORT_ONCE(ateDelaySlot, JIT, "Ate a branch op.");
	}
	if (js.inDelaySlot) {
		ERROR_LOG_REPORT_ONCE(ateInDelaySlot, JIT, "Ate an instruction inside a delay slot.");
	}

	js.numInstructions++;
	js.compilerPC += 4;
	js.downcountAmount += MIPSGetInstructionCycleEstimate(op);
}

void IRJit::CompileDelaySlot() {
	js.inDelaySlot = true;
	MIPSOpcode op = GetOffsetInstruction(1);
	MIPSCompileOp(op, this);
	js.inDelaySlot = false;
}

void IRJit::Compile(u32 em_address) {
	PROFILE_THIS_SCOPE("jitc");

	int block_num = blocks_.AllocateBlock(em_address);
	IRBlock *b = blocks_.GetBlock(block_num);
	DoJit(em_address, b);

	bool cleanSlate = false;

	if (js.hasSetRounding && !js.lastSetRounding) {
		WARN_LOG(JIT, "Detected rounding mode usage, rebuilding jit with checks");
		// Won't loop, since hasSetRounding is only ever set to 1.
		js.lastSetRounding = js.hasSetRounding;
		cleanSlate = true;
	}

	// Drat.  The VFPU hit an uneaten prefix at the end of a block.
	if (js.startDefaultPrefix && js.MayHavePrefix()) {
		WARN_LOG(JIT, "An uneaten prefix at end of block: %08x", GetCompilerPC() - 4);
		js.LogPrefix();

		// Let's try that one more time.  We won't get back here because we toggled the value.
		js.startDefaultPrefix = false;
		// TODO ARM64: This crashes.
		//cleanSlate = true;
	}

	if (cleanSlate) {
		// Our assumptions are all wrong so it's clean-slate time.
		ClearCache();
		Compile(em_address);
	}
}

void IRJit::RunLoopUntil(u64 globalticks) {
	PROFILE_THIS_SCOPE("jit");
	((void (*)())enterDispatcher)();
}

u32 IRJit::GetCompilerPC() {
	return js.compilerPC;
}

MIPSOpcode IRJit::GetOffsetInstruction(int offset) {
	return Memory::Read_Instruction(GetCompilerPC() + 4 * offset);
}

void IRJit::DoJit(u32 em_address, IRBlock *b) {
	js.cancel = false;
	js.blockStart = mips_->pc;
	js.compilerPC = mips_->pc;
	js.lastContinuedPC = 0;
	js.initialBlockSize = 0;
	js.nextExit = 0;
	js.downcountAmount = 0;
	js.curBlock = nullptr;
	js.compiling = true;
	js.inDelaySlot = false;
	js.PrefixStart();
	ir.Clear();

	gpr.Start(&ir);

	int partialFlushOffset = 0;

	js.numInstructions = 0;
	while (js.compiling) {
		MIPSOpcode inst = Memory::Read_Opcode_JIT(GetCompilerPC());
		js.downcountAmount += MIPSGetInstructionCycleEstimate(inst);
		MIPSCompileOp(inst, this);
		js.compilerPC += 4;
		js.numInstructions++;
	}

	b->SetInstructions(ir.GetInstructions(), ir.GetConstants());

	char temp[256];
	if (logBlocks > 0 && dontLogBlocks == 0) {
		ILOG("=============== mips %d ===============", blocks_.GetNumBlocks());
		for (u32 cpc = em_address; cpc != GetCompilerPC() + 4; cpc += 4) {
			MIPSDisAsm(Memory::Read_Opcode_JIT(cpc), cpc, temp, true);
			ILOG("M: %08x   %s", cpc, temp);
		}
	}

	if (logBlocks > 0 && dontLogBlocks == 0) {
		ILOG("=============== IR (%d instructions) ===============", js.numInstructions);
		for (int i = 0; i < js.numInstructions; i++) {
			char buf[256];
			DisassembleIR(buf, sizeof(buf), ir.GetInstructions()[i], ir.GetConstants().data());
			ILOG("%s", buf);
		}
	}

	if (logBlocks > 0)
		logBlocks--;
	if (dontLogBlocks > 0)
		dontLogBlocks--;
}

bool IRJit::DescribeCodePtr(const u8 *ptr, std::string &name) {
	// Used in disassembly viewer.
	return false;
}

void IRJit::Comp_RunBlock(MIPSOpcode op) {
	// This shouldn't be necessary, the dispatcher should catch us before we get here.
	ERROR_LOG(JIT, "Comp_RunBlock should never be reached!");
}

void IRJit::LinkBlock(u8 *exitPoint, const u8 *checkedEntry) {
	Crash();
}

void IRJit::UnlinkBlock(u8 *checkedEntry, u32 originalAddress) {
	Crash();
}

bool IRJit::ReplaceJalTo(u32 dest) {
	Crash();
	return false;
}

void IRJit::Comp_ReplacementFunc(MIPSOpcode op) {
	Crash();
}

void IRJit::Comp_Generic(MIPSOpcode op) {
	ir.Write(IROp::Interpret, ir.AddConstant(op.encoding));
	const MIPSInfo info = MIPSGetInfo(op);
	if ((info & IS_VFPU) != 0 && (info & VFPU_NO_PREFIX) == 0) {
		// If it does eat them, it'll happen in MIPSCompileOp().
		if ((info & OUT_EAT_PREFIX) == 0)
			js.PrefixUnknown();
	}
}

// Destroys SCRATCH2
void IRJit::RestoreRoundingMode(bool force) {
	// If the game has never set an interesting rounding mode, we can safely skip this.
	if (force || js.hasSetRounding) {
		ir.Write(IROp::RestoreRoundingMode);
	}
}

// Destroys SCRATCH1 and SCRATCH2
void IRJit::ApplyRoundingMode(bool force) {
	// If the game has never set an interesting rounding mode, we can safely skip this.
	if (force || js.hasSetRounding) {
		ir.Write(IROp::ApplyRoundingMode);
	}
}

// Destroys SCRATCH1 and SCRATCH2
void IRJit::UpdateRoundingMode() {
	ir.Write(IROp::UpdateRoundingMode);
}

void IRJit::Comp_DoNothing(MIPSOpcode op) { 
}

int IRJit::Replace_fabsf() {
	Crash();
	return 0;
}

void IRBlockCache::Clear() {
	blocks_.clear();
}

void IRBlockCache::InvalidateICache(u32 addess, u32 length) {
	// TODO
}

}  // namespace MIPSComp
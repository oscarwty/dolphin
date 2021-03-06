// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.


// AID / AUDIO_DMA controls pushing audio out to the SRC and then the speakers.
// The audio DMA pushes audio through a small FIFO 32 bytes at a time, as
// needed.

// The SRC behind the fifo eats stereo 16-bit data at a sample rate of 32khz,
// that is, 4 bytes at 32 khz, which is 32 bytes at 4 khz. We thereforce
// schedule an event that runs at 4khz, that eats audio from the fifo. Thus, we
// have homebrew audio.

// The AID interrupt is set when the fifo STARTS a transfer. It latches address
// and count into internal registers and starts copying. This means that the
// interrupt handler can simply set the registers to where the next buffer is,
// and start filling it. When the DMA is complete, it will automatically
// relatch and fire a new interrupt.

// Then there's the DSP... what likely happens is that the
// fifo-latched-interrupt handler kicks off the DSP, requesting it to fill up
// the just used buffer through the AXList (or whatever it might be called in
// Nintendo games).

#include "Common/MemoryUtil.h"

#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/AudioInterface.h"
#include "Core/HW/CPU.h"
#include "Core/HW/DSP.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/MMIO.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/PowerPC.h"

namespace DSP
{

// register offsets
enum
{
	DSP_MAIL_TO_DSP_HI      = 0x5000,
	DSP_MAIL_TO_DSP_LO      = 0x5002,
	DSP_MAIL_FROM_DSP_HI    = 0x5004,
	DSP_MAIL_FROM_DSP_LO    = 0x5006,
	DSP_CONTROL             = 0x500A,
	DSP_INTERRUPT_CONTROL   = 0x5010,
	AR_INFO                 = 0x5012,  // These names are a good guess at best
	AR_MODE                 = 0x5016,  //
	AR_REFRESH              = 0x501a,
	AR_DMA_MMADDR_H         = 0x5020,
	AR_DMA_MMADDR_L         = 0x5022,
	AR_DMA_ARADDR_H         = 0x5024,
	AR_DMA_ARADDR_L         = 0x5026,
	AR_DMA_CNT_H            = 0x5028,
	AR_DMA_CNT_L            = 0x502A,
	AUDIO_DMA_START_HI      = 0x5030,
	AUDIO_DMA_START_LO      = 0x5032,
	AUDIO_DMA_BLOCKS_LENGTH = 0x5034,  // Ever used?
	AUDIO_DMA_CONTROL_LEN   = 0x5036,
	AUDIO_DMA_BLOCKS_LEFT   = 0x503A,
};

// UARAMCount
union UARAMCount
{
	u32 Hex;
	struct
	{
		u32 count : 31;
		u32 dir   : 1; // 0: MRAM -> ARAM 1: ARAM -> MRAM
	};
};

// UDSPControl
#define DSP_CONTROL_MASK 0x0C07
union UDSPControl
{
	u16 Hex;
	struct
	{
		// DSP Control
		u16 DSPReset     : 1; // Write 1 to reset and waits for 0
		u16 DSPAssertInt : 1;
		u16 DSPHalt      : 1;
		// Interrupt for DMA to the AI/speakers
		u16 AID          : 1;
		u16 AID_mask     : 1;
		// ARAM DMA interrupt
		u16 ARAM         : 1;
		u16 ARAM_mask    : 1;
		// DSP DMA interrupt
		u16 DSP          : 1;
		u16 DSP_mask     : 1;
		// Other ???
		u16 DMAState     : 1; // DSPGetDMAStatus() uses this flag. __ARWaitForDMA() uses it too...maybe it's just general DMA flag
		u16 unk3         : 1;
		u16 DSPInit      : 1; // DSPInit() writes to this flag
		u16 pad          : 4;
	};
};

// DSPState
struct DSPState
{
	UDSPControl DSPControl;
	DSPState()
	{
		DSPControl.Hex = 0;
	}
};

// Blocks are 32 bytes.
union UAudioDMAControl
{
	u16 Hex;
	struct
	{
		u16 NumBlocks  : 15;
		u16 Enable     : 1;
	};

	UAudioDMAControl(u16 _Hex = 0) : Hex(_Hex)
	{}
};

// AudioDMA
struct AudioDMA
{
	u32 SourceAddress;
	u32 ReadAddress;
	UAudioDMAControl AudioDMAControl;
	int BlocksLeft;

	AudioDMA()
	{
		SourceAddress = 0;
		ReadAddress = 0;
		AudioDMAControl.Hex = 0;
		BlocksLeft = 0;
	}
};

// ARAM_DMA
struct ARAM_DMA
{
	u32 MMAddr;
	u32 ARAddr;
	UARAMCount Cnt;

	ARAM_DMA()
	{
		MMAddr = 0;
		ARAddr = 0;
		Cnt.Hex = 0;
	}
};

// So we may abstract gc/wii differences a little
struct ARAMInfo
{
	bool wii_mode; // wii EXRAM is managed in Memory:: so we need to skip statesaving, etc
	u32 size;
	u32 mask;
	u8* ptr; // aka audio ram, auxiliary ram, MEM2, EXRAM, etc...

	// Default to GC mode
	ARAMInfo()
	{
		wii_mode = false;
		size = ARAM_SIZE;
		mask = ARAM_MASK;
		ptr = NULL;
	}
};

// STATE_TO_SAVE
static ARAMInfo g_ARAM;
static DSPState g_dspState;
static AudioDMA g_audioDMA;
static ARAM_DMA g_arDMA;

union ARAM_Info
{
	u16 Hex;
	struct
	{
		u16 size : 6;
		u16 unk  : 1;
		u16      : 9;
	};
};
static ARAM_Info g_ARAM_Info;
// Contains bitfields for some stuff we don't care about (and nothing ever reads):
//  CAS latency/burst length/addressing mode/write mode
// We care about the LSB tho. It indicates that the ARAM controller has finished initializing
static u16 g_AR_MODE;
static u16 g_AR_REFRESH;

DSPEmulator *dsp_emulator;

static int dsp_slice = 0;
static bool dsp_is_lle = false;

//time given to lle dsp on every read of the high bits in a mailbox
static const int DSP_MAIL_SLICE=72;

void DoState(PointerWrap &p)
{
	if (!g_ARAM.wii_mode)
		p.DoArray(g_ARAM.ptr, g_ARAM.size);
	p.DoPOD(g_dspState);
	p.DoPOD(g_audioDMA);
	p.DoPOD(g_arDMA);
	p.Do(g_ARAM_Info);
	p.Do(g_AR_MODE);
	p.Do(g_AR_REFRESH);
	p.Do(dsp_slice);

	dsp_emulator->DoState(p);
}


void UpdateInterrupts();
void Do_ARAM_DMA();
void WriteARAM(u8 _iValue, u32 _iAddress);
bool Update_DSP_ReadRegister();
void Update_DSP_WriteRegister();

int et_GenerateDSPInterrupt;

void GenerateDSPInterrupt_Wrapper(u64 userdata, int cyclesLate)
{
	GenerateDSPInterrupt((DSPInterruptType)(userdata&0xFFFF), (bool)((userdata>>16) & 1));
}

DSPEmulator *GetDSPEmulator()
{
	return dsp_emulator;
}

void Init(bool hle)
{
	dsp_emulator = CreateDSPEmulator(hle);
	dsp_is_lle = dsp_emulator->IsLLE();

	if (SConfig::GetInstance().m_LocalCoreStartupParameter.bWii)
	{
		g_ARAM.wii_mode = true;
		g_ARAM.size = Memory::EXRAM_SIZE;
		g_ARAM.mask = Memory::EXRAM_MASK;
		g_ARAM.ptr = Memory::GetPointer(0x10000000);
	}
	else
	{
		// On the GC, ARAM is accessible only through this interface.
		g_ARAM.wii_mode = false;
		g_ARAM.size = ARAM_SIZE;
		g_ARAM.mask = ARAM_MASK;
		g_ARAM.ptr = (u8 *)AllocateMemoryPages(g_ARAM.size);
	}

	memset(&g_audioDMA, 0, sizeof(g_audioDMA));
	memset(&g_arDMA, 0, sizeof(g_arDMA));

	g_dspState.DSPControl.Hex = 0;
	g_dspState.DSPControl.DSPHalt = 1;

	g_ARAM_Info.Hex = 0;
	g_AR_MODE = 1; // ARAM Controller has init'd
	g_AR_REFRESH = 156; // 156MHz

	et_GenerateDSPInterrupt = CoreTiming::RegisterEvent("DSPint", GenerateDSPInterrupt_Wrapper);
}

void Shutdown()
{
	if (!g_ARAM.wii_mode)
	{
		FreeMemoryPages(g_ARAM.ptr, g_ARAM.size);
		g_ARAM.ptr = NULL;
	}

	dsp_emulator->Shutdown();
	delete dsp_emulator;
	dsp_emulator = NULL;
}

void RegisterMMIO(MMIO::Mapping* mmio, u32 base)
{
	// Declare all the boilerplate direct MMIOs.
	struct {
		u32 addr;
		u16* ptr;
		bool align_writes_on_32_bytes;
	} directly_mapped_vars[] = {
		{ AR_INFO, &g_ARAM_Info.Hex },
		{ AR_MODE, &g_AR_MODE },
		{ AR_REFRESH, &g_AR_REFRESH },
		{ AR_DMA_MMADDR_H, MMIO::Utils::HighPart(&g_arDMA.MMAddr) },
		{ AR_DMA_MMADDR_L, MMIO::Utils::LowPart(&g_arDMA.MMAddr), true },
		{ AR_DMA_ARADDR_H, MMIO::Utils::HighPart(&g_arDMA.ARAddr) },
		{ AR_DMA_ARADDR_L, MMIO::Utils::LowPart(&g_arDMA.ARAddr), true },
		{ AR_DMA_CNT_H, MMIO::Utils::HighPart(&g_arDMA.Cnt.Hex) },
		// AR_DMA_CNT_L triggers DMA
		{ AUDIO_DMA_START_HI, MMIO::Utils::HighPart(&g_audioDMA.SourceAddress) },
		{ AUDIO_DMA_START_LO, MMIO::Utils::LowPart(&g_audioDMA.SourceAddress) },
	};
	for (auto& mapped_var : directly_mapped_vars)
	{
		u16 write_mask = mapped_var.align_writes_on_32_bytes ? 0xFFE0 : 0xFFFF;
		mmio->Register(base | mapped_var.addr,
			MMIO::DirectRead<u16>(mapped_var.ptr),
			MMIO::DirectWrite<u16>(mapped_var.ptr, write_mask)
		);
	}

	// DSP mail MMIOs call DSP emulator functions to get results or write data.
	mmio->Register(base | DSP_MAIL_TO_DSP_HI,
		MMIO::ComplexRead<u16>([](u32) {
			if (dsp_slice > DSP_MAIL_SLICE && dsp_is_lle)
			{
				dsp_emulator->DSP_Update(DSP_MAIL_SLICE);
				dsp_slice -= DSP_MAIL_SLICE;
			}
			return dsp_emulator->DSP_ReadMailBoxHigh(true);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			dsp_emulator->DSP_WriteMailBoxHigh(true, val);
		})
	);
	mmio->Register(base | DSP_MAIL_TO_DSP_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return dsp_emulator->DSP_ReadMailBoxLow(true);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			dsp_emulator->DSP_WriteMailBoxLow(true, val);
		})
	);
	mmio->Register(base | DSP_MAIL_FROM_DSP_HI,
		MMIO::ComplexRead<u16>([](u32) {
			if (dsp_slice > DSP_MAIL_SLICE && dsp_is_lle)
			{
				dsp_emulator->DSP_Update(DSP_MAIL_SLICE);
				dsp_slice -= DSP_MAIL_SLICE;
			}
			return dsp_emulator->DSP_ReadMailBoxHigh(false);
		}),
		MMIO::InvalidWrite<u16>()
	);
	mmio->Register(base | DSP_MAIL_FROM_DSP_LO,
		MMIO::ComplexRead<u16>([](u32) {
			return dsp_emulator->DSP_ReadMailBoxLow(false);
		}),
		MMIO::InvalidWrite<u16>()
	);

	mmio->Register(base | DSP_CONTROL,
		MMIO::ComplexRead<u16>([](u32) {
			return (g_dspState.DSPControl.Hex & ~DSP_CONTROL_MASK) |
			       (dsp_emulator->DSP_ReadControlRegister() & DSP_CONTROL_MASK);
		}),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			UDSPControl tmpControl;
			tmpControl.Hex = (val & ~DSP_CONTROL_MASK) |
				(dsp_emulator->DSP_WriteControlRegister(val) & DSP_CONTROL_MASK);

			// Not really sure if this is correct, but it works...
			// Kind of a hack because DSP_CONTROL_MASK should make this bit
			// only viewable to dsp emulator
			if (val & 1 /*DSPReset*/)
			{
				g_audioDMA.AudioDMAControl.Hex = 0;
			}

			// Update DSP related flags
			g_dspState.DSPControl.DSPReset     = tmpControl.DSPReset;
			g_dspState.DSPControl.DSPAssertInt = tmpControl.DSPAssertInt;
			g_dspState.DSPControl.DSPHalt      = tmpControl.DSPHalt;
			g_dspState.DSPControl.DSPInit      = tmpControl.DSPInit;

			// Interrupt (mask)
			g_dspState.DSPControl.AID_mask  = tmpControl.AID_mask;
			g_dspState.DSPControl.ARAM_mask = tmpControl.ARAM_mask;
			g_dspState.DSPControl.DSP_mask  = tmpControl.DSP_mask;

			// Interrupt
			if (tmpControl.AID)  g_dspState.DSPControl.AID  = 0;
			if (tmpControl.ARAM) g_dspState.DSPControl.ARAM = 0;
			if (tmpControl.DSP)  g_dspState.DSPControl.DSP  = 0;

			// unknown
			g_dspState.DSPControl.unk3 = tmpControl.unk3;
			g_dspState.DSPControl.pad  = tmpControl.pad;
			if (g_dspState.DSPControl.pad != 0)
			{
				PanicAlert("DSPInterface (w) g_dspState.DSPControl (CC00500A) gets a value with junk in the padding %08x", val);
			}

			UpdateInterrupts();
		})
	);

	// ARAM MMIO controlling the DMA start.
	mmio->Register(base | AR_DMA_CNT_L,
		MMIO::DirectRead<u16>(MMIO::Utils::LowPart(&g_arDMA.Cnt.Hex)),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			g_arDMA.Cnt.Hex = (g_arDMA.Cnt.Hex & 0xFFFF0000) | (val & ~31);
			Do_ARAM_DMA();
		})
	);

	// Audio DMA MMIO controlling the DMA start.
	mmio->Register(base | AUDIO_DMA_CONTROL_LEN,
		MMIO::DirectRead<u16>(&g_audioDMA.AudioDMAControl.Hex),
		MMIO::ComplexWrite<u16>([](u32, u16 val) {
			g_audioDMA.AudioDMAControl.Hex = val;
			g_audioDMA.ReadAddress = g_audioDMA.SourceAddress;
			g_audioDMA.BlocksLeft = g_audioDMA.AudioDMAControl.NumBlocks;
		})
	);

	// Audio DMA blocks remaining is invalid to write to, and requires logic on
	// the read side.
	mmio->Register(base | AUDIO_DMA_BLOCKS_LEFT,
		MMIO::ComplexRead<u16>([](u32) {
			return (g_audioDMA.BlocksLeft > 0 ? g_audioDMA.BlocksLeft - 1 : 0);
		}),
		MMIO::InvalidWrite<u16>()
	);

	// 32 bit reads/writes are a combination of two 16 bit accesses.
	for (int i = 0; i < 0x1000; i += 4)
	{
		mmio->Register(base | i,
			MMIO::ReadToSmaller<u32>(mmio, base | i, base | (i + 2)),
			MMIO::WriteToSmaller<u32>(mmio, base | i, base | (i + 2))
		);
	}
}

// UpdateInterrupts
void UpdateInterrupts()
{
	if ((g_dspState.DSPControl.AID  & g_dspState.DSPControl.AID_mask) ||
		(g_dspState.DSPControl.ARAM & g_dspState.DSPControl.ARAM_mask) ||
		(g_dspState.DSPControl.DSP  & g_dspState.DSPControl.DSP_mask))
	{
		ProcessorInterface::SetInterrupt(ProcessorInterface::INT_CAUSE_DSP, true);
	}
	else
	{
		ProcessorInterface::SetInterrupt(ProcessorInterface::INT_CAUSE_DSP, false);
	}
}

void GenerateDSPInterrupt(DSPInterruptType type, bool _bSet)
{
	switch (type)
	{
	case INT_DSP:  g_dspState.DSPControl.DSP  = _bSet ? 1 : 0; break;
	case INT_ARAM: g_dspState.DSPControl.ARAM = _bSet ? 1 : 0; if (_bSet) g_dspState.DSPControl.DMAState = 0; break;
	case INT_AID:  g_dspState.DSPControl.AID  = _bSet ? 1 : 0; break;
	}

	UpdateInterrupts();
}

// CALLED FROM DSP EMULATOR, POSSIBLY THREADED
void GenerateDSPInterruptFromDSPEmu(DSPInterruptType type, bool _bSet)
{
	CoreTiming::ScheduleEvent_Threadsafe(
		0, et_GenerateDSPInterrupt, type | (_bSet<<16));
	CoreTiming::ForceExceptionCheck(100);
}

// called whenever SystemTimers thinks the dsp deserves a few more cycles
void UpdateDSPSlice(int cycles)
{
	if (dsp_is_lle)
	{
		//use up the rest of the slice(if any)
		dsp_emulator->DSP_Update(dsp_slice);
		dsp_slice %= 6;
		//note the new budget
		dsp_slice += cycles;
	}
	else
	{
		dsp_emulator->DSP_Update(cycles);
	}
}

// This happens at 4 khz, since 32 bytes at 4khz = 4 bytes at 32 khz (16bit stereo pcm)
void UpdateAudioDMA()
{
	if (g_audioDMA.AudioDMAControl.Enable && g_audioDMA.BlocksLeft)
	{
		// Read audio at g_audioDMA.ReadAddress in RAM and push onto an
		// external audio fifo in the emulator, to be mixed with the disc
		// streaming output. If that audio queue fills up, we delay the
		// emulator.

		g_audioDMA.BlocksLeft--;
		g_audioDMA.ReadAddress += 32;

		if (g_audioDMA.BlocksLeft == 0)
		{
			dsp_emulator->DSP_SendAIBuffer(g_audioDMA.SourceAddress, 8*g_audioDMA.AudioDMAControl.NumBlocks);
			GenerateDSPInterrupt(DSP::INT_AID);
			g_audioDMA.BlocksLeft = g_audioDMA.AudioDMAControl.NumBlocks;
			g_audioDMA.ReadAddress = g_audioDMA.SourceAddress;
		}
	}
	else
	{
		// Send silence. Yeah, it's a bit of a waste to sample rate convert
		// silence.  or hm. Maybe we shouldn't do this :)
		dsp_emulator->DSP_SendAIBuffer(0, AudioInterface::GetAIDSampleRate());
	}
}

void Do_ARAM_DMA()
{
	if (g_arDMA.Cnt.count == 32)
	{
		// Beyond Good and Evil (GGEE41) sends count 32
		// Lost Kingdoms 2 needs the exception check here in DSP HLE mode
		GenerateDSPInterrupt(INT_ARAM);
		CoreTiming::ForceExceptionCheck(100);
	}
	else
	{
		g_dspState.DSPControl.DMAState = 1;
		CoreTiming::ScheduleEvent_Threadsafe(0, et_GenerateDSPInterrupt, INT_ARAM | (1<<16));

		// Force an early exception check on large transfers. Fixes RE2 audio.
		// NFS:HP2 (<= 6144)
		// Viewtiful Joe (<= 6144)
		// Sonic Mega Collection (> 2048)
		// Paper Mario battles (> 32)
		// Mario Super Baseball (> 32)
		// Knockout Kings 2003 loading (> 32)
		// WWE DOR (> 32)
		if (g_arDMA.Cnt.count > 2048 && g_arDMA.Cnt.count <= 6144)
			CoreTiming::ForceExceptionCheck(100);
	}

	// Real hardware DMAs in 32byte chunks, but we can get by with 8byte chunks
	if (g_arDMA.Cnt.dir)
	{
		// ARAM -> MRAM
		INFO_LOG(DSPINTERFACE, "DMA %08x bytes from ARAM %08x to MRAM %08x PC: %08x",
			g_arDMA.Cnt.count, g_arDMA.ARAddr, g_arDMA.MMAddr, PC);

		// Outgoing data from ARAM is mirrored every 64MB (verified on real HW)
		g_arDMA.ARAddr &= 0x3ffffff;
		g_arDMA.MMAddr &= 0x3ffffff;

		if (g_arDMA.ARAddr < g_ARAM.size)
		{
			while (g_arDMA.Cnt.count)
			{
				// These are logically separated in code to show that a memory map has been set up
				// See below in the write section for more information
				if ((g_ARAM_Info.Hex & 0xf) == 3)
				{
					Memory::Write_U64_Swap(*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask], g_arDMA.MMAddr);
				}
				else if ((g_ARAM_Info.Hex & 0xf) == 4)
				{
					Memory::Write_U64_Swap(*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask], g_arDMA.MMAddr);
				}
				else
				{
					Memory::Write_U64_Swap(*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask], g_arDMA.MMAddr);
				}

				g_arDMA.MMAddr += 8;
				g_arDMA.ARAddr += 8;
				g_arDMA.Cnt.count -= 8;
			}
		}
		else
		{
			// Assuming no external ARAM installed; returns zeroes on out of bounds reads (verified on real HW)
			while (g_arDMA.Cnt.count)
			{
				Memory::Write_U64(0, g_arDMA.MMAddr);
				g_arDMA.MMAddr += 8;
				g_arDMA.ARAddr += 8;
				g_arDMA.Cnt.count -= 8;
			}
		}
	}
	else
	{
		// MRAM -> ARAM
		INFO_LOG(DSPINTERFACE, "DMA %08x bytes from MRAM %08x to ARAM %08x PC: %08x",
			g_arDMA.Cnt.count, g_arDMA.MMAddr, g_arDMA.ARAddr, PC);

		// Incoming data into ARAM is mirrored every 64MB (verified on real HW)
		g_arDMA.ARAddr &= 0x3ffffff;
		g_arDMA.MMAddr &= 0x3ffffff;

		if (g_arDMA.ARAddr < g_ARAM.size)
		{
			while (g_arDMA.Cnt.count)
			{
				if ((g_ARAM_Info.Hex & 0xf) == 3)
				{
					*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask] = Common::swap64(Memory::Read_U64(g_arDMA.MMAddr));
				}
				else if ((g_ARAM_Info.Hex & 0xf) == 4)
				{
					if (g_arDMA.ARAddr < 0x400000)
					{
						*(u64*)&g_ARAM.ptr[(g_arDMA.ARAddr + 0x400000) & g_ARAM.mask] = Common::swap64(Memory::Read_U64(g_arDMA.MMAddr));
					}
					*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask] = Common::swap64(Memory::Read_U64(g_arDMA.MMAddr));
				}
				else
				{
					*(u64*)&g_ARAM.ptr[g_arDMA.ARAddr & g_ARAM.mask] = Common::swap64(Memory::Read_U64(g_arDMA.MMAddr));
				}

				g_arDMA.MMAddr += 8;
				g_arDMA.ARAddr += 8;
				g_arDMA.Cnt.count -= 8;
			}
		}
		else
		{
			// Assuming no external ARAM installed; writes nothing to ARAM when out of bounds (verified on real HW)
			g_arDMA.MMAddr += g_arDMA.Cnt.count;
			g_arDMA.ARAddr += g_arDMA.Cnt.count;
			g_arDMA.Cnt.count = 0;
		}
	}
}

// (shuffle2) I still don't believe that this hack is actually needed... :(
// Maybe the wii sports ucode is processed incorrectly?
// (LM) It just means that dsp reads via '0xffdd' on WII can end up in EXRAM or main RAM
u8 ReadARAM(u32 _iAddress)
{
	//NOTICE_LOG(DSPINTERFACE, "ReadARAM 0x%08x", _iAddress);
	if (g_ARAM.wii_mode)
	{
		if (_iAddress & 0x10000000)
			return g_ARAM.ptr[_iAddress & g_ARAM.mask];
		else
			return Memory::Read_U8(_iAddress & Memory::RAM_MASK);
	}
	else
	{
		return g_ARAM.ptr[_iAddress & g_ARAM.mask];
	}
}

void WriteARAM(u8 value, u32 _uAddress)
{
	//NOTICE_LOG(DSPINTERFACE, "WriteARAM 0x%08x", _uAddress);
	//TODO: verify this on WII
	g_ARAM.ptr[_uAddress & g_ARAM.mask] = value;
}

u8 *GetARAMPtr()
{
	return g_ARAM.ptr;
}

} // end of namespace DSP


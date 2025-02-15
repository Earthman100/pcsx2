/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2010  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"
#include "MemoryTypes.h"
#include "App.h"

#include "System/SysThreads.h"
#include "SaveState.h"
#include "VUmicro.h"

#include "ZipTools/ThreadedZipTools.h"
#include "Utilities/pxStreams.h"
#include "SPU2/spu2.h"
#include "USB/USB.h"
#ifdef _WIN32
#include "PAD/Windows/PAD.h"
#else
#include "PAD/Linux/PAD.h"
#endif

#include "ConsoleLogger.h"

#include <wx/wfstream.h>
#include <memory>

#include "Patch.h"

// Used to hold the current state backup (fullcopy of PS2 memory and subcomponents states).
//static VmStateBuffer state_buffer( L"Public Savestate Buffer" );

static const wxChar* EntryFilename_StateVersion = L"PCSX2 Savestate Version.id";
static const wxChar* EntryFilename_Screenshot = L"Screenshot.jpg";
static const wxChar* EntryFilename_InternalStructures = L"PCSX2 Internal Structures.dat";

struct SysState_Component
{
	const char* name;
	int (*freeze)(FreezeAction, freezeData*);
};

int SysState_MTGSFreeze(FreezeAction mode, freezeData* fP)
{
	ScopedCoreThreadPause paused_core;
	MTGS_FreezeData sstate = {fP, 0};
	GetMTGS().Freeze(mode, sstate);
	paused_core.AllowResume();
	return sstate.retval;
}

static constexpr SysState_Component SPU2{"SPU2", SPU2freeze};
static constexpr SysState_Component PAD{"PAD", PADfreeze};
static constexpr SysState_Component USB{"USB", USBfreeze};
static constexpr SysState_Component GS{"GS", SysState_MTGSFreeze};


void SysState_ComponentFreezeOutRoot(void* dest, SysState_Component comp)
{
	freezeData fP = {0, (char*)dest};
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		return;
	if (!fP.size)
		return;

	Console.Indent().WriteLn("Saving %s", comp.name);

	if (comp.freeze(FreezeAction::Save, &fP) != 0)
		throw std::runtime_error(std::string(" * ") + comp.name + std::string(": Error saving state!\n"));
}

void SysState_ComponentFreezeIn(pxInputStream& infp, SysState_Component comp)
{
	freezeData fP = {0, nullptr};
	if (comp.freeze(FreezeAction::Size, &fP) != 0)
		fP.size = 0;

	Console.Indent().WriteLn("Loading %s", comp.name);

	if (!infp.IsOk() || !infp.Length())
	{
		// no state data to read, but component expects some state data?
		// Issue a warning to console...
		if (fP.size != 0)
			Console.Indent().Warning("Warning: No data for %s found. Status may be unpredictable.", comp.name);

		return;
	}

	ScopedAlloc<s8> data(fP.size);
	fP.data = data.GetPtr();

	infp.Read(fP.data, fP.size);
	if (comp.freeze(FreezeAction::Load, &fP) != 0)
		throw std::runtime_error(std::string(" * ") + comp.name + std::string(": Error loading state!\n"));
}

void SysState_ComponentFreezeOut(SaveStateBase& writer, SysState_Component comp)
{
	freezeData fP = {0, NULL};
	if (comp.freeze(FreezeAction::Size, &fP) == 0)
	{
		const int size = fP.size;
		writer.PrepBlock(size);
		SysState_ComponentFreezeOutRoot(writer.GetBlockPtr(), comp);
		writer.CommitBlock(size);
	}
	return;
}

// --------------------------------------------------------------------------------------
//  BaseSavestateEntry
// --------------------------------------------------------------------------------------
class BaseSavestateEntry
{
protected:
	BaseSavestateEntry() = default;

public:
	virtual ~BaseSavestateEntry() = default;

	virtual wxString GetFilename() const = 0;
	virtual void FreezeIn(pxInputStream& reader) const = 0;
	virtual void FreezeOut(SaveStateBase& writer) const = 0;
	virtual bool IsRequired() const = 0;
};

class MemorySavestateEntry : public BaseSavestateEntry
{
protected:
	MemorySavestateEntry() {}
	virtual ~MemorySavestateEntry() = default;

public:
	virtual void FreezeIn(pxInputStream& reader) const;
	virtual void FreezeOut(SaveStateBase& writer) const;
	virtual bool IsRequired() const { return true; }

protected:
	virtual u8* GetDataPtr() const = 0;
	virtual uint GetDataSize() const = 0;
};

void MemorySavestateEntry::FreezeIn(pxInputStream& reader) const
{
	const uint entrySize = reader.Length();
	const uint expectedSize = GetDataSize();

	if (entrySize < expectedSize)
	{
		Console.WriteLn(Color_Yellow, " '%s' is incomplete (expected 0x%x bytes, loading only 0x%x bytes)",
						WX_STR(GetFilename()), expectedSize, entrySize);
	}

	uint copylen = std::min(entrySize, expectedSize);
	reader.Read(GetDataPtr(), copylen);
}

void MemorySavestateEntry::FreezeOut(SaveStateBase& writer) const
{
	writer.FreezeMem(GetDataPtr(), GetDataSize());
}

// --------------------------------------------------------------------------------------
//  SavestateEntry_* (EmotionMemory, IopMemory, etc)
// --------------------------------------------------------------------------------------
// Implementation Rationale:
//  The address locations of PS2 virtual memory components is fully dynamic, so we need to
//  resolve the pointers at the time they are requested (eeMem, iopMem, etc).  Thusly, we
//  cannot use static struct member initializers -- we need virtual functions that compute
//  and resolve the addresses on-demand instead... --air

class SavestateEntry_EmotionMemory : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_EmotionMemory() = default;

	wxString GetFilename() const { return L"eeMemory.bin"; }
	u8* GetDataPtr() const { return eeMem->Main; }
	uint GetDataSize() const { return sizeof(eeMem->Main); }

	virtual void FreezeIn(pxInputStream& reader) const
	{
		SysClearExecutionCache();
		MemorySavestateEntry::FreezeIn(reader);
	}
};

class SavestateEntry_IopMemory : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_IopMemory() = default;

	wxString GetFilename() const { return L"iopMemory.bin"; }
	u8* GetDataPtr() const { return iopMem->Main; }
	uint GetDataSize() const { return sizeof(iopMem->Main); }
};

class SavestateEntry_HwRegs : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_HwRegs() = default;

	wxString GetFilename() const { return L"eeHwRegs.bin"; }
	u8* GetDataPtr() const { return eeHw; }
	uint GetDataSize() const { return sizeof(eeHw); }
};

class SavestateEntry_IopHwRegs : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_IopHwRegs() = default;

	wxString GetFilename() const { return L"iopHwRegs.bin"; }
	u8* GetDataPtr() const { return iopHw; }
	uint GetDataSize() const { return sizeof(iopHw); }
};

class SavestateEntry_Scratchpad : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_Scratchpad() = default;

	wxString GetFilename() const { return L"Scratchpad.bin"; }
	u8* GetDataPtr() const { return eeMem->Scratch; }
	uint GetDataSize() const { return sizeof(eeMem->Scratch); }
};

class SavestateEntry_VU0mem : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU0mem() = default;

	wxString GetFilename() const { return L"vu0Memory.bin"; }
	u8* GetDataPtr() const { return vuRegs[0].Mem; }
	uint GetDataSize() const { return VU0_MEMSIZE; }
};

class SavestateEntry_VU1mem : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU1mem() = default;

	wxString GetFilename() const { return L"vu1Memory.bin"; }
	u8* GetDataPtr() const { return vuRegs[1].Mem; }
	uint GetDataSize() const { return VU1_MEMSIZE; }
};

class SavestateEntry_VU0prog : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU0prog() = default;

	wxString GetFilename() const { return L"vu0MicroMem.bin"; }
	u8* GetDataPtr() const { return vuRegs[0].Micro; }
	uint GetDataSize() const { return VU0_PROGSIZE; }
};

class SavestateEntry_VU1prog : public MemorySavestateEntry
{
public:
	virtual ~SavestateEntry_VU1prog() = default;

	wxString GetFilename() const { return L"vu1MicroMem.bin"; }
	u8* GetDataPtr() const { return vuRegs[1].Micro; }
	uint GetDataSize() const { return VU1_PROGSIZE; }
};

class SavestateEntry_SPU2 : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_SPU2() = default;

	wxString GetFilename() const { return L"SPU2.bin"; }
	void FreezeIn(pxInputStream& reader) const { return SysState_ComponentFreezeIn(reader, SPU2); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, SPU2); }
	bool IsRequired() const { return true; }
};

class SavestateEntry_USB : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_USB() = default;

	wxString GetFilename() const { return L"USB.bin"; }
	void FreezeIn(pxInputStream& reader) const { return SysState_ComponentFreezeIn(reader, USB); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, USB); }
	bool IsRequired() const { return true; }
};

class SavestateEntry_PAD : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_PAD() = default;

	wxString GetFilename() const { return L"PAD.bin"; }
	void FreezeIn(pxInputStream& reader) const { return SysState_ComponentFreezeIn(reader, PAD); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, PAD); }
	bool IsRequired() const { return true; }
};

class SavestateEntry_GS : public BaseSavestateEntry
{
public:
	virtual ~SavestateEntry_GS() = default;

	wxString GetFilename() const { return L"GS.bin"; }
	void FreezeIn(pxInputStream& reader) const { return SysState_ComponentFreezeIn(reader, GS); }
	void FreezeOut(SaveStateBase& writer) const { return SysState_ComponentFreezeOut(writer, GS); }
	bool IsRequired() const { return true; }
};



// (cpuRegs, iopRegs, VPU/GIF/DMAC structures should all remain as part of a larger unified
//  block, since they're all PCSX2-dependent and having separate files in the archie for them
//  would not be useful).
//

static const std::unique_ptr<BaseSavestateEntry> SavestateEntries[] = {
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_EmotionMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopMemory),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_HwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_IopHwRegs),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_Scratchpad),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1mem),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU0prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_VU1prog),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_SPU2),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_USB),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_PAD),
	std::unique_ptr<BaseSavestateEntry>(new SavestateEntry_GS),
};

// It's bad mojo to have savestates trying to read and write from the same file at the
// same time.  To prevent that we use this mutex lock, which is used by both the
// CompressThread and the UnzipFromDisk events.  (note that CompressThread locks the
// mutex during OnStartInThread, which ensures that the ZipToDisk event blocks; preventing
// the SysExecutor's Idle Event from re-enabing savestates and slots.)
//
static Mutex mtx_CompressToDisk;

static void CheckVersion(pxInputStream& thr)
{
	u32 savever;
	thr.Read(savever);

	// Major version mismatch.  Means we can't load this savestate at all.  Support for it
	// was removed entirely.
	if (savever > g_SaveVersion)
		throw Exception::SaveStateLoadError(thr.GetStreamName())
			.SetDiagMsg(pxsFmt(L"Savestate uses an unsupported or unknown savestate version.\n(PCSX2 ver=%x, state ver=%x)", g_SaveVersion, savever))
			.SetUserMsg(_("Cannot load this savestate. The state is an unsupported version."));

	// check for a "minor" version incompatibility; which happens if the savestate being loaded is a newer version
	// than the emulator recognizes.  99% chance that trying to load it will just corrupt emulation or crash.
	if ((savever >> 16) != (g_SaveVersion >> 16))
		throw Exception::SaveStateLoadError(thr.GetStreamName())
			.SetDiagMsg(pxsFmt(L"Savestate uses an unknown savestate version.\n(PCSX2 ver=%x, state ver=%x)", g_SaveVersion, savever))
			.SetUserMsg(_("Cannot load this savestate. The state is an unsupported version."));
};

// --------------------------------------------------------------------------------------
//  SysExecEvent_DownloadState
// --------------------------------------------------------------------------------------
// Pauses core emulation and downloads the savestate into a memory buffer.  The memory buffer
// is then mailed to another thread for zip archiving, while the main emulation process is
// allowed to continue execution.
//
class SysExecEvent_DownloadState : public SysExecEvent
{
protected:
	ArchiveEntryList* m_dest_list;

public:
	wxString GetEventName() const { return L"VM_Download"; }

	virtual ~SysExecEvent_DownloadState() = default;
	SysExecEvent_DownloadState* Clone() const { return new SysExecEvent_DownloadState(*this); }
	SysExecEvent_DownloadState(ArchiveEntryList* dest_list = NULL)
	{
		m_dest_list = dest_list;
	}

	bool IsCriticalEvent() const { return true; }
	bool AllowCancelOnExit() const { return false; }

protected:
	void InvokeEvent()
	{
		ScopedCoreThreadPause paused_core;

		if (!SysHasValidState())
			throw Exception::RuntimeError()
				.SetDiagMsg(L"SysExecEvent_DownloadState: Cannot freeze/download an invalid VM state!")
				.SetUserMsg(_("There is no active virtual machine state to download or save."));

		memSavingState saveme(m_dest_list->GetBuffer());
		ArchiveEntry internals(EntryFilename_InternalStructures);
		internals.SetDataIndex(saveme.GetCurrentPos());

		saveme.FreezeBios();
		saveme.FreezeInternals();

		internals.SetDataSize(saveme.GetCurrentPos() - internals.GetDataIndex());
		m_dest_list->Add(internals);

		for (uint i = 0; i < ArraySize(SavestateEntries); ++i)
		{
			uint startpos = saveme.GetCurrentPos();
			SavestateEntries[i]->FreezeOut(saveme);
			m_dest_list->Add(ArchiveEntry(SavestateEntries[i]->GetFilename())
								 .SetDataIndex(startpos)
								 .SetDataSize(saveme.GetCurrentPos() - startpos));
		}

		UI_EnableStateActions();
		paused_core.AllowResume();
	}
};


// --------------------------------------------------------------------------------------
//  CompressThread_VmState
// --------------------------------------------------------------------------------------
class VmStateCompressThread : public BaseCompressThread
{
	typedef BaseCompressThread _parent;

protected:
	ScopedLock m_lock_Compress;

public:
	VmStateCompressThread()
	{
		m_lock_Compress.Assign(mtx_CompressToDisk);
	}

	virtual ~VmStateCompressThread() = default;

protected:
	void OnStartInThread()
	{
		_parent::OnStartInThread();
		m_lock_Compress.Acquire();
	}

	void OnCleanupInThread()
	{
		m_lock_Compress.Release();
		_parent::OnCleanupInThread();
	}
};

// --------------------------------------------------------------------------------------
//  SysExecEvent_ZipToDisk
// --------------------------------------------------------------------------------------
class SysExecEvent_ZipToDisk : public SysExecEvent
{
protected:
	ArchiveEntryList* m_src_list;
	wxString m_filename;

public:
	wxString GetEventName() const { return L"VM_ZipToDisk"; }

	virtual ~SysExecEvent_ZipToDisk() = default;

	SysExecEvent_ZipToDisk* Clone() const { return new SysExecEvent_ZipToDisk(*this); }

	SysExecEvent_ZipToDisk(ArchiveEntryList& srclist, const wxString& filename)
		: m_filename(filename)
	{
		m_src_list = &srclist;
	}

	SysExecEvent_ZipToDisk(ArchiveEntryList* srclist, const wxString& filename)
		: m_filename(filename)
	{
		m_src_list = srclist;
	}

	bool IsCriticalEvent() const { return true; }
	bool AllowCancelOnExit() const { return false; }

protected:
	void InvokeEvent()
	{
		// Provisionals for scoped cleanup, in case of exception:
		std::unique_ptr<ArchiveEntryList> elist(m_src_list);

		wxString tempfile(m_filename + L".tmp");

		wxFFileOutputStream* woot = new wxFFileOutputStream(tempfile);
		if (!woot->IsOk())
			throw Exception::CannotCreateStream(tempfile);

		// Scheduler hint (yield) -- creating and saving the file is low priority compared to
		// the emulator/vm thread.  Sleeping the executor thread briefly before doing file
		// transactions should help reduce overhead. --air

		pxYield(4);

		// Write the version and screenshot:
		std::unique_ptr<pxOutputStream> out(new pxOutputStream(tempfile, new wxZipOutputStream(woot)));
		wxZipOutputStream* gzfp = (wxZipOutputStream*)out->GetWxStreamBase();

		{
			wxZipEntry* vent = new wxZipEntry(EntryFilename_StateVersion);
			vent->SetMethod(wxZIP_METHOD_STORE);
			gzfp->PutNextEntry(vent);
			out->Write(g_SaveVersion);
			gzfp->CloseEntry();
		}

		std::unique_ptr<wxImage> m_screenshot;

		if (m_screenshot)
		{
			wxZipEntry* vent = new wxZipEntry(EntryFilename_Screenshot);
			vent->SetMethod(wxZIP_METHOD_STORE);
			gzfp->PutNextEntry(vent);
			m_screenshot->SaveFile(*gzfp, wxBITMAP_TYPE_JPEG);
			gzfp->CloseEntry();
		}

		(*new VmStateCompressThread())
			.SetSource(elist.get())
			.SetOutStream(out.get())
			.SetFinishedPath(m_filename)
			.Start();

		// No errors?  Release cleanup handlers:
		elist.release();
		out.release();
	}

	void CleanupEvent()
	{
	}
};

// --------------------------------------------------------------------------------------
//  SysExecEvent_UnzipFromDisk
// --------------------------------------------------------------------------------------
// Note: Unzipping always goes directly into the SysCoreThread's static VM state, and is
// always a blocking action on the SysExecutor thread (the system cannot execute other
// commands while states are unzipping or uploading into the system).
//
class SysExecEvent_UnzipFromDisk : public SysExecEvent
{
protected:
	wxString m_filename;

public:
	wxString GetEventName() const { return L"VM_UnzipFromDisk"; }

	virtual ~SysExecEvent_UnzipFromDisk() = default;
	SysExecEvent_UnzipFromDisk* Clone() const { return new SysExecEvent_UnzipFromDisk(*this); }
	SysExecEvent_UnzipFromDisk(const wxString& filename)
		: m_filename(filename)
	{
	}

	wxString GetStreamName() const { return m_filename; }

protected:
	void InvokeEvent()
	{
		ScopedLock lock(mtx_CompressToDisk);

		// Ugh.  Exception handling made crappy because wxWidgets classes don't support scoped pointers yet.

		std::unique_ptr<wxFFileInputStream> woot(new wxFFileInputStream(m_filename));
		if (!woot->IsOk())
			throw Exception::CannotCreateStream(m_filename).SetDiagMsg(L"Cannot open file for reading.");

		std::unique_ptr<pxInputStream> reader(new pxInputStream(m_filename, new wxZipInputStream(woot.get())));
		woot.release();

		if (!reader->IsOk())
		{
			throw Exception::SaveStateLoadError(m_filename)
				.SetDiagMsg(L"Savestate file is not a valid gzip archive.")
				.SetUserMsg(_("This savestate cannot be loaded because it is not a valid gzip archive.  It may have been created by an older unsupported version of PCSX2, or it may be corrupted."));
		}

		wxZipInputStream* gzreader = (wxZipInputStream*)reader->GetWxStreamBase();

		// look for version and screenshot information in the zip stream:

		bool foundVersion = false;
		//bool foundScreenshot = false;
		//bool foundEntry[ArraySize(SavestateEntries)] = false;

		std::unique_ptr<wxZipEntry> foundInternal;
		std::unique_ptr<wxZipEntry> foundEntry[ArraySize(SavestateEntries)];

		while (true)
		{
			Threading::pxTestCancel();

			std::unique_ptr<wxZipEntry> entry(gzreader->GetNextEntry());
			if (!entry)
				break;

			if (entry->GetName().CmpNoCase(EntryFilename_StateVersion) == 0)
			{
				DevCon.WriteLn(Color_Green, L" ... found '%s'", EntryFilename_StateVersion);
				foundVersion = true;
				CheckVersion(*reader);
				continue;
			}

			if (entry->GetName().CmpNoCase(EntryFilename_InternalStructures) == 0)
			{
				DevCon.WriteLn(Color_Green, L" ... found '%s'", EntryFilename_InternalStructures);
				foundInternal = std::move(entry);
				continue;
			}

			// No point in finding screenshots when loading states -- the screenshots are
			// only useful for the UI savestate browser.
			/*if (entry->GetName().CmpNoCase(EntryFilename_Screenshot) == 0)
			{
				foundScreenshot = true;
			}*/

			for (uint i = 0; i < ArraySize(SavestateEntries); ++i)
			{
				if (entry->GetName().CmpNoCase(SavestateEntries[i]->GetFilename()) == 0)
				{
					DevCon.WriteLn(Color_Green, L" ... found '%s'", WX_STR(SavestateEntries[i]->GetFilename()));
					foundEntry[i] = std::move(entry);
					break;
				}
			}
		}

		if (!foundVersion || !foundInternal)
		{
			throw Exception::SaveStateLoadError(m_filename)
				.SetDiagMsg(pxsFmt(L"Savestate file does not contain '%s'",
								   !foundVersion ? EntryFilename_StateVersion : EntryFilename_InternalStructures))
				.SetUserMsg(_("This file is not a valid PCSX2 savestate.  See the logfile for details."));
		}

		// Log any parts and pieces that are missing, and then generate an exception.
		bool throwIt = false;
		for (uint i = 0; i < ArraySize(SavestateEntries); ++i)
		{
			if (foundEntry[i])
				continue;

			if (SavestateEntries[i]->IsRequired())
			{
				throwIt = true;
				Console.WriteLn(Color_Red, " ... not found '%s'!", WX_STR(SavestateEntries[i]->GetFilename()));
			}
		}

		if (throwIt)
			throw Exception::SaveStateLoadError(m_filename)
				.SetDiagMsg(L"Savestate cannot be loaded: some required components were not found or are incomplete.")
				.SetUserMsg(_("This savestate cannot be loaded due to missing critical components.  See the log file for details."));

		// We use direct Suspend/Resume control here, since it's desirable that emulation
		// *ALWAYS* start execution after the new savestate is loaded.

		PatchesVerboseReset();

		GetCoreThread().Pause();
		SysClearExecutionCache();

		for (uint i = 0; i < ArraySize(SavestateEntries); ++i)
		{
			if (!foundEntry[i])
				continue;

			Threading::pxTestCancel();

			gzreader->OpenEntry(*foundEntry[i]);
			SavestateEntries[i]->FreezeIn(*reader);
		}

		// Load all the internal data

		gzreader->OpenEntry(*foundInternal);

		VmStateBuffer buffer(foundInternal->GetSize(), L"StateBuffer_UnzipFromDisk"); // start with an 8 meg buffer to avoid frequent reallocation.
		reader->Read(buffer.GetPtr(), foundInternal->GetSize());

		memLoadingState(buffer).FreezeBios().FreezeInternals();
		GetCoreThread().Resume(); // force resume regardless of emulation state earlier.
	}
};

// =====================================================================================================
//  StateCopy Public Interface
// =====================================================================================================

void StateCopy_SaveToFile(const wxString& file)
{
	UI_DisableStateActions();

	std::unique_ptr<ArchiveEntryList> ziplist(new ArchiveEntryList(new VmStateBuffer(L"Zippable Savestate")));

	GetSysExecutorThread().PostEvent(new SysExecEvent_DownloadState(ziplist.get()));
	GetSysExecutorThread().PostEvent(new SysExecEvent_ZipToDisk(ziplist.get(), file));

	ziplist.release();
}

void StateCopy_LoadFromFile(const wxString& file)
{
	UI_DisableSysActions();
	GetSysExecutorThread().PostEvent(new SysExecEvent_UnzipFromDisk(file));
}

// Saves recovery state info to the given saveslot, or saves the active emulation state
// (if one exists) and no recovery data was found.  This is needed because when a recovery
// state is made, the emulation state is usually reset so the only persisting state is
// the one in the memory save. :)
void StateCopy_SaveToSlot(uint num)
{
	const wxString file(SaveStateBase::GetFilename(num));

	// Backup old Savestate if one exists.
	if (wxFileExists(file) && EmuConfig.BackupSavestate)
	{
		const wxString copy(SaveStateBase::GetFilename(num) + pxsFmt(L".backup"));

		Console.Indent().WriteLn(Color_StrongGreen, L"Backing up existing state in slot %d.", num);
		wxRenameFile(file, copy);
	}

	OSDlog(Color_StrongGreen, true, "Saving savestate to slot %d...", num);
	Console.Indent().WriteLn(Color_StrongGreen, L"filename: %s", WX_STR(file));

	StateCopy_SaveToFile(file);
#ifdef USE_NEW_SAVESLOTS_UI
	UI_UpdateSysControls();
#endif
}

void StateCopy_LoadFromSlot(uint slot, bool isFromBackup)
{
	wxString file(SaveStateBase::GetFilename(slot) + wxString(isFromBackup ? L".backup" : L""));

	if (!wxFileExists(file))
	{
		OSDlog(Color_StrongGreen, true, "Savestate slot %d%s is empty.", slot, isFromBackup ? " (backup)" : "");
		return;
	}

	OSDlog(Color_StrongGreen, true, "Loading savestate from slot %d...%s", slot, isFromBackup ? " (backup)" : "");
	Console.Indent().WriteLn(Color_StrongGreen, L"filename: %s", WX_STR(file));

	StateCopy_LoadFromFile(file);
#ifdef USE_NEW_SAVESLOTS_UI
	UI_UpdateSysControls();
#endif
}


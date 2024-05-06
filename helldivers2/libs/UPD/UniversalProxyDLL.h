#pragma once

#include <Windows.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <thread>
#include <map>
#include <iomanip>
#include <sstream>

// The DLL will crash with incremental linking enabled.
// This linker comment will override any other project setting.
#pragma comment(linker, "/INCREMENTAL:NO")

extern std::vector<void*> forwardAddresses;
extern std::vector<void*> forwardOrdinalAddresses;
extern std::vector<void*> forwardSharedAddresses;

#define SharedExportIndex_DllCanUnloadNow 0
#define SharedExportIndex_DllGetClassObject 1
#define SharedExportIndex_SetAppCompatStringPointer 2

namespace UPD
{
	struct ExportData
	{
		std::string name = "N/A";
		WORD ordinal = 0;
		uintptr_t relativeAddress = 0;
		uintptr_t asmCodeAddress = 0;
		void* forwardFunctionAddress = nullptr;
	};

	struct Callback
	{
		uintptr_t callbackAddress = 0;
		uintptr_t returnAddress = 0;
	};

	std::map<std::string, WORD> sharedExports =
	{
		{ "DllCanUnloadNow", SharedExportIndex_DllCanUnloadNow },
		{ "DllGetClassObject", SharedExportIndex_DllGetClassObject },
		{ "SetAppCompatStringPointer", SharedExportIndex_SetAppCompatStringPointer }
	};

	// Global variables
	bool muteLogging = true;
	alignas(256) bool isProxyReady = false;
	std::ofstream logFile;
	std::vector<ExportData> managedExports;
	std::map<std::string, Callback> callbacks;
	std::vector<unsigned char> dllMemory;
	PIMAGE_SECTION_HEADER sectionHeadersBase = 0;
	WORD numSections = 0;

	// Function prototypes
	void* RegisterCallback(std::string exportName, void* callback);
	void CreateProxy(HMODULE dllToProxy, std::string specificPathToSearch);
	std::string GetModuleFileNameFromModuleHandle(HMODULE handle);
	std::string FindOriginalDLL(std::string dllFileName, std::string specificPathToSearch);
	std::string SearchForDLLInSpecificPath(std::string dllFileName, std::string specificPath);
	std::string SearchForDLLUsingStandardSearchOrder(std::string dllFileName);
	std::string SearchForDLLInCurrentFolder(std::string dllFileName);
	std::string SearchForDLLInSystemFolder(std::string dllFileName);
	bool DoesFileExist(std::string filePath);
	std::vector<unsigned char> LoadFileToVectorOfBytes(std::string filePath);
	void ReadRequiredDataFromDLLExportTable();
	PIMAGE_DOS_HEADER GetDLLDosHeader(void* peHeaderBase);
	PIMAGE_NT_HEADERS GetDLLNtHeaders(PIMAGE_DOS_HEADER dosHeader);
	void SetSectionHeadersInfo(PIMAGE_FILE_HEADER fileHeader, PIMAGE_OPTIONAL_HEADER optionalHeader);
	PIMAGE_EXPORT_DIRECTORY GetExportDirectory(PIMAGE_OPTIONAL_HEADER optionalHeader);
	uintptr_t RvaToRawAddress(DWORD rva);
	void PrepareForwardFunctionsWithJumpsToAsm();
	uintptr_t CreateMemoryWithAssemblyForwardingCode();
	std::vector<unsigned char> GetAsmForwardingCode32();
	std::vector<unsigned char> GetAsmForwardingCode64();
	void StartProxyCreationThread(std::string originalDllPath);
	DWORD WINAPI ThreadCreateProxy(LPVOID lpParam);
	void InjectFunctionAddressesIntoAsmForwardingCode(HMODULE module);
	void InjectFunctionAddresses32(HMODULE module);
	void InjectFunctionAddresses64(HMODULE module);
	void MemCopy(uintptr_t destination, uintptr_t source, size_t numBytes);
	void MemSet(uintptr_t address, unsigned char byte, size_t numBytes);
	void ToggleMemoryProtection(bool enableProtection, uintptr_t address, size_t size);
	void Hook(uintptr_t address, uintptr_t destination, size_t extraClearance = 0);
	void Hook32(uintptr_t address, uintptr_t destination, size_t clearance);
	void Hook64(uintptr_t address, uintptr_t destination, size_t clearance);
	int32_t CalculateDisplacementForRelativeJump(uintptr_t relativeJumpAddress, uintptr_t destinationAddress);
	void LogAndThrow(std::string exceptionMessage);
	template<typename... Types> void Log(Types... args);
	template<typename T> std::string NumberToHexString(T number);

	void* RegisterCallback(std::string exportName, void* callback)
	{
		if (callback != nullptr)
		{
			callbacks[exportName].callbackAddress = (uintptr_t)callback;
			Log("Registered callback: ", exportName);
			return &callbacks[exportName].returnAddress;
		}
		return nullptr;
	}

	void CreateProxy(HMODULE dllToProxy, std::string specificPathToSearch = "")
	{
		std::string dllToProxyFileName = GetModuleFileNameFromModuleHandle(dllToProxy);
		std::string dllFilePath = FindOriginalDLL(dllToProxyFileName, specificPathToSearch);
		dllMemory = LoadFileToVectorOfBytes(dllFilePath);
		ReadRequiredDataFromDLLExportTable();
		PrepareForwardFunctionsWithJumpsToAsm();
		StartProxyCreationThread(dllFilePath);
	}

	std::string GetModuleFileNameFromModuleHandle(HMODULE handle)
	{
		std::string moduleFileName = "";
		char lpFilename[MAX_PATH];
		GetModuleFileNameA(handle, lpFilename, sizeof(lpFilename));
		char* lastSlashPos = strrchr(lpFilename, '\\');
		if (lastSlashPos != nullptr)
		{
			moduleFileName = lastSlashPos;
			moduleFileName = moduleFileName.substr(1, moduleFileName.length());
		}
		Log("Module name: ", moduleFileName.c_str());

		if (moduleFileName == "")
		{
			LogAndThrow("Could not get module name from module handle");
		}
		return moduleFileName;
	}

	std::string FindOriginalDLL(std::string dllFileName, std::string specificPathToSearch = "")
	{
		std::string dllPath = "";
		if (specificPathToSearch != "")
		{
			dllPath = SearchForDLLInSpecificPath(dllFileName, specificPathToSearch);
		}
		
		if (dllPath == "")
		{
			dllPath = SearchForDLLUsingStandardSearchOrder(dllFileName);
		}

		if (dllPath == "")
		{
			LogAndThrow("Could not find DLL to proxy");
		}

		Log("DLL found: ", dllPath);
		return dllPath;
	}

	std::string SearchForDLLInSpecificPath(std::string dllFileName, std::string specificPath)
	{
		std::string dllPath = specificPath + "\\" + dllFileName;
		if (!DoesFileExist(dllPath))
		{
			return "";
		}
		return dllPath;
	}

	std::string SearchForDLLUsingStandardSearchOrder(std::string dllFileName)
	{
		std::string dllPath = SearchForDLLInCurrentFolder("_" + dllFileName);
		if (dllPath == "")
		{
			dllPath = SearchForDLLInSystemFolder(dllFileName);
		}
		return dllPath;
	}

	std::string SearchForDLLInCurrentFolder(std::string dllFileName)
	{
		std::string dllPath = ".\\" + dllFileName;
		if (!DoesFileExist(dllPath))
		{
			return "";
		}
		return dllPath;
	}

	std::string SearchForDLLInSystemFolder(std::string dllFileName)
	{
		char lpBuffer[MAX_PATH];
		UINT pathLength = GetSystemDirectoryA(lpBuffer, sizeof(lpBuffer));
		std::string systemFolderPath = lpBuffer;
		std::string dllPath = systemFolderPath + "\\" + dllFileName;
		if (!DoesFileExist(dllPath))
		{
			return "";
		}
		return dllPath;
	}

	bool DoesFileExist(std::string filePath)
	{
		std::ifstream file(filePath);
		if (file.is_open())
		{
			return true;
		}
		return false;
	}

	std::vector<unsigned char> LoadFileToVectorOfBytes(std::string filePath)
	{
		std::ifstream file;
		file.open(filePath, std::ios::in | std::ios::binary);
		if (!file.is_open())
		{
			LogAndThrow("Failed to read DLL: " + filePath);
		}
		return std::vector<unsigned char>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	void ReadRequiredDataFromDLLExportTable()
	{
		void* peHeaderBase = &dllMemory[0];
		PIMAGE_DOS_HEADER dosHeader = GetDLLDosHeader(peHeaderBase);
		PIMAGE_NT_HEADERS ntHeaders = GetDLLNtHeaders(dosHeader);
		PIMAGE_OPTIONAL_HEADER optionalHeader = &ntHeaders->OptionalHeader;
		PIMAGE_FILE_HEADER fileHeader = &ntHeaders->FileHeader;

		SetSectionHeadersInfo(fileHeader, optionalHeader);

		PIMAGE_EXPORT_DIRECTORY exportDirectory = GetExportDirectory(optionalHeader);
		DWORD numberOfExports = exportDirectory->NumberOfFunctions;
		DWORD numberOfNamedExports = exportDirectory->NumberOfNames;
		Log("Number of exports: ", numberOfExports);

		uintptr_t tableOfRvaPointersToExportNames = RvaToRawAddress(exportDirectory->AddressOfNames);
		uintptr_t tableOfNameOrdinals = RvaToRawAddress(exportDirectory->AddressOfNameOrdinals);
		uintptr_t tableOfRvaPointersToExportAddresses = RvaToRawAddress(exportDirectory->AddressOfFunctions);
		
		std::map<WORD, WORD> indicesOfNamePointersInOrdinalOrder;
		std::vector<WORD> buffer(numberOfNamedExports, 0);
		MemCopy((uintptr_t)&buffer[0], (uintptr_t)tableOfNameOrdinals, numberOfNamedExports * sizeof(WORD));
		for (WORD i = 0; i < numberOfExports; i++)
		{
			auto it = std::find(buffer.begin(), buffer.end(), i);
			if (it != buffer.end())
			{
				WORD bufferIndex = (WORD)(it - buffer.begin());
				indicesOfNamePointersInOrdinalOrder[i] = bufferIndex;
			}
		}

		for (WORD i = 0; i < numberOfExports; i++)
		{
			ExportData exportData;

			if (indicesOfNamePointersInOrdinalOrder.find(i) != indicesOfNamePointersInOrdinalOrder.end())
			{
				DWORD* exportNameAddress = ((DWORD*)tableOfRvaPointersToExportNames + indicesOfNamePointersInOrdinalOrder[i]);
				exportData.name = (char*)RvaToRawAddress(*exportNameAddress);
			}
			exportData.ordinal = (WORD)exportDirectory->Base + i;
			exportData.relativeAddress = *((DWORD*)tableOfRvaPointersToExportAddresses + i);

			Log("Export name: ", exportData.name);
			Log("Export ordinal: ", exportData.ordinal);
			Log("Export RVA: ", NumberToHexString(exportData.relativeAddress));

			bool exportIsUnnamed = exportData.name == "N/A";
			bool exportIsShared = sharedExports.find(exportData.name) != sharedExports.end();
			if (exportIsUnnamed)
			{
				exportData.forwardFunctionAddress = forwardOrdinalAddresses[exportData.ordinal - 1];
			}
			else if (exportIsShared)
			{
				exportData.forwardFunctionAddress = forwardSharedAddresses[sharedExports[exportData.name]];
			}
			else
			{
				exportData.forwardFunctionAddress = forwardAddresses[i];
			}

			managedExports.push_back(exportData);
		}
	}

	PIMAGE_DOS_HEADER GetDLLDosHeader(void* peHeaderBase)
	{
		PIMAGE_DOS_HEADER dosHeader = (PIMAGE_DOS_HEADER)peHeaderBase;
		if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
		{
			LogAndThrow("Incorrect DOS signature");
		}
		return dosHeader;
	}

	PIMAGE_NT_HEADERS GetDLLNtHeaders(PIMAGE_DOS_HEADER dosHeader)
	{
		PIMAGE_NT_HEADERS ntHeaders = (PIMAGE_NT_HEADERS)((unsigned char*)dosHeader + dosHeader->e_lfanew);
		if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
		{
			LogAndThrow("Incorrect NT signature");
		}
		return ntHeaders;
	}

	void SetSectionHeadersInfo(PIMAGE_FILE_HEADER fileHeader, PIMAGE_OPTIONAL_HEADER optionalHeader)
	{
		DWORD sizeOfOptionalHeader = fileHeader->SizeOfOptionalHeader;
		sectionHeadersBase = (PIMAGE_SECTION_HEADER)((unsigned char*)optionalHeader + sizeOfOptionalHeader);
		numSections = fileHeader->NumberOfSections;
	}

	PIMAGE_EXPORT_DIRECTORY GetExportDirectory(PIMAGE_OPTIONAL_HEADER optionalHeader)
	{
		PIMAGE_DATA_DIRECTORY exportDataDirectory = &(optionalHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]);
		PIMAGE_EXPORT_DIRECTORY exportDirectory = (PIMAGE_EXPORT_DIRECTORY)RvaToRawAddress(exportDataDirectory->VirtualAddress);
		return exportDirectory;
	}

	uintptr_t RvaToRawAddress(DWORD rva)
	{
		for (DWORD i = 0; i < numSections; i++)
		{
			PIMAGE_SECTION_HEADER section = sectionHeadersBase + i;
			DWORD sectionVirtualBegin = section->VirtualAddress;
			DWORD sectionVirtualEnd = section->VirtualAddress + section->Misc.VirtualSize;
			bool isWithinSection = rva >= sectionVirtualBegin && rva <= sectionVirtualEnd;
			if (isWithinSection)
			{
				uintptr_t moduleBaseAddress = (uintptr_t)&dllMemory[0];
				uintptr_t rawAddress = (uintptr_t)((section->PointerToRawData + (rva - section->VirtualAddress)) + moduleBaseAddress);
				return rawAddress;
			}
		}

		LogAndThrow("Could not convert RVA to raw address");
		return 0;
	}

	void PrepareForwardFunctionsWithJumpsToAsm()
	{
		for (auto& managedExport : managedExports)
		{
			managedExport.asmCodeAddress = CreateMemoryWithAssemblyForwardingCode();
			Hook((uintptr_t)managedExport.forwardFunctionAddress, managedExport.asmCodeAddress);

			bool exportIsNamed = managedExport.name != "N/A";
			if (exportIsNamed)
			{
				Hook((uintptr_t)forwardOrdinalAddresses[managedExport.ordinal - 1], managedExport.asmCodeAddress);
			}

			Log("Prepared forward function ", NumberToHexString((uintptr_t)managedExport.forwardFunctionAddress));
		}
	}

	uintptr_t CreateMemoryWithAssemblyForwardingCode()
	{
		std::vector<unsigned char> asmBytes;
		size_t asmOffsetOfBoolAddress = 0;
#ifndef _WIN64
		asmBytes = GetAsmForwardingCode32();
		asmOffsetOfBoolAddress = 1;
#else
		asmBytes = GetAsmForwardingCode64();
		asmOffsetOfBoolAddress = 2;
#endif

		void* pointerToBool = &isProxyReady;
		MemCopy((uintptr_t)&asmBytes[asmOffsetOfBoolAddress], (uintptr_t)&pointerToBool, sizeof(uintptr_t));

		uintptr_t memoryAddress = (uintptr_t)VirtualAlloc(NULL, asmBytes.size(), MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (memoryAddress == NULL)
		{
			LogAndThrow("Failed to allocate memory");
		}

		MemCopy((uintptr_t)memoryAddress, (uintptr_t)&asmBytes[0], asmBytes.size());
		Log("Created assembly forwarding code at ", NumberToHexString(memoryAddress));

		return memoryAddress;
	}

	std::vector<unsigned char> GetAsmForwardingCode32()
	{
		std::vector<unsigned char> asmBytes =
		{
			0xb8, 0x00, 0x00, 0x00, 0x00, // mov eax,isProxyReady
			0x38, 0x00, // cmp [eax],al
			0x74, 0x05, // je <spinwait>
			// <jmptofunc>:
			0xe9, 0x00, 0x00, 0x00, 0x00, // jmp funcAddr
			// <spinwait>:
			0xf3, 0x90, // pause
			0xf0, 0x00, 0x00, // lock add [eax],al
			0x74, 0xf9, // je <spinwait>
			0xe9, 0xef, 0xff, 0xff, 0xff // jmp <jmptofunc>
		};
		return asmBytes;
	}

	std::vector<unsigned char> GetAsmForwardingCode64()
	{
		std::vector<unsigned char> asmBytes = {
			// Courtesy of my buddy Dasaav
			0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mov rax,isProxyReady
			0x38, 0x00, // cmp [rax],al
			0x74, 0x06, // je <spinwait>
			0xff, 0x25, 0x0d, 0x00, 0x00, 0x00,  // jmp qword ptr [funcAddr]
			// <spinwait>:
			0xf3, 0x90, // pause
			0xf0, 0x00, 0x00, // lock add [rax],al
			0x74, 0xf9, // je <spinwait>
			0xff, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 // jmp funcAddr
		};
		return asmBytes;
	}

	void StartProxyCreationThread(std::string originalDllPath)
	{
		// Thread will run only after DllMain is finished!
		// This is enforced by the DLL loader. This means any calls to LoadLibrary, etc. is safe.
		std::string* dllPath = new std::string(originalDllPath);
		CreateThread(0, 0, &ThreadCreateProxy, dllPath, 0, NULL);
	}
	
	DWORD WINAPI ThreadCreateProxy(LPVOID lpParam)
	{
		Log("Starting proxy creation thread...");

		std::string dllPath = *(std::string*)lpParam;
		delete (std::string*)lpParam;
		HMODULE originalDll = LoadLibraryA(dllPath.c_str());
		if (!originalDll)
		{
			LogAndThrow("LoadLibrary call for original DLL failed!");
		}
		Log("Module base address: ", NumberToHexString(originalDll));

		InjectFunctionAddressesIntoAsmForwardingCode(originalDll);

		// We have created/modified instructions, flush the CPU instruction cache just in case
		FlushInstructionCache(GetCurrentProcess(), NULL, NULL);

		isProxyReady = true;
		Log("Proxy creation finished");

		return 0;
	}

	void InjectFunctionAddressesIntoAsmForwardingCode(HMODULE module)
	{
#ifndef _WIN64
		InjectFunctionAddresses32(module);
#else 
		InjectFunctionAddresses64(module);
#endif
	}

	void InjectFunctionAddresses32(HMODULE module)
	{
		const size_t asmOffsetOfJmpAddr = 10;
		for (auto& managedExport : managedExports)
		{
			uintptr_t exportAbsoluteAddress = (uintptr_t)module + managedExport.relativeAddress;
			uintptr_t jumpTargetAddress = exportAbsoluteAddress;
			Log("Export absolute address: ", NumberToHexString(exportAbsoluteAddress));

			bool isCallbackRegistered = callbacks.find(managedExport.name) != callbacks.end();
			if (isCallbackRegistered)
			{
				auto& callback = callbacks[managedExport.name];
				callback.returnAddress = exportAbsoluteAddress;
				jumpTargetAddress = callback.callbackAddress;
				Log("Added callback in ", managedExport.name, " to ", NumberToHexString(callback.callbackAddress));
			}

			int32_t relativeDisplacement = CalculateDisplacementForRelativeJump(managedExport.asmCodeAddress + asmOffsetOfJmpAddr - 1, jumpTargetAddress);
			MemCopy(managedExport.asmCodeAddress + asmOffsetOfJmpAddr, (uintptr_t)&relativeDisplacement, sizeof(uintptr_t));
		}
	}

	void InjectFunctionAddresses64(HMODULE module)
	{
		const size_t asmOffsetOfJmpAddr = 33;
		for (auto& managedExport : managedExports)
		{
			uintptr_t exportAbsoluteAddress = (uintptr_t)module + managedExport.relativeAddress;
			uintptr_t jumpTargetAddress = exportAbsoluteAddress;
			Log("Export absolute address: ", NumberToHexString(exportAbsoluteAddress));

			bool isCallbackRegistered = callbacks.find(managedExport.name) != callbacks.end();
			if (isCallbackRegistered)
			{
				auto& callback = callbacks[managedExport.name];
				callback.returnAddress = exportAbsoluteAddress;
				jumpTargetAddress = callback.callbackAddress;
				Log("Added callback in ", managedExport.name, " to ", NumberToHexString(callback.callbackAddress));
			}

			MemCopy(managedExport.asmCodeAddress + asmOffsetOfJmpAddr, (uintptr_t)&jumpTargetAddress, sizeof(uintptr_t));
		}
	}

	void Hook(uintptr_t address, uintptr_t destination, size_t extraClearance)
	{
		size_t clearance = 0;
#ifndef _WIN64
		clearance = 5 + extraClearance;
		Hook32(address, destination, extraClearance);
#else
		clearance = 14 + extraClearance;
		Hook64(address, destination, extraClearance);
#endif
		Log("Created jump from ", NumberToHexString(address), " to ", NumberToHexString(destination), " with a clearance of ", clearance);
	}

	void Hook32(uintptr_t address, uintptr_t destination, size_t clearance)
	{
		MemSet(address, 0x90, clearance);
		unsigned char jumpByte = 0xe9;
		MemCopy(address, (uintptr_t)&jumpByte, 1);
		int32_t relativeDisplacement = CalculateDisplacementForRelativeJump(address, destination);
		MemCopy(address + 1, (uintptr_t)&relativeDisplacement, sizeof(uintptr_t));
	}

	void Hook64(uintptr_t address, uintptr_t destination, size_t clearance)
	{
		MemSet(address, 0x90, clearance);
		std::vector<unsigned char> jumpBytes = { 0xff, 0x25, 0x00, 0x00, 0x00, 0x00 };
		MemCopy(address, (uintptr_t)&jumpBytes[0], jumpBytes.size());
		MemCopy((address + 6), (uintptr_t)&destination, sizeof(uintptr_t));
	}

	int32_t CalculateDisplacementForRelativeJump(uintptr_t relativeJumpAddress, uintptr_t destinationAddress)
	{
		const size_t sizeOfE9Jmp = 5;
		return -int32_t(relativeJumpAddress + sizeOfE9Jmp - destinationAddress);
	}

	void MemCopy(uintptr_t destination, uintptr_t source, size_t numBytes)
	{
		ToggleMemoryProtection(false, destination, numBytes);
		ToggleMemoryProtection(false, source, numBytes);
		memcpy((void*)destination, (void*)source, numBytes);
		ToggleMemoryProtection(true, source, numBytes);
		ToggleMemoryProtection(true, destination, numBytes);
	}

	void MemSet(uintptr_t address, unsigned char byte, size_t numBytes)
	{
		ToggleMemoryProtection(false, address, numBytes);
		memset((void*)address, byte, numBytes);
		ToggleMemoryProtection(true, address, numBytes);
	}

	void ToggleMemoryProtection(bool enableProtection, uintptr_t address, size_t size)
	{
		static std::map<uintptr_t, DWORD> protectionHistory;
		if (enableProtection && protectionHistory.find(address) != protectionHistory.end())
		{
			VirtualProtect((void*)address, size, protectionHistory[address], &protectionHistory[address]);
			protectionHistory.erase(address);
		}
		else if (!enableProtection && protectionHistory.find(address) == protectionHistory.end())
		{
			DWORD oldProtection = 0;
			VirtualProtect((void*)address, size, PAGE_EXECUTE_READWRITE, &oldProtection);
			protectionHistory[address] = oldProtection;
		}
	}

	void LogAndThrow(std::string exceptionMessage)
	{
		Log("UniversalProxyDLL > Exception thrown: ", exceptionMessage);
		throw std::runtime_error(exceptionMessage);
	}

	template<typename... Types>
	void Log(Types... args)
	{
		if (muteLogging)
		{
			return;
		}

		if (!logFile.is_open())
		{
			logFile.open("upd_log.txt");
		}

		std::stringstream stream;
		stream << "UniversalProxyDLL > ";

		// Magic to fold variadic arguments prior to C++17
		// https://stackoverflow.com/a/55717030
		// If it doesn't work for you, switch to C++17 and replace the code block with:
		// (stream << ... << args) << std::endl;
		// std::cout << stream.str();
		using do_ = int[];
		do_{ 0, (stream << args, 0)... };
		std::cout << stream.str() << std::endl;

		if (logFile.is_open())
		{
			logFile << stream.str() << std::endl;
			logFile.flush();
		}
	}

	template<typename T>
	std::string NumberToHexString(T number)
	{
		std::stringstream stream;
		stream
			<< std::setfill('0')
			<< std::setw(sizeof(T) * 2)
			<< std::hex
			<< number;
		return stream.str();
	}

	void MuteLogging(bool mute = true)
	{
		muteLogging = mute;
	}

	void OpenDebugTerminal()
	{
		if (AllocConsole())
		{
			freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
		}
	}
}

/*
* Macro hell begins!
* We have to populate our export table with functions from all supported DLLs, otherwise
* the application will immediately fail to launch as the exports are checked.
*/ 

#define GenerateForwardFunction(N) extern "C" const char* Forward##N() { return("F"#N); }
#define GenerateForwardOrdinalFunction(N) extern "C" const char* ForwardOrdinal##N() { return ("FO"#N); }
#define GenerateForwardSharedFunction(N) extern "C" const char* ForwardShared##N() { return ("FS"#N); }

GenerateForwardFunction(0) GenerateForwardFunction(1) GenerateForwardFunction(2) GenerateForwardFunction(3) GenerateForwardFunction(4) GenerateForwardFunction(5) GenerateForwardFunction(6) GenerateForwardFunction(7) GenerateForwardFunction(8) GenerateForwardFunction(9) GenerateForwardFunction(10) GenerateForwardFunction(11) GenerateForwardFunction(12) GenerateForwardFunction(13) GenerateForwardFunction(14) GenerateForwardFunction(15) GenerateForwardFunction(16) GenerateForwardFunction(17) GenerateForwardFunction(18) GenerateForwardFunction(19) GenerateForwardFunction(20) GenerateForwardFunction(21) GenerateForwardFunction(22) GenerateForwardFunction(23) GenerateForwardFunction(24) GenerateForwardFunction(25) GenerateForwardFunction(26) GenerateForwardFunction(27) GenerateForwardFunction(28) GenerateForwardFunction(29) GenerateForwardFunction(30) GenerateForwardFunction(31) GenerateForwardFunction(32) GenerateForwardFunction(33) GenerateForwardFunction(34) GenerateForwardFunction(35) GenerateForwardFunction(36) GenerateForwardFunction(37) GenerateForwardFunction(38) GenerateForwardFunction(39) GenerateForwardFunction(40) GenerateForwardFunction(41) GenerateForwardFunction(42) GenerateForwardFunction(43) GenerateForwardFunction(44) GenerateForwardFunction(45) GenerateForwardFunction(46) GenerateForwardFunction(47) GenerateForwardFunction(48) GenerateForwardFunction(49)
GenerateForwardFunction(50) GenerateForwardFunction(51) GenerateForwardFunction(52) GenerateForwardFunction(53) GenerateForwardFunction(54) GenerateForwardFunction(55) GenerateForwardFunction(56) GenerateForwardFunction(57) GenerateForwardFunction(58) GenerateForwardFunction(59) GenerateForwardFunction(60) GenerateForwardFunction(61) GenerateForwardFunction(62) GenerateForwardFunction(63) GenerateForwardFunction(64) GenerateForwardFunction(65) GenerateForwardFunction(66) GenerateForwardFunction(67) GenerateForwardFunction(68) GenerateForwardFunction(69) GenerateForwardFunction(70) GenerateForwardFunction(71) GenerateForwardFunction(72) GenerateForwardFunction(73) GenerateForwardFunction(74) GenerateForwardFunction(75) GenerateForwardFunction(76) GenerateForwardFunction(77) GenerateForwardFunction(78) GenerateForwardFunction(79) GenerateForwardFunction(80) GenerateForwardFunction(81) GenerateForwardFunction(82) GenerateForwardFunction(83) GenerateForwardFunction(84) GenerateForwardFunction(85) GenerateForwardFunction(86) GenerateForwardFunction(87) GenerateForwardFunction(88) GenerateForwardFunction(89) GenerateForwardFunction(90) GenerateForwardFunction(91) GenerateForwardFunction(92) GenerateForwardFunction(93) GenerateForwardFunction(94) GenerateForwardFunction(95) GenerateForwardFunction(96) GenerateForwardFunction(97) GenerateForwardFunction(98) GenerateForwardFunction(99)
GenerateForwardFunction(100) GenerateForwardFunction(101) GenerateForwardFunction(102) GenerateForwardFunction(103) GenerateForwardFunction(104) GenerateForwardFunction(105) GenerateForwardFunction(106) GenerateForwardFunction(107) GenerateForwardFunction(108) GenerateForwardFunction(109) GenerateForwardFunction(110) GenerateForwardFunction(111) GenerateForwardFunction(112) GenerateForwardFunction(113) GenerateForwardFunction(114) GenerateForwardFunction(115) GenerateForwardFunction(116) GenerateForwardFunction(117) GenerateForwardFunction(118) GenerateForwardFunction(119) GenerateForwardFunction(120) GenerateForwardFunction(121) GenerateForwardFunction(122) GenerateForwardFunction(123) GenerateForwardFunction(124) GenerateForwardFunction(125) GenerateForwardFunction(126) GenerateForwardFunction(127) GenerateForwardFunction(128) GenerateForwardFunction(129) GenerateForwardFunction(130) GenerateForwardFunction(131) GenerateForwardFunction(132) GenerateForwardFunction(133) GenerateForwardFunction(134) GenerateForwardFunction(135) GenerateForwardFunction(136) GenerateForwardFunction(137) GenerateForwardFunction(138) GenerateForwardFunction(139) GenerateForwardFunction(140) GenerateForwardFunction(141) GenerateForwardFunction(142) GenerateForwardFunction(143) GenerateForwardFunction(144) GenerateForwardFunction(145) GenerateForwardFunction(146) GenerateForwardFunction(147) GenerateForwardFunction(148) GenerateForwardFunction(149)
GenerateForwardFunction(150) GenerateForwardFunction(151) GenerateForwardFunction(152) GenerateForwardFunction(153) GenerateForwardFunction(154) GenerateForwardFunction(155) GenerateForwardFunction(156) GenerateForwardFunction(157) GenerateForwardFunction(158) GenerateForwardFunction(159) GenerateForwardFunction(160) GenerateForwardFunction(161) GenerateForwardFunction(162) GenerateForwardFunction(163) GenerateForwardFunction(164) GenerateForwardFunction(165) GenerateForwardFunction(166) GenerateForwardFunction(167) GenerateForwardFunction(168) GenerateForwardFunction(169) GenerateForwardFunction(170) GenerateForwardFunction(171) GenerateForwardFunction(172) GenerateForwardFunction(173) GenerateForwardFunction(174) GenerateForwardFunction(175) GenerateForwardFunction(176) GenerateForwardFunction(177) GenerateForwardFunction(178) GenerateForwardFunction(179) GenerateForwardFunction(180) GenerateForwardFunction(181) GenerateForwardFunction(182) GenerateForwardFunction(183) GenerateForwardFunction(184) GenerateForwardFunction(185) GenerateForwardFunction(186) GenerateForwardFunction(187) GenerateForwardFunction(188) GenerateForwardFunction(189) GenerateForwardFunction(190) GenerateForwardFunction(191) GenerateForwardFunction(192) GenerateForwardFunction(193) GenerateForwardFunction(194) GenerateForwardFunction(195) GenerateForwardFunction(196) GenerateForwardFunction(197) GenerateForwardFunction(198) GenerateForwardFunction(199)
GenerateForwardFunction(200) GenerateForwardFunction(201) GenerateForwardFunction(202) GenerateForwardFunction(203) GenerateForwardFunction(204) GenerateForwardFunction(205) GenerateForwardFunction(206) GenerateForwardFunction(207) GenerateForwardFunction(208) GenerateForwardFunction(209) GenerateForwardFunction(210) GenerateForwardFunction(211) GenerateForwardFunction(212) GenerateForwardFunction(213) GenerateForwardFunction(214) GenerateForwardFunction(215) GenerateForwardFunction(216) GenerateForwardFunction(217) GenerateForwardFunction(218) GenerateForwardFunction(219) GenerateForwardFunction(220) GenerateForwardFunction(221) GenerateForwardFunction(222) GenerateForwardFunction(223) GenerateForwardFunction(224) GenerateForwardFunction(225) GenerateForwardFunction(226) GenerateForwardFunction(227) GenerateForwardFunction(228) GenerateForwardFunction(229) GenerateForwardFunction(230) GenerateForwardFunction(231) GenerateForwardFunction(232) GenerateForwardFunction(233) GenerateForwardFunction(234) GenerateForwardFunction(235) GenerateForwardFunction(236) GenerateForwardFunction(237) GenerateForwardFunction(238) GenerateForwardFunction(239) GenerateForwardFunction(240) GenerateForwardFunction(241) GenerateForwardFunction(242) GenerateForwardFunction(243) GenerateForwardFunction(244) GenerateForwardFunction(245) GenerateForwardFunction(246) GenerateForwardFunction(247) GenerateForwardFunction(248) GenerateForwardFunction(249)
GenerateForwardFunction(250) GenerateForwardFunction(251) GenerateForwardFunction(252) GenerateForwardFunction(253) GenerateForwardFunction(254) GenerateForwardFunction(255) GenerateForwardFunction(256) GenerateForwardFunction(257) GenerateForwardFunction(258) GenerateForwardFunction(259) GenerateForwardFunction(260) GenerateForwardFunction(261) GenerateForwardFunction(262) GenerateForwardFunction(263) GenerateForwardFunction(264) GenerateForwardFunction(265) GenerateForwardFunction(266) GenerateForwardFunction(267) GenerateForwardFunction(268) GenerateForwardFunction(269) GenerateForwardFunction(270) GenerateForwardFunction(271) GenerateForwardFunction(272) GenerateForwardFunction(273) GenerateForwardFunction(274) GenerateForwardFunction(275) GenerateForwardFunction(276) GenerateForwardFunction(277) GenerateForwardFunction(278) GenerateForwardFunction(279) GenerateForwardFunction(280) GenerateForwardFunction(281) GenerateForwardFunction(282) GenerateForwardFunction(283) GenerateForwardFunction(284) GenerateForwardFunction(285) GenerateForwardFunction(286) GenerateForwardFunction(287) GenerateForwardFunction(288) GenerateForwardFunction(289) GenerateForwardFunction(290) GenerateForwardFunction(291) GenerateForwardFunction(292) GenerateForwardFunction(293) GenerateForwardFunction(294) GenerateForwardFunction(295) GenerateForwardFunction(296) GenerateForwardFunction(297) GenerateForwardFunction(298) GenerateForwardFunction(299)
GenerateForwardFunction(300) GenerateForwardFunction(301) GenerateForwardFunction(302) GenerateForwardFunction(303) GenerateForwardFunction(304) GenerateForwardFunction(305) GenerateForwardFunction(306) GenerateForwardFunction(307) GenerateForwardFunction(308) GenerateForwardFunction(309) GenerateForwardFunction(310) GenerateForwardFunction(311) GenerateForwardFunction(312) GenerateForwardFunction(313) GenerateForwardFunction(314) GenerateForwardFunction(315) GenerateForwardFunction(316) GenerateForwardFunction(317) GenerateForwardFunction(318) GenerateForwardFunction(319) GenerateForwardFunction(320) GenerateForwardFunction(321) GenerateForwardFunction(322) GenerateForwardFunction(323) GenerateForwardFunction(324) GenerateForwardFunction(325) GenerateForwardFunction(326) GenerateForwardFunction(327) GenerateForwardFunction(328) GenerateForwardFunction(329) GenerateForwardFunction(330) GenerateForwardFunction(331) GenerateForwardFunction(332) GenerateForwardFunction(333) GenerateForwardFunction(334) GenerateForwardFunction(335) GenerateForwardFunction(336) GenerateForwardFunction(337) GenerateForwardFunction(338) GenerateForwardFunction(339) GenerateForwardFunction(340) GenerateForwardFunction(341) GenerateForwardFunction(342) GenerateForwardFunction(343) GenerateForwardFunction(344) GenerateForwardFunction(345) GenerateForwardFunction(346) GenerateForwardFunction(347) GenerateForwardFunction(348) GenerateForwardFunction(349)
GenerateForwardFunction(350) GenerateForwardFunction(351) GenerateForwardFunction(352) GenerateForwardFunction(353) GenerateForwardFunction(354) GenerateForwardFunction(355) GenerateForwardFunction(356) GenerateForwardFunction(357) GenerateForwardFunction(358) GenerateForwardFunction(359) GenerateForwardFunction(360) GenerateForwardFunction(361) GenerateForwardFunction(362) GenerateForwardFunction(363) GenerateForwardFunction(364) GenerateForwardFunction(365) GenerateForwardFunction(366) GenerateForwardFunction(367) GenerateForwardFunction(368) GenerateForwardFunction(369) GenerateForwardFunction(370) GenerateForwardFunction(371) GenerateForwardFunction(372) GenerateForwardFunction(373) GenerateForwardFunction(374) GenerateForwardFunction(375) GenerateForwardFunction(376) GenerateForwardFunction(377) GenerateForwardFunction(378) GenerateForwardFunction(379) GenerateForwardFunction(380) GenerateForwardFunction(381) GenerateForwardFunction(382) GenerateForwardFunction(383) GenerateForwardFunction(384) GenerateForwardFunction(385) GenerateForwardFunction(386) GenerateForwardFunction(387) GenerateForwardFunction(388) GenerateForwardFunction(389) GenerateForwardFunction(390) GenerateForwardFunction(391) GenerateForwardFunction(392) GenerateForwardFunction(393) GenerateForwardFunction(394) GenerateForwardFunction(395) GenerateForwardFunction(396) GenerateForwardFunction(397) GenerateForwardFunction(398) GenerateForwardFunction(399)
GenerateForwardFunction(400) GenerateForwardFunction(401) GenerateForwardFunction(402) GenerateForwardFunction(403) GenerateForwardFunction(404) GenerateForwardFunction(405) GenerateForwardFunction(406) GenerateForwardFunction(407) GenerateForwardFunction(408) GenerateForwardFunction(409) GenerateForwardFunction(410) GenerateForwardFunction(411) GenerateForwardFunction(412) GenerateForwardFunction(413) GenerateForwardFunction(414) GenerateForwardFunction(415) GenerateForwardFunction(416) GenerateForwardFunction(417) GenerateForwardFunction(418) GenerateForwardFunction(419) GenerateForwardFunction(420) GenerateForwardFunction(421) GenerateForwardFunction(422) GenerateForwardFunction(423) GenerateForwardFunction(424) GenerateForwardFunction(425) GenerateForwardFunction(426) GenerateForwardFunction(427) GenerateForwardFunction(428) GenerateForwardFunction(429) GenerateForwardFunction(430) GenerateForwardFunction(431) GenerateForwardFunction(432) GenerateForwardFunction(433) GenerateForwardFunction(434) GenerateForwardFunction(435) GenerateForwardFunction(436) GenerateForwardFunction(437) GenerateForwardFunction(438) GenerateForwardFunction(439) GenerateForwardFunction(440) GenerateForwardFunction(441) GenerateForwardFunction(442) GenerateForwardFunction(443) GenerateForwardFunction(444) GenerateForwardFunction(445) GenerateForwardFunction(446) GenerateForwardFunction(447) GenerateForwardFunction(448) GenerateForwardFunction(449)
GenerateForwardFunction(450) GenerateForwardFunction(451) GenerateForwardFunction(452) GenerateForwardFunction(453) GenerateForwardFunction(454) GenerateForwardFunction(455) GenerateForwardFunction(456) GenerateForwardFunction(457) GenerateForwardFunction(458) GenerateForwardFunction(459) GenerateForwardFunction(460) GenerateForwardFunction(461) GenerateForwardFunction(462) GenerateForwardFunction(463) GenerateForwardFunction(464) GenerateForwardFunction(465) GenerateForwardFunction(466) GenerateForwardFunction(467) GenerateForwardFunction(468) GenerateForwardFunction(469) GenerateForwardFunction(470) GenerateForwardFunction(471) GenerateForwardFunction(472) GenerateForwardFunction(473) GenerateForwardFunction(474) GenerateForwardFunction(475) GenerateForwardFunction(476) GenerateForwardFunction(477) GenerateForwardFunction(478) GenerateForwardFunction(479) GenerateForwardFunction(480) GenerateForwardFunction(481) GenerateForwardFunction(482) GenerateForwardFunction(483) GenerateForwardFunction(484) GenerateForwardFunction(485) GenerateForwardFunction(486) GenerateForwardFunction(487) GenerateForwardFunction(488) GenerateForwardFunction(489) GenerateForwardFunction(490) GenerateForwardFunction(491) GenerateForwardFunction(492) GenerateForwardFunction(493) GenerateForwardFunction(494) GenerateForwardFunction(495) GenerateForwardFunction(496) GenerateForwardFunction(497) GenerateForwardFunction(498) GenerateForwardFunction(499)
GenerateForwardFunction(500) GenerateForwardFunction(501) GenerateForwardFunction(502) GenerateForwardFunction(503) GenerateForwardFunction(504) GenerateForwardFunction(505) GenerateForwardFunction(506) GenerateForwardFunction(507) GenerateForwardFunction(508) GenerateForwardFunction(509) GenerateForwardFunction(510) GenerateForwardFunction(511) GenerateForwardFunction(512) GenerateForwardFunction(513) GenerateForwardFunction(514) GenerateForwardFunction(515) GenerateForwardFunction(516) GenerateForwardFunction(517) GenerateForwardFunction(518) GenerateForwardFunction(519) GenerateForwardFunction(520) GenerateForwardFunction(521) GenerateForwardFunction(522) GenerateForwardFunction(523) GenerateForwardFunction(524) GenerateForwardFunction(525) GenerateForwardFunction(526) GenerateForwardFunction(527) GenerateForwardFunction(528) GenerateForwardFunction(529) GenerateForwardFunction(530) GenerateForwardFunction(531) GenerateForwardFunction(532) GenerateForwardFunction(533) GenerateForwardFunction(534) GenerateForwardFunction(535) GenerateForwardFunction(536) GenerateForwardFunction(537) GenerateForwardFunction(538) GenerateForwardFunction(539) GenerateForwardFunction(540) GenerateForwardFunction(541) GenerateForwardFunction(542) GenerateForwardFunction(543) GenerateForwardFunction(544) GenerateForwardFunction(545) GenerateForwardFunction(546) GenerateForwardFunction(547) GenerateForwardFunction(548) GenerateForwardFunction(549)
GenerateForwardFunction(550) GenerateForwardFunction(551) GenerateForwardFunction(552) GenerateForwardFunction(553) GenerateForwardFunction(554) GenerateForwardFunction(555) GenerateForwardFunction(556) GenerateForwardFunction(557) GenerateForwardFunction(558) GenerateForwardFunction(559) GenerateForwardFunction(560) GenerateForwardFunction(561) GenerateForwardFunction(562) GenerateForwardFunction(563) GenerateForwardFunction(564) GenerateForwardFunction(565) GenerateForwardFunction(566) GenerateForwardFunction(567) GenerateForwardFunction(568) GenerateForwardFunction(569) GenerateForwardFunction(570) GenerateForwardFunction(571) GenerateForwardFunction(572) GenerateForwardFunction(573) GenerateForwardFunction(574) GenerateForwardFunction(575) GenerateForwardFunction(576) GenerateForwardFunction(577) GenerateForwardFunction(578) GenerateForwardFunction(579) GenerateForwardFunction(580) GenerateForwardFunction(581) GenerateForwardFunction(582) GenerateForwardFunction(583) GenerateForwardFunction(584) GenerateForwardFunction(585) GenerateForwardFunction(586) GenerateForwardFunction(587) GenerateForwardFunction(588) GenerateForwardFunction(589) GenerateForwardFunction(590) GenerateForwardFunction(591) GenerateForwardFunction(592) GenerateForwardFunction(593) GenerateForwardFunction(594) GenerateForwardFunction(595) GenerateForwardFunction(596) GenerateForwardFunction(597) GenerateForwardFunction(598) GenerateForwardFunction(599)
GenerateForwardFunction(600) GenerateForwardFunction(601) GenerateForwardFunction(602) GenerateForwardFunction(603) GenerateForwardFunction(604) GenerateForwardFunction(605) GenerateForwardFunction(606) GenerateForwardFunction(607) GenerateForwardFunction(608) GenerateForwardFunction(609) GenerateForwardFunction(610) GenerateForwardFunction(611) GenerateForwardFunction(612) GenerateForwardFunction(613) GenerateForwardFunction(614) GenerateForwardFunction(615) GenerateForwardFunction(616) GenerateForwardFunction(617) GenerateForwardFunction(618) GenerateForwardFunction(619) GenerateForwardFunction(620) GenerateForwardFunction(621) GenerateForwardFunction(622) GenerateForwardFunction(623) GenerateForwardFunction(624) GenerateForwardFunction(625) GenerateForwardFunction(626) GenerateForwardFunction(627) GenerateForwardFunction(628) GenerateForwardFunction(629) GenerateForwardFunction(630) GenerateForwardFunction(631) GenerateForwardFunction(632) GenerateForwardFunction(633) GenerateForwardFunction(634) GenerateForwardFunction(635) GenerateForwardFunction(636) GenerateForwardFunction(637) GenerateForwardFunction(638) GenerateForwardFunction(639) GenerateForwardFunction(640) GenerateForwardFunction(641) GenerateForwardFunction(642) GenerateForwardFunction(643) GenerateForwardFunction(644) GenerateForwardFunction(645) GenerateForwardFunction(646) GenerateForwardFunction(647) GenerateForwardFunction(648) GenerateForwardFunction(649)
GenerateForwardFunction(650) GenerateForwardFunction(651) GenerateForwardFunction(652) GenerateForwardFunction(653) GenerateForwardFunction(654) GenerateForwardFunction(655) GenerateForwardFunction(656) GenerateForwardFunction(657) GenerateForwardFunction(658) GenerateForwardFunction(659) GenerateForwardFunction(660) GenerateForwardFunction(661) GenerateForwardFunction(662) GenerateForwardFunction(663) GenerateForwardFunction(664) GenerateForwardFunction(665) GenerateForwardFunction(666) GenerateForwardFunction(667) GenerateForwardFunction(668) GenerateForwardFunction(669) GenerateForwardFunction(670) GenerateForwardFunction(671) GenerateForwardFunction(672) GenerateForwardFunction(673) GenerateForwardFunction(674) GenerateForwardFunction(675) GenerateForwardFunction(676) GenerateForwardFunction(677) GenerateForwardFunction(678) GenerateForwardFunction(679) GenerateForwardFunction(680) GenerateForwardFunction(681) GenerateForwardFunction(682) GenerateForwardFunction(683) GenerateForwardFunction(684) GenerateForwardFunction(685) GenerateForwardFunction(686) GenerateForwardFunction(687) GenerateForwardFunction(688) GenerateForwardFunction(689) GenerateForwardFunction(690) GenerateForwardFunction(691) GenerateForwardFunction(692) GenerateForwardFunction(693) GenerateForwardFunction(694) GenerateForwardFunction(695) GenerateForwardFunction(696) GenerateForwardFunction(697) GenerateForwardFunction(698) GenerateForwardFunction(699)
GenerateForwardFunction(700) GenerateForwardFunction(701) GenerateForwardFunction(702) GenerateForwardFunction(703) GenerateForwardFunction(704) GenerateForwardFunction(705) GenerateForwardFunction(706) GenerateForwardFunction(707) GenerateForwardFunction(708) GenerateForwardFunction(709) GenerateForwardFunction(710) GenerateForwardFunction(711) GenerateForwardFunction(712) GenerateForwardFunction(713) GenerateForwardFunction(714) GenerateForwardFunction(715) GenerateForwardFunction(716) GenerateForwardFunction(717) GenerateForwardFunction(718) GenerateForwardFunction(719) GenerateForwardFunction(720) GenerateForwardFunction(721) GenerateForwardFunction(722) GenerateForwardFunction(723) GenerateForwardFunction(724) GenerateForwardFunction(725) GenerateForwardFunction(726) GenerateForwardFunction(727) GenerateForwardFunction(728) GenerateForwardFunction(729) GenerateForwardFunction(730) GenerateForwardFunction(731) GenerateForwardFunction(732) GenerateForwardFunction(733) GenerateForwardFunction(734) GenerateForwardFunction(735) GenerateForwardFunction(736) GenerateForwardFunction(737) GenerateForwardFunction(738) GenerateForwardFunction(739) GenerateForwardFunction(740) GenerateForwardFunction(741) GenerateForwardFunction(742) GenerateForwardFunction(743) GenerateForwardFunction(744) GenerateForwardFunction(745) GenerateForwardFunction(746) GenerateForwardFunction(747) GenerateForwardFunction(748) GenerateForwardFunction(749)
GenerateForwardFunction(750) GenerateForwardFunction(751) GenerateForwardFunction(752) GenerateForwardFunction(753) GenerateForwardFunction(754) GenerateForwardFunction(755) GenerateForwardFunction(756) GenerateForwardFunction(757) GenerateForwardFunction(758) GenerateForwardFunction(759) GenerateForwardFunction(760) GenerateForwardFunction(761) GenerateForwardFunction(762) GenerateForwardFunction(763) GenerateForwardFunction(764) GenerateForwardFunction(765) GenerateForwardFunction(766) GenerateForwardFunction(767) GenerateForwardFunction(768) GenerateForwardFunction(769) GenerateForwardFunction(770) GenerateForwardFunction(771) GenerateForwardFunction(772) GenerateForwardFunction(773) GenerateForwardFunction(774) GenerateForwardFunction(775) GenerateForwardFunction(776) GenerateForwardFunction(777) GenerateForwardFunction(778) GenerateForwardFunction(779) GenerateForwardFunction(780) GenerateForwardFunction(781) GenerateForwardFunction(782) GenerateForwardFunction(783) GenerateForwardFunction(784) GenerateForwardFunction(785) GenerateForwardFunction(786) GenerateForwardFunction(787) GenerateForwardFunction(788) GenerateForwardFunction(789) GenerateForwardFunction(790) GenerateForwardFunction(791) GenerateForwardFunction(792) GenerateForwardFunction(793) GenerateForwardFunction(794) GenerateForwardFunction(795) GenerateForwardFunction(796) GenerateForwardFunction(797) GenerateForwardFunction(798) GenerateForwardFunction(799)
GenerateForwardFunction(800) GenerateForwardFunction(801) GenerateForwardFunction(802) GenerateForwardFunction(803) GenerateForwardFunction(804) GenerateForwardFunction(805) GenerateForwardFunction(806) GenerateForwardFunction(807) GenerateForwardFunction(808) GenerateForwardFunction(809) GenerateForwardFunction(810) GenerateForwardFunction(811) GenerateForwardFunction(812) GenerateForwardFunction(813) GenerateForwardFunction(814) GenerateForwardFunction(815) GenerateForwardFunction(816) GenerateForwardFunction(817) GenerateForwardFunction(818) GenerateForwardFunction(819) GenerateForwardFunction(820) GenerateForwardFunction(821) GenerateForwardFunction(822) GenerateForwardFunction(823) GenerateForwardFunction(824) GenerateForwardFunction(825) GenerateForwardFunction(826) GenerateForwardFunction(827) GenerateForwardFunction(828) GenerateForwardFunction(829) GenerateForwardFunction(830) GenerateForwardFunction(831) GenerateForwardFunction(832) GenerateForwardFunction(833) GenerateForwardFunction(834) GenerateForwardFunction(835) GenerateForwardFunction(836) GenerateForwardFunction(837) GenerateForwardFunction(838) GenerateForwardFunction(839) GenerateForwardFunction(840) GenerateForwardFunction(841) GenerateForwardFunction(842) GenerateForwardFunction(843) GenerateForwardFunction(844) GenerateForwardFunction(845) GenerateForwardFunction(846) GenerateForwardFunction(847) GenerateForwardFunction(848) GenerateForwardFunction(849)
GenerateForwardFunction(850) GenerateForwardFunction(851) GenerateForwardFunction(852) GenerateForwardFunction(853) GenerateForwardFunction(854) GenerateForwardFunction(855) GenerateForwardFunction(856) GenerateForwardFunction(857) GenerateForwardFunction(858) GenerateForwardFunction(859) GenerateForwardFunction(860) GenerateForwardFunction(861) GenerateForwardFunction(862) GenerateForwardFunction(863) GenerateForwardFunction(864) GenerateForwardFunction(865) GenerateForwardFunction(866) GenerateForwardFunction(867) GenerateForwardFunction(868) GenerateForwardFunction(869) GenerateForwardFunction(870) GenerateForwardFunction(871) GenerateForwardFunction(872) GenerateForwardFunction(873) GenerateForwardFunction(874) GenerateForwardFunction(875) GenerateForwardFunction(876) GenerateForwardFunction(877) GenerateForwardFunction(878) GenerateForwardFunction(879) GenerateForwardFunction(880) GenerateForwardFunction(881) GenerateForwardFunction(882) GenerateForwardFunction(883) GenerateForwardFunction(884) GenerateForwardFunction(885) GenerateForwardFunction(886) GenerateForwardFunction(887) GenerateForwardFunction(888) GenerateForwardFunction(889) GenerateForwardFunction(890) GenerateForwardFunction(891) GenerateForwardFunction(892) GenerateForwardFunction(893) GenerateForwardFunction(894) GenerateForwardFunction(895) GenerateForwardFunction(896) GenerateForwardFunction(897) GenerateForwardFunction(898) GenerateForwardFunction(899)
GenerateForwardFunction(900) GenerateForwardFunction(901) GenerateForwardFunction(902) GenerateForwardFunction(903) GenerateForwardFunction(904) GenerateForwardFunction(905) GenerateForwardFunction(906) GenerateForwardFunction(907) GenerateForwardFunction(908) GenerateForwardFunction(909) GenerateForwardFunction(910) GenerateForwardFunction(911) GenerateForwardFunction(912) GenerateForwardFunction(913) GenerateForwardFunction(914) GenerateForwardFunction(915) GenerateForwardFunction(916) GenerateForwardFunction(917) GenerateForwardFunction(918) GenerateForwardFunction(919) GenerateForwardFunction(920) GenerateForwardFunction(921) GenerateForwardFunction(922) GenerateForwardFunction(923) GenerateForwardFunction(924) GenerateForwardFunction(925) GenerateForwardFunction(926) GenerateForwardFunction(927) GenerateForwardFunction(928) GenerateForwardFunction(929) GenerateForwardFunction(930) GenerateForwardFunction(931) GenerateForwardFunction(932) GenerateForwardFunction(933) GenerateForwardFunction(934) GenerateForwardFunction(935) GenerateForwardFunction(936) GenerateForwardFunction(937) GenerateForwardFunction(938) GenerateForwardFunction(939) GenerateForwardFunction(940) GenerateForwardFunction(941) GenerateForwardFunction(942) GenerateForwardFunction(943) GenerateForwardFunction(944) GenerateForwardFunction(945) GenerateForwardFunction(946) GenerateForwardFunction(947) GenerateForwardFunction(948) GenerateForwardFunction(949)
GenerateForwardFunction(950) GenerateForwardFunction(951) GenerateForwardFunction(952) GenerateForwardFunction(953) GenerateForwardFunction(954) GenerateForwardFunction(955) GenerateForwardFunction(956) GenerateForwardFunction(957) GenerateForwardFunction(958) GenerateForwardFunction(959) GenerateForwardFunction(960) GenerateForwardFunction(961) GenerateForwardFunction(962) GenerateForwardFunction(963) GenerateForwardFunction(964) GenerateForwardFunction(965) GenerateForwardFunction(966) GenerateForwardFunction(967) GenerateForwardFunction(968) GenerateForwardFunction(969) GenerateForwardFunction(970) GenerateForwardFunction(971) GenerateForwardFunction(972) GenerateForwardFunction(973) GenerateForwardFunction(974) GenerateForwardFunction(975) GenerateForwardFunction(976) GenerateForwardFunction(977) GenerateForwardFunction(978) GenerateForwardFunction(979) GenerateForwardFunction(980) GenerateForwardFunction(981) GenerateForwardFunction(982) GenerateForwardFunction(983) GenerateForwardFunction(984) GenerateForwardFunction(985) GenerateForwardFunction(986) GenerateForwardFunction(987) GenerateForwardFunction(988) GenerateForwardFunction(989) GenerateForwardFunction(990) GenerateForwardFunction(991) GenerateForwardFunction(992) GenerateForwardFunction(993) GenerateForwardFunction(994) GenerateForwardFunction(995) GenerateForwardFunction(996) GenerateForwardFunction(997) GenerateForwardFunction(998) GenerateForwardFunction(999)
GenerateForwardFunction(1000) GenerateForwardFunction(1001) GenerateForwardFunction(1002) GenerateForwardFunction(1003) GenerateForwardFunction(1004) GenerateForwardFunction(1005) GenerateForwardFunction(1006) GenerateForwardFunction(1007) GenerateForwardFunction(1008) GenerateForwardFunction(1009) GenerateForwardFunction(1010) GenerateForwardFunction(1011) GenerateForwardFunction(1012) GenerateForwardFunction(1013) GenerateForwardFunction(1014) GenerateForwardFunction(1015) GenerateForwardFunction(1016) GenerateForwardFunction(1017) GenerateForwardFunction(1018)
std::vector<void*> forwardAddresses = {
	&Forward0, &Forward1, &Forward2, &Forward3, &Forward4, &Forward5, &Forward6, &Forward7, &Forward8, &Forward9, &Forward10, &Forward11, &Forward12, &Forward13, &Forward14, &Forward15, &Forward16, &Forward17, &Forward18, &Forward19, &Forward20, &Forward21, &Forward22, &Forward23, &Forward24, &Forward25, &Forward26, &Forward27, &Forward28, &Forward29, &Forward30, &Forward31, &Forward32, &Forward33, &Forward34, &Forward35, &Forward36, &Forward37, &Forward38, &Forward39, &Forward40, &Forward41, &Forward42, &Forward43, &Forward44, &Forward45, &Forward46, &Forward47, &Forward48, &Forward49,
	&Forward50, &Forward51, &Forward52, &Forward53, &Forward54, &Forward55, &Forward56, &Forward57, &Forward58, &Forward59, &Forward60, &Forward61, &Forward62, &Forward63, &Forward64, &Forward65, &Forward66, &Forward67, &Forward68, &Forward69, &Forward70, &Forward71, &Forward72, &Forward73, &Forward74, &Forward75, &Forward76, &Forward77, &Forward78, &Forward79, &Forward80, &Forward81, &Forward82, &Forward83, &Forward84, &Forward85, &Forward86, &Forward87, &Forward88, &Forward89, &Forward90, &Forward91, &Forward92, &Forward93, &Forward94, &Forward95, &Forward96, &Forward97, &Forward98, &Forward99,
	&Forward100, &Forward101, &Forward102, &Forward103, &Forward104, &Forward105, &Forward106, &Forward107, &Forward108, &Forward109, &Forward110, &Forward111, &Forward112, &Forward113, &Forward114, &Forward115, &Forward116, &Forward117, &Forward118, &Forward119, &Forward120, &Forward121, &Forward122, &Forward123, &Forward124, &Forward125, &Forward126, &Forward127, &Forward128, &Forward129, &Forward130, &Forward131, &Forward132, &Forward133, &Forward134, &Forward135, &Forward136, &Forward137, &Forward138, &Forward139, &Forward140, &Forward141, &Forward142, &Forward143, &Forward144, &Forward145, &Forward146, &Forward147, &Forward148, &Forward149,
	&Forward150, &Forward151, &Forward152, &Forward153, &Forward154, &Forward155, &Forward156, &Forward157, &Forward158, &Forward159, &Forward160, &Forward161, &Forward162, &Forward163, &Forward164, &Forward165, &Forward166, &Forward167, &Forward168, &Forward169, &Forward170, &Forward171, &Forward172, &Forward173, &Forward174, &Forward175, &Forward176, &Forward177, &Forward178, &Forward179, &Forward180, &Forward181, &Forward182, &Forward183, &Forward184, &Forward185, &Forward186, &Forward187, &Forward188, &Forward189, &Forward190, &Forward191, &Forward192, &Forward193, &Forward194, &Forward195, &Forward196, &Forward197, &Forward198, &Forward199,
	&Forward200, &Forward201, &Forward202, &Forward203, &Forward204, &Forward205, &Forward206, &Forward207, &Forward208, &Forward209, &Forward210, &Forward211, &Forward212, &Forward213, &Forward214, &Forward215, &Forward216, &Forward217, &Forward218, &Forward219, &Forward220, &Forward221, &Forward222, &Forward223, &Forward224, &Forward225, &Forward226, &Forward227, &Forward228, &Forward229, &Forward230, &Forward231, &Forward232, &Forward233, &Forward234, &Forward235, &Forward236, &Forward237, &Forward238, &Forward239, &Forward240, &Forward241, &Forward242, &Forward243, &Forward244, &Forward245, &Forward246, &Forward247, &Forward248, &Forward249,
	&Forward250, &Forward251, &Forward252, &Forward253, &Forward254, &Forward255, &Forward256, &Forward257, &Forward258, &Forward259, &Forward260, &Forward261, &Forward262, &Forward263, &Forward264, &Forward265, &Forward266, &Forward267, &Forward268, &Forward269, &Forward270, &Forward271, &Forward272, &Forward273, &Forward274, &Forward275, &Forward276, &Forward277, &Forward278, &Forward279, &Forward280, &Forward281, &Forward282, &Forward283, &Forward284, &Forward285, &Forward286, &Forward287, &Forward288, &Forward289, &Forward290, &Forward291, &Forward292, &Forward293, &Forward294, &Forward295, &Forward296, &Forward297, &Forward298, &Forward299,
	&Forward300, &Forward301, &Forward302, &Forward303, &Forward304, &Forward305, &Forward306, &Forward307, &Forward308, &Forward309, &Forward310, &Forward311, &Forward312, &Forward313, &Forward314, &Forward315, &Forward316, &Forward317, &Forward318, &Forward319, &Forward320, &Forward321, &Forward322, &Forward323, &Forward324, &Forward325, &Forward326, &Forward327, &Forward328, &Forward329, &Forward330, &Forward331, &Forward332, &Forward333, &Forward334, &Forward335, &Forward336, &Forward337, &Forward338, &Forward339, &Forward340, &Forward341, &Forward342, &Forward343, &Forward344, &Forward345, &Forward346, &Forward347, &Forward348, &Forward349,
	&Forward350, &Forward351, &Forward352, &Forward353, &Forward354, &Forward355, &Forward356, &Forward357, &Forward358, &Forward359, &Forward360, &Forward361, &Forward362, &Forward363, &Forward364, &Forward365, &Forward366, &Forward367, &Forward368, &Forward369, &Forward370, &Forward371, &Forward372, &Forward373, &Forward374, &Forward375, &Forward376, &Forward377, &Forward378, &Forward379, &Forward380, &Forward381, &Forward382, &Forward383, &Forward384, &Forward385, &Forward386, &Forward387, &Forward388, &Forward389, &Forward390, &Forward391, &Forward392, &Forward393, &Forward394, &Forward395, &Forward396, &Forward397, &Forward398, &Forward399,
	&Forward400, &Forward401, &Forward402, &Forward403, &Forward404, &Forward405, &Forward406, &Forward407, &Forward408, &Forward409, &Forward410, &Forward411, &Forward412, &Forward413, &Forward414, &Forward415, &Forward416, &Forward417, &Forward418, &Forward419, &Forward420, &Forward421, &Forward422, &Forward423, &Forward424, &Forward425, &Forward426, &Forward427, &Forward428, &Forward429, &Forward430, &Forward431, &Forward432, &Forward433, &Forward434, &Forward435, &Forward436, &Forward437, &Forward438, &Forward439, &Forward440, &Forward441, &Forward442, &Forward443, &Forward444, &Forward445, &Forward446, &Forward447, &Forward448, &Forward449,
	&Forward450, &Forward451, &Forward452, &Forward453, &Forward454, &Forward455, &Forward456, &Forward457, &Forward458, &Forward459, &Forward460, &Forward461, &Forward462, &Forward463, &Forward464, &Forward465, &Forward466, &Forward467, &Forward468, &Forward469, &Forward470, &Forward471, &Forward472, &Forward473, &Forward474, &Forward475, &Forward476, &Forward477, &Forward478, &Forward479, &Forward480, &Forward481, &Forward482, &Forward483, &Forward484, &Forward485, &Forward486, &Forward487, &Forward488, &Forward489, &Forward490, &Forward491, &Forward492, &Forward493, &Forward494, &Forward495, &Forward496, &Forward497, &Forward498, &Forward499,
	&Forward500, &Forward501, &Forward502, &Forward503, &Forward504, &Forward505, &Forward506, &Forward507, &Forward508, &Forward509, &Forward510, &Forward511, &Forward512, &Forward513, &Forward514, &Forward515, &Forward516, &Forward517, &Forward518, &Forward519, &Forward520, &Forward521, &Forward522, &Forward523, &Forward524, &Forward525, &Forward526, &Forward527, &Forward528, &Forward529, &Forward530, &Forward531, &Forward532, &Forward533, &Forward534, &Forward535, &Forward536, &Forward537, &Forward538, &Forward539, &Forward540, &Forward541, &Forward542, &Forward543, &Forward544, &Forward545, &Forward546, &Forward547, &Forward548, &Forward549,
	&Forward550, &Forward551, &Forward552, &Forward553, &Forward554, &Forward555, &Forward556, &Forward557, &Forward558, &Forward559, &Forward560, &Forward561, &Forward562, &Forward563, &Forward564, &Forward565, &Forward566, &Forward567, &Forward568, &Forward569, &Forward570, &Forward571, &Forward572, &Forward573, &Forward574, &Forward575, &Forward576, &Forward577, &Forward578, &Forward579, &Forward580, &Forward581, &Forward582, &Forward583, &Forward584, &Forward585, &Forward586, &Forward587, &Forward588, &Forward589, &Forward590, &Forward591, &Forward592, &Forward593, &Forward594, &Forward595, &Forward596, &Forward597, &Forward598, &Forward599,
	&Forward600, &Forward601, &Forward602, &Forward603, &Forward604, &Forward605, &Forward606, &Forward607, &Forward608, &Forward609, &Forward610, &Forward611, &Forward612, &Forward613, &Forward614, &Forward615, &Forward616, &Forward617, &Forward618, &Forward619, &Forward620, &Forward621, &Forward622, &Forward623, &Forward624, &Forward625, &Forward626, &Forward627, &Forward628, &Forward629, &Forward630, &Forward631, &Forward632, &Forward633, &Forward634, &Forward635, &Forward636, &Forward637, &Forward638, &Forward639, &Forward640, &Forward641, &Forward642, &Forward643, &Forward644, &Forward645, &Forward646, &Forward647, &Forward648, &Forward649,
	&Forward650, &Forward651, &Forward652, &Forward653, &Forward654, &Forward655, &Forward656, &Forward657, &Forward658, &Forward659, &Forward660, &Forward661, &Forward662, &Forward663, &Forward664, &Forward665, &Forward666, &Forward667, &Forward668, &Forward669, &Forward670, &Forward671, &Forward672, &Forward673, &Forward674, &Forward675, &Forward676, &Forward677, &Forward678, &Forward679, &Forward680, &Forward681, &Forward682, &Forward683, &Forward684, &Forward685, &Forward686, &Forward687, &Forward688, &Forward689, &Forward690, &Forward691, &Forward692, &Forward693, &Forward694, &Forward695, &Forward696, &Forward697, &Forward698, &Forward699,
	&Forward700, &Forward701, &Forward702, &Forward703, &Forward704, &Forward705, &Forward706, &Forward707, &Forward708, &Forward709, &Forward710, &Forward711, &Forward712, &Forward713, &Forward714, &Forward715, &Forward716, &Forward717, &Forward718, &Forward719, &Forward720, &Forward721, &Forward722, &Forward723, &Forward724, &Forward725, &Forward726, &Forward727, &Forward728, &Forward729, &Forward730, &Forward731, &Forward732, &Forward733, &Forward734, &Forward735, &Forward736, &Forward737, &Forward738, &Forward739, &Forward740, &Forward741, &Forward742, &Forward743, &Forward744, &Forward745, &Forward746, &Forward747, &Forward748, &Forward749,
	&Forward750, &Forward751, &Forward752, &Forward753, &Forward754, &Forward755, &Forward756, &Forward757, &Forward758, &Forward759, &Forward760, &Forward761, &Forward762, &Forward763, &Forward764, &Forward765, &Forward766, &Forward767, &Forward768, &Forward769, &Forward770, &Forward771, &Forward772, &Forward773, &Forward774, &Forward775, &Forward776, &Forward777, &Forward778, &Forward779, &Forward780, &Forward781, &Forward782, &Forward783, &Forward784, &Forward785, &Forward786, &Forward787, &Forward788, &Forward789, &Forward790, &Forward791, &Forward792, &Forward793, &Forward794, &Forward795, &Forward796, &Forward797, &Forward798, &Forward799,
	&Forward800, &Forward801, &Forward802, &Forward803, &Forward804, &Forward805, &Forward806, &Forward807, &Forward808, &Forward809, &Forward810, &Forward811, &Forward812, &Forward813, &Forward814, &Forward815, &Forward816, &Forward817, &Forward818, &Forward819, &Forward820, &Forward821, &Forward822, &Forward823, &Forward824, &Forward825, &Forward826, &Forward827, &Forward828, &Forward829, &Forward830, &Forward831, &Forward832, &Forward833, &Forward834, &Forward835, &Forward836, &Forward837, &Forward838, &Forward839, &Forward840, &Forward841, &Forward842, &Forward843, &Forward844, &Forward845, &Forward846, &Forward847, &Forward848, &Forward849,
	&Forward850, &Forward851, &Forward852, &Forward853, &Forward854, &Forward855, &Forward856, &Forward857, &Forward858, &Forward859, &Forward860, &Forward861, &Forward862, &Forward863, &Forward864, &Forward865, &Forward866, &Forward867, &Forward868, &Forward869, &Forward870, &Forward871, &Forward872, &Forward873, &Forward874, &Forward875, &Forward876, &Forward877, &Forward878, &Forward879, &Forward880, &Forward881, &Forward882, &Forward883, &Forward884, &Forward885, &Forward886, &Forward887, &Forward888, &Forward889, &Forward890, &Forward891, &Forward892, &Forward893, &Forward894, &Forward895, &Forward896, &Forward897, &Forward898, &Forward899,
	&Forward900, &Forward901, &Forward902, &Forward903, &Forward904, &Forward905, &Forward906, &Forward907, &Forward908, &Forward909, &Forward910, &Forward911, &Forward912, &Forward913, &Forward914, &Forward915, &Forward916, &Forward917, &Forward918, &Forward919, &Forward920, &Forward921, &Forward922, &Forward923, &Forward924, &Forward925, &Forward926, &Forward927, &Forward928, &Forward929, &Forward930, &Forward931, &Forward932, &Forward933, &Forward934, &Forward935, &Forward936, &Forward937, &Forward938, &Forward939, &Forward940, &Forward941, &Forward942, &Forward943, &Forward944, &Forward945, &Forward946, &Forward947, &Forward948, &Forward949,
	&Forward950, &Forward951, &Forward952, &Forward953, &Forward954, &Forward955, &Forward956, &Forward957, &Forward958, &Forward959, &Forward960, &Forward961, &Forward962, &Forward963, &Forward964, &Forward965, &Forward966, &Forward967, &Forward968, &Forward969, &Forward970, &Forward971, &Forward972, &Forward973, &Forward974, &Forward975, &Forward976, &Forward977, &Forward978, &Forward979, &Forward980, &Forward981, &Forward982, &Forward983, &Forward984, &Forward985, &Forward986, &Forward987, &Forward988, &Forward989, &Forward990, &Forward991, &Forward992, &Forward993, &Forward994, &Forward995, &Forward996, &Forward997, &Forward998, &Forward999,
	&Forward1000, &Forward1001, &Forward1002, &Forward1003, &Forward1004, &Forward1005, &Forward1006, &Forward1007, &Forward1008, &Forward1009, &Forward1010, &Forward1011, &Forward1012, &Forward1013, &Forward1014, &Forward1015, &Forward1016, &Forward1017, &Forward1018
};

GenerateForwardOrdinalFunction(1) GenerateForwardOrdinalFunction(2) GenerateForwardOrdinalFunction(3) GenerateForwardOrdinalFunction(4) GenerateForwardOrdinalFunction(5) GenerateForwardOrdinalFunction(6) GenerateForwardOrdinalFunction(7) GenerateForwardOrdinalFunction(8) GenerateForwardOrdinalFunction(9) GenerateForwardOrdinalFunction(10) GenerateForwardOrdinalFunction(11) GenerateForwardOrdinalFunction(12) GenerateForwardOrdinalFunction(13) GenerateForwardOrdinalFunction(14) GenerateForwardOrdinalFunction(15) GenerateForwardOrdinalFunction(16) GenerateForwardOrdinalFunction(17) GenerateForwardOrdinalFunction(18) GenerateForwardOrdinalFunction(19) GenerateForwardOrdinalFunction(20) GenerateForwardOrdinalFunction(21) GenerateForwardOrdinalFunction(22) GenerateForwardOrdinalFunction(23) GenerateForwardOrdinalFunction(24) GenerateForwardOrdinalFunction(25) GenerateForwardOrdinalFunction(26) GenerateForwardOrdinalFunction(27) GenerateForwardOrdinalFunction(28) GenerateForwardOrdinalFunction(29) GenerateForwardOrdinalFunction(30) GenerateForwardOrdinalFunction(31) GenerateForwardOrdinalFunction(32) GenerateForwardOrdinalFunction(33) GenerateForwardOrdinalFunction(34) GenerateForwardOrdinalFunction(35) GenerateForwardOrdinalFunction(36) GenerateForwardOrdinalFunction(37) GenerateForwardOrdinalFunction(38) GenerateForwardOrdinalFunction(39) GenerateForwardOrdinalFunction(40) GenerateForwardOrdinalFunction(41) GenerateForwardOrdinalFunction(42) GenerateForwardOrdinalFunction(43) GenerateForwardOrdinalFunction(44) GenerateForwardOrdinalFunction(45) GenerateForwardOrdinalFunction(46) GenerateForwardOrdinalFunction(47) GenerateForwardOrdinalFunction(48) GenerateForwardOrdinalFunction(49) GenerateForwardOrdinalFunction(50)
GenerateForwardOrdinalFunction(51) GenerateForwardOrdinalFunction(52) GenerateForwardOrdinalFunction(53) GenerateForwardOrdinalFunction(54) GenerateForwardOrdinalFunction(55) GenerateForwardOrdinalFunction(56) GenerateForwardOrdinalFunction(57) GenerateForwardOrdinalFunction(58) GenerateForwardOrdinalFunction(59) GenerateForwardOrdinalFunction(60) GenerateForwardOrdinalFunction(61) GenerateForwardOrdinalFunction(62) GenerateForwardOrdinalFunction(63) GenerateForwardOrdinalFunction(64) GenerateForwardOrdinalFunction(65) GenerateForwardOrdinalFunction(66) GenerateForwardOrdinalFunction(67) GenerateForwardOrdinalFunction(68) GenerateForwardOrdinalFunction(69) GenerateForwardOrdinalFunction(70) GenerateForwardOrdinalFunction(71) GenerateForwardOrdinalFunction(72) GenerateForwardOrdinalFunction(73) GenerateForwardOrdinalFunction(74) GenerateForwardOrdinalFunction(75) GenerateForwardOrdinalFunction(76) GenerateForwardOrdinalFunction(77) GenerateForwardOrdinalFunction(78) GenerateForwardOrdinalFunction(79) GenerateForwardOrdinalFunction(80) GenerateForwardOrdinalFunction(81) GenerateForwardOrdinalFunction(82) GenerateForwardOrdinalFunction(83) GenerateForwardOrdinalFunction(84) GenerateForwardOrdinalFunction(85) GenerateForwardOrdinalFunction(86) GenerateForwardOrdinalFunction(87) GenerateForwardOrdinalFunction(88) GenerateForwardOrdinalFunction(89) GenerateForwardOrdinalFunction(90) GenerateForwardOrdinalFunction(91) GenerateForwardOrdinalFunction(92) GenerateForwardOrdinalFunction(93) GenerateForwardOrdinalFunction(94) GenerateForwardOrdinalFunction(95) GenerateForwardOrdinalFunction(96) GenerateForwardOrdinalFunction(97) GenerateForwardOrdinalFunction(98) GenerateForwardOrdinalFunction(99) GenerateForwardOrdinalFunction(100)
GenerateForwardOrdinalFunction(101) GenerateForwardOrdinalFunction(102) GenerateForwardOrdinalFunction(103) GenerateForwardOrdinalFunction(104) GenerateForwardOrdinalFunction(105) GenerateForwardOrdinalFunction(106) GenerateForwardOrdinalFunction(107) GenerateForwardOrdinalFunction(108) GenerateForwardOrdinalFunction(109) GenerateForwardOrdinalFunction(110) GenerateForwardOrdinalFunction(111) GenerateForwardOrdinalFunction(112) GenerateForwardOrdinalFunction(113) GenerateForwardOrdinalFunction(114) GenerateForwardOrdinalFunction(115) GenerateForwardOrdinalFunction(116) GenerateForwardOrdinalFunction(117) GenerateForwardOrdinalFunction(118) GenerateForwardOrdinalFunction(119) GenerateForwardOrdinalFunction(120) GenerateForwardOrdinalFunction(121) GenerateForwardOrdinalFunction(122) GenerateForwardOrdinalFunction(123) GenerateForwardOrdinalFunction(124) GenerateForwardOrdinalFunction(125) GenerateForwardOrdinalFunction(126) GenerateForwardOrdinalFunction(127) GenerateForwardOrdinalFunction(128) GenerateForwardOrdinalFunction(129) GenerateForwardOrdinalFunction(130) GenerateForwardOrdinalFunction(131) GenerateForwardOrdinalFunction(132) GenerateForwardOrdinalFunction(133) GenerateForwardOrdinalFunction(134) GenerateForwardOrdinalFunction(135) GenerateForwardOrdinalFunction(136) GenerateForwardOrdinalFunction(137) GenerateForwardOrdinalFunction(138) GenerateForwardOrdinalFunction(139) GenerateForwardOrdinalFunction(140) GenerateForwardOrdinalFunction(141) GenerateForwardOrdinalFunction(142) GenerateForwardOrdinalFunction(143) GenerateForwardOrdinalFunction(144) GenerateForwardOrdinalFunction(145) GenerateForwardOrdinalFunction(146) GenerateForwardOrdinalFunction(147) GenerateForwardOrdinalFunction(148) GenerateForwardOrdinalFunction(149) GenerateForwardOrdinalFunction(150)
GenerateForwardOrdinalFunction(151) GenerateForwardOrdinalFunction(152) GenerateForwardOrdinalFunction(153) GenerateForwardOrdinalFunction(154) GenerateForwardOrdinalFunction(155) GenerateForwardOrdinalFunction(156) GenerateForwardOrdinalFunction(157) GenerateForwardOrdinalFunction(158) GenerateForwardOrdinalFunction(159) GenerateForwardOrdinalFunction(160) GenerateForwardOrdinalFunction(161) GenerateForwardOrdinalFunction(162) GenerateForwardOrdinalFunction(163) GenerateForwardOrdinalFunction(164) GenerateForwardOrdinalFunction(165) GenerateForwardOrdinalFunction(166) GenerateForwardOrdinalFunction(167) GenerateForwardOrdinalFunction(168) GenerateForwardOrdinalFunction(169) GenerateForwardOrdinalFunction(170) GenerateForwardOrdinalFunction(171) GenerateForwardOrdinalFunction(172) GenerateForwardOrdinalFunction(173) GenerateForwardOrdinalFunction(174) GenerateForwardOrdinalFunction(175) GenerateForwardOrdinalFunction(176) GenerateForwardOrdinalFunction(177) GenerateForwardOrdinalFunction(178) GenerateForwardOrdinalFunction(179) GenerateForwardOrdinalFunction(180) GenerateForwardOrdinalFunction(181) GenerateForwardOrdinalFunction(182) GenerateForwardOrdinalFunction(183) GenerateForwardOrdinalFunction(184) GenerateForwardOrdinalFunction(185) GenerateForwardOrdinalFunction(186) GenerateForwardOrdinalFunction(187) GenerateForwardOrdinalFunction(188) GenerateForwardOrdinalFunction(189) GenerateForwardOrdinalFunction(190) GenerateForwardOrdinalFunction(191) GenerateForwardOrdinalFunction(192) GenerateForwardOrdinalFunction(193) GenerateForwardOrdinalFunction(194) GenerateForwardOrdinalFunction(195) GenerateForwardOrdinalFunction(196) GenerateForwardOrdinalFunction(197) GenerateForwardOrdinalFunction(198) GenerateForwardOrdinalFunction(199) GenerateForwardOrdinalFunction(200)
GenerateForwardOrdinalFunction(201) GenerateForwardOrdinalFunction(202) GenerateForwardOrdinalFunction(203) GenerateForwardOrdinalFunction(204) GenerateForwardOrdinalFunction(205) GenerateForwardOrdinalFunction(206) GenerateForwardOrdinalFunction(207) GenerateForwardOrdinalFunction(208) GenerateForwardOrdinalFunction(209) GenerateForwardOrdinalFunction(210) GenerateForwardOrdinalFunction(211) GenerateForwardOrdinalFunction(212) GenerateForwardOrdinalFunction(213) GenerateForwardOrdinalFunction(214) GenerateForwardOrdinalFunction(215) GenerateForwardOrdinalFunction(216) GenerateForwardOrdinalFunction(217) GenerateForwardOrdinalFunction(218) GenerateForwardOrdinalFunction(219) GenerateForwardOrdinalFunction(220) GenerateForwardOrdinalFunction(221) GenerateForwardOrdinalFunction(222) GenerateForwardOrdinalFunction(223) GenerateForwardOrdinalFunction(224) GenerateForwardOrdinalFunction(225) GenerateForwardOrdinalFunction(226) GenerateForwardOrdinalFunction(227) GenerateForwardOrdinalFunction(228) GenerateForwardOrdinalFunction(229) GenerateForwardOrdinalFunction(230) GenerateForwardOrdinalFunction(231) GenerateForwardOrdinalFunction(232) GenerateForwardOrdinalFunction(233) GenerateForwardOrdinalFunction(234) GenerateForwardOrdinalFunction(235) GenerateForwardOrdinalFunction(236) GenerateForwardOrdinalFunction(237) GenerateForwardOrdinalFunction(238) GenerateForwardOrdinalFunction(239) GenerateForwardOrdinalFunction(240) GenerateForwardOrdinalFunction(241) GenerateForwardOrdinalFunction(242) GenerateForwardOrdinalFunction(243) GenerateForwardOrdinalFunction(244) GenerateForwardOrdinalFunction(245) GenerateForwardOrdinalFunction(246) GenerateForwardOrdinalFunction(247) GenerateForwardOrdinalFunction(248) GenerateForwardOrdinalFunction(249) GenerateForwardOrdinalFunction(250)
GenerateForwardOrdinalFunction(251) GenerateForwardOrdinalFunction(252) GenerateForwardOrdinalFunction(253) GenerateForwardOrdinalFunction(254) GenerateForwardOrdinalFunction(255) GenerateForwardOrdinalFunction(256) GenerateForwardOrdinalFunction(257) GenerateForwardOrdinalFunction(258) GenerateForwardOrdinalFunction(259) GenerateForwardOrdinalFunction(260) GenerateForwardOrdinalFunction(261) GenerateForwardOrdinalFunction(262) GenerateForwardOrdinalFunction(263) GenerateForwardOrdinalFunction(264) GenerateForwardOrdinalFunction(265) GenerateForwardOrdinalFunction(266) GenerateForwardOrdinalFunction(267) GenerateForwardOrdinalFunction(268) GenerateForwardOrdinalFunction(269) GenerateForwardOrdinalFunction(270) GenerateForwardOrdinalFunction(271) GenerateForwardOrdinalFunction(272) GenerateForwardOrdinalFunction(273) GenerateForwardOrdinalFunction(274) GenerateForwardOrdinalFunction(275) GenerateForwardOrdinalFunction(276) GenerateForwardOrdinalFunction(277) GenerateForwardOrdinalFunction(278) GenerateForwardOrdinalFunction(279) GenerateForwardOrdinalFunction(280) GenerateForwardOrdinalFunction(281) GenerateForwardOrdinalFunction(282) GenerateForwardOrdinalFunction(283) GenerateForwardOrdinalFunction(284) GenerateForwardOrdinalFunction(285) GenerateForwardOrdinalFunction(286) GenerateForwardOrdinalFunction(287) GenerateForwardOrdinalFunction(288) GenerateForwardOrdinalFunction(289) GenerateForwardOrdinalFunction(290) GenerateForwardOrdinalFunction(291) GenerateForwardOrdinalFunction(292) GenerateForwardOrdinalFunction(293) GenerateForwardOrdinalFunction(294) GenerateForwardOrdinalFunction(295) GenerateForwardOrdinalFunction(296) GenerateForwardOrdinalFunction(297) GenerateForwardOrdinalFunction(298) GenerateForwardOrdinalFunction(299) GenerateForwardOrdinalFunction(300)
GenerateForwardOrdinalFunction(301) GenerateForwardOrdinalFunction(302) GenerateForwardOrdinalFunction(303) GenerateForwardOrdinalFunction(304) GenerateForwardOrdinalFunction(305) GenerateForwardOrdinalFunction(306) GenerateForwardOrdinalFunction(307) GenerateForwardOrdinalFunction(308) GenerateForwardOrdinalFunction(309) GenerateForwardOrdinalFunction(310) GenerateForwardOrdinalFunction(311) GenerateForwardOrdinalFunction(312) GenerateForwardOrdinalFunction(313) GenerateForwardOrdinalFunction(314) GenerateForwardOrdinalFunction(315) GenerateForwardOrdinalFunction(316) GenerateForwardOrdinalFunction(317) GenerateForwardOrdinalFunction(318) GenerateForwardOrdinalFunction(319) GenerateForwardOrdinalFunction(320) GenerateForwardOrdinalFunction(321) GenerateForwardOrdinalFunction(322) GenerateForwardOrdinalFunction(323) GenerateForwardOrdinalFunction(324) GenerateForwardOrdinalFunction(325) GenerateForwardOrdinalFunction(326) GenerateForwardOrdinalFunction(327) GenerateForwardOrdinalFunction(328) GenerateForwardOrdinalFunction(329) GenerateForwardOrdinalFunction(330) GenerateForwardOrdinalFunction(331) GenerateForwardOrdinalFunction(332) GenerateForwardOrdinalFunction(333) GenerateForwardOrdinalFunction(334) GenerateForwardOrdinalFunction(335) GenerateForwardOrdinalFunction(336) GenerateForwardOrdinalFunction(337) GenerateForwardOrdinalFunction(338) GenerateForwardOrdinalFunction(339) GenerateForwardOrdinalFunction(340) GenerateForwardOrdinalFunction(341) GenerateForwardOrdinalFunction(342) GenerateForwardOrdinalFunction(343) GenerateForwardOrdinalFunction(344) GenerateForwardOrdinalFunction(345) GenerateForwardOrdinalFunction(346) GenerateForwardOrdinalFunction(347) GenerateForwardOrdinalFunction(348) GenerateForwardOrdinalFunction(349) GenerateForwardOrdinalFunction(350)
GenerateForwardOrdinalFunction(351) GenerateForwardOrdinalFunction(352) GenerateForwardOrdinalFunction(353) GenerateForwardOrdinalFunction(354) GenerateForwardOrdinalFunction(355) GenerateForwardOrdinalFunction(356) GenerateForwardOrdinalFunction(357) GenerateForwardOrdinalFunction(358) GenerateForwardOrdinalFunction(359) GenerateForwardOrdinalFunction(360) GenerateForwardOrdinalFunction(361) GenerateForwardOrdinalFunction(362) GenerateForwardOrdinalFunction(363) GenerateForwardOrdinalFunction(364) GenerateForwardOrdinalFunction(365) GenerateForwardOrdinalFunction(366) GenerateForwardOrdinalFunction(367) GenerateForwardOrdinalFunction(368) GenerateForwardOrdinalFunction(369) GenerateForwardOrdinalFunction(370) GenerateForwardOrdinalFunction(371) GenerateForwardOrdinalFunction(372) GenerateForwardOrdinalFunction(373) GenerateForwardOrdinalFunction(374) GenerateForwardOrdinalFunction(375) GenerateForwardOrdinalFunction(376) GenerateForwardOrdinalFunction(377) GenerateForwardOrdinalFunction(378) GenerateForwardOrdinalFunction(379) GenerateForwardOrdinalFunction(380) GenerateForwardOrdinalFunction(381) GenerateForwardOrdinalFunction(382) GenerateForwardOrdinalFunction(383) GenerateForwardOrdinalFunction(384) GenerateForwardOrdinalFunction(385) GenerateForwardOrdinalFunction(386) GenerateForwardOrdinalFunction(387) GenerateForwardOrdinalFunction(388) GenerateForwardOrdinalFunction(389) GenerateForwardOrdinalFunction(390) GenerateForwardOrdinalFunction(391) GenerateForwardOrdinalFunction(392) GenerateForwardOrdinalFunction(393) GenerateForwardOrdinalFunction(394) GenerateForwardOrdinalFunction(395) GenerateForwardOrdinalFunction(396) GenerateForwardOrdinalFunction(397) GenerateForwardOrdinalFunction(398) GenerateForwardOrdinalFunction(399) GenerateForwardOrdinalFunction(400)
GenerateForwardOrdinalFunction(401) GenerateForwardOrdinalFunction(402) GenerateForwardOrdinalFunction(403) GenerateForwardOrdinalFunction(404) GenerateForwardOrdinalFunction(405) GenerateForwardOrdinalFunction(406) GenerateForwardOrdinalFunction(407) GenerateForwardOrdinalFunction(408) GenerateForwardOrdinalFunction(409) GenerateForwardOrdinalFunction(410) GenerateForwardOrdinalFunction(411) GenerateForwardOrdinalFunction(412) GenerateForwardOrdinalFunction(413) GenerateForwardOrdinalFunction(414) GenerateForwardOrdinalFunction(415) GenerateForwardOrdinalFunction(416) GenerateForwardOrdinalFunction(417) GenerateForwardOrdinalFunction(418) GenerateForwardOrdinalFunction(419) GenerateForwardOrdinalFunction(420) GenerateForwardOrdinalFunction(421) GenerateForwardOrdinalFunction(422) GenerateForwardOrdinalFunction(423) GenerateForwardOrdinalFunction(424) GenerateForwardOrdinalFunction(425) GenerateForwardOrdinalFunction(426) GenerateForwardOrdinalFunction(427) GenerateForwardOrdinalFunction(428) GenerateForwardOrdinalFunction(429) GenerateForwardOrdinalFunction(430) GenerateForwardOrdinalFunction(431) GenerateForwardOrdinalFunction(432) GenerateForwardOrdinalFunction(433) GenerateForwardOrdinalFunction(434) GenerateForwardOrdinalFunction(435) GenerateForwardOrdinalFunction(436) GenerateForwardOrdinalFunction(437) GenerateForwardOrdinalFunction(438) GenerateForwardOrdinalFunction(439) GenerateForwardOrdinalFunction(440) GenerateForwardOrdinalFunction(441) GenerateForwardOrdinalFunction(442) GenerateForwardOrdinalFunction(443) GenerateForwardOrdinalFunction(444) GenerateForwardOrdinalFunction(445) GenerateForwardOrdinalFunction(446) GenerateForwardOrdinalFunction(447) GenerateForwardOrdinalFunction(448) GenerateForwardOrdinalFunction(449) GenerateForwardOrdinalFunction(450)
GenerateForwardOrdinalFunction(451) GenerateForwardOrdinalFunction(452) GenerateForwardOrdinalFunction(453) GenerateForwardOrdinalFunction(454) GenerateForwardOrdinalFunction(455) GenerateForwardOrdinalFunction(456) GenerateForwardOrdinalFunction(457) GenerateForwardOrdinalFunction(458) GenerateForwardOrdinalFunction(459) GenerateForwardOrdinalFunction(460) GenerateForwardOrdinalFunction(461) GenerateForwardOrdinalFunction(462) GenerateForwardOrdinalFunction(463) GenerateForwardOrdinalFunction(464) GenerateForwardOrdinalFunction(465) GenerateForwardOrdinalFunction(466) GenerateForwardOrdinalFunction(467) GenerateForwardOrdinalFunction(468) GenerateForwardOrdinalFunction(469) GenerateForwardOrdinalFunction(470) GenerateForwardOrdinalFunction(471) GenerateForwardOrdinalFunction(472) GenerateForwardOrdinalFunction(473) GenerateForwardOrdinalFunction(474) GenerateForwardOrdinalFunction(475) GenerateForwardOrdinalFunction(476) GenerateForwardOrdinalFunction(477) GenerateForwardOrdinalFunction(478) GenerateForwardOrdinalFunction(479) GenerateForwardOrdinalFunction(480) GenerateForwardOrdinalFunction(481) GenerateForwardOrdinalFunction(482) GenerateForwardOrdinalFunction(483) GenerateForwardOrdinalFunction(484) GenerateForwardOrdinalFunction(485) GenerateForwardOrdinalFunction(486) GenerateForwardOrdinalFunction(487) GenerateForwardOrdinalFunction(488) GenerateForwardOrdinalFunction(489) GenerateForwardOrdinalFunction(490) GenerateForwardOrdinalFunction(491) GenerateForwardOrdinalFunction(492) GenerateForwardOrdinalFunction(493) GenerateForwardOrdinalFunction(494) GenerateForwardOrdinalFunction(495) GenerateForwardOrdinalFunction(496) GenerateForwardOrdinalFunction(497) GenerateForwardOrdinalFunction(498) GenerateForwardOrdinalFunction(499) GenerateForwardOrdinalFunction(500)
GenerateForwardOrdinalFunction(501) GenerateForwardOrdinalFunction(502) GenerateForwardOrdinalFunction(503) GenerateForwardOrdinalFunction(504) GenerateForwardOrdinalFunction(505) GenerateForwardOrdinalFunction(506) GenerateForwardOrdinalFunction(507) GenerateForwardOrdinalFunction(508) GenerateForwardOrdinalFunction(509) GenerateForwardOrdinalFunction(510) GenerateForwardOrdinalFunction(511) GenerateForwardOrdinalFunction(512) GenerateForwardOrdinalFunction(513) GenerateForwardOrdinalFunction(514) GenerateForwardOrdinalFunction(515) GenerateForwardOrdinalFunction(516) GenerateForwardOrdinalFunction(517) GenerateForwardOrdinalFunction(518) GenerateForwardOrdinalFunction(519) GenerateForwardOrdinalFunction(520) GenerateForwardOrdinalFunction(521) GenerateForwardOrdinalFunction(522) GenerateForwardOrdinalFunction(523) GenerateForwardOrdinalFunction(524) GenerateForwardOrdinalFunction(525) GenerateForwardOrdinalFunction(526) GenerateForwardOrdinalFunction(527) GenerateForwardOrdinalFunction(528) GenerateForwardOrdinalFunction(529) GenerateForwardOrdinalFunction(530) GenerateForwardOrdinalFunction(531) GenerateForwardOrdinalFunction(532) GenerateForwardOrdinalFunction(533) GenerateForwardOrdinalFunction(534) GenerateForwardOrdinalFunction(535) GenerateForwardOrdinalFunction(536) GenerateForwardOrdinalFunction(537) GenerateForwardOrdinalFunction(538) GenerateForwardOrdinalFunction(539) GenerateForwardOrdinalFunction(540) GenerateForwardOrdinalFunction(541) GenerateForwardOrdinalFunction(542) GenerateForwardOrdinalFunction(543) GenerateForwardOrdinalFunction(544) GenerateForwardOrdinalFunction(545) GenerateForwardOrdinalFunction(546) GenerateForwardOrdinalFunction(547) GenerateForwardOrdinalFunction(548) GenerateForwardOrdinalFunction(549) GenerateForwardOrdinalFunction(550)
GenerateForwardOrdinalFunction(551) GenerateForwardOrdinalFunction(552) GenerateForwardOrdinalFunction(553) GenerateForwardOrdinalFunction(554) GenerateForwardOrdinalFunction(555) GenerateForwardOrdinalFunction(556) GenerateForwardOrdinalFunction(557) GenerateForwardOrdinalFunction(558) GenerateForwardOrdinalFunction(559) GenerateForwardOrdinalFunction(560) GenerateForwardOrdinalFunction(561) GenerateForwardOrdinalFunction(562) GenerateForwardOrdinalFunction(563) GenerateForwardOrdinalFunction(564) GenerateForwardOrdinalFunction(565) GenerateForwardOrdinalFunction(566) GenerateForwardOrdinalFunction(567) GenerateForwardOrdinalFunction(568) GenerateForwardOrdinalFunction(569) GenerateForwardOrdinalFunction(570) GenerateForwardOrdinalFunction(571) GenerateForwardOrdinalFunction(572) GenerateForwardOrdinalFunction(573) GenerateForwardOrdinalFunction(574) GenerateForwardOrdinalFunction(575) GenerateForwardOrdinalFunction(576) GenerateForwardOrdinalFunction(577) GenerateForwardOrdinalFunction(578) GenerateForwardOrdinalFunction(579) GenerateForwardOrdinalFunction(580) GenerateForwardOrdinalFunction(581) GenerateForwardOrdinalFunction(582) GenerateForwardOrdinalFunction(583) GenerateForwardOrdinalFunction(584) GenerateForwardOrdinalFunction(585) GenerateForwardOrdinalFunction(586) GenerateForwardOrdinalFunction(587) GenerateForwardOrdinalFunction(588) GenerateForwardOrdinalFunction(589) GenerateForwardOrdinalFunction(590) GenerateForwardOrdinalFunction(591) GenerateForwardOrdinalFunction(592) GenerateForwardOrdinalFunction(593) GenerateForwardOrdinalFunction(594) GenerateForwardOrdinalFunction(595) GenerateForwardOrdinalFunction(596) GenerateForwardOrdinalFunction(597) GenerateForwardOrdinalFunction(598) GenerateForwardOrdinalFunction(599) GenerateForwardOrdinalFunction(600)
GenerateForwardOrdinalFunction(601) GenerateForwardOrdinalFunction(602) GenerateForwardOrdinalFunction(603) GenerateForwardOrdinalFunction(604) GenerateForwardOrdinalFunction(605) GenerateForwardOrdinalFunction(606) GenerateForwardOrdinalFunction(607) GenerateForwardOrdinalFunction(608) GenerateForwardOrdinalFunction(609) GenerateForwardOrdinalFunction(610) GenerateForwardOrdinalFunction(611) GenerateForwardOrdinalFunction(612) GenerateForwardOrdinalFunction(613) GenerateForwardOrdinalFunction(614) GenerateForwardOrdinalFunction(615) GenerateForwardOrdinalFunction(616) GenerateForwardOrdinalFunction(617) GenerateForwardOrdinalFunction(618) GenerateForwardOrdinalFunction(619) GenerateForwardOrdinalFunction(620) GenerateForwardOrdinalFunction(621) GenerateForwardOrdinalFunction(622) GenerateForwardOrdinalFunction(623) GenerateForwardOrdinalFunction(624) GenerateForwardOrdinalFunction(625) GenerateForwardOrdinalFunction(626) GenerateForwardOrdinalFunction(627) GenerateForwardOrdinalFunction(628) GenerateForwardOrdinalFunction(629) GenerateForwardOrdinalFunction(630) GenerateForwardOrdinalFunction(631) GenerateForwardOrdinalFunction(632) GenerateForwardOrdinalFunction(633) GenerateForwardOrdinalFunction(634) GenerateForwardOrdinalFunction(635) GenerateForwardOrdinalFunction(636) GenerateForwardOrdinalFunction(637) GenerateForwardOrdinalFunction(638) GenerateForwardOrdinalFunction(639) GenerateForwardOrdinalFunction(640) GenerateForwardOrdinalFunction(641) GenerateForwardOrdinalFunction(642) GenerateForwardOrdinalFunction(643) GenerateForwardOrdinalFunction(644) GenerateForwardOrdinalFunction(645) GenerateForwardOrdinalFunction(646) GenerateForwardOrdinalFunction(647) GenerateForwardOrdinalFunction(648) GenerateForwardOrdinalFunction(649) GenerateForwardOrdinalFunction(650)
GenerateForwardOrdinalFunction(651) GenerateForwardOrdinalFunction(652) GenerateForwardOrdinalFunction(653) GenerateForwardOrdinalFunction(654) GenerateForwardOrdinalFunction(655) GenerateForwardOrdinalFunction(656) GenerateForwardOrdinalFunction(657) GenerateForwardOrdinalFunction(658) GenerateForwardOrdinalFunction(659) GenerateForwardOrdinalFunction(660) GenerateForwardOrdinalFunction(661) GenerateForwardOrdinalFunction(662) GenerateForwardOrdinalFunction(663) GenerateForwardOrdinalFunction(664) GenerateForwardOrdinalFunction(665) GenerateForwardOrdinalFunction(666) GenerateForwardOrdinalFunction(667) GenerateForwardOrdinalFunction(668) GenerateForwardOrdinalFunction(669) GenerateForwardOrdinalFunction(670) GenerateForwardOrdinalFunction(671) GenerateForwardOrdinalFunction(672) GenerateForwardOrdinalFunction(673) GenerateForwardOrdinalFunction(674) GenerateForwardOrdinalFunction(675) GenerateForwardOrdinalFunction(676) GenerateForwardOrdinalFunction(677) GenerateForwardOrdinalFunction(678) GenerateForwardOrdinalFunction(679) GenerateForwardOrdinalFunction(680) GenerateForwardOrdinalFunction(681) GenerateForwardOrdinalFunction(682) GenerateForwardOrdinalFunction(683) GenerateForwardOrdinalFunction(684) GenerateForwardOrdinalFunction(685) GenerateForwardOrdinalFunction(686) GenerateForwardOrdinalFunction(687) GenerateForwardOrdinalFunction(688) GenerateForwardOrdinalFunction(689) GenerateForwardOrdinalFunction(690) GenerateForwardOrdinalFunction(691) GenerateForwardOrdinalFunction(692) GenerateForwardOrdinalFunction(693) GenerateForwardOrdinalFunction(694) GenerateForwardOrdinalFunction(695) GenerateForwardOrdinalFunction(696) GenerateForwardOrdinalFunction(697) GenerateForwardOrdinalFunction(698) GenerateForwardOrdinalFunction(699) GenerateForwardOrdinalFunction(700)
GenerateForwardOrdinalFunction(701) GenerateForwardOrdinalFunction(702) GenerateForwardOrdinalFunction(703) GenerateForwardOrdinalFunction(704) GenerateForwardOrdinalFunction(705) GenerateForwardOrdinalFunction(706) GenerateForwardOrdinalFunction(707) GenerateForwardOrdinalFunction(708) GenerateForwardOrdinalFunction(709) GenerateForwardOrdinalFunction(710) GenerateForwardOrdinalFunction(711) GenerateForwardOrdinalFunction(712) GenerateForwardOrdinalFunction(713) GenerateForwardOrdinalFunction(714) GenerateForwardOrdinalFunction(715) GenerateForwardOrdinalFunction(716) GenerateForwardOrdinalFunction(717) GenerateForwardOrdinalFunction(718) GenerateForwardOrdinalFunction(719) GenerateForwardOrdinalFunction(720) GenerateForwardOrdinalFunction(721) GenerateForwardOrdinalFunction(722) GenerateForwardOrdinalFunction(723) GenerateForwardOrdinalFunction(724) GenerateForwardOrdinalFunction(725) GenerateForwardOrdinalFunction(726) GenerateForwardOrdinalFunction(727) GenerateForwardOrdinalFunction(728) GenerateForwardOrdinalFunction(729) GenerateForwardOrdinalFunction(730) GenerateForwardOrdinalFunction(731) GenerateForwardOrdinalFunction(732) GenerateForwardOrdinalFunction(733) GenerateForwardOrdinalFunction(734) GenerateForwardOrdinalFunction(735) GenerateForwardOrdinalFunction(736) GenerateForwardOrdinalFunction(737) GenerateForwardOrdinalFunction(738) GenerateForwardOrdinalFunction(739) GenerateForwardOrdinalFunction(740) GenerateForwardOrdinalFunction(741) GenerateForwardOrdinalFunction(742) GenerateForwardOrdinalFunction(743) GenerateForwardOrdinalFunction(744) GenerateForwardOrdinalFunction(745) GenerateForwardOrdinalFunction(746) GenerateForwardOrdinalFunction(747) GenerateForwardOrdinalFunction(748) GenerateForwardOrdinalFunction(749) GenerateForwardOrdinalFunction(750)
GenerateForwardOrdinalFunction(751) GenerateForwardOrdinalFunction(752) GenerateForwardOrdinalFunction(753) GenerateForwardOrdinalFunction(754) GenerateForwardOrdinalFunction(755) GenerateForwardOrdinalFunction(756) GenerateForwardOrdinalFunction(757) GenerateForwardOrdinalFunction(758) GenerateForwardOrdinalFunction(759) GenerateForwardOrdinalFunction(760) GenerateForwardOrdinalFunction(761) GenerateForwardOrdinalFunction(762) GenerateForwardOrdinalFunction(763) GenerateForwardOrdinalFunction(764) GenerateForwardOrdinalFunction(765) GenerateForwardOrdinalFunction(766) GenerateForwardOrdinalFunction(767) GenerateForwardOrdinalFunction(768) GenerateForwardOrdinalFunction(769) GenerateForwardOrdinalFunction(770) GenerateForwardOrdinalFunction(771) GenerateForwardOrdinalFunction(772) GenerateForwardOrdinalFunction(773) GenerateForwardOrdinalFunction(774) GenerateForwardOrdinalFunction(775) GenerateForwardOrdinalFunction(776) GenerateForwardOrdinalFunction(777) GenerateForwardOrdinalFunction(778) GenerateForwardOrdinalFunction(779) GenerateForwardOrdinalFunction(780) GenerateForwardOrdinalFunction(781) GenerateForwardOrdinalFunction(782) GenerateForwardOrdinalFunction(783) GenerateForwardOrdinalFunction(784) GenerateForwardOrdinalFunction(785) GenerateForwardOrdinalFunction(786) GenerateForwardOrdinalFunction(787) GenerateForwardOrdinalFunction(788) GenerateForwardOrdinalFunction(789) GenerateForwardOrdinalFunction(790) GenerateForwardOrdinalFunction(791) GenerateForwardOrdinalFunction(792) GenerateForwardOrdinalFunction(793) GenerateForwardOrdinalFunction(794) GenerateForwardOrdinalFunction(795) GenerateForwardOrdinalFunction(796) GenerateForwardOrdinalFunction(797) GenerateForwardOrdinalFunction(798) GenerateForwardOrdinalFunction(799) GenerateForwardOrdinalFunction(800)
GenerateForwardOrdinalFunction(801) GenerateForwardOrdinalFunction(802) GenerateForwardOrdinalFunction(803) GenerateForwardOrdinalFunction(804) GenerateForwardOrdinalFunction(805) GenerateForwardOrdinalFunction(806) GenerateForwardOrdinalFunction(807) GenerateForwardOrdinalFunction(808) GenerateForwardOrdinalFunction(809) GenerateForwardOrdinalFunction(810) GenerateForwardOrdinalFunction(811) GenerateForwardOrdinalFunction(812) GenerateForwardOrdinalFunction(813) GenerateForwardOrdinalFunction(814) GenerateForwardOrdinalFunction(815) GenerateForwardOrdinalFunction(816) GenerateForwardOrdinalFunction(817) GenerateForwardOrdinalFunction(818) GenerateForwardOrdinalFunction(819) GenerateForwardOrdinalFunction(820) GenerateForwardOrdinalFunction(821) GenerateForwardOrdinalFunction(822) GenerateForwardOrdinalFunction(823) GenerateForwardOrdinalFunction(824) GenerateForwardOrdinalFunction(825) GenerateForwardOrdinalFunction(826) GenerateForwardOrdinalFunction(827) GenerateForwardOrdinalFunction(828) GenerateForwardOrdinalFunction(829) GenerateForwardOrdinalFunction(830) GenerateForwardOrdinalFunction(831) GenerateForwardOrdinalFunction(832) GenerateForwardOrdinalFunction(833) GenerateForwardOrdinalFunction(834) GenerateForwardOrdinalFunction(835) GenerateForwardOrdinalFunction(836) GenerateForwardOrdinalFunction(837) GenerateForwardOrdinalFunction(838) GenerateForwardOrdinalFunction(839) GenerateForwardOrdinalFunction(840) GenerateForwardOrdinalFunction(841) GenerateForwardOrdinalFunction(842) GenerateForwardOrdinalFunction(843) GenerateForwardOrdinalFunction(844) GenerateForwardOrdinalFunction(845) GenerateForwardOrdinalFunction(846) GenerateForwardOrdinalFunction(847) GenerateForwardOrdinalFunction(848) GenerateForwardOrdinalFunction(849) GenerateForwardOrdinalFunction(850)
GenerateForwardOrdinalFunction(851) GenerateForwardOrdinalFunction(852) GenerateForwardOrdinalFunction(853) GenerateForwardOrdinalFunction(854) GenerateForwardOrdinalFunction(855) GenerateForwardOrdinalFunction(856) GenerateForwardOrdinalFunction(857) GenerateForwardOrdinalFunction(858) GenerateForwardOrdinalFunction(859) GenerateForwardOrdinalFunction(860) GenerateForwardOrdinalFunction(861) GenerateForwardOrdinalFunction(862) GenerateForwardOrdinalFunction(863) GenerateForwardOrdinalFunction(864) GenerateForwardOrdinalFunction(865) GenerateForwardOrdinalFunction(866) GenerateForwardOrdinalFunction(867) GenerateForwardOrdinalFunction(868) GenerateForwardOrdinalFunction(869) GenerateForwardOrdinalFunction(870) GenerateForwardOrdinalFunction(871) GenerateForwardOrdinalFunction(872) GenerateForwardOrdinalFunction(873) GenerateForwardOrdinalFunction(874) GenerateForwardOrdinalFunction(875) GenerateForwardOrdinalFunction(876) GenerateForwardOrdinalFunction(877) GenerateForwardOrdinalFunction(878) GenerateForwardOrdinalFunction(879) GenerateForwardOrdinalFunction(880) GenerateForwardOrdinalFunction(881) GenerateForwardOrdinalFunction(882) GenerateForwardOrdinalFunction(883) GenerateForwardOrdinalFunction(884) GenerateForwardOrdinalFunction(885) GenerateForwardOrdinalFunction(886) GenerateForwardOrdinalFunction(887) GenerateForwardOrdinalFunction(888) GenerateForwardOrdinalFunction(889) GenerateForwardOrdinalFunction(890) GenerateForwardOrdinalFunction(891) GenerateForwardOrdinalFunction(892) GenerateForwardOrdinalFunction(893) GenerateForwardOrdinalFunction(894) GenerateForwardOrdinalFunction(895) GenerateForwardOrdinalFunction(896) GenerateForwardOrdinalFunction(897) GenerateForwardOrdinalFunction(898) GenerateForwardOrdinalFunction(899) GenerateForwardOrdinalFunction(900)
GenerateForwardOrdinalFunction(901) GenerateForwardOrdinalFunction(902) GenerateForwardOrdinalFunction(903) GenerateForwardOrdinalFunction(904) GenerateForwardOrdinalFunction(905) GenerateForwardOrdinalFunction(906) GenerateForwardOrdinalFunction(907) GenerateForwardOrdinalFunction(908) GenerateForwardOrdinalFunction(909) GenerateForwardOrdinalFunction(910) GenerateForwardOrdinalFunction(911) GenerateForwardOrdinalFunction(912) GenerateForwardOrdinalFunction(913) GenerateForwardOrdinalFunction(914) GenerateForwardOrdinalFunction(915) GenerateForwardOrdinalFunction(916) GenerateForwardOrdinalFunction(917) GenerateForwardOrdinalFunction(918) GenerateForwardOrdinalFunction(919) GenerateForwardOrdinalFunction(920) GenerateForwardOrdinalFunction(921) GenerateForwardOrdinalFunction(922) GenerateForwardOrdinalFunction(923) GenerateForwardOrdinalFunction(924) GenerateForwardOrdinalFunction(925) GenerateForwardOrdinalFunction(926) GenerateForwardOrdinalFunction(927) GenerateForwardOrdinalFunction(928) GenerateForwardOrdinalFunction(929) GenerateForwardOrdinalFunction(930) GenerateForwardOrdinalFunction(931) GenerateForwardOrdinalFunction(932) GenerateForwardOrdinalFunction(933) GenerateForwardOrdinalFunction(934) GenerateForwardOrdinalFunction(935) GenerateForwardOrdinalFunction(936) GenerateForwardOrdinalFunction(937) GenerateForwardOrdinalFunction(938) GenerateForwardOrdinalFunction(939) GenerateForwardOrdinalFunction(940) GenerateForwardOrdinalFunction(941) GenerateForwardOrdinalFunction(942) GenerateForwardOrdinalFunction(943) GenerateForwardOrdinalFunction(944) GenerateForwardOrdinalFunction(945) GenerateForwardOrdinalFunction(946) GenerateForwardOrdinalFunction(947) GenerateForwardOrdinalFunction(948) GenerateForwardOrdinalFunction(949) GenerateForwardOrdinalFunction(950)
GenerateForwardOrdinalFunction(951) GenerateForwardOrdinalFunction(952) GenerateForwardOrdinalFunction(953) GenerateForwardOrdinalFunction(954) GenerateForwardOrdinalFunction(955) GenerateForwardOrdinalFunction(956) GenerateForwardOrdinalFunction(957) GenerateForwardOrdinalFunction(958) GenerateForwardOrdinalFunction(959) GenerateForwardOrdinalFunction(960) GenerateForwardOrdinalFunction(961) GenerateForwardOrdinalFunction(962) GenerateForwardOrdinalFunction(963) GenerateForwardOrdinalFunction(964) GenerateForwardOrdinalFunction(965) GenerateForwardOrdinalFunction(966) GenerateForwardOrdinalFunction(967) GenerateForwardOrdinalFunction(968) GenerateForwardOrdinalFunction(969) GenerateForwardOrdinalFunction(970) GenerateForwardOrdinalFunction(971) GenerateForwardOrdinalFunction(972) GenerateForwardOrdinalFunction(973) GenerateForwardOrdinalFunction(974) GenerateForwardOrdinalFunction(975) GenerateForwardOrdinalFunction(976) GenerateForwardOrdinalFunction(977) GenerateForwardOrdinalFunction(978) GenerateForwardOrdinalFunction(979) GenerateForwardOrdinalFunction(980) GenerateForwardOrdinalFunction(981) GenerateForwardOrdinalFunction(982) GenerateForwardOrdinalFunction(983) GenerateForwardOrdinalFunction(984) GenerateForwardOrdinalFunction(985) GenerateForwardOrdinalFunction(986) GenerateForwardOrdinalFunction(987) GenerateForwardOrdinalFunction(988) GenerateForwardOrdinalFunction(989) GenerateForwardOrdinalFunction(990) GenerateForwardOrdinalFunction(991) GenerateForwardOrdinalFunction(992) GenerateForwardOrdinalFunction(993) GenerateForwardOrdinalFunction(994) GenerateForwardOrdinalFunction(995) GenerateForwardOrdinalFunction(996) GenerateForwardOrdinalFunction(997) GenerateForwardOrdinalFunction(998) GenerateForwardOrdinalFunction(999) GenerateForwardOrdinalFunction(1000)
GenerateForwardOrdinalFunction(1001) GenerateForwardOrdinalFunction(1002) GenerateForwardOrdinalFunction(1003) GenerateForwardOrdinalFunction(1004) GenerateForwardOrdinalFunction(1005) GenerateForwardOrdinalFunction(1006) GenerateForwardOrdinalFunction(1007) GenerateForwardOrdinalFunction(1008) GenerateForwardOrdinalFunction(1009) GenerateForwardOrdinalFunction(1010) GenerateForwardOrdinalFunction(1011) GenerateForwardOrdinalFunction(1012) GenerateForwardOrdinalFunction(1013) GenerateForwardOrdinalFunction(1014) GenerateForwardOrdinalFunction(1015) GenerateForwardOrdinalFunction(1016) GenerateForwardOrdinalFunction(1017) GenerateForwardOrdinalFunction(1018) GenerateForwardOrdinalFunction(1019)
std::vector<void*> forwardOrdinalAddresses = {
	&ForwardOrdinal1, &ForwardOrdinal2, &ForwardOrdinal3, &ForwardOrdinal4, &ForwardOrdinal5, &ForwardOrdinal6, &ForwardOrdinal7, &ForwardOrdinal8, &ForwardOrdinal9, &ForwardOrdinal10, &ForwardOrdinal11, &ForwardOrdinal12, &ForwardOrdinal13, &ForwardOrdinal14, &ForwardOrdinal15, &ForwardOrdinal16, &ForwardOrdinal17, &ForwardOrdinal18, &ForwardOrdinal19, &ForwardOrdinal20, &ForwardOrdinal21, &ForwardOrdinal22, &ForwardOrdinal23, &ForwardOrdinal24, &ForwardOrdinal25, &ForwardOrdinal26, &ForwardOrdinal27, &ForwardOrdinal28, &ForwardOrdinal29, &ForwardOrdinal30, &ForwardOrdinal31, &ForwardOrdinal32, &ForwardOrdinal33, &ForwardOrdinal34, &ForwardOrdinal35, &ForwardOrdinal36, &ForwardOrdinal37, &ForwardOrdinal38, &ForwardOrdinal39, &ForwardOrdinal40, &ForwardOrdinal41, &ForwardOrdinal42, &ForwardOrdinal43, &ForwardOrdinal44, &ForwardOrdinal45, &ForwardOrdinal46, &ForwardOrdinal47, &ForwardOrdinal48, &ForwardOrdinal49, &ForwardOrdinal50,
	&ForwardOrdinal51, &ForwardOrdinal52, &ForwardOrdinal53, &ForwardOrdinal54, &ForwardOrdinal55, &ForwardOrdinal56, &ForwardOrdinal57, &ForwardOrdinal58, &ForwardOrdinal59, &ForwardOrdinal60, &ForwardOrdinal61, &ForwardOrdinal62, &ForwardOrdinal63, &ForwardOrdinal64, &ForwardOrdinal65, &ForwardOrdinal66, &ForwardOrdinal67, &ForwardOrdinal68, &ForwardOrdinal69, &ForwardOrdinal70, &ForwardOrdinal71, &ForwardOrdinal72, &ForwardOrdinal73, &ForwardOrdinal74, &ForwardOrdinal75, &ForwardOrdinal76, &ForwardOrdinal77, &ForwardOrdinal78, &ForwardOrdinal79, &ForwardOrdinal80, &ForwardOrdinal81, &ForwardOrdinal82, &ForwardOrdinal83, &ForwardOrdinal84, &ForwardOrdinal85, &ForwardOrdinal86, &ForwardOrdinal87, &ForwardOrdinal88, &ForwardOrdinal89, &ForwardOrdinal90, &ForwardOrdinal91, &ForwardOrdinal92, &ForwardOrdinal93, &ForwardOrdinal94, &ForwardOrdinal95, &ForwardOrdinal96, &ForwardOrdinal97, &ForwardOrdinal98, &ForwardOrdinal99, &ForwardOrdinal100,
	&ForwardOrdinal101, &ForwardOrdinal102, &ForwardOrdinal103, &ForwardOrdinal104, &ForwardOrdinal105, &ForwardOrdinal106, &ForwardOrdinal107, &ForwardOrdinal108, &ForwardOrdinal109, &ForwardOrdinal110, &ForwardOrdinal111, &ForwardOrdinal112, &ForwardOrdinal113, &ForwardOrdinal114, &ForwardOrdinal115, &ForwardOrdinal116, &ForwardOrdinal117, &ForwardOrdinal118, &ForwardOrdinal119, &ForwardOrdinal120, &ForwardOrdinal121, &ForwardOrdinal122, &ForwardOrdinal123, &ForwardOrdinal124, &ForwardOrdinal125, &ForwardOrdinal126, &ForwardOrdinal127, &ForwardOrdinal128, &ForwardOrdinal129, &ForwardOrdinal130, &ForwardOrdinal131, &ForwardOrdinal132, &ForwardOrdinal133, &ForwardOrdinal134, &ForwardOrdinal135, &ForwardOrdinal136, &ForwardOrdinal137, &ForwardOrdinal138, &ForwardOrdinal139, &ForwardOrdinal140, &ForwardOrdinal141, &ForwardOrdinal142, &ForwardOrdinal143, &ForwardOrdinal144, &ForwardOrdinal145, &ForwardOrdinal146, &ForwardOrdinal147, &ForwardOrdinal148, &ForwardOrdinal149, &ForwardOrdinal150,
	&ForwardOrdinal151, &ForwardOrdinal152, &ForwardOrdinal153, &ForwardOrdinal154, &ForwardOrdinal155, &ForwardOrdinal156, &ForwardOrdinal157, &ForwardOrdinal158, &ForwardOrdinal159, &ForwardOrdinal160, &ForwardOrdinal161, &ForwardOrdinal162, &ForwardOrdinal163, &ForwardOrdinal164, &ForwardOrdinal165, &ForwardOrdinal166, &ForwardOrdinal167, &ForwardOrdinal168, &ForwardOrdinal169, &ForwardOrdinal170, &ForwardOrdinal171, &ForwardOrdinal172, &ForwardOrdinal173, &ForwardOrdinal174, &ForwardOrdinal175, &ForwardOrdinal176, &ForwardOrdinal177, &ForwardOrdinal178, &ForwardOrdinal179, &ForwardOrdinal180, &ForwardOrdinal181, &ForwardOrdinal182, &ForwardOrdinal183, &ForwardOrdinal184, &ForwardOrdinal185, &ForwardOrdinal186, &ForwardOrdinal187, &ForwardOrdinal188, &ForwardOrdinal189, &ForwardOrdinal190, &ForwardOrdinal191, &ForwardOrdinal192, &ForwardOrdinal193, &ForwardOrdinal194, &ForwardOrdinal195, &ForwardOrdinal196, &ForwardOrdinal197, &ForwardOrdinal198, &ForwardOrdinal199, &ForwardOrdinal200,
	&ForwardOrdinal201, &ForwardOrdinal202, &ForwardOrdinal203, &ForwardOrdinal204, &ForwardOrdinal205, &ForwardOrdinal206, &ForwardOrdinal207, &ForwardOrdinal208, &ForwardOrdinal209, &ForwardOrdinal210, &ForwardOrdinal211, &ForwardOrdinal212, &ForwardOrdinal213, &ForwardOrdinal214, &ForwardOrdinal215, &ForwardOrdinal216, &ForwardOrdinal217, &ForwardOrdinal218, &ForwardOrdinal219, &ForwardOrdinal220, &ForwardOrdinal221, &ForwardOrdinal222, &ForwardOrdinal223, &ForwardOrdinal224, &ForwardOrdinal225, &ForwardOrdinal226, &ForwardOrdinal227, &ForwardOrdinal228, &ForwardOrdinal229, &ForwardOrdinal230, &ForwardOrdinal231, &ForwardOrdinal232, &ForwardOrdinal233, &ForwardOrdinal234, &ForwardOrdinal235, &ForwardOrdinal236, &ForwardOrdinal237, &ForwardOrdinal238, &ForwardOrdinal239, &ForwardOrdinal240, &ForwardOrdinal241, &ForwardOrdinal242, &ForwardOrdinal243, &ForwardOrdinal244, &ForwardOrdinal245, &ForwardOrdinal246, &ForwardOrdinal247, &ForwardOrdinal248, &ForwardOrdinal249, &ForwardOrdinal250,
	&ForwardOrdinal251, &ForwardOrdinal252, &ForwardOrdinal253, &ForwardOrdinal254, &ForwardOrdinal255, &ForwardOrdinal256, &ForwardOrdinal257, &ForwardOrdinal258, &ForwardOrdinal259, &ForwardOrdinal260, &ForwardOrdinal261, &ForwardOrdinal262, &ForwardOrdinal263, &ForwardOrdinal264, &ForwardOrdinal265, &ForwardOrdinal266, &ForwardOrdinal267, &ForwardOrdinal268, &ForwardOrdinal269, &ForwardOrdinal270, &ForwardOrdinal271, &ForwardOrdinal272, &ForwardOrdinal273, &ForwardOrdinal274, &ForwardOrdinal275, &ForwardOrdinal276, &ForwardOrdinal277, &ForwardOrdinal278, &ForwardOrdinal279, &ForwardOrdinal280, &ForwardOrdinal281, &ForwardOrdinal282, &ForwardOrdinal283, &ForwardOrdinal284, &ForwardOrdinal285, &ForwardOrdinal286, &ForwardOrdinal287, &ForwardOrdinal288, &ForwardOrdinal289, &ForwardOrdinal290, &ForwardOrdinal291, &ForwardOrdinal292, &ForwardOrdinal293, &ForwardOrdinal294, &ForwardOrdinal295, &ForwardOrdinal296, &ForwardOrdinal297, &ForwardOrdinal298, &ForwardOrdinal299, &ForwardOrdinal300,
	&ForwardOrdinal301, &ForwardOrdinal302, &ForwardOrdinal303, &ForwardOrdinal304, &ForwardOrdinal305, &ForwardOrdinal306, &ForwardOrdinal307, &ForwardOrdinal308, &ForwardOrdinal309, &ForwardOrdinal310, &ForwardOrdinal311, &ForwardOrdinal312, &ForwardOrdinal313, &ForwardOrdinal314, &ForwardOrdinal315, &ForwardOrdinal316, &ForwardOrdinal317, &ForwardOrdinal318, &ForwardOrdinal319, &ForwardOrdinal320, &ForwardOrdinal321, &ForwardOrdinal322, &ForwardOrdinal323, &ForwardOrdinal324, &ForwardOrdinal325, &ForwardOrdinal326, &ForwardOrdinal327, &ForwardOrdinal328, &ForwardOrdinal329, &ForwardOrdinal330, &ForwardOrdinal331, &ForwardOrdinal332, &ForwardOrdinal333, &ForwardOrdinal334, &ForwardOrdinal335, &ForwardOrdinal336, &ForwardOrdinal337, &ForwardOrdinal338, &ForwardOrdinal339, &ForwardOrdinal340, &ForwardOrdinal341, &ForwardOrdinal342, &ForwardOrdinal343, &ForwardOrdinal344, &ForwardOrdinal345, &ForwardOrdinal346, &ForwardOrdinal347, &ForwardOrdinal348, &ForwardOrdinal349, &ForwardOrdinal350,
	&ForwardOrdinal351, &ForwardOrdinal352, &ForwardOrdinal353, &ForwardOrdinal354, &ForwardOrdinal355, &ForwardOrdinal356, &ForwardOrdinal357, &ForwardOrdinal358, &ForwardOrdinal359, &ForwardOrdinal360, &ForwardOrdinal361, &ForwardOrdinal362, &ForwardOrdinal363, &ForwardOrdinal364, &ForwardOrdinal365, &ForwardOrdinal366, &ForwardOrdinal367, &ForwardOrdinal368, &ForwardOrdinal369, &ForwardOrdinal370, &ForwardOrdinal371, &ForwardOrdinal372, &ForwardOrdinal373, &ForwardOrdinal374, &ForwardOrdinal375, &ForwardOrdinal376, &ForwardOrdinal377, &ForwardOrdinal378, &ForwardOrdinal379, &ForwardOrdinal380, &ForwardOrdinal381, &ForwardOrdinal382, &ForwardOrdinal383, &ForwardOrdinal384, &ForwardOrdinal385, &ForwardOrdinal386, &ForwardOrdinal387, &ForwardOrdinal388, &ForwardOrdinal389, &ForwardOrdinal390, &ForwardOrdinal391, &ForwardOrdinal392, &ForwardOrdinal393, &ForwardOrdinal394, &ForwardOrdinal395, &ForwardOrdinal396, &ForwardOrdinal397, &ForwardOrdinal398, &ForwardOrdinal399, &ForwardOrdinal400,
	&ForwardOrdinal401, &ForwardOrdinal402, &ForwardOrdinal403, &ForwardOrdinal404, &ForwardOrdinal405, &ForwardOrdinal406, &ForwardOrdinal407, &ForwardOrdinal408, &ForwardOrdinal409, &ForwardOrdinal410, &ForwardOrdinal411, &ForwardOrdinal412, &ForwardOrdinal413, &ForwardOrdinal414, &ForwardOrdinal415, &ForwardOrdinal416, &ForwardOrdinal417, &ForwardOrdinal418, &ForwardOrdinal419, &ForwardOrdinal420, &ForwardOrdinal421, &ForwardOrdinal422, &ForwardOrdinal423, &ForwardOrdinal424, &ForwardOrdinal425, &ForwardOrdinal426, &ForwardOrdinal427, &ForwardOrdinal428, &ForwardOrdinal429, &ForwardOrdinal430, &ForwardOrdinal431, &ForwardOrdinal432, &ForwardOrdinal433, &ForwardOrdinal434, &ForwardOrdinal435, &ForwardOrdinal436, &ForwardOrdinal437, &ForwardOrdinal438, &ForwardOrdinal439, &ForwardOrdinal440, &ForwardOrdinal441, &ForwardOrdinal442, &ForwardOrdinal443, &ForwardOrdinal444, &ForwardOrdinal445, &ForwardOrdinal446, &ForwardOrdinal447, &ForwardOrdinal448, &ForwardOrdinal449, &ForwardOrdinal450,
	&ForwardOrdinal451, &ForwardOrdinal452, &ForwardOrdinal453, &ForwardOrdinal454, &ForwardOrdinal455, &ForwardOrdinal456, &ForwardOrdinal457, &ForwardOrdinal458, &ForwardOrdinal459, &ForwardOrdinal460, &ForwardOrdinal461, &ForwardOrdinal462, &ForwardOrdinal463, &ForwardOrdinal464, &ForwardOrdinal465, &ForwardOrdinal466, &ForwardOrdinal467, &ForwardOrdinal468, &ForwardOrdinal469, &ForwardOrdinal470, &ForwardOrdinal471, &ForwardOrdinal472, &ForwardOrdinal473, &ForwardOrdinal474, &ForwardOrdinal475, &ForwardOrdinal476, &ForwardOrdinal477, &ForwardOrdinal478, &ForwardOrdinal479, &ForwardOrdinal480, &ForwardOrdinal481, &ForwardOrdinal482, &ForwardOrdinal483, &ForwardOrdinal484, &ForwardOrdinal485, &ForwardOrdinal486, &ForwardOrdinal487, &ForwardOrdinal488, &ForwardOrdinal489, &ForwardOrdinal490, &ForwardOrdinal491, &ForwardOrdinal492, &ForwardOrdinal493, &ForwardOrdinal494, &ForwardOrdinal495, &ForwardOrdinal496, &ForwardOrdinal497, &ForwardOrdinal498, &ForwardOrdinal499, &ForwardOrdinal500,
	&ForwardOrdinal501, &ForwardOrdinal502, &ForwardOrdinal503, &ForwardOrdinal504, &ForwardOrdinal505, &ForwardOrdinal506, &ForwardOrdinal507, &ForwardOrdinal508, &ForwardOrdinal509, &ForwardOrdinal510, &ForwardOrdinal511, &ForwardOrdinal512, &ForwardOrdinal513, &ForwardOrdinal514, &ForwardOrdinal515, &ForwardOrdinal516, &ForwardOrdinal517, &ForwardOrdinal518, &ForwardOrdinal519, &ForwardOrdinal520, &ForwardOrdinal521, &ForwardOrdinal522, &ForwardOrdinal523, &ForwardOrdinal524, &ForwardOrdinal525, &ForwardOrdinal526, &ForwardOrdinal527, &ForwardOrdinal528, &ForwardOrdinal529, &ForwardOrdinal530, &ForwardOrdinal531, &ForwardOrdinal532, &ForwardOrdinal533, &ForwardOrdinal534, &ForwardOrdinal535, &ForwardOrdinal536, &ForwardOrdinal537, &ForwardOrdinal538, &ForwardOrdinal539, &ForwardOrdinal540, &ForwardOrdinal541, &ForwardOrdinal542, &ForwardOrdinal543, &ForwardOrdinal544, &ForwardOrdinal545, &ForwardOrdinal546, &ForwardOrdinal547, &ForwardOrdinal548, &ForwardOrdinal549, &ForwardOrdinal550,
	&ForwardOrdinal551, &ForwardOrdinal552, &ForwardOrdinal553, &ForwardOrdinal554, &ForwardOrdinal555, &ForwardOrdinal556, &ForwardOrdinal557, &ForwardOrdinal558, &ForwardOrdinal559, &ForwardOrdinal560, &ForwardOrdinal561, &ForwardOrdinal562, &ForwardOrdinal563, &ForwardOrdinal564, &ForwardOrdinal565, &ForwardOrdinal566, &ForwardOrdinal567, &ForwardOrdinal568, &ForwardOrdinal569, &ForwardOrdinal570, &ForwardOrdinal571, &ForwardOrdinal572, &ForwardOrdinal573, &ForwardOrdinal574, &ForwardOrdinal575, &ForwardOrdinal576, &ForwardOrdinal577, &ForwardOrdinal578, &ForwardOrdinal579, &ForwardOrdinal580, &ForwardOrdinal581, &ForwardOrdinal582, &ForwardOrdinal583, &ForwardOrdinal584, &ForwardOrdinal585, &ForwardOrdinal586, &ForwardOrdinal587, &ForwardOrdinal588, &ForwardOrdinal589, &ForwardOrdinal590, &ForwardOrdinal591, &ForwardOrdinal592, &ForwardOrdinal593, &ForwardOrdinal594, &ForwardOrdinal595, &ForwardOrdinal596, &ForwardOrdinal597, &ForwardOrdinal598, &ForwardOrdinal599, &ForwardOrdinal600,
	&ForwardOrdinal601, &ForwardOrdinal602, &ForwardOrdinal603, &ForwardOrdinal604, &ForwardOrdinal605, &ForwardOrdinal606, &ForwardOrdinal607, &ForwardOrdinal608, &ForwardOrdinal609, &ForwardOrdinal610, &ForwardOrdinal611, &ForwardOrdinal612, &ForwardOrdinal613, &ForwardOrdinal614, &ForwardOrdinal615, &ForwardOrdinal616, &ForwardOrdinal617, &ForwardOrdinal618, &ForwardOrdinal619, &ForwardOrdinal620, &ForwardOrdinal621, &ForwardOrdinal622, &ForwardOrdinal623, &ForwardOrdinal624, &ForwardOrdinal625, &ForwardOrdinal626, &ForwardOrdinal627, &ForwardOrdinal628, &ForwardOrdinal629, &ForwardOrdinal630, &ForwardOrdinal631, &ForwardOrdinal632, &ForwardOrdinal633, &ForwardOrdinal634, &ForwardOrdinal635, &ForwardOrdinal636, &ForwardOrdinal637, &ForwardOrdinal638, &ForwardOrdinal639, &ForwardOrdinal640, &ForwardOrdinal641, &ForwardOrdinal642, &ForwardOrdinal643, &ForwardOrdinal644, &ForwardOrdinal645, &ForwardOrdinal646, &ForwardOrdinal647, &ForwardOrdinal648, &ForwardOrdinal649, &ForwardOrdinal650,
	&ForwardOrdinal651, &ForwardOrdinal652, &ForwardOrdinal653, &ForwardOrdinal654, &ForwardOrdinal655, &ForwardOrdinal656, &ForwardOrdinal657, &ForwardOrdinal658, &ForwardOrdinal659, &ForwardOrdinal660, &ForwardOrdinal661, &ForwardOrdinal662, &ForwardOrdinal663, &ForwardOrdinal664, &ForwardOrdinal665, &ForwardOrdinal666, &ForwardOrdinal667, &ForwardOrdinal668, &ForwardOrdinal669, &ForwardOrdinal670, &ForwardOrdinal671, &ForwardOrdinal672, &ForwardOrdinal673, &ForwardOrdinal674, &ForwardOrdinal675, &ForwardOrdinal676, &ForwardOrdinal677, &ForwardOrdinal678, &ForwardOrdinal679, &ForwardOrdinal680, &ForwardOrdinal681, &ForwardOrdinal682, &ForwardOrdinal683, &ForwardOrdinal684, &ForwardOrdinal685, &ForwardOrdinal686, &ForwardOrdinal687, &ForwardOrdinal688, &ForwardOrdinal689, &ForwardOrdinal690, &ForwardOrdinal691, &ForwardOrdinal692, &ForwardOrdinal693, &ForwardOrdinal694, &ForwardOrdinal695, &ForwardOrdinal696, &ForwardOrdinal697, &ForwardOrdinal698, &ForwardOrdinal699, &ForwardOrdinal700,
	&ForwardOrdinal701, &ForwardOrdinal702, &ForwardOrdinal703, &ForwardOrdinal704, &ForwardOrdinal705, &ForwardOrdinal706, &ForwardOrdinal707, &ForwardOrdinal708, &ForwardOrdinal709, &ForwardOrdinal710, &ForwardOrdinal711, &ForwardOrdinal712, &ForwardOrdinal713, &ForwardOrdinal714, &ForwardOrdinal715, &ForwardOrdinal716, &ForwardOrdinal717, &ForwardOrdinal718, &ForwardOrdinal719, &ForwardOrdinal720, &ForwardOrdinal721, &ForwardOrdinal722, &ForwardOrdinal723, &ForwardOrdinal724, &ForwardOrdinal725, &ForwardOrdinal726, &ForwardOrdinal727, &ForwardOrdinal728, &ForwardOrdinal729, &ForwardOrdinal730, &ForwardOrdinal731, &ForwardOrdinal732, &ForwardOrdinal733, &ForwardOrdinal734, &ForwardOrdinal735, &ForwardOrdinal736, &ForwardOrdinal737, &ForwardOrdinal738, &ForwardOrdinal739, &ForwardOrdinal740, &ForwardOrdinal741, &ForwardOrdinal742, &ForwardOrdinal743, &ForwardOrdinal744, &ForwardOrdinal745, &ForwardOrdinal746, &ForwardOrdinal747, &ForwardOrdinal748, &ForwardOrdinal749, &ForwardOrdinal750,
	&ForwardOrdinal751, &ForwardOrdinal752, &ForwardOrdinal753, &ForwardOrdinal754, &ForwardOrdinal755, &ForwardOrdinal756, &ForwardOrdinal757, &ForwardOrdinal758, &ForwardOrdinal759, &ForwardOrdinal760, &ForwardOrdinal761, &ForwardOrdinal762, &ForwardOrdinal763, &ForwardOrdinal764, &ForwardOrdinal765, &ForwardOrdinal766, &ForwardOrdinal767, &ForwardOrdinal768, &ForwardOrdinal769, &ForwardOrdinal770, &ForwardOrdinal771, &ForwardOrdinal772, &ForwardOrdinal773, &ForwardOrdinal774, &ForwardOrdinal775, &ForwardOrdinal776, &ForwardOrdinal777, &ForwardOrdinal778, &ForwardOrdinal779, &ForwardOrdinal780, &ForwardOrdinal781, &ForwardOrdinal782, &ForwardOrdinal783, &ForwardOrdinal784, &ForwardOrdinal785, &ForwardOrdinal786, &ForwardOrdinal787, &ForwardOrdinal788, &ForwardOrdinal789, &ForwardOrdinal790, &ForwardOrdinal791, &ForwardOrdinal792, &ForwardOrdinal793, &ForwardOrdinal794, &ForwardOrdinal795, &ForwardOrdinal796, &ForwardOrdinal797, &ForwardOrdinal798, &ForwardOrdinal799, &ForwardOrdinal800,
	&ForwardOrdinal801, &ForwardOrdinal802, &ForwardOrdinal803, &ForwardOrdinal804, &ForwardOrdinal805, &ForwardOrdinal806, &ForwardOrdinal807, &ForwardOrdinal808, &ForwardOrdinal809, &ForwardOrdinal810, &ForwardOrdinal811, &ForwardOrdinal812, &ForwardOrdinal813, &ForwardOrdinal814, &ForwardOrdinal815, &ForwardOrdinal816, &ForwardOrdinal817, &ForwardOrdinal818, &ForwardOrdinal819, &ForwardOrdinal820, &ForwardOrdinal821, &ForwardOrdinal822, &ForwardOrdinal823, &ForwardOrdinal824, &ForwardOrdinal825, &ForwardOrdinal826, &ForwardOrdinal827, &ForwardOrdinal828, &ForwardOrdinal829, &ForwardOrdinal830, &ForwardOrdinal831, &ForwardOrdinal832, &ForwardOrdinal833, &ForwardOrdinal834, &ForwardOrdinal835, &ForwardOrdinal836, &ForwardOrdinal837, &ForwardOrdinal838, &ForwardOrdinal839, &ForwardOrdinal840, &ForwardOrdinal841, &ForwardOrdinal842, &ForwardOrdinal843, &ForwardOrdinal844, &ForwardOrdinal845, &ForwardOrdinal846, &ForwardOrdinal847, &ForwardOrdinal848, &ForwardOrdinal849, &ForwardOrdinal850,
	&ForwardOrdinal851, &ForwardOrdinal852, &ForwardOrdinal853, &ForwardOrdinal854, &ForwardOrdinal855, &ForwardOrdinal856, &ForwardOrdinal857, &ForwardOrdinal858, &ForwardOrdinal859, &ForwardOrdinal860, &ForwardOrdinal861, &ForwardOrdinal862, &ForwardOrdinal863, &ForwardOrdinal864, &ForwardOrdinal865, &ForwardOrdinal866, &ForwardOrdinal867, &ForwardOrdinal868, &ForwardOrdinal869, &ForwardOrdinal870, &ForwardOrdinal871, &ForwardOrdinal872, &ForwardOrdinal873, &ForwardOrdinal874, &ForwardOrdinal875, &ForwardOrdinal876, &ForwardOrdinal877, &ForwardOrdinal878, &ForwardOrdinal879, &ForwardOrdinal880, &ForwardOrdinal881, &ForwardOrdinal882, &ForwardOrdinal883, &ForwardOrdinal884, &ForwardOrdinal885, &ForwardOrdinal886, &ForwardOrdinal887, &ForwardOrdinal888, &ForwardOrdinal889, &ForwardOrdinal890, &ForwardOrdinal891, &ForwardOrdinal892, &ForwardOrdinal893, &ForwardOrdinal894, &ForwardOrdinal895, &ForwardOrdinal896, &ForwardOrdinal897, &ForwardOrdinal898, &ForwardOrdinal899, &ForwardOrdinal900,
	&ForwardOrdinal901, &ForwardOrdinal902, &ForwardOrdinal903, &ForwardOrdinal904, &ForwardOrdinal905, &ForwardOrdinal906, &ForwardOrdinal907, &ForwardOrdinal908, &ForwardOrdinal909, &ForwardOrdinal910, &ForwardOrdinal911, &ForwardOrdinal912, &ForwardOrdinal913, &ForwardOrdinal914, &ForwardOrdinal915, &ForwardOrdinal916, &ForwardOrdinal917, &ForwardOrdinal918, &ForwardOrdinal919, &ForwardOrdinal920, &ForwardOrdinal921, &ForwardOrdinal922, &ForwardOrdinal923, &ForwardOrdinal924, &ForwardOrdinal925, &ForwardOrdinal926, &ForwardOrdinal927, &ForwardOrdinal928, &ForwardOrdinal929, &ForwardOrdinal930, &ForwardOrdinal931, &ForwardOrdinal932, &ForwardOrdinal933, &ForwardOrdinal934, &ForwardOrdinal935, &ForwardOrdinal936, &ForwardOrdinal937, &ForwardOrdinal938, &ForwardOrdinal939, &ForwardOrdinal940, &ForwardOrdinal941, &ForwardOrdinal942, &ForwardOrdinal943, &ForwardOrdinal944, &ForwardOrdinal945, &ForwardOrdinal946, &ForwardOrdinal947, &ForwardOrdinal948, &ForwardOrdinal949, &ForwardOrdinal950,
	&ForwardOrdinal951, &ForwardOrdinal952, &ForwardOrdinal953, &ForwardOrdinal954, &ForwardOrdinal955, &ForwardOrdinal956, &ForwardOrdinal957, &ForwardOrdinal958, &ForwardOrdinal959, &ForwardOrdinal960, &ForwardOrdinal961, &ForwardOrdinal962, &ForwardOrdinal963, &ForwardOrdinal964, &ForwardOrdinal965, &ForwardOrdinal966, &ForwardOrdinal967, &ForwardOrdinal968, &ForwardOrdinal969, &ForwardOrdinal970, &ForwardOrdinal971, &ForwardOrdinal972, &ForwardOrdinal973, &ForwardOrdinal974, &ForwardOrdinal975, &ForwardOrdinal976, &ForwardOrdinal977, &ForwardOrdinal978, &ForwardOrdinal979, &ForwardOrdinal980, &ForwardOrdinal981, &ForwardOrdinal982, &ForwardOrdinal983, &ForwardOrdinal984, &ForwardOrdinal985, &ForwardOrdinal986, &ForwardOrdinal987, &ForwardOrdinal988, &ForwardOrdinal989, &ForwardOrdinal990, &ForwardOrdinal991, &ForwardOrdinal992, &ForwardOrdinal993, &ForwardOrdinal994, &ForwardOrdinal995, &ForwardOrdinal996, &ForwardOrdinal997, &ForwardOrdinal998, &ForwardOrdinal999, &ForwardOrdinal1000,
	&ForwardOrdinal1001, &ForwardOrdinal1002, &ForwardOrdinal1003, &ForwardOrdinal1004, &ForwardOrdinal1005, &ForwardOrdinal1006, &ForwardOrdinal1007, &ForwardOrdinal1008, &ForwardOrdinal1009, &ForwardOrdinal1010, &ForwardOrdinal1011, &ForwardOrdinal1012, &ForwardOrdinal1013, &ForwardOrdinal1014, &ForwardOrdinal1015, &ForwardOrdinal1016, &ForwardOrdinal1017, &ForwardOrdinal1018, &ForwardOrdinal1019
};

GenerateForwardSharedFunction(0) GenerateForwardSharedFunction(1) GenerateForwardSharedFunction(2) GenerateForwardSharedFunction(3) GenerateForwardSharedFunction(4) GenerateForwardSharedFunction(5) GenerateForwardSharedFunction(6) GenerateForwardSharedFunction(7) GenerateForwardSharedFunction(8) GenerateForwardSharedFunction(9) GenerateForwardSharedFunction(10) GenerateForwardSharedFunction(11) GenerateForwardSharedFunction(12) GenerateForwardSharedFunction(13) GenerateForwardSharedFunction(14) GenerateForwardSharedFunction(15) GenerateForwardSharedFunction(16) GenerateForwardSharedFunction(17) GenerateForwardSharedFunction(18) GenerateForwardSharedFunction(19) GenerateForwardSharedFunction(20) GenerateForwardSharedFunction(21) GenerateForwardSharedFunction(22) GenerateForwardSharedFunction(23) GenerateForwardSharedFunction(24) GenerateForwardSharedFunction(25) GenerateForwardSharedFunction(26) GenerateForwardSharedFunction(27) GenerateForwardSharedFunction(28) GenerateForwardSharedFunction(29) GenerateForwardSharedFunction(30) GenerateForwardSharedFunction(31) GenerateForwardSharedFunction(32) GenerateForwardSharedFunction(33) GenerateForwardSharedFunction(34) GenerateForwardSharedFunction(35) GenerateForwardSharedFunction(36) GenerateForwardSharedFunction(37) GenerateForwardSharedFunction(38) GenerateForwardSharedFunction(39) GenerateForwardSharedFunction(40) GenerateForwardSharedFunction(41) GenerateForwardSharedFunction(42) GenerateForwardSharedFunction(43) GenerateForwardSharedFunction(44) GenerateForwardSharedFunction(45) GenerateForwardSharedFunction(46) GenerateForwardSharedFunction(47) GenerateForwardSharedFunction(48) GenerateForwardSharedFunction(49)
std::vector<void*> forwardSharedAddresses = {
	&ForwardShared0, &ForwardShared1, &ForwardShared2, &ForwardShared3, &ForwardShared4, &ForwardShared5, &ForwardShared6, &ForwardShared7, &ForwardShared8, &ForwardShared9, &ForwardShared10, &ForwardShared11, &ForwardShared12, &ForwardShared13, &ForwardShared14, &ForwardShared15, &ForwardShared16, &ForwardShared17, &ForwardShared18, &ForwardShared19, &ForwardShared20, &ForwardShared21, &ForwardShared22, &ForwardShared23, &ForwardShared24, &ForwardShared25, &ForwardShared26, &ForwardShared27, &ForwardShared28, &ForwardShared29, &ForwardShared30, &ForwardShared31, &ForwardShared32, &ForwardShared33, &ForwardShared34, &ForwardShared35, &ForwardShared36, &ForwardShared37, &ForwardShared38, &ForwardShared39, &ForwardShared40, &ForwardShared41, &ForwardShared42, &ForwardShared43, &ForwardShared44, &ForwardShared45, &ForwardShared46, &ForwardShared47, &ForwardShared48, &ForwardShared49
};

#ifndef _WIN64
#define Export_(index, name) __pragma(comment(linker, "/export:"#name"=_Forward"#index))
#define ExportOrdinal_(ordinal) __pragma(comment(linker, "/export:"#ordinal"=_ForwardOrdinal"#ordinal",@"#ordinal",NONAME"))
#define ExportShared_(index, name) __pragma(comment(linker, "/export:"#name"=_ForwardShared"##index))
#else
#define Export_(index, name) __pragma(comment(linker, "/export:"#name"=Forward"#index))
#define ExportOrdinal_(ordinal) __pragma(comment(linker, "/export:"#ordinal"=ForwardOrdinal"#ordinal",@"#ordinal",NONAME"))
#define ExportShared_(index, name) __pragma(comment(linker, "/export:"#name"=ForwardShared"##index))
#endif

#define Export(index, name) Export_(index, name)
#define ExportOrdinal(ordinal) ExportOrdinal_(ordinal)
#define ExportShared(index, name) ExportShared_(index, name)

// Handle export names shared by multiple DLLs
// Note: it's fine if these are duplicated in the normal export list
#define ExpandNumber_(num) #num
#define ExpandNumber(num) ExpandNumber_(num)
ExportShared(ExpandNumber(SharedExportIndex_DllCanUnloadNow), DllCanUnloadNow)
ExportShared(ExpandNumber(SharedExportIndex_DllGetClassObject), DllGetClassObject)
ExportShared(ExpandNumber(SharedExportIndex_SetAppCompatStringPointer), SetAppCompatStringPointer)

// Export an amount of ordinals equal to highest amount of exports exported by one of the supported DLLs
ExportOrdinal(1) ExportOrdinal(2) ExportOrdinal(3) ExportOrdinal(4) ExportOrdinal(5) ExportOrdinal(6) ExportOrdinal(7) ExportOrdinal(8) ExportOrdinal(9) ExportOrdinal(10) ExportOrdinal(11) ExportOrdinal(12) ExportOrdinal(13) ExportOrdinal(14) ExportOrdinal(15) ExportOrdinal(16) ExportOrdinal(17) ExportOrdinal(18) ExportOrdinal(19) ExportOrdinal(20) ExportOrdinal(21) ExportOrdinal(22) ExportOrdinal(23) ExportOrdinal(24) ExportOrdinal(25) ExportOrdinal(26) ExportOrdinal(27) ExportOrdinal(28) ExportOrdinal(29) ExportOrdinal(30) ExportOrdinal(31) ExportOrdinal(32) ExportOrdinal(33) ExportOrdinal(34) ExportOrdinal(35) ExportOrdinal(36) ExportOrdinal(37) ExportOrdinal(38) ExportOrdinal(39) ExportOrdinal(40) ExportOrdinal(41) ExportOrdinal(42) ExportOrdinal(43) ExportOrdinal(44) ExportOrdinal(45) ExportOrdinal(46) ExportOrdinal(47) ExportOrdinal(48) ExportOrdinal(49) ExportOrdinal(50)
ExportOrdinal(51) ExportOrdinal(52) ExportOrdinal(53) ExportOrdinal(54) ExportOrdinal(55) ExportOrdinal(56) ExportOrdinal(57) ExportOrdinal(58) ExportOrdinal(59) ExportOrdinal(60) ExportOrdinal(61) ExportOrdinal(62) ExportOrdinal(63) ExportOrdinal(64) ExportOrdinal(65) ExportOrdinal(66) ExportOrdinal(67) ExportOrdinal(68) ExportOrdinal(69) ExportOrdinal(70) ExportOrdinal(71) ExportOrdinal(72) ExportOrdinal(73) ExportOrdinal(74) ExportOrdinal(75) ExportOrdinal(76) ExportOrdinal(77) ExportOrdinal(78) ExportOrdinal(79) ExportOrdinal(80) ExportOrdinal(81) ExportOrdinal(82) ExportOrdinal(83) ExportOrdinal(84) ExportOrdinal(85) ExportOrdinal(86) ExportOrdinal(87) ExportOrdinal(88) ExportOrdinal(89) ExportOrdinal(90) ExportOrdinal(91) ExportOrdinal(92) ExportOrdinal(93) ExportOrdinal(94) ExportOrdinal(95) ExportOrdinal(96) ExportOrdinal(97) ExportOrdinal(98) ExportOrdinal(99) ExportOrdinal(100)
ExportOrdinal(101) ExportOrdinal(102) ExportOrdinal(103) ExportOrdinal(104) ExportOrdinal(105) ExportOrdinal(106) ExportOrdinal(107) ExportOrdinal(108) ExportOrdinal(109) ExportOrdinal(110) ExportOrdinal(111) ExportOrdinal(112) ExportOrdinal(113) ExportOrdinal(114) ExportOrdinal(115) ExportOrdinal(116) ExportOrdinal(117) ExportOrdinal(118) ExportOrdinal(119) ExportOrdinal(120) ExportOrdinal(121) ExportOrdinal(122) ExportOrdinal(123) ExportOrdinal(124) ExportOrdinal(125) ExportOrdinal(126) ExportOrdinal(127) ExportOrdinal(128) ExportOrdinal(129) ExportOrdinal(130) ExportOrdinal(131) ExportOrdinal(132) ExportOrdinal(133) ExportOrdinal(134) ExportOrdinal(135) ExportOrdinal(136) ExportOrdinal(137) ExportOrdinal(138) ExportOrdinal(139) ExportOrdinal(140) ExportOrdinal(141) ExportOrdinal(142) ExportOrdinal(143) ExportOrdinal(144) ExportOrdinal(145) ExportOrdinal(146) ExportOrdinal(147) ExportOrdinal(148) ExportOrdinal(149) ExportOrdinal(150)
ExportOrdinal(151) ExportOrdinal(152) ExportOrdinal(153) ExportOrdinal(154) ExportOrdinal(155) ExportOrdinal(156) ExportOrdinal(157) ExportOrdinal(158) ExportOrdinal(159) ExportOrdinal(160) ExportOrdinal(161) ExportOrdinal(162) ExportOrdinal(163) ExportOrdinal(164) ExportOrdinal(165) ExportOrdinal(166) ExportOrdinal(167) ExportOrdinal(168) ExportOrdinal(169) ExportOrdinal(170) ExportOrdinal(171) ExportOrdinal(172) ExportOrdinal(173) ExportOrdinal(174) ExportOrdinal(175) ExportOrdinal(176) ExportOrdinal(177) ExportOrdinal(178) ExportOrdinal(179) ExportOrdinal(180) ExportOrdinal(181) ExportOrdinal(182) ExportOrdinal(183) ExportOrdinal(184) ExportOrdinal(185) ExportOrdinal(186) ExportOrdinal(187) ExportOrdinal(188) ExportOrdinal(189) ExportOrdinal(190) ExportOrdinal(191) ExportOrdinal(192) ExportOrdinal(193) ExportOrdinal(194) ExportOrdinal(195) ExportOrdinal(196) ExportOrdinal(197) ExportOrdinal(198) ExportOrdinal(199) ExportOrdinal(200)
ExportOrdinal(201) ExportOrdinal(202) ExportOrdinal(203) ExportOrdinal(204) ExportOrdinal(205) ExportOrdinal(206) ExportOrdinal(207) ExportOrdinal(208) ExportOrdinal(209) ExportOrdinal(210) ExportOrdinal(211) ExportOrdinal(212) ExportOrdinal(213) ExportOrdinal(214) ExportOrdinal(215) ExportOrdinal(216) ExportOrdinal(217) ExportOrdinal(218) ExportOrdinal(219) ExportOrdinal(220) ExportOrdinal(221) ExportOrdinal(222) ExportOrdinal(223) ExportOrdinal(224) ExportOrdinal(225) ExportOrdinal(226) ExportOrdinal(227) ExportOrdinal(228) ExportOrdinal(229) ExportOrdinal(230) ExportOrdinal(231) ExportOrdinal(232) ExportOrdinal(233) ExportOrdinal(234) ExportOrdinal(235) ExportOrdinal(236) ExportOrdinal(237) ExportOrdinal(238) ExportOrdinal(239) ExportOrdinal(240) ExportOrdinal(241) ExportOrdinal(242) ExportOrdinal(243) ExportOrdinal(244) ExportOrdinal(245) ExportOrdinal(246) ExportOrdinal(247) ExportOrdinal(248) ExportOrdinal(249) ExportOrdinal(250)
ExportOrdinal(251) ExportOrdinal(252) ExportOrdinal(253) ExportOrdinal(254) ExportOrdinal(255) ExportOrdinal(256) ExportOrdinal(257) ExportOrdinal(258) ExportOrdinal(259) ExportOrdinal(260) ExportOrdinal(261) ExportOrdinal(262) ExportOrdinal(263) ExportOrdinal(264) ExportOrdinal(265) ExportOrdinal(266) ExportOrdinal(267) ExportOrdinal(268) ExportOrdinal(269) ExportOrdinal(270) ExportOrdinal(271) ExportOrdinal(272) ExportOrdinal(273) ExportOrdinal(274) ExportOrdinal(275) ExportOrdinal(276) ExportOrdinal(277) ExportOrdinal(278) ExportOrdinal(279) ExportOrdinal(280) ExportOrdinal(281) ExportOrdinal(282) ExportOrdinal(283) ExportOrdinal(284) ExportOrdinal(285) ExportOrdinal(286) ExportOrdinal(287) ExportOrdinal(288) ExportOrdinal(289) ExportOrdinal(290) ExportOrdinal(291) ExportOrdinal(292) ExportOrdinal(293) ExportOrdinal(294) ExportOrdinal(295) ExportOrdinal(296) ExportOrdinal(297) ExportOrdinal(298) ExportOrdinal(299) ExportOrdinal(300)
ExportOrdinal(301) ExportOrdinal(302) ExportOrdinal(303) ExportOrdinal(304) ExportOrdinal(305) ExportOrdinal(306) ExportOrdinal(307) ExportOrdinal(308) ExportOrdinal(309) ExportOrdinal(310) ExportOrdinal(311) ExportOrdinal(312) ExportOrdinal(313) ExportOrdinal(314) ExportOrdinal(315) ExportOrdinal(316) ExportOrdinal(317) ExportOrdinal(318) ExportOrdinal(319) ExportOrdinal(320) ExportOrdinal(321) ExportOrdinal(322) ExportOrdinal(323) ExportOrdinal(324) ExportOrdinal(325) ExportOrdinal(326) ExportOrdinal(327) ExportOrdinal(328) ExportOrdinal(329) ExportOrdinal(330) ExportOrdinal(331) ExportOrdinal(332) ExportOrdinal(333) ExportOrdinal(334) ExportOrdinal(335) ExportOrdinal(336) ExportOrdinal(337) ExportOrdinal(338) ExportOrdinal(339) ExportOrdinal(340) ExportOrdinal(341) ExportOrdinal(342) ExportOrdinal(343) ExportOrdinal(344) ExportOrdinal(345) ExportOrdinal(346) ExportOrdinal(347) ExportOrdinal(348) ExportOrdinal(349) ExportOrdinal(350)
ExportOrdinal(351) ExportOrdinal(352) ExportOrdinal(353) ExportOrdinal(354) ExportOrdinal(355) ExportOrdinal(356) ExportOrdinal(357) ExportOrdinal(358) ExportOrdinal(359) ExportOrdinal(360) ExportOrdinal(361) ExportOrdinal(362) ExportOrdinal(363) ExportOrdinal(364) ExportOrdinal(365) ExportOrdinal(366) ExportOrdinal(367) ExportOrdinal(368) ExportOrdinal(369) ExportOrdinal(370) ExportOrdinal(371) ExportOrdinal(372) ExportOrdinal(373) ExportOrdinal(374) ExportOrdinal(375) ExportOrdinal(376) ExportOrdinal(377) ExportOrdinal(378) ExportOrdinal(379) ExportOrdinal(380) ExportOrdinal(381) ExportOrdinal(382) ExportOrdinal(383) ExportOrdinal(384) ExportOrdinal(385) ExportOrdinal(386) ExportOrdinal(387) ExportOrdinal(388) ExportOrdinal(389) ExportOrdinal(390) ExportOrdinal(391) ExportOrdinal(392) ExportOrdinal(393) ExportOrdinal(394) ExportOrdinal(395) ExportOrdinal(396) ExportOrdinal(397) ExportOrdinal(398) ExportOrdinal(399) ExportOrdinal(400)
ExportOrdinal(401) ExportOrdinal(402) ExportOrdinal(403) ExportOrdinal(404) ExportOrdinal(405) ExportOrdinal(406) ExportOrdinal(407) ExportOrdinal(408) ExportOrdinal(409) ExportOrdinal(410) ExportOrdinal(411) ExportOrdinal(412) ExportOrdinal(413) ExportOrdinal(414) ExportOrdinal(415) ExportOrdinal(416) ExportOrdinal(417) ExportOrdinal(418) ExportOrdinal(419) ExportOrdinal(420) ExportOrdinal(421) ExportOrdinal(422) ExportOrdinal(423) ExportOrdinal(424) ExportOrdinal(425) ExportOrdinal(426) ExportOrdinal(427) ExportOrdinal(428) ExportOrdinal(429) ExportOrdinal(430) ExportOrdinal(431) ExportOrdinal(432) ExportOrdinal(433) ExportOrdinal(434) ExportOrdinal(435) ExportOrdinal(436) ExportOrdinal(437) ExportOrdinal(438) ExportOrdinal(439) ExportOrdinal(440) ExportOrdinal(441) ExportOrdinal(442) ExportOrdinal(443) ExportOrdinal(444) ExportOrdinal(445) ExportOrdinal(446) ExportOrdinal(447) ExportOrdinal(448) ExportOrdinal(449) ExportOrdinal(450)
ExportOrdinal(451) ExportOrdinal(452) ExportOrdinal(453) ExportOrdinal(454) ExportOrdinal(455) ExportOrdinal(456) ExportOrdinal(457) ExportOrdinal(458) ExportOrdinal(459) ExportOrdinal(460) ExportOrdinal(461) ExportOrdinal(462) ExportOrdinal(463) ExportOrdinal(464) ExportOrdinal(465) ExportOrdinal(466) ExportOrdinal(467) ExportOrdinal(468) ExportOrdinal(469) ExportOrdinal(470) ExportOrdinal(471) ExportOrdinal(472) ExportOrdinal(473) ExportOrdinal(474) ExportOrdinal(475) ExportOrdinal(476) ExportOrdinal(477) ExportOrdinal(478) ExportOrdinal(479) ExportOrdinal(480) ExportOrdinal(481) ExportOrdinal(482) ExportOrdinal(483) ExportOrdinal(484) ExportOrdinal(485) ExportOrdinal(486) ExportOrdinal(487) ExportOrdinal(488) ExportOrdinal(489) ExportOrdinal(490) ExportOrdinal(491) ExportOrdinal(492) ExportOrdinal(493) ExportOrdinal(494) ExportOrdinal(495) ExportOrdinal(496) ExportOrdinal(497) ExportOrdinal(498) ExportOrdinal(499) ExportOrdinal(500)
ExportOrdinal(501) ExportOrdinal(502) ExportOrdinal(503) ExportOrdinal(504) ExportOrdinal(505) ExportOrdinal(506) ExportOrdinal(507) ExportOrdinal(508) ExportOrdinal(509) ExportOrdinal(510) ExportOrdinal(511) ExportOrdinal(512) ExportOrdinal(513) ExportOrdinal(514) ExportOrdinal(515) ExportOrdinal(516) ExportOrdinal(517) ExportOrdinal(518) ExportOrdinal(519) ExportOrdinal(520) ExportOrdinal(521) ExportOrdinal(522) ExportOrdinal(523) ExportOrdinal(524) ExportOrdinal(525) ExportOrdinal(526) ExportOrdinal(527) ExportOrdinal(528) ExportOrdinal(529) ExportOrdinal(530) ExportOrdinal(531) ExportOrdinal(532) ExportOrdinal(533) ExportOrdinal(534) ExportOrdinal(535) ExportOrdinal(536) ExportOrdinal(537) ExportOrdinal(538) ExportOrdinal(539) ExportOrdinal(540) ExportOrdinal(541) ExportOrdinal(542) ExportOrdinal(543) ExportOrdinal(544) ExportOrdinal(545) ExportOrdinal(546) ExportOrdinal(547) ExportOrdinal(548) ExportOrdinal(549) ExportOrdinal(550)
ExportOrdinal(551) ExportOrdinal(552) ExportOrdinal(553) ExportOrdinal(554) ExportOrdinal(555) ExportOrdinal(556) ExportOrdinal(557) ExportOrdinal(558) ExportOrdinal(559) ExportOrdinal(560) ExportOrdinal(561) ExportOrdinal(562) ExportOrdinal(563) ExportOrdinal(564) ExportOrdinal(565) ExportOrdinal(566) ExportOrdinal(567) ExportOrdinal(568) ExportOrdinal(569) ExportOrdinal(570) ExportOrdinal(571) ExportOrdinal(572) ExportOrdinal(573) ExportOrdinal(574) ExportOrdinal(575) ExportOrdinal(576) ExportOrdinal(577) ExportOrdinal(578) ExportOrdinal(579) ExportOrdinal(580) ExportOrdinal(581) ExportOrdinal(582) ExportOrdinal(583) ExportOrdinal(584) ExportOrdinal(585) ExportOrdinal(586) ExportOrdinal(587) ExportOrdinal(588) ExportOrdinal(589) ExportOrdinal(590) ExportOrdinal(591) ExportOrdinal(592) ExportOrdinal(593) ExportOrdinal(594) ExportOrdinal(595) ExportOrdinal(596) ExportOrdinal(597) ExportOrdinal(598) ExportOrdinal(599) ExportOrdinal(600)
ExportOrdinal(601) ExportOrdinal(602) ExportOrdinal(603) ExportOrdinal(604) ExportOrdinal(605) ExportOrdinal(606) ExportOrdinal(607) ExportOrdinal(608) ExportOrdinal(609) ExportOrdinal(610) ExportOrdinal(611) ExportOrdinal(612) ExportOrdinal(613) ExportOrdinal(614) ExportOrdinal(615) ExportOrdinal(616) ExportOrdinal(617) ExportOrdinal(618) ExportOrdinal(619) ExportOrdinal(620) ExportOrdinal(621) ExportOrdinal(622) ExportOrdinal(623) ExportOrdinal(624) ExportOrdinal(625) ExportOrdinal(626) ExportOrdinal(627) ExportOrdinal(628) ExportOrdinal(629) ExportOrdinal(630) ExportOrdinal(631) ExportOrdinal(632) ExportOrdinal(633) ExportOrdinal(634) ExportOrdinal(635) ExportOrdinal(636) ExportOrdinal(637) ExportOrdinal(638) ExportOrdinal(639) ExportOrdinal(640) ExportOrdinal(641) ExportOrdinal(642) ExportOrdinal(643) ExportOrdinal(644) ExportOrdinal(645) ExportOrdinal(646) ExportOrdinal(647) ExportOrdinal(648) ExportOrdinal(649) ExportOrdinal(650)
ExportOrdinal(651) ExportOrdinal(652) ExportOrdinal(653) ExportOrdinal(654) ExportOrdinal(655) ExportOrdinal(656) ExportOrdinal(657) ExportOrdinal(658) ExportOrdinal(659) ExportOrdinal(660) ExportOrdinal(661) ExportOrdinal(662) ExportOrdinal(663) ExportOrdinal(664) ExportOrdinal(665) ExportOrdinal(666) ExportOrdinal(667) ExportOrdinal(668) ExportOrdinal(669) ExportOrdinal(670) ExportOrdinal(671) ExportOrdinal(672) ExportOrdinal(673) ExportOrdinal(674) ExportOrdinal(675) ExportOrdinal(676) ExportOrdinal(677) ExportOrdinal(678) ExportOrdinal(679) ExportOrdinal(680) ExportOrdinal(681) ExportOrdinal(682) ExportOrdinal(683) ExportOrdinal(684) ExportOrdinal(685) ExportOrdinal(686) ExportOrdinal(687) ExportOrdinal(688) ExportOrdinal(689) ExportOrdinal(690) ExportOrdinal(691) ExportOrdinal(692) ExportOrdinal(693) ExportOrdinal(694) ExportOrdinal(695) ExportOrdinal(696) ExportOrdinal(697) ExportOrdinal(698) ExportOrdinal(699) ExportOrdinal(700)
ExportOrdinal(701) ExportOrdinal(702) ExportOrdinal(703) ExportOrdinal(704) ExportOrdinal(705) ExportOrdinal(706) ExportOrdinal(707) ExportOrdinal(708) ExportOrdinal(709) ExportOrdinal(710) ExportOrdinal(711) ExportOrdinal(712) ExportOrdinal(713) ExportOrdinal(714) ExportOrdinal(715) ExportOrdinal(716) ExportOrdinal(717) ExportOrdinal(718) ExportOrdinal(719) ExportOrdinal(720) ExportOrdinal(721) ExportOrdinal(722) ExportOrdinal(723) ExportOrdinal(724) ExportOrdinal(725) ExportOrdinal(726) ExportOrdinal(727) ExportOrdinal(728) ExportOrdinal(729) ExportOrdinal(730) ExportOrdinal(731) ExportOrdinal(732) ExportOrdinal(733) ExportOrdinal(734) ExportOrdinal(735) ExportOrdinal(736) ExportOrdinal(737) ExportOrdinal(738) ExportOrdinal(739) ExportOrdinal(740) ExportOrdinal(741) ExportOrdinal(742) ExportOrdinal(743) ExportOrdinal(744) ExportOrdinal(745) ExportOrdinal(746) ExportOrdinal(747) ExportOrdinal(748) ExportOrdinal(749) ExportOrdinal(750)
ExportOrdinal(751) ExportOrdinal(752) ExportOrdinal(753) ExportOrdinal(754) ExportOrdinal(755) ExportOrdinal(756) ExportOrdinal(757) ExportOrdinal(758) ExportOrdinal(759) ExportOrdinal(760) ExportOrdinal(761) ExportOrdinal(762) ExportOrdinal(763) ExportOrdinal(764) ExportOrdinal(765) ExportOrdinal(766) ExportOrdinal(767) ExportOrdinal(768) ExportOrdinal(769) ExportOrdinal(770) ExportOrdinal(771) ExportOrdinal(772) ExportOrdinal(773) ExportOrdinal(774) ExportOrdinal(775) ExportOrdinal(776) ExportOrdinal(777) ExportOrdinal(778) ExportOrdinal(779) ExportOrdinal(780) ExportOrdinal(781) ExportOrdinal(782) ExportOrdinal(783) ExportOrdinal(784) ExportOrdinal(785) ExportOrdinal(786) ExportOrdinal(787) ExportOrdinal(788) ExportOrdinal(789) ExportOrdinal(790) ExportOrdinal(791) ExportOrdinal(792) ExportOrdinal(793) ExportOrdinal(794) ExportOrdinal(795) ExportOrdinal(796) ExportOrdinal(797) ExportOrdinal(798) ExportOrdinal(799) ExportOrdinal(800)
ExportOrdinal(801) ExportOrdinal(802) ExportOrdinal(803) ExportOrdinal(804) ExportOrdinal(805) ExportOrdinal(806) ExportOrdinal(807) ExportOrdinal(808) ExportOrdinal(809) ExportOrdinal(810) ExportOrdinal(811) ExportOrdinal(812) ExportOrdinal(813) ExportOrdinal(814) ExportOrdinal(815) ExportOrdinal(816) ExportOrdinal(817) ExportOrdinal(818) ExportOrdinal(819) ExportOrdinal(820) ExportOrdinal(821) ExportOrdinal(822) ExportOrdinal(823) ExportOrdinal(824) ExportOrdinal(825) ExportOrdinal(826) ExportOrdinal(827) ExportOrdinal(828) ExportOrdinal(829) ExportOrdinal(830) ExportOrdinal(831) ExportOrdinal(832) ExportOrdinal(833) ExportOrdinal(834) ExportOrdinal(835) ExportOrdinal(836) ExportOrdinal(837) ExportOrdinal(838) ExportOrdinal(839) ExportOrdinal(840) ExportOrdinal(841) ExportOrdinal(842) ExportOrdinal(843) ExportOrdinal(844) ExportOrdinal(845) ExportOrdinal(846) ExportOrdinal(847) ExportOrdinal(848) ExportOrdinal(849) ExportOrdinal(850)
ExportOrdinal(851) ExportOrdinal(852) ExportOrdinal(853) ExportOrdinal(854) ExportOrdinal(855) ExportOrdinal(856) ExportOrdinal(857) ExportOrdinal(858) ExportOrdinal(859) ExportOrdinal(860) ExportOrdinal(861) ExportOrdinal(862) ExportOrdinal(863) ExportOrdinal(864) ExportOrdinal(865) ExportOrdinal(866) ExportOrdinal(867) ExportOrdinal(868) ExportOrdinal(869) ExportOrdinal(870) ExportOrdinal(871) ExportOrdinal(872) ExportOrdinal(873) ExportOrdinal(874) ExportOrdinal(875) ExportOrdinal(876) ExportOrdinal(877) ExportOrdinal(878) ExportOrdinal(879) ExportOrdinal(880) ExportOrdinal(881) ExportOrdinal(882) ExportOrdinal(883) ExportOrdinal(884) ExportOrdinal(885) ExportOrdinal(886) ExportOrdinal(887) ExportOrdinal(888) ExportOrdinal(889) ExportOrdinal(890) ExportOrdinal(891) ExportOrdinal(892) ExportOrdinal(893) ExportOrdinal(894) ExportOrdinal(895) ExportOrdinal(896) ExportOrdinal(897) ExportOrdinal(898) ExportOrdinal(899) ExportOrdinal(900)
ExportOrdinal(901) ExportOrdinal(902) ExportOrdinal(903) ExportOrdinal(904) ExportOrdinal(905) ExportOrdinal(906) ExportOrdinal(907) ExportOrdinal(908) ExportOrdinal(909) ExportOrdinal(910) ExportOrdinal(911) ExportOrdinal(912) ExportOrdinal(913) ExportOrdinal(914) ExportOrdinal(915) ExportOrdinal(916) ExportOrdinal(917) ExportOrdinal(918) ExportOrdinal(919) ExportOrdinal(920) ExportOrdinal(921) ExportOrdinal(922) ExportOrdinal(923) ExportOrdinal(924) ExportOrdinal(925) ExportOrdinal(926) ExportOrdinal(927) ExportOrdinal(928) ExportOrdinal(929) ExportOrdinal(930) ExportOrdinal(931) ExportOrdinal(932) ExportOrdinal(933) ExportOrdinal(934) ExportOrdinal(935) ExportOrdinal(936) ExportOrdinal(937) ExportOrdinal(938) ExportOrdinal(939) ExportOrdinal(940) ExportOrdinal(941) ExportOrdinal(942) ExportOrdinal(943) ExportOrdinal(944) ExportOrdinal(945) ExportOrdinal(946) ExportOrdinal(947) ExportOrdinal(948) ExportOrdinal(949) ExportOrdinal(950)
ExportOrdinal(951) ExportOrdinal(952) ExportOrdinal(953) ExportOrdinal(954) ExportOrdinal(955) ExportOrdinal(956) ExportOrdinal(957) ExportOrdinal(958) ExportOrdinal(959) ExportOrdinal(960) ExportOrdinal(961) ExportOrdinal(962) ExportOrdinal(963) ExportOrdinal(964) ExportOrdinal(965) ExportOrdinal(966) ExportOrdinal(967) ExportOrdinal(968) ExportOrdinal(969) ExportOrdinal(970) ExportOrdinal(971) ExportOrdinal(972) ExportOrdinal(973) ExportOrdinal(974) ExportOrdinal(975) ExportOrdinal(976) ExportOrdinal(977) ExportOrdinal(978) ExportOrdinal(979) ExportOrdinal(980) ExportOrdinal(981) ExportOrdinal(982) ExportOrdinal(983) ExportOrdinal(984) ExportOrdinal(985) ExportOrdinal(986) ExportOrdinal(987) ExportOrdinal(988) ExportOrdinal(989) ExportOrdinal(990) ExportOrdinal(991) ExportOrdinal(992) ExportOrdinal(993) ExportOrdinal(994) ExportOrdinal(995) ExportOrdinal(996) ExportOrdinal(997) ExportOrdinal(998) ExportOrdinal(999) ExportOrdinal(1000)
ExportOrdinal(1001) ExportOrdinal(1002) ExportOrdinal(1003) ExportOrdinal(1004) ExportOrdinal(1005) ExportOrdinal(1006) ExportOrdinal(1007) ExportOrdinal(1008) ExportOrdinal(1009) ExportOrdinal(1010) ExportOrdinal(1011) ExportOrdinal(1012) ExportOrdinal(1013) ExportOrdinal(1014) ExportOrdinal(1015) ExportOrdinal(1016) ExportOrdinal(1017) ExportOrdinal(1018) ExportOrdinal(1019)

// dxgi
Export(0, ApplyCompatResolutionQuirking) Export(1, CompatString) Export(2, CompatValue) Export(3, DXGIDumpJournal) Export(4, PIXBeginCapture) Export(5, PIXEndCapture) Export(6, PIXGetCaptureState) Export(7, SetAppCompatStringPointer) Export(8, UpdateHMDEmulationStatus) Export(9, CreateDXGIFactory) Export(10, CreateDXGIFactory1) Export(11, CreateDXGIFactory2) Export(12, DXGID3D10CreateDevice) Export(13, DXGID3D10CreateLayeredDevice) Export(14, DXGID3D10GetLayeredDeviceSize) Export(15, DXGID3D10RegisterLayers) Export(16, DXGIDeclareAdapterRemovalSupport) Export(17, DXGIGetDebugInterface1) Export(18, DXGIReportAdapterConfiguration)

// version
Export(0, GetFileVersionInfoA) Export(1, GetFileVersionInfoByHandle) Export(2, GetFileVersionInfoExA) Export(3, GetFileVersionInfoExW) Export(4, GetFileVersionInfoSizeA) Export(5, GetFileVersionInfoSizeExA) Export(6, GetFileVersionInfoSizeExW) Export(7, GetFileVersionInfoSizeW) Export(8, GetFileVersionInfoW) Export(9, VerFindFileA) Export(10, VerFindFileW) Export(11, VerInstallFileA) Export(12, VerInstallFileW) Export(13, VerLanguageNameA) Export(14, VerLanguageNameW) Export(15, VerQueryValueA) Export(16, VerQueryValueW)

// bcrypt
Export(0, BCryptAddContextFunction) Export(1, BCryptAddContextFunctionProvider) Export(2, BCryptCloseAlgorithmProvider) Export(3, BCryptConfigureContext) Export(4, BCryptConfigureContextFunction) Export(5, BCryptCreateContext) Export(6, BCryptCreateHash) Export(7, BCryptCreateMultiHash) Export(8, BCryptDecrypt) Export(9, BCryptDeleteContext) Export(10, BCryptDeriveKey) Export(11, BCryptDeriveKeyCapi) Export(12, BCryptDeriveKeyPBKDF2) Export(13, BCryptDestroyHash) Export(14, BCryptDestroyKey) Export(15, BCryptDestroySecret) Export(16, BCryptDuplicateHash) Export(17, BCryptDuplicateKey) Export(18, BCryptEncrypt) Export(19, BCryptEnumAlgorithms) Export(20, BCryptEnumContextFunctionProviders) Export(21, BCryptEnumContextFunctions) Export(22, BCryptEnumContexts) Export(23, BCryptEnumProviders) Export(24, BCryptEnumRegisteredProviders) Export(25, BCryptExportKey) Export(26, BCryptFinalizeKeyPair) Export(27, BCryptFinishHash) Export(28, BCryptFreeBuffer) Export(29, BCryptGenRandom) Export(30, BCryptGenerateKeyPair) Export(31, BCryptGenerateSymmetricKey) Export(32, BCryptGetFipsAlgorithmMode) Export(33, BCryptGetProperty) Export(34, BCryptHash) Export(35, BCryptHashData) Export(36, BCryptImportKey) Export(37, BCryptImportKeyPair) Export(38, BCryptKeyDerivation) Export(39, BCryptOpenAlgorithmProvider) Export(40, BCryptProcessMultiOperations) Export(41, BCryptQueryContextConfiguration) Export(42, BCryptQueryContextFunctionConfiguration) Export(43, BCryptQueryContextFunctionProperty) Export(44, BCryptQueryProviderRegistration) Export(45, BCryptRegisterConfigChangeNotify) Export(46, BCryptRegisterProvider) Export(47, BCryptRemoveContextFunction) Export(48, BCryptRemoveContextFunctionProvider) Export(49, BCryptResolveProviders)
Export(50, BCryptSecretAgreement) Export(51, BCryptSetAuditingInterface) Export(52, BCryptSetContextFunctionProperty) Export(53, BCryptSetProperty) Export(54, BCryptSignHash) Export(55, BCryptUnregisterConfigChangeNotify) Export(56, BCryptUnregisterProvider) Export(57, BCryptVerifySignature)

#pragma warning(disable:4996)
#pragma warning(disable:4200)
#include "IDAController.h"
#include <string>
#include "LogOperation.h"

#define strtoul10(X) strtoul(X, NULL, 10)

//DB Related
#include "sqlite3.h"
#include "DBWrapper.h"
#include "DataBaseWriter.h"

#include <hash_set>
using namespace std;
using namespace stdext;

#include "LogOperation.h"

extern LogOperation Logger;

#define DEBUG_LEVEL 0

char *MapInfoTypesStr[] = {"Call", "Cref From", "Cref To", "Dref From", "Dref To"};
int types[] = {CREF_FROM, CREF_TO, CALL, DREF_FROM, DREF_TO, CALLED};

IDAController::IDAController(DBWrapper *storage_db):
	ClientAnalysisInfo(NULL),
	TargetFunctionAddress(0),
	m_OriginalFilePath(NULL),
	DisasmLine(NULL),
	Socket(INVALID_SOCKET),
	m_FileID(0)
{
	ClientAnalysisInfo = new AnalysisInfo;
	m_StorageDB = storage_db;
}

IDAController::~IDAController()
{
	if(m_OriginalFilePath)
		free(m_OriginalFilePath);

	if( ClientAnalysisInfo )
	{
		ClientAnalysisInfo->name_hash_map.clear();

		multimap <DWORD,  PMapInfo>::iterator map_info_hash_map_iter;
		for(map_info_hash_map_iter = ClientAnalysisInfo->map_info_hash_map.begin();
			map_info_hash_map_iter != ClientAnalysisInfo->map_info_hash_map.end();
			map_info_hash_map_iter++)
		{
			if( map_info_hash_map_iter->second )
				delete map_info_hash_map_iter->second;
		}

		ClientAnalysisInfo->map_info_hash_map.clear();

		multimap <DWORD, unsigned char *>::iterator address_fingerprint_hash_map_Iter;
		for(address_fingerprint_hash_map_Iter = ClientAnalysisInfo->address_fingerprint_hash_map.begin();
			address_fingerprint_hash_map_Iter != ClientAnalysisInfo->address_fingerprint_hash_map.end();
			address_fingerprint_hash_map_Iter++)
		{
			if( address_fingerprint_hash_map_Iter->second )
			{
				free( address_fingerprint_hash_map_Iter->second );
			}
		}
		ClientAnalysisInfo->address_fingerprint_hash_map.clear();
		ClientAnalysisInfo->fingerprint_hash_map.clear();

		delete ClientAnalysisInfo;
	}
}

int ReadBasicBlockDataCallback(void *arg, int argc, char **argv, char **names)
{
	AnalysisInfo *ClientAnalysisInfo = (AnalysisInfo *)arg;
	if (argv[1] && argv[1][0] != NULL)
	{
		DWORD Address = strtoul10(argv[0]);
		unsigned char *FingerprintStr = HexToBytesWithLengthAmble(argv[1]);
		if (FingerprintStr)
		{
			ClientAnalysisInfo->address_fingerprint_hash_map.insert(AddressFingerPrintAddress_Pair(Address, FingerprintStr));
		}

		if (strtoul10(argv[3]) == 1 && strlen(argv[2]) > 0)
		{
			char *name = argv[2];
			ClientAnalysisInfo->name_hash_map.insert(NameAddress_Pair(name, Address));
		}
	}
	return 0;
}

static int ReadFunctionAddressesCallback(void *arg, int argc, char **argv, char **names)
{
	hash_set <DWORD> *FunctionAddressHash = (hash_set <DWORD> *)arg;
	if (FunctionAddressHash)
	{
#if DEBUG_LEVEL > 1
		if (DebugLevel & 1) Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d strtoul10(%s) = 0x%X\n", __FUNCTION__, m_FileID, argv[0], strtoul10(argv[0]));
#endif
		FunctionAddressHash->insert(strtoul10(argv[0]));
	}
	return 0;
}

static int ReadFunctionMemberAddressesCallback(void *arg, int argc, char **argv, char **names)
{
	list <BLOCK> *p_address_list = (list <BLOCK> *)arg;
	if (p_address_list)
	{
#if DEBUG_LEVEL > 1
		if (DebugLevel & 1) Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d strtoul10(%s) = 0x%X\n", __FUNCTION__, m_FileID, argv[0], strtoul10(argv[0]));
#endif
		BLOCK block;
		block.Start = strtoul10(argv[0]);
		block.End = strtoul10(argv[1]);
		p_address_list->push_back(block);
	}
	return 0;
}

void IDAController::SetSocket(SOCKET socket)
{
	Socket = socket;
}

BOOL IDAController::LoadIDARawDataFromSocket(SOCKET socket)
{
	Socket = socket;
	ClientAnalysisInfo = NULL;
	char shared_memory_name[1024];
	_snprintf(shared_memory_name, 1024, TEXT("DG Shared Memory - %u - %u"),
		GetCurrentProcessId(), 
		GetCurrentThreadId());

	Logger.Log( 10, LOG_IDA_CONTROLLER, "%s: ID = %d InitDataSharer\n", __FUNCTION__);

#define SHARED_MEMORY_SIZE 100000
	if(!InitDataSharer(&IDADataSharer, 
		shared_memory_name, 
		SHARED_MEMORY_SIZE, 
		TRUE))
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d InitDataSharer failed\n", __FUNCTION__);
		return FALSE;
	}
	char data[1024+sizeof(DWORD)];
	*(DWORD *)data = SHARED_MEMORY_SIZE;
	memcpy(data+sizeof(DWORD), shared_memory_name, strlen(shared_memory_name)+1);
	
	Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d SendTLVData SEND_ANALYSIS_DATA\n", __FUNCTION__);

	if(SendTLVData(SEND_ANALYSIS_DATA, (PBYTE)data, sizeof(DWORD)+strlen(shared_memory_name)+1))
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d LoadIDARawData\n", __FUNCTION__);
		LoadIDARawData((PBYTE (*)(PVOID Context, BYTE *Type, DWORD *Length))GetData, (PVOID)&IDADataSharer);
		return TRUE;
	}
	return FALSE;
}

DWORD *IDAController::GetMappedAddresses(DWORD address, int type, int *p_length)
{
	DWORD *addresses = NULL;
	int current_size = 50;

	addresses = (DWORD *)malloc(sizeof(DWORD)*current_size);
	int addresses_i = 0;

	multimap <DWORD, PMapInfo> *p_map_info_hash_map;

	if (ClientAnalysisInfo && ClientAnalysisInfo->map_info_hash_map.size()>0)
	{
		p_map_info_hash_map = &ClientAnalysisInfo->map_info_hash_map;
	}
	else
	{
		p_map_info_hash_map = new multimap <DWORD, PMapInfo>();
		LoadMapInfo(p_map_info_hash_map, address);
	}

	multimap <DWORD, PMapInfo>::iterator map_info_hash_map_pIter;
	
	for (map_info_hash_map_pIter = p_map_info_hash_map->find(address);
		map_info_hash_map_pIter != p_map_info_hash_map->end();
		map_info_hash_map_pIter++
		)
	{
		if (map_info_hash_map_pIter->first != address)
			break;
		if (map_info_hash_map_pIter->second->Type == type)
		{
			//map_info_hash_map_pIter->second->Dst
			//TODO: add
			if (current_size < addresses_i + 2)
			{
				current_size += 50;
				addresses = (DWORD *)realloc(addresses, sizeof(DWORD)*(current_size));
			}
			addresses[addresses_i] = map_info_hash_map_pIter->second->Dst;
			addresses_i++;
			addresses[addresses_i] = NULL;
		}
	}

	if (!ClientAnalysisInfo)
	{
		p_map_info_hash_map->clear();
		free(p_map_info_hash_map);
	}

	if(p_length)
		*p_length = addresses_i;
	if(addresses_i  ==  0)
	{
		free(addresses);
		addresses = NULL;
	}
	return addresses;
}


list <DWORD> *IDAController::GetFunctionAddresses()
{
	if (TargetFunctionAddress != 0)
	{
		list <DWORD> *function_addresses = new list<DWORD>;
		if (function_addresses)
		{
			function_addresses->push_back(TargetFunctionAddress);
		}

		return function_addresses;
	}

	int DoCrefFromCheck = FALSE;
	int DoCallCheck = TRUE;
	hash_set <DWORD> function_address_hash;
	hash_map <DWORD, short> addresses;

	if (DoCrefFromCheck)
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "addresses.size() = %u\n", addresses.size());
		for (multimap <DWORD, PMapInfo>::iterator it = ClientAnalysisInfo->map_info_hash_map.begin();
			it != ClientAnalysisInfo->map_info_hash_map.end();
			it++
			)
		{
			Logger.Log(10, LOG_IDA_CONTROLLER, "%X-%X(%s) ", it->first, it->second->Dst, MapInfoTypesStr[it->second->Type]);
			if (it->second->Type == CREF_FROM)
			{
				hash_map <DWORD, short>::iterator iter = addresses.find(it->second->Dst);
				if (iter != addresses.end())
				{
					iter->second = FALSE;
				}
			}
		}
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s\n", __FUNCTION__);
		multimap <DWORD, unsigned char *>::iterator address_fingerprint_hash_map_iter;
		for (address_fingerprint_hash_map_iter = ClientAnalysisInfo->address_fingerprint_hash_map.begin();
			address_fingerprint_hash_map_iter != ClientAnalysisInfo->address_fingerprint_hash_map.end();
			address_fingerprint_hash_map_iter++)
		{
			addresses.insert(pair<DWORD, short>(address_fingerprint_hash_map_iter->first, DoCrefFromCheck ? TRUE : FALSE));
		}
		
		Logger.Log(10, LOG_IDA_CONTROLLER, "addresses.size() = %u\n", addresses.size());
		for (hash_map <DWORD, short>::iterator it = addresses.begin(); it != addresses.end(); it++)
		{
			if (it->second)
			{
				Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Function %X\n", __FUNCTION__, m_FileID, it->first);
				function_address_hash.insert(it->first);
			}
		}
	}
	else
	{
		if (m_StorageDB)
			m_StorageDB->ExecuteStatement(ReadFunctionAddressesCallback, &function_address_hash, "SELECT DISTINCT(FunctionAddress) FROM BasicBlock WHERE FileID = %u AND BlockType = %u", m_FileID, FUNCTION_BLOCK);
	}

	if (DoCallCheck && ClientAnalysisInfo)
	{
		for (multimap <DWORD, PMapInfo>::iterator it = ClientAnalysisInfo->map_info_hash_map.begin();
			it != ClientAnalysisInfo->map_info_hash_map.end();
			it++
			)
		{
			if (it->second->Type == CALL)
			{
				if (function_address_hash.find(it->second->Dst) == function_address_hash.end())
				{
					Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Function %X (by Call Recognition)\n", __FUNCTION__, m_FileID, it->second->Dst);
					function_address_hash.insert(it->second->Dst);
				}
			}
		}
	}

	list <DWORD> *function_addresses = new list<DWORD>;
	if (function_addresses)
	{
		for (hash_set <DWORD>::iterator it = function_address_hash.begin();
			it != function_address_hash.end();
			it++)
		{
			function_addresses->push_back(*it);
			Logger.Log(11, LOG_IDA_CONTROLLER, "%s: ID = %d Function %X\n", __FUNCTION__, m_FileID, *it);
		}
		
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Returns(%u entries)\n", __FUNCTION__, m_FileID, function_addresses->size());
	}
	return function_addresses;
}

#undef USE_LEGACY_MAP_FOR_ADDRESS_HASH_MAP
void IDAController::RemoveFromFingerprintHash(DWORD address)
{
	unsigned char *Fingerprint = NULL;

	char *FingerprintStr = NULL;

	if (m_StorageDB)
		m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordStringCallback, &FingerprintStr, "SELECT Fingerprint FROM BasicBlock WHERE FileID = %u and StartAddress = %u", m_FileID, address);
	if (FingerprintStr)
	{
		Fingerprint = HexToBytesWithLengthAmble(FingerprintStr);
	}

	if (Fingerprint)
	{
		multimap <unsigned char *, DWORD, hash_compare_fingerprint>::iterator fingerprint_hash_map_PIter;
		for (fingerprint_hash_map_PIter = ClientAnalysisInfo->fingerprint_hash_map.find(Fingerprint);
			fingerprint_hash_map_PIter != ClientAnalysisInfo->fingerprint_hash_map.end();
			fingerprint_hash_map_PIter++
			)
		{
			if (!IsEqualByteWithLengthAmble(fingerprint_hash_map_PIter->first, Fingerprint))
				break;
			if (fingerprint_hash_map_PIter->second == address)
			{
				ClientAnalysisInfo->fingerprint_hash_map.erase(fingerprint_hash_map_PIter);
				break;
			}
		}
		free(Fingerprint);
	}
}

char *IDAController::GetFingerPrintStr(DWORD address)
{
	if (ClientAnalysisInfo && ClientAnalysisInfo->address_fingerprint_hash_map.size()>0)
	{
		multimap <DWORD , unsigned char *>::iterator address_fingerprint_hash_map_PIter = ClientAnalysisInfo->address_fingerprint_hash_map.find(address);
		if(address_fingerprint_hash_map_PIter != ClientAnalysisInfo->address_fingerprint_hash_map.end())
		{
			return BytesWithLengthAmbleToHex(address_fingerprint_hash_map_PIter->second);
		}
	}else
	{
		char *FingerprintPtr = NULL;
		if( m_StorageDB )
			m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordStringCallback, &FingerprintPtr, "SELECT Fingerprint FROM BasicBlock WHERE FileID = %u and StartAddress = %u", m_FileID, address);
		return FingerprintPtr;
	}
	return NULL;
}

char *IDAController::GetName(DWORD address)
{
#ifdef USE_LEGACY_MAP
	multimap <DWORD,  string>::iterator address_name_hash_map_iter;

	address_name_hash_map_iter = ClientAnalysisInfo->address_name_hash_map.find(address);
	if(address_name_hash_map_iter != ClientAnalysisInfo->address_name_hash_map.end())
	{
		return _strdup((*address_name_hash_map_iter).second.c_str());
	}
	return NULL;
#else
	char *Name = NULL;
	if( m_StorageDB )
		m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordStringCallback, &Name, "SELECT Name FROM BasicBlock WHERE FileID = %u and StartAddress = %u", m_FileID, address);
	return Name;
#endif
}

DWORD IDAController::GetBlockAddress(DWORD address)
{
#ifdef USE_LEGACY_MAP
	while(1)
	{
		if(ClientAnalysisInfo->address_hash_map.find(address) != ClientAnalysisInfo->address_hash_map.end())
			break;
		address--;
	}
	return address;
#else
	DWORD BlockAddress = address;
	if( m_StorageDB )
		m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordIntegerCallback, &BlockAddress, "SELECT StartAddress FROM BasicBlock WHERE FileID = %u and StartAddress <=  %u  and %u <=  EndAddress LIMIT 1", m_FileID, address, address);
	return BlockAddress;
#endif
}

void IDAController::DumpBlockInfo(DWORD block_address)
{
	int addresses_number;
	char *type_descriptions[] = {"Cref From", "Cref To", "Call", "Dref From", "Dref To"};
	for(int i = 0;i<sizeof(types)/sizeof(int);i++)
	{
		DWORD *addresses = GetMappedAddresses(
			block_address, 
			types[i], 
			&addresses_number);
		if(addresses)
		{
			Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d %s: ", __FUNCTION__, m_FileID, type_descriptions[i]);
			for(int j = 0;j<addresses_number;j++)
			{
				Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d %X ", __FUNCTION__, m_FileID, addresses[j]);
			}
			Logger.Log(10, LOG_IDA_CONTROLLER, "\n");
		}
	}
	char *hex_str = GetFingerPrintStr(block_address);
	if(hex_str)
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d fingerprint: %s\n", __FUNCTION__, m_FileID, hex_str);
		free(hex_str);
	}
}

const char *GetAnalysisDataTypeStr(int type)
{
	static const char *Types[] = {"BASIC_BLOCK", "MAP_INFO", "FILE_INFO", "END_OF_DATA"};
	if(type<sizeof(Types)/sizeof(Types[0]))
		return Types[type];
	return "Unknown";
}

enum {TYPE_FILE_INFO, TYPE_ADDRESS_HASH_MAP, TYPE_ADDRESS_DISASSEMBLY_MAP, TYPE_FINGERPRINT_HASH_MAP, TYPE_TWO_LEVEL_FINGERPRINT_HASH_MAP, TYPE_ADDRESS_FINGERPRINT_HASH_MAP, TYPE_NAME_HASH_MAP, TYPE_ADDRESS_NAME_HASH_MAP, TYPE_MAP_INFO_HASH_MAP};

const char *GetFileDataTypeStr(int type)
{
	static const char *Types[] = {"FILE_INFO", "ADDRESS_HASH_MAP", "ADDRESS_DISASSEMBLY_MAP", "FINGERPRINT_HASH_MAP", "TWO_LEVEL_FINGERPRINT_HASH_MAP", "ADDRESS_FINGERPRINT_HASH_MAP", "NAME_HASH_MAP", "ADDRESS_NAME_HASH_MAP", "MAP_INFO_HASH_MAP"};
	if(type<sizeof(Types)/sizeof(Types[0]))
		return Types[type];
	return "Unknown";
}

BOOL IDAController::Save(char *DataFile, DWORD Offset, DWORD dwMoveMethod, hash_set <DWORD> *pSelectedAddresses)
{
	return TRUE;
}

BOOL IDAController::Retrieve(char *DataFile, DWORD Offset, DWORD Length)
{
	return TRUE;
}

int ReadMapInfoCallback(void *arg, int argc, char **argv, char **names)
{
	//Logger.Log( 10, "%s: %s %s %s %s\n", __FUNCTION__, m_FileID, argv[0], argv[1], argv[2], argv[3]);
	multimap <DWORD, PMapInfo> *p_map_info_hash_map = (multimap <DWORD, PMapInfo> *)arg;

	PMapInfo p_map_info = new MapInfo;
	p_map_info->Type = strtoul10(argv[0]);
	p_map_info->SrcBlock = strtoul10(argv[1]);
	p_map_info->SrcBlockEnd = strtoul10(argv[2]);
	p_map_info->Dst = strtoul10(argv[3]);
#if DEBUG_LEVEL > 1
	Logger.Log( 10, "%s: ID = %d strtoul10(%s) = 0x%X, strtoul10(%s) = 0x%X, strtoul10(%s) = 0x%X, strtoul10(%s) = 0x%X\n", __FUNCTION__, m_FileID, 
		argv[0], strtoul10(argv[0]), 
		argv[1], strtoul10(argv[1]), 
		argv[2], strtoul10(argv[2]), 
		argv[3], strtoul10(argv[3])
	);
#endif
	p_map_info_hash_map->insert(AddrPMapInfo_Pair(p_map_info->SrcBlock, p_map_info));
	return 0;
}

char *IDAController::GetOriginalFilePath()
{
	return m_OriginalFilePath;
}

BOOL IDAController::LoadBasicBlock()
{
	if (ClientAnalysisInfo->fingerprint_hash_map.size() == 0)
	{
		char FunctionAddressConditionBuffer[50] = { 0, };
		if (TargetFunctionAddress)
		{
			_snprintf(FunctionAddressConditionBuffer, sizeof(FunctionAddressConditionBuffer)-1, "AND FunctionAddress = '%d'", TargetFunctionAddress);
		}

		if (m_StorageDB)
			m_StorageDB->ExecuteStatement(ReadBasicBlockDataCallback,
			(void *)ClientAnalysisInfo,
			"SELECT StartAddress, Fingerprint, Name, BlockType FROM BasicBlock WHERE FileID = %u %s",
			m_FileID,
			FunctionAddressConditionBuffer);

		GenerateFingerprintHashMap();
	}
	return TRUE;
}

/*
FunctionAddress = 0 : Retrieve All Functions
	else			: Retrieve That Specific Function
*/

void IDAController::SetFileID(int FileID)
{
	m_FileID = FileID;
}

void IDAController::LoadMapInfo(multimap <DWORD, PMapInfo> *p_map_info_hash_map, DWORD Address, bool IsFunction)
{
	if (Address == 0)
	{
		m_StorageDB->ExecuteStatement(ReadMapInfoCallback, (void *)p_map_info_hash_map,
			"SELECT Type, SrcBlock, SrcBlockEnd, Dst From MapInfo WHERE FileID = %u", 
			m_FileID);
	}
	else
	{
		if (IsFunction)
		{
			m_StorageDB->ExecuteStatement(ReadMapInfoCallback, (void *)p_map_info_hash_map,
				"SELECT Type, SrcBlock, SrcBlockEnd, Dst From MapInfo "
				"WHERE FileID = %u "
				"AND ( SrcBlock IN ( SELECT StartAddress FROM BasicBlock WHERE FunctionAddress='%d') )",
				m_FileID, Address);
		}
		else
		{
			m_StorageDB->ExecuteStatement(ReadMapInfoCallback, (void *)p_map_info_hash_map,
				"SELECT Type, SrcBlock, SrcBlockEnd, Dst From MapInfo "
				"WHERE FileID = %u "
				"AND SrcBlock  = '%d'",
				m_FileID, Address);
		}
	}

	BuildCrefToMap(p_map_info_hash_map);
}


void IDAController::BuildCrefToMap(multimap <DWORD, PMapInfo> *p_map_info_hash_map)
{
	for (multimap <DWORD, PMapInfo>::iterator it = p_map_info_hash_map->begin();
		it != p_map_info_hash_map->end();
		it++
	)
	{
		if (it->second->Type == CREF_FROM)
		{
			CrefToMap.insert(pair<DWORD, DWORD>(it->second->Dst, it->first));
		}
	}
}

BOOL IDAController::Load()
{
	Logger.Log(10, LOG_IDA_CONTROLLER, "%s: %s\n", __FUNCTION__, m_StorageDB->GetDatabaseName());

	m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordStringCallback, &m_OriginalFilePath, "SELECT OriginalFilePath FROM FileInfo WHERE id = %u", m_FileID);

	LoadBasicBlock();
	LoadMapInfo(&(ClientAnalysisInfo->map_info_hash_map), TargetFunctionAddress, true);

	return TRUE;
}

void IDAController::DeleteMatchInfo( DBWrapper *InputDB, int FileID, DWORD FunctionAddress )
{
	if( m_StorageDB )
	{
		m_StorageDB->ExecuteStatement( NULL, (void *)ClientAnalysisInfo, "DELETE FROM  MatchMap WHERE TheSourceFileID='%d' AND TheSourceAddress IN (SELECT StartAddress FROM BasicBlock WHERE FileID = '%d' AND FunctionAddress='%d')", FileID, FileID, FunctionAddress );
		m_StorageDB->ExecuteStatement( NULL, (void *)ClientAnalysisInfo, "DELETE FROM  FunctionMatchInfo WHERE TheSourceFileID='%d' AND TheSourceAddress ='%d'", FileID, FunctionAddress );

		m_StorageDB->ExecuteStatement( NULL, (void *)ClientAnalysisInfo, "DELETE FROM  MatchMap WHERE TheTargetFileID='%d' AND TheTargetAddress IN (SELECT StartAddress FROM BasicBlock WHERE FileID = '%d' AND FunctionAddress='%d')", FileID, FileID, FunctionAddress );
		m_StorageDB->ExecuteStatement( NULL, (void *)ClientAnalysisInfo, "DELETE FROM  FunctionMatchInfo WHERE TheTargetFileID='%d' AND TheTargetAddress ='%d'", FileID, FunctionAddress );
	}
}

void IDAController::AddAnalysisTargetFunction( DWORD FunctionAddress )
{
	Logger.Log(10, LOG_IDA_CONTROLLER, "Add Analysis Target Function: %X\n", FunctionAddress);
	TargetFunctionAddress = FunctionAddress;
}

typedef struct {
	DWORD address;
	DWORD child_address;
} AddressPair;

void IDAController::LoadIDARawData(PBYTE (*RetrieveCallback)(PVOID Context, BYTE *Type, DWORD *Length), PVOID Context)
{
	BYTE type;
	DWORD length;

	multimap <DWORD,  PBasicBlock>::iterator address_hash_map_pIter;
	multimap <string,  DWORD>::iterator fingerprint_hash_map_pIter;
	multimap <string,  DWORD>::iterator name_hash_map_pIter;
	multimap <DWORD,  PMapInfo>::iterator map_info_hash_map_pIter;

	DWORD current_addr = 0L;

	if( m_StorageDB )
		m_StorageDB->BeginTransaction();
	while(1)
	{	
		PBYTE data = RetrieveCallback(Context, &type, &length);
#if DEBUG_LEVEL > 0
		Logger.Log( 10, "%s: ID = %d type = %u Data(0x%X) is Read %u Bytes Long\n", __FUNCTION__, m_FileID, type, data, length);
#endif

		if(type  ==  END_OF_DATA)
		{
#if DEBUG_LEVEL > -1
			Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d End of Analysis\n", __FUNCTION__);
			Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d address_hash_map:%u/address_fingerprint_hash_map:%u/fingerprint_hash_map:%u/name_hash_map:%u/map_info_hash_map:%u\n",
				__FUNCTION__, m_FileID, 
				ClientAnalysisInfo->address_hash_map.size(), 
				ClientAnalysisInfo->address_fingerprint_hash_map.size(), 
				ClientAnalysisInfo->fingerprint_hash_map.size(), 
				ClientAnalysisInfo->name_hash_map.size(), 
				ClientAnalysisInfo->map_info_hash_map.size()
			);
#endif
			if(data)
				free(data);
			break;
		}
		if(!data)
			continue;

		if( m_StorageDB )
			m_FileID = DatabaseWriterWrapper(m_StorageDB, type, data, length);
		if(type  ==  BASIC_BLOCK && sizeof(BasicBlock)<= length)
		{
			PBasicBlock pBasicBlock = (PBasicBlock)data;
			current_addr = pBasicBlock->StartAddress;
			Logger.Log(11, LOG_IDA_CONTROLLER, "%s: ID = %d BASIC_BLOCK[StartAddress = %X Flag = %u function addr = %X BlockType = %u]\n", __FUNCTION__, m_FileID,
				pBasicBlock->StartAddress, //ea_t
				pBasicBlock->Flag,  //Flag_t
				pBasicBlock->FunctionAddress, 
				pBasicBlock->BlockType);
#ifdef USE_LEGACY_MAP
			ClientAnalysisInfo->address_hash_map.insert(AddrPBasicBlock_Pair(pBasicBlock->StartAddress, pBasicBlock) );			
#endif
			ClientAnalysisInfo->name_hash_map.insert(NameAddress_Pair(pBasicBlock->Data, pBasicBlock->StartAddress));
			if(pBasicBlock->FingerprintLen>0)
			{
				unsigned char *FingerprintBuffer = (unsigned char *)malloc(pBasicBlock->FingerprintLen+sizeof(short));
				*(unsigned short *)FingerprintBuffer = pBasicBlock->FingerprintLen;
				memcpy(FingerprintBuffer+sizeof(short), pBasicBlock->Data+pBasicBlock->NameLen+pBasicBlock->DisasmLinesLen, *(unsigned short *)FingerprintBuffer);
				ClientAnalysisInfo->address_fingerprint_hash_map.insert(AddressFingerPrintAddress_Pair(pBasicBlock->StartAddress, FingerprintBuffer));
			}
			free(data);
		}else if(type  ==  MAP_INFO && length  ==  sizeof(MapInfo))
		{
			PMapInfo p_map_info = (PMapInfo)data;
#if DEBUG_LEVEL > 2
			Logger.Log( 10, "%s: ID = %d %s %X(%X)->%X\n", __FUNCTION__, m_FileID, 
				MapInfoTypesStr[p_map_info->Type], 
				p_map_info->SrcBlock, 
				p_map_info->SrcBlockEnd, 
				p_map_info->Dst);
#endif
			ClientAnalysisInfo->map_info_hash_map.insert(AddrPMapInfo_Pair(p_map_info->SrcBlock, p_map_info));
			/*
			We don't use backward CFG anymore.
			if(p_map_info->Type  ==  CREF_FROM || p_map_info->Type  ==  CALL)
			{
				PMapInfo p_new_map_info = (PMapInfo)malloc(sizeof(MapInfo));
				p_new_map_info->SrcBlock = p_map_info->Dst;
				p_new_map_info->Src = p_map_info->Dst;
				p_new_map_info->Dst = p_map_info->SrcBlock;
				if(p_map_info->Type  ==  CREF_FROM)
					p_new_map_info->Type = CREF_TO;
				else
					p_new_map_info->Type = CALLED;
				ClientAnalysisInfo->map_info_hash_map.insert(AddrPMapInfo_Pair(p_new_map_info->SrcBlock, p_new_map_info));
			}*/
		}else
		{
			free(data);
		}
	}

	if( m_StorageDB )
		m_StorageDB->EndTransaction();
	FixFunctionAddresses();
	GenerateFingerprintHashMap();
}

void IDAController::GenerateFingerprintHashMap()
{
	multimap <DWORD,  PBasicBlock>::iterator address_hash_map_pIter;
	list <AddressPair> AddressPairs;
	multimap <DWORD, PBasicBlock>::iterator iter;
	for(iter = ClientAnalysisInfo->address_hash_map.begin();
		iter != ClientAnalysisInfo->address_hash_map.end();
		iter++)
	{
		DWORD address = iter->first;
		multimap <DWORD,  PMapInfo>::iterator map_info_hash_map_iter;
		int matched_children_count = 0;
		DWORD matched_child_addr = 0L;
		for(map_info_hash_map_iter = ClientAnalysisInfo->map_info_hash_map.find(address);
			map_info_hash_map_iter != ClientAnalysisInfo->map_info_hash_map.end();
			map_info_hash_map_iter++
			)
		{
			if(map_info_hash_map_iter->first != address)
				break;
			PMapInfo p_map_info = map_info_hash_map_iter->second;
			if(p_map_info->Type  ==  CREF_FROM)
			{
				matched_child_addr = map_info_hash_map_iter->second->Dst;
				matched_children_count++;
			}
		}
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d 0x%X children count: %u\n", __FUNCTION__, m_FileID, address, matched_children_count);
		if(matched_children_count  ==  1 && matched_child_addr != 0L)
		{
			int matched_parents_count = 0;
			for(map_info_hash_map_iter = ClientAnalysisInfo->map_info_hash_map.find(matched_child_addr);
				map_info_hash_map_iter != ClientAnalysisInfo->map_info_hash_map.end();
				map_info_hash_map_iter++
				)
			{
				if(map_info_hash_map_iter->first != matched_child_addr)
					break;
				PMapInfo p_map_info = map_info_hash_map_iter->second;
				if(p_map_info->Type  ==  CREF_TO || p_map_info->Type  ==  CALLED)
					matched_parents_count++;
			}
			Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d 0x%X -> 0x%X parent count: %u\n", __FUNCTION__, m_FileID, address, matched_child_addr, matched_parents_count);
			if(matched_parents_count  ==  1)
			{
				address_hash_map_pIter = ClientAnalysisInfo->address_hash_map.find(matched_child_addr);
				if(address_hash_map_pIter != ClientAnalysisInfo->address_hash_map.end())
				{
					PBasicBlock pBasicBlock = (PBasicBlock)address_hash_map_pIter->second;
					if(pBasicBlock->FunctionAddress != matched_child_addr)
					{
						AddressPair address_pair;
						address_pair.address = address;
						address_pair.child_address = matched_child_addr;
						AddressPairs.push_back(address_pair);
					}
				}
			}
		}
	}

	list <AddressPair>::iterator AddressPairsIter;
	for(AddressPairsIter = AddressPairs.begin();
		AddressPairsIter != AddressPairs.end();
		AddressPairsIter++)
	{
		DWORD address = (*AddressPairsIter).address;
		DWORD child_address = (*AddressPairsIter).child_address;
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Joining 0x%X-0x%X\n", __FUNCTION__, m_FileID, address, child_address);

		DWORD matched_child_addr = 0L;

		multimap <DWORD,  PMapInfo>::iterator map_info_hash_map_iter;
		for(map_info_hash_map_iter = ClientAnalysisInfo->map_info_hash_map.find(child_address);
			map_info_hash_map_iter != ClientAnalysisInfo->map_info_hash_map.end();
			map_info_hash_map_iter++
			)
		{
			if(map_info_hash_map_iter->first != child_address)
				break;
			PMapInfo p_map_info = map_info_hash_map_iter->second;
			PMapInfo p_new_map_info = (PMapInfo)malloc(sizeof(MapInfo));
			p_new_map_info->SrcBlockEnd = address;
			p_new_map_info->SrcBlock = address;
			p_new_map_info->Dst = p_map_info->Dst;
			p_new_map_info->Type = p_map_info->Type;
			ClientAnalysisInfo->map_info_hash_map.insert(AddrPMapInfo_Pair(address, p_new_map_info));
		}
		for(map_info_hash_map_iter = ClientAnalysisInfo->map_info_hash_map.find(address);
			map_info_hash_map_iter != ClientAnalysisInfo->map_info_hash_map.end();
			map_info_hash_map_iter++
			)
		{
			if(map_info_hash_map_iter->first != address)
				break;
			PMapInfo p_map_info = map_info_hash_map_iter->second;
			if(p_map_info->Dst  ==  child_address)
			{
				ClientAnalysisInfo->map_info_hash_map.erase(map_info_hash_map_iter);
				break;
			}
		}
		multimap <DWORD,  string>::iterator child_address_disassembly_hash_map_iter;
		child_address_disassembly_hash_map_iter = ClientAnalysisInfo->address_disassembly_hash_map.find(child_address);
		if(child_address_disassembly_hash_map_iter != ClientAnalysisInfo->address_disassembly_hash_map.end())
		{
			multimap <DWORD,  string>::iterator address_disassembly_hash_map_iter;
			address_disassembly_hash_map_iter = ClientAnalysisInfo->address_disassembly_hash_map.find(address);
			if(address_disassembly_hash_map_iter != ClientAnalysisInfo->address_disassembly_hash_map.end())
			{
				address_disassembly_hash_map_iter->second += child_address_disassembly_hash_map_iter->second;
			}
		}

		multimap <DWORD, unsigned char *>::iterator child_address_fingerprint_hash_map_iter;
		child_address_fingerprint_hash_map_iter = ClientAnalysisInfo->address_fingerprint_hash_map.find(child_address);
		if(child_address_fingerprint_hash_map_iter != ClientAnalysisInfo->address_fingerprint_hash_map.end())
		{
			multimap <DWORD, unsigned char *>::iterator address_fingerprint_hash_map_iter;
			address_fingerprint_hash_map_iter = ClientAnalysisInfo->address_fingerprint_hash_map.find(address);
			if(address_fingerprint_hash_map_iter != ClientAnalysisInfo->address_fingerprint_hash_map.end())
			{
				//TODO: address_fingerprint_hash_map_iter->second += child_address_fingerprint_hash_map_iter->second;
			}
		}
		ClientAnalysisInfo->address_hash_map.erase((*AddressPairsIter).child_address);
		ClientAnalysisInfo->address_name_hash_map.erase((*AddressPairsIter).child_address);
		ClientAnalysisInfo->map_info_hash_map.erase((*AddressPairsIter).child_address);
		ClientAnalysisInfo->address_disassembly_hash_map.erase((*AddressPairsIter).child_address);
		ClientAnalysisInfo->address_fingerprint_hash_map.erase((*AddressPairsIter).child_address);
	}
	AddressPairs.clear();

	multimap <DWORD, unsigned char *>::iterator address_fingerprint_hash_map_Iter;
	for(address_fingerprint_hash_map_Iter = ClientAnalysisInfo->address_fingerprint_hash_map.begin();
		address_fingerprint_hash_map_Iter != ClientAnalysisInfo->address_fingerprint_hash_map.end();
		address_fingerprint_hash_map_Iter++)
	{
		ClientAnalysisInfo->fingerprint_hash_map.insert(FingerPrintAddress_Pair(address_fingerprint_hash_map_Iter->second, address_fingerprint_hash_map_Iter->first));
	}
	GenerateTwoLevelFingerPrint();
}

void IDAController::GenerateTwoLevelFingerPrint()
{
	/*
	multimap <unsigned char *, DWORD, hash_compare_fingerprint>::iterator fingerprint_hash_map_pIter;
	for(fingerprint_hash_map_pIter = ClientAnalysisInfo->fingerprint_hash_map.begin();
		fingerprint_hash_map_pIter != ClientAnalysisInfo->fingerprint_hash_map.end();
		fingerprint_hash_map_pIter++)

	{
		if(ClientAnalysisInfo->fingerprint_hash_map.count(fingerprint_hash_map_pIter->first)>1)
		{
			int addresses_number = 0;
			DWORD *addresses = GetMappedAddresses(fingerprint_hash_map_pIter->second, CREF_FROM, &addresses_number);
			if(!addresses)
				addresses = GetMappedAddresses(fingerprint_hash_map_pIter->second, CREF_TO, NULL);
			if(addresses)
			{
				int TwoLevelFingerprintLength = 0;
				TwoLevelFingerprintLength += *(unsigned short *)fingerprint_hash_map_pIter->first; //+
				multimap <DWORD,  unsigned char *>::iterator address_fingerprint_hash_map_Iter;
				for(int i = 0;i<addresses_number;i++)
				{
					address_fingerprint_hash_map_Iter = ClientAnalysisInfo->address_fingerprint_hash_map.find(addresses[i]);
					if(address_fingerprint_hash_map_Iter != ClientAnalysisInfo->address_fingerprint_hash_map.end())
					{
						TwoLevelFingerprintLength += *(unsigned short *)address_fingerprint_hash_map_Iter->second; //+
					}
				}

				if(TwoLevelFingerprintLength>0)
				{
					unsigned char *TwoLevelFingerprint = (unsigned char *)malloc(TwoLevelFingerprintLength+sizeof(short));
					if(TwoLevelFingerprint)
					{
						*(unsigned short *)TwoLevelFingerprint = TwoLevelFingerprintLength;

						int Offset = sizeof(short);
						memcpy(TwoLevelFingerprint+Offset, fingerprint_hash_map_pIter->first+sizeof(short), *(unsigned short *)fingerprint_hash_map_pIter->first);
						Offset += *(unsigned short *)fingerprint_hash_map_pIter->first;
						for(int i = 0;i<addresses_number;i++)
						{
							address_fingerprint_hash_map_Iter = ClientAnalysisInfo->address_fingerprint_hash_map.find(addresses[i]);
							if(address_fingerprint_hash_map_Iter != ClientAnalysisInfo->address_fingerprint_hash_map.end())
							{
								memcpy(TwoLevelFingerprint+Offset, address_fingerprint_hash_map_Iter->second+sizeof(short), *(unsigned short *)address_fingerprint_hash_map_Iter->second);
								Offset += *(unsigned short *)address_fingerprint_hash_map_Iter->second;
							}
						}
						ClientAnalysisInfo->fingerprint_hash_map.insert(FingerPrintAddress_Pair(TwoLevelFingerprint, fingerprint_hash_map_pIter->second));
					}
				}
			}
		}
	}*/
}

void IDAController::DumpAnalysisInfo()
{
	if(ClientAnalysisInfo)
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "OriginalFilePath = %s\n", ClientAnalysisInfo->file_info.OriginalFilePath);
		Logger.Log(10, LOG_IDA_CONTROLLER, "ComputerName = %s\n", ClientAnalysisInfo->file_info.ComputerName);
		Logger.Log(10, LOG_IDA_CONTROLLER, "UserName = %s\n", ClientAnalysisInfo->file_info.UserName);
		Logger.Log(10, LOG_IDA_CONTROLLER, "CompanyName = %s\n", ClientAnalysisInfo->file_info.CompanyName);
		Logger.Log(10, LOG_IDA_CONTROLLER, "FileVersion = %s\n", ClientAnalysisInfo->file_info.FileVersion);
		Logger.Log(10, LOG_IDA_CONTROLLER, "FileDescription = %s\n", ClientAnalysisInfo->file_info.FileDescription);
		Logger.Log(10, LOG_IDA_CONTROLLER, "InternalName = %s\n", ClientAnalysisInfo->file_info.InternalName);
		Logger.Log(10, LOG_IDA_CONTROLLER, "ProductName = %s\n", ClientAnalysisInfo->file_info.ProductName);
		Logger.Log(10, LOG_IDA_CONTROLLER, "ModifiedTime = %s\n", ClientAnalysisInfo->file_info.ModifiedTime);
		Logger.Log(10, LOG_IDA_CONTROLLER, "MD5Sum = %s\n", ClientAnalysisInfo->file_info.MD5Sum);

		Logger.Log(10, LOG_IDA_CONTROLLER, "fingerprint_hash_map = %u\n", ClientAnalysisInfo->fingerprint_hash_map.size());
	}
}

BOOL IDAController::SendTLVData(char type, PBYTE data, DWORD data_length)
{
	if(Socket != INVALID_SOCKET)
	{
		BOOL ret = ::SendTLVData(Socket, 
			type, 
			data, 
			data_length);
		if(!ret)
			Socket = INVALID_SOCKET;
		return ret;
	}
	return FALSE;
}

char *IDAController::GetDisasmLines(unsigned long StartAddress, unsigned long EndAddress)
{
#ifdef USE_LEGACY_MAP
	//Look for p_analysis_info->address_disassembly_hash_map first
	multimap <DWORD,  string>::iterator address_disassembly_hash_map_pIter;
	address_disassembly_hash_map_pIter = ClientAnalysisInfo->address_disassembly_hash_map.find(StartAddress);
	if(address_disassembly_hash_map_pIter != ClientAnalysisInfo->address_disassembly_hash_map.end())
	{
		return _strdup(address_disassembly_hash_map_pIter->second.c_str());
	}
	CodeBlock code_block;
	code_block.StartAddress = StartAddress;
	if(Socket  ==  INVALID_SOCKET)
		return strdup("");

	multimap <DWORD,  PBasicBlock>::iterator address_hash_map_pIter;
	if(EndAddress  ==  0)
	{
		address_hash_map_pIter = ClientAnalysisInfo->address_hash_map.find(StartAddress);
		if(address_hash_map_pIter != ClientAnalysisInfo->address_hash_map.end())
		{
			PBasicBlock pBasicBlock = (PBasicBlock)address_hash_map_pIter->second;
			EndAddress = pBasicBlock->EndAddress;
		}
	}
	code_block.EndAddress = EndAddress;
	DisasmLine = NULL;
	if(SendTLVData(GET_DISASM_LINES, (PBYTE)&code_block, sizeof(code_block)))
	{
		char type;
		DWORD length;
		PBYTE data = RecvTLVData(Socket, &type, &length);
		if(data)
			DisasmLine = (char *)data;
			return (char *)data;
	}
	return strdup("");
#else
	char *DisasmLines = NULL;

	if( m_StorageDB )
		m_StorageDB->ExecuteStatement(m_StorageDB->ReadRecordStringCallback, &DisasmLines, "SELECT DisasmLines FROM BasicBlock WHERE FileID = %u and StartAddress = %u", m_FileID, StartAddress);
	if(DisasmLines)
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "DisasmLines = %s\n", DisasmLines);
		return DisasmLines;
	}
	return _strdup("");
#endif
}

string IDAController::GetInputName()
{
	string input_name;

	if (SendTLVData(GET_INPUT_NAME, (PBYTE)"", 1))
	{
		char type;
		DWORD length;

		PBYTE data = RecvTLVData(Socket, &type, &length);
		input_name = (char *)data;
	}
	return input_name;
}

void IDAController::RetrieveIdentity()
{
	Identity = GetInputName();
}

string IDAController::GetIdentity()
{
	return Identity;
}

int ReadBasicBlockCallback(void *arg, int argc, char **argv, char **names)
{
	PBasicBlock p_basic_block = (PBasicBlock)arg;
	p_basic_block->StartAddress = strtoul10(argv[0]);
	p_basic_block->EndAddress = strtoul10(argv[1]);
	p_basic_block->Flag = strtoul10(argv[2]);
	p_basic_block->FunctionAddress = strtoul10(argv[3]);
	p_basic_block->BlockType = strtoul10(argv[4]);
	p_basic_block->FingerprintLen = strlen(argv[5]);

	Logger.Log(11, LOG_IDA_CONTROLLER | LOG_BASIC_BLOCK, "%s: %X Block Type: %d\n", __FUNCTION__, p_basic_block->StartAddress, p_basic_block->BlockType);
	if (p_basic_block->BlockType == FUNCTION_BLOCK)
	{		
		Logger.Log(11, LOG_IDA_CONTROLLER | LOG_BASIC_BLOCK, "%s: Function Block: %X\n", __FUNCTION__, p_basic_block->StartAddress);
	}
	return 0;
}

PBasicBlock IDAController::GetBasicBlock(DWORD address)
{
	PBasicBlock p_basic_block = (PBasicBlock)malloc(sizeof(BasicBlock));
	if( m_StorageDB )
		m_StorageDB->ExecuteStatement(ReadBasicBlockCallback, p_basic_block, "SELECT StartAddress, EndAddress, Flag, FunctionAddress, BlockType, FingerPrint FROM BasicBlock WHERE FileID = %u and StartAddress = %u", m_FileID, address);
	return p_basic_block;
}

void IDAController::FreeDisasmLines()
{
	if(DisasmLine)
		free(DisasmLine);
}

void IDAController::JumpToAddress(unsigned long address)
{
	SendTLVData(JUMP_TO_ADDR, (PBYTE)&address, sizeof(DWORD));
}

void IDAController::ColorAddress(unsigned long start_address, unsigned long end_address, unsigned long color)
{
	unsigned long data[3];
	data[0] = start_address;
	data[1] = end_address;
	data[2] = color;
	SendTLVData(COLOR_ADDRESS, (PBYTE)data, sizeof(data));
}

list <BLOCK> IDAController::GetFunctionMemberBlocks(unsigned long function_address)
{
	list <BLOCK> block_list;

	if (ClientAnalysisInfo)
	{
		list <DWORD> address_list;
		list <DWORD>::iterator address_list_iter;
		hash_set <DWORD> checked_addresses;
		address_list.push_back(function_address);
		
		BLOCK block;
		block.Start = function_address;
		PBasicBlock pBasicBlock = GetBasicBlock(function_address);
		block.End = pBasicBlock->EndAddress;
		block_list.push_back(block);

		checked_addresses.insert(function_address);

		for (address_list_iter = address_list.begin();
			address_list_iter != address_list.end();
			address_list_iter++
			)
		{
			int addresses_number;
			DWORD *p_addresses = GetMappedAddresses(*address_list_iter, CREF_FROM, &addresses_number);
			if (p_addresses && addresses_number > 0)
			{
				for (int i = 0; i < addresses_number; i++)
				{
					DWORD address = p_addresses[i];
					if (address)
					{
						if (FunctionHeads.find(address) != FunctionHeads.end())
							continue;

						if (checked_addresses.find(address) == checked_addresses.end())
						{
							address_list.push_back(address);
							block.Start = address;
							PBasicBlock pBasicBlock = GetBasicBlock(address);
							block.End = pBasicBlock->EndAddress;
							block_list.push_back(block);

							checked_addresses.insert(address);
						}
					}
				}
				free(p_addresses);
			}
		}
	}
	else
	{
		m_StorageDB->ExecuteStatement(ReadFunctionMemberAddressesCallback, (void *)&block_list,
			"SELECT StartAddress, EndAddress FROM BasicBlock WHERE FileID = '%d' AND FunctionAddress='%d'"
			"ORDER BY ID ASC",
			m_FileID, function_address);
	}

	return block_list;
}

void IDAController::MergeBlocks()
{
	multimap <DWORD,  PMapInfo>::iterator last_iter = ClientAnalysisInfo->map_info_hash_map.end();
	multimap <DWORD,  PMapInfo>::iterator iter;
	multimap <DWORD,  PMapInfo>::iterator child_iter;

	int NumberOfChildren = 1;
	for(iter = ClientAnalysisInfo->map_info_hash_map.begin();
		iter != ClientAnalysisInfo->map_info_hash_map.end();
		iter++
		)
	{
		if(iter->second->Type  ==  CREF_FROM)
		{
			BOOL bHasOnlyOneChild = FALSE;
			if(last_iter != ClientAnalysisInfo->map_info_hash_map.end())
			{
				if(last_iter->first  ==  iter->first)
				{
					NumberOfChildren++;
				}else
				{
					Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Number Of Children for %X  = %u\n",
											__FUNCTION__, m_FileID, 
											last_iter->first, 
											NumberOfChildren);
					if(NumberOfChildren  ==  1)
						bHasOnlyOneChild = TRUE;
					multimap <DWORD,  PMapInfo>::iterator next_iter = iter;
					next_iter++;
					if(next_iter  ==  ClientAnalysisInfo->map_info_hash_map.end())
					{
						last_iter = iter;
						bHasOnlyOneChild = TRUE;
					}
					NumberOfChildren = 1;
				}
			}
			if(bHasOnlyOneChild)
			{
				int NumberOfParents = 0;
				for(child_iter = ClientAnalysisInfo->map_info_hash_map.find(last_iter->second->Dst);
					child_iter != ClientAnalysisInfo->map_info_hash_map.end() && child_iter->first  ==  last_iter->second->Dst;
					child_iter++)
				{
					if(child_iter->second->Type  ==  CREF_TO && child_iter->second->Dst != last_iter->first)
					{
						Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Found %X -> %X\n",
							__FUNCTION__, m_FileID, 
							child_iter->second->Dst, child_iter->first);
						NumberOfParents++;
					}
				}
				if(NumberOfParents  ==  0)
				{
					Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Found Mergable Nodes %X -> %X\n",
						__FUNCTION__, m_FileID, 
						last_iter->first, last_iter->second->Dst);
				}
			}
			last_iter = iter;
		}
	}
}

int IDAController::GetFileID()
{
	return m_FileID;
}

unsigned char HexToChar(char *Hex)
{
	int ReturnValue = 0;
	for(int i = 0;Hex[i] && i<2;i++)
	{
		int CurrentInt = -1;
		char c = Hex[i];
		if('0' <=  c && c <= '9')
		{
			CurrentInt = c-'0';
		}else if('a' <=  c && c <= 'f')
		{
			CurrentInt = c-'a'+10;
		}else if('A' <=  c && c <= 'F')
		{
			CurrentInt = c-'A'+10;
		}
		if(CurrentInt >= 0)
			ReturnValue = ReturnValue*16+CurrentInt;
	}
	return ReturnValue;
}

unsigned char *HexToBytes(char *HexBytes, int *pLen)
{
	int StrLen = strlen(HexBytes);
	*pLen = StrLen/2;
	unsigned char *Bytes = (unsigned char *)malloc(*pLen);
	if(Bytes)
	{
		for(int i = 0;i<StrLen;i += 2)
		{
			Bytes[i/2] = HexToChar(HexBytes+i);
		}
	}
	return Bytes;
}

unsigned char *HexToBytesWithLengthAmble(char *HexBytes)
{
	int StrLen = strlen(HexBytes);
	unsigned char *Bytes = (unsigned char *)malloc(StrLen/2+sizeof(short));
	*(unsigned short *)Bytes = StrLen/2;
	if(Bytes)
	{
		for(int i = 0;i<StrLen;i += 2)
		{
			Bytes[sizeof(short)+i/2] = HexToChar(HexBytes+i);
		}
	}
	return Bytes;
}

char *BytesWithLengthAmbleToHex(unsigned char *Bytes)
{
	int Len = *(unsigned short *)Bytes;

	char *Hex = (char *)malloc(Len*2+1);
	Hex[0] = NULL;
	for(int i = 0;i<Len;i++)
	{
		char tmp_buffer[10] = {0, };
		_snprintf(tmp_buffer, sizeof(tmp_buffer)-1, "%.2x", Bytes[sizeof(short)+i]);
		strcat(Hex, tmp_buffer);
	}
	return Hex;
}

int IsEqualByteWithLengthAmble(unsigned char *Bytes01, unsigned char *Bytes02)
{
	if(*(unsigned short *)Bytes01  ==  *(unsigned short *)Bytes02)
	{
		return (memcmp(Bytes01+sizeof(unsigned short), Bytes02+sizeof(unsigned short), *(unsigned short *)Bytes01)  ==  0);
	}
	return FALSE;
}

multimap <DWORD, DWORD> *IDAController::GetFunctionToBlock()
{
	Logger.Log(10, LOG_IDA_CONTROLLER, "LoadFunctionMembersMap\n");
	return &FunctionToBlock;
}

static int ReadAddressToFunctionMapResultsCallback(void *arg, int argc, char **argv, char **names)
{
	hash_map <DWORD, DWORD> *AddressToFunctionMap = (hash_map <DWORD, DWORD> *)arg;
	if(AddressToFunctionMap)
	{
#if DEBUG_LEVEL > 1
		Logger.Log( 10, "%s: ID = %d strtoul10(%s) = 0x%X, strtoul10(%s) = 0x%X\n", __FUNCTION__, m_FileID, argv[0], strtoul10(argv[0]), argv[1], strtoul10(argv[1]));
#endif
		AddressToFunctionMap->insert(pair <DWORD, DWORD>(strtoul10(argv[0]), strtoul10(argv[1])));
	}
	return 0;
}

void IDAController::LoadBlockToFunction()
{
	int Count = 0;
	
	Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d GetFunctionAddresses\n", __FUNCTION__);
	list <DWORD> *function_addresses = GetFunctionAddresses();
	if (function_addresses)
	{
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d Function %u entries\n", __FUNCTION__, m_FileID, function_addresses->size());
		
		hash_map<DWORD, DWORD> addresses;
		hash_map<DWORD, DWORD> membership_hash;
		for (list <DWORD>::iterator it = function_addresses->begin(); it != function_addresses->end(); it++)
		{
			list <BLOCK> function_member_blocks = GetFunctionMemberBlocks(*it);

			for (list <BLOCK>::iterator it2 = function_member_blocks.begin();
				it2 != function_member_blocks.end();
				it2++
			)
			{
				DWORD addr = (*it2).Start;
				BlockToFunction.insert(pair <DWORD, DWORD>(addr, *it));

				if (addresses.find(addr) == addresses.end())
				{
					addresses.insert(pair<DWORD, DWORD>(addr, 1));
				}
				else
				{
					addresses[addr] += 1;
				}

				if (membership_hash.find(addr) == membership_hash.end())
				{
					membership_hash.insert(pair<DWORD, DWORD>(addr, *it));
				}
				else
				{
					membership_hash[addr] += *it;
				}
			}
		}

		for (hash_map<DWORD, DWORD>::iterator it = addresses.begin();
			it != addresses.end();
			it++)
		{
			if (it->second > 1)
			{
				bool function_start = true;
				for (multimap<DWORD, DWORD>::iterator it2 = CrefToMap.find(it->first);
					it2 != CrefToMap.end() && it2->first==it->first;
					it2++
				)
				{
					hash_map<DWORD, DWORD>::iterator current_membership_it = membership_hash.find(it->first);
					DWORD parent=it2->second;
					Logger.Log(10, LOG_IDA_CONTROLLER, "Found parent for %X -> %X\n", it->first, parent);
					hash_map<DWORD, DWORD>::iterator parent_membership_it = membership_hash.find(parent);
					if (current_membership_it!=membership_hash.end() && parent_membership_it != membership_hash.end())
					{
						if (current_membership_it->second==parent_membership_it->second)
						{
							function_start = false;
							break;
						}
					}
				}

				Logger.Log(10, LOG_IDA_CONTROLLER, "Multiple function membership: %X (%d) %s\n", it->first, it->second, function_start ? "Possible Head" : "Member");

				if (function_start)
				{
					DWORD function_start_addr = it->first;
					FunctionHeads.insert(function_start_addr);
					list <BLOCK> function_member_blocks = GetFunctionMemberBlocks(function_start_addr);
					hash_map<DWORD, DWORD>::iterator function_start_membership_it = membership_hash.find(function_start_addr);

					for (list <BLOCK>::iterator it2 = function_member_blocks.begin();
						it2 != function_member_blocks.end();
						it2++
						)
					{
						DWORD addr = (*it2).Start;

						hash_map<DWORD, DWORD>::iterator current_membership_it = membership_hash.find(addr);

						if (function_start_membership_it->second != current_membership_it->second)
							continue;

						for (multimap <DWORD, DWORD>::iterator a2f_it = BlockToFunction.find(addr);
							a2f_it != BlockToFunction.end() && a2f_it->first == addr;
							a2f_it++
							)
						{
							Logger.Log(10, LOG_IDA_CONTROLLER, "\tRemoving Block: %X Function: %X\n", a2f_it->first, a2f_it->second);
							a2f_it = BlockToFunction.erase(a2f_it);
						}
						BlockToFunction.insert(pair <DWORD, DWORD>(addr, function_start_addr));
						Logger.Log(10, LOG_IDA_CONTROLLER, "\tAdding Block: %X Function: %X\n", addr, function_start_addr);
					}
				}
			}
		}
		function_addresses->clear();
		delete function_addresses;

		for (multimap <DWORD, DWORD>::iterator a2f_it = BlockToFunction.begin();
			a2f_it != BlockToFunction.end();
			a2f_it++
			)
		{
			FunctionToBlock.insert(pair<DWORD, DWORD>(a2f_it->second, a2f_it->first));
		}
		
		Logger.Log(10, LOG_IDA_CONTROLLER, "%s: ID = %d BlockToFunction %u entries\n", __FUNCTION__, m_FileID, BlockToFunction.size());
	}
}

BOOL IDAController::FixFunctionAddresses()
{
	BOOL is_fixed = FALSE;
	Logger.Log(10, LOG_IDA_CONTROLLER, "%s", __FUNCTION__);
	LoadBlockToFunction();

	if( m_StorageDB )
		m_StorageDB->BeginTransaction();

	for (multimap <DWORD, DWORD>::iterator it = BlockToFunction.begin(); 
		it != BlockToFunction.end();
		it++
	)
	{
		//StartAddress: it->first
		//FunctionAddress: it->second
		Logger.Log(10, LOG_IDA_CONTROLLER, "Updating BasicBlockTable Address = %X Function = %X\n",
			it->second,
			it->first);

		if( m_StorageDB )
			m_StorageDB->ExecuteStatement(NULL, NULL, UPDATE_BASIC_BLOCK_TABLE_FUNCTION_ADDRESS_STATEMENT, 
																				it->second,
																				it->second == it->first ? FUNCTION_BLOCK : UNKNOWN_BLOCK,
																				m_FileID, 
																				it->first);

		is_fixed = TRUE;
	}

	if( m_StorageDB )
		m_StorageDB->EndTransaction();

	ClearBlockToFunction();

	return is_fixed;
}

bool IDAController::SendMatchedAddrTLVData(FunctionMatchInfo &Data)
{
	return SendTLVData(
		MATCHED_ADDR,
		(PBYTE)&(Data),
		sizeof(Data));
}

bool IDAController::SendAddrTypeTLVData(int Type, DWORD Start, DWORD End)
{
	DWORD StartToEnd[2];

	StartToEnd[0] = Start;
	StartToEnd[1] = End;

	return SendTLVData(
		Type,
		(PBYTE)StartToEnd,
		sizeof(StartToEnd));
}


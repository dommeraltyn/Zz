#include "connection.h"

#include <base/dbg.h>
#include <base/math.h>
#include <base/mem.h>
#include <base/net.h>
#include <base/str.h>

#include <engine/console.h>
#include <engine/shared/config.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

class CSqliteConnection : public IDbConnection
{
public:
	CSqliteConnection(const char *pFilename, bool Setup);
	~CSqliteConnection() override;
	void Print(IConsole *pConsole, const char *pMode) override;

	const char *BinaryCollate() const override { return "BINARY"; }
	void ToUnixTimestamp(const char *pTimestamp, char *aBuf, unsigned int BufferSize) override;
	const char *InsertTimestampAsUtc() const override { return "DATETIME(?, 'utc')"; }
	const char *CollateNocase() const override { return "? COLLATE NOCASE"; }
	const char *InsertIgnore() const override { return "INSERT OR IGNORE"; }
	const char *Random() const override { return "RANDOM()"; }
	const char *MedianMapTime(char *pBuffer, int BufferSize) const override;
	// Since SQLite 3.23.0 true/false literals are recognized, but still cleaner to use 1/0, because:
	// > For compatibility, if there exist columns named "true" or "false", then
	// > the identifiers refer to the columns rather than Boolean constants.
	const char *False() const override { return "0"; }
	const char *True() const override { return "1"; }

	bool Connect(char *pError, int ErrorSize) override;
	void Disconnect() override;

	bool PrepareStatement(const char *pStmt, char *pError, int ErrorSize) override;

	void BindString(int Idx, const char *pString) override;
	void BindBlob(int Idx, unsigned char *pBlob, int Size) override;
	void BindInt(int Idx, int Value) override;
	void BindInt64(int Idx, int64_t Value) override;
	void BindFloat(int Idx, float Value) override;
	void BindNull(int Idx) override;

	void Print() override;
	bool Step(bool *pEnd, char *pError, int ErrorSize) override;
	bool ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize) override;

	bool IsNull(int Col) override;
	float GetFloat(int Col) override;
	int GetInt(int Col) override;
	int64_t GetInt64(int Col) override;
	void GetString(int Col, char *pBuffer, int BufferSize) override;
	// passing a negative buffer size is undefined behavior
	int GetBlob(int Col, unsigned char *pBuffer, int BufferSize) override;

	bool AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize) override;

	// fail safe
	bool CreateFailsafeTables();

private:
	// copy of config vars
	char m_aFilename[IO_MAX_PATH_LENGTH];
	bool m_Setup;

	sqlite3 *m_pDb;
	sqlite3_stmt *m_pStmt;
	bool m_Done; // no more rows available for Step
	// returns false, if the query succeeded
	bool Execute(const char *pQuery, char *pError, int ErrorSize);
	// returns true on failure
	bool ConnectImpl(char *pError, int ErrorSize);

	// returns true if an error was formatted
	bool FormatError(int Result, char *pError, int ErrorSize);
	void AssertNoError(int Result);

	std::atomic_bool m_InUse;
};

CSqliteConnection::CSqliteConnection(const char *pFilename, bool Setup) :
	IDbConnection("record"),
	m_Setup(Setup),
	m_pDb(nullptr),
	m_pStmt(nullptr),
	m_Done(true),
	m_InUse(false)
{
	str_copy(m_aFilename, pFilename);
}

CSqliteConnection::~CSqliteConnection()
{
	if(m_pStmt != nullptr)
		sqlite3_finalize(m_pStmt);
	sqlite3_close(m_pDb);
	m_pDb = nullptr;
}

void CSqliteConnection::Print(IConsole *pConsole, const char *pMode)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"SQLite-%s: DB: '%s'",
		pMode, m_aFilename);
	pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CSqliteConnection::ToUnixTimestamp(const char *pTimestamp, char *aBuf, unsigned int BufferSize)
{
	str_format(aBuf, BufferSize, "strftime('%%s', %s)", pTimestamp);
}

bool CSqliteConnection::Connect(char *pError, int ErrorSize)
{
	if(m_InUse.exchange(true))
	{
		dbg_assert_failed("Tried connecting while the connection is in use");
	}
	if(!ConnectImpl(pError, ErrorSize))
	{
		m_InUse.store(false);
		return false;
	}
	return true;
}

bool CSqliteConnection::ConnectImpl(char *pError, int ErrorSize)
{
	if(m_pDb != nullptr)
	{
		return true;
	}

	if(sqlite3_libversion_number() < 3025000)
	{
		dbg_msg("sql", "SQLite version %s is not supported, use at least version 3.25.0", sqlite3_libversion());
	}

	int Result = sqlite3_open(m_aFilename, &m_pDb);
	if(Result != SQLITE_OK)
	{
		str_format(pError, ErrorSize, "Can't open sqlite database: '%s'", sqlite3_errmsg(m_pDb));
		return false;
	}

	// wait for database to unlock so we don't have to handle SQLITE_BUSY errors
	sqlite3_busy_timeout(m_pDb, -1);

	// Ensure account tables exist even when setup is disabled (e.g. existing DB from older schema).
	{
		char aBuf[1024];
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_accounts ("
			"  Id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"  Username VARCHAR(%d) NOT NULL, "
			"  PwHash BLOB NOT NULL, "
			"  PwSalt BLOB NOT NULL, "
			"  RegisterDate TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
			"  LastLoginDate TIMESTAMP NULL DEFAULT NULL, "
			"  LastIgn VARCHAR(%d) NOT NULL DEFAULT '', "
			"  LastIP VARCHAR(64) NOT NULL DEFAULT '', "
			"  UNIQUE (Username)"
			")",
			GetPrefix(), MAX_NAME_LENGTH_SQL, MAX_NAME_LENGTH_SQL);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_account_stats ("
			"  AccountId INTEGER NOT NULL, "
			"  XP INT NOT NULL DEFAULT 0, "
			"  Money BIGINT NOT NULL DEFAULT 0, "
			"  PlaytimeSeconds BIGINT NOT NULL DEFAULT 0, "
			"  Deaths BIGINT NOT NULL DEFAULT 0, "
			"  Kills BIGINT NOT NULL DEFAULT 0, "
			"  Combo BIGINT NOT NULL DEFAULT 0, "
			"  PRIMARY KEY (AccountId)"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		// Schema migration for existing DBs: add missing columns (SQLite has no IF NOT EXISTS for ADD COLUMN)
		{
			char aAlter[256];
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_stats ADD COLUMN Kills BIGINT NOT NULL DEFAULT 0", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_stats ADD COLUMN Combo BIGINT NOT NULL DEFAULT 0", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
		}
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_account_settings ("
			"  AccountId INTEGER NOT NULL, "
			"  Language INT NOT NULL DEFAULT 0, "
			"  LangHintShown INT NOT NULL DEFAULT 0, "
			"  LangManual INT NOT NULL DEFAULT 0, "
			"  Role INT NOT NULL DEFAULT 0, "
			"  AutoLogin INT NOT NULL DEFAULT 1, "
			"  PRIMARY KEY (AccountId)"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		// Schema migration for existing DBs
		{
			char aAlter[256];
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_settings ADD COLUMN LangManual INT NOT NULL DEFAULT 0", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_settings ADD COLUMN Role INT NOT NULL DEFAULT 0", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_settings ADD COLUMN AutoLogin INT NOT NULL DEFAULT 1", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_accounts ADD COLUMN LastIP VARCHAR(64) NOT NULL DEFAULT ''", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name"))
					return false;
				pError[0] = 0;
			}
		}
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_ip_language ("
			"  IpAddr VARCHAR(64) NOT NULL, "
			"  Language INT NOT NULL DEFAULT 0, "
			"  UpdatedAt BIGINT NOT NULL DEFAULT 0, "
			"  PRIMARY KEY (IpAddr)"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_account_mailbox ("
			"  MailId INTEGER PRIMARY KEY AUTOINCREMENT, "
			"  AccountId INTEGER NOT NULL, "
			"  Subject TEXT NOT NULL, "
			"  Message TEXT NOT NULL, "
			"  CreatedAt BIGINT NOT NULL, "
			"  ExpiresAt BIGINT NOT NULL, "
			"  Unread INT NOT NULL DEFAULT 1, "
			"  RewardXP INT NOT NULL DEFAULT 0, "
			"  RewardMoney BIGINT NOT NULL DEFAULT 0, "
			"  RewardClaimed INT NOT NULL DEFAULT 0"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_account_cosmetics ("
			"  AccountId INTEGER NOT NULL, "
			"  RainbowOwned INT NOT NULL DEFAULT 0, "
			"  RainbowActive INT NOT NULL DEFAULT 0, "
			"  PRIMARY KEY (AccountId)"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_account_inventory ("
			"  AccountId INTEGER NOT NULL, "
			"  ItemName TEXT NOT NULL, "
			"  GroupId INT NOT NULL DEFAULT 0, "
			"  Equipped INT NOT NULL DEFAULT 0, "
			"  PRIMARY KEY (AccountId, ItemName)"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		// Schema migration for existing DBs
		{
			char aAlter[256];
			str_format(aAlter, sizeof(aAlter), "ALTER TABLE %s_account_cosmetics ADD COLUMN RainbowActive INT NOT NULL DEFAULT 0", GetPrefix());
			if(!Execute(aAlter, pError, ErrorSize))
			{
				if(!str_find(pError, "duplicate column name") && !str_find(pError, "no such table"))
					return false;
				pError[0] = 0;
			}
		}
		str_format(aBuf, sizeof(aBuf),
			"CREATE TABLE IF NOT EXISTS %s_farmers ("
			"  FarmerId INTEGER PRIMARY KEY AUTOINCREMENT, "
			"  Map VARCHAR(128) NOT NULL, "
			"  Type INT NOT NULL, "
			"  X1 REAL NOT NULL, "
			"  Y1 REAL NOT NULL, "
			"  X2 REAL NOT NULL, "
			"  Y2 REAL NOT NULL, "
			"  MoneyPerSec BIGINT NOT NULL DEFAULT 0, "
			"  XPPerSec INT NOT NULL DEFAULT 0, "
			"  CreatedAt BIGINT NOT NULL"
			")",
			GetPrefix());
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
	}

	if(m_Setup)
	{
		if(!Execute("PRAGMA journal_mode=WAL", pError, ErrorSize))
			return false;
		char aBuf[1024];
		FormatCreateRace(aBuf, sizeof(aBuf), /* Backup */ false);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreateTeamrace(aBuf, sizeof(aBuf), "BLOB", /* Backup */ false);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreateMaps(aBuf, sizeof(aBuf));
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreateSaves(aBuf, sizeof(aBuf), /* Backup */ false);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreatePoints(aBuf, sizeof(aBuf));
		if(!Execute(aBuf, pError, ErrorSize))
			return false;

		FormatCreateRace(aBuf, sizeof(aBuf), /* Backup */ true);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreateTeamrace(aBuf, sizeof(aBuf), "BLOB", /* Backup */ true);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		FormatCreateSaves(aBuf, sizeof(aBuf), /* Backup */ true);
		if(!Execute(aBuf, pError, ErrorSize))
			return false;
		m_Setup = false;
	}
	return true;
}

void CSqliteConnection::Disconnect()
{
	if(m_pStmt != nullptr)
		sqlite3_finalize(m_pStmt);
	m_pStmt = nullptr;
	m_InUse.store(false);
}

bool CSqliteConnection::PrepareStatement(const char *pStmt, char *pError, int ErrorSize)
{
	if(m_pStmt != nullptr)
		sqlite3_finalize(m_pStmt);
	m_pStmt = nullptr;
	int Result = sqlite3_prepare_v2(
		m_pDb,
		pStmt,
		-1, // pStmt can be any length
		&m_pStmt,
		nullptr);
	if(FormatError(Result, pError, ErrorSize))
	{
		return false;
	}
	m_Done = false;
	return true;
}

void CSqliteConnection::BindString(int Idx, const char *pString)
{
	int Result = sqlite3_bind_text(m_pStmt, Idx, pString, -1, nullptr);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindBlob(int Idx, unsigned char *pBlob, int Size)
{
	int Result = sqlite3_bind_blob(m_pStmt, Idx, pBlob, Size, nullptr);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindInt(int Idx, int Value)
{
	int Result = sqlite3_bind_int(m_pStmt, Idx, Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindInt64(int Idx, int64_t Value)
{
	int Result = sqlite3_bind_int64(m_pStmt, Idx, Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindFloat(int Idx, float Value)
{
	int Result = sqlite3_bind_double(m_pStmt, Idx, (double)Value);
	AssertNoError(Result);
	m_Done = false;
}

void CSqliteConnection::BindNull(int Idx)
{
	int Result = sqlite3_bind_null(m_pStmt, Idx);
	AssertNoError(Result);
	m_Done = false;
}

// Keep support for SQLite < 3.14 on older Linux distributions
// MinGW does not support weak attribute: https://sourceware.org/bugzilla/show_bug.cgi?id=9687
#if !defined(__MINGW32__)
[[gnu::weak]] extern char *sqlite3_expanded_sql(sqlite3_stmt *pStmt); // NOLINT(readability-redundant-declaration)
#endif

void CSqliteConnection::Print()
{
	if(m_pStmt != nullptr
#if !defined(__MINGW32__)
		&& sqlite3_expanded_sql != nullptr
#endif
	)
	{
		char *pExpandedStmt = sqlite3_expanded_sql(m_pStmt);
		dbg_msg("sql", "SQLite statement: %s", pExpandedStmt);
		sqlite3_free(pExpandedStmt);
	}
}

bool CSqliteConnection::Step(bool *pEnd, char *pError, int ErrorSize)
{
	if(m_Done)
	{
		*pEnd = true;
		return true;
	}
	int Result = sqlite3_step(m_pStmt);
	if(Result == SQLITE_ROW)
	{
		*pEnd = false;
		return true;
	}
	else if(Result == SQLITE_DONE)
	{
		m_Done = true;
		*pEnd = true;
		return true;
	}
	else
	{
		if(FormatError(Result, pError, ErrorSize))
		{
			return false;
		}
	}
	*pEnd = true;
	return true;
}

bool CSqliteConnection::ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize)
{
	bool End;
	if(!Step(&End, pError, ErrorSize))
	{
		return false;
	}
	*pNumUpdated = sqlite3_changes(m_pDb);
	return true;
}

bool CSqliteConnection::IsNull(int Col)
{
	return sqlite3_column_type(m_pStmt, Col - 1) == SQLITE_NULL;
}

float CSqliteConnection::GetFloat(int Col)
{
	return (float)sqlite3_column_double(m_pStmt, Col - 1);
}

int CSqliteConnection::GetInt(int Col)
{
	return sqlite3_column_int(m_pStmt, Col - 1);
}

int64_t CSqliteConnection::GetInt64(int Col)
{
	return sqlite3_column_int64(m_pStmt, Col - 1);
}

void CSqliteConnection::GetString(int Col, char *pBuffer, int BufferSize)
{
	str_copy(pBuffer, (const char *)sqlite3_column_text(m_pStmt, Col - 1), BufferSize);
}

int CSqliteConnection::GetBlob(int Col, unsigned char *pBuffer, int BufferSize)
{
	int Size = sqlite3_column_bytes(m_pStmt, Col - 1);
	Size = minimum(Size, BufferSize);
	mem_copy(pBuffer, sqlite3_column_blob(m_pStmt, Col - 1), Size);
	return Size;
}

const char *CSqliteConnection::MedianMapTime(char *pBuffer, int BufferSize) const
{
	str_format(pBuffer, BufferSize,
		"SELECT AVG("
		"  CASE counter %% 2 "
		"    WHEN 0 THEN CASE WHEN rn IN (counter / 2, counter / 2 + 1) THEN Time END "
		"    WHEN 1 THEN CASE WHEN rn = counter / 2 + 1 THEN Time END END) "
		"  OVER (PARTITION BY Map) AS Median "
		"FROM ("
		"  SELECT *, ROW_NUMBER() "
		"  OVER (PARTITION BY Map ORDER BY Time) rn, COUNT(*) "
		"  OVER (PARTITION BY Map) counter "
		"  FROM %s_race where Map = l.Map) as r",
		GetPrefix());
	return pBuffer;
}

bool CSqliteConnection::Execute(const char *pQuery, char *pError, int ErrorSize)
{
	char *pErrorMsg;
	int Result = sqlite3_exec(m_pDb, pQuery, nullptr, nullptr, &pErrorMsg);
	if(Result != SQLITE_OK)
	{
		str_format(pError, ErrorSize, "error executing query: '%s'", pErrorMsg);
		sqlite3_free(pErrorMsg);
		return false;
	}
	return true;
}

bool CSqliteConnection::FormatError(int Result, char *pError, int ErrorSize)
{
	if(Result != SQLITE_OK)
	{
		str_copy(pError, sqlite3_errmsg(m_pDb), ErrorSize);
		return true;
	}
	return false;
}

void CSqliteConnection::AssertNoError(int Result)
{
	char aBuf[128];
	if(FormatError(Result, aBuf, sizeof(aBuf)))
	{
		dbg_msg("sqlite", "unexpected sqlite error: %s", aBuf);
		dbg_assert(0, "sqlite error");
	}
}

bool CSqliteConnection::AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"INSERT INTO %s_points(Name, Points) "
		"VALUES (?, ?) "
		"ON CONFLICT(Name) DO UPDATE SET Points=Points+?",
		GetPrefix());
	if(!PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	BindString(1, pPlayer);
	BindInt(2, Points);
	BindInt(3, Points);
	bool End;
	return Step(&End, pError, ErrorSize);
}

namespace
{
enum class EBindValueType
{
	NULL_VALUE,
	STRING,
	BLOB,
	INT32,
	INT64,
	FLOAT64,
};

struct CBindValue
{
	EBindValueType m_Type{EBindValueType::NULL_VALUE};
	std::string m_String;
	std::vector<unsigned char> m_Blob;
	int m_Int{0};
	int64_t m_Int64{0};
	double m_Float{0.0};
};

struct CRemoteCell
{
	int m_Type{0};
	std::string m_Text;
	std::vector<unsigned char> m_Blob;
};

static bool IsWriteSql(const std::string &Sql)
{
	size_t i = 0;
	while(i < Sql.size() && std::isspace((unsigned char)Sql[i]))
		++i;
	std::string Token;
	while(i < Sql.size() && std::isalpha((unsigned char)Sql[i]))
	{
		Token.push_back((char)std::toupper((unsigned char)Sql[i]));
		++i;
	}
	return Token == "INSERT" || Token == "UPDATE" || Token == "DELETE" || Token == "REPLACE" || Token == "CREATE" || Token == "ALTER" || Token == "DROP" || Token == "PRAGMA" || Token == "VACUUM";
}

static std::string Base64Encode(const std::string &Raw)
{
	const int OutSize = ((int)Raw.size() + 2) / 3 * 4 + 8;
	std::string Out((size_t)OutSize, '\0');
	str_base64(Out.data(), OutSize, Raw.data(), (int)Raw.size());
	Out.resize(str_length(Out.c_str()));
	return Out;
}
}

class CSqliteSyncConnection : public IDbConnection
{
public:
	CSqliteSyncConnection();
	~CSqliteSyncConnection() override;
	void Print(IConsole *pConsole, const char *pMode) override;

	const char *BinaryCollate() const override { return "BINARY"; }
	void ToUnixTimestamp(const char *pTimestamp, char *aBuf, unsigned int BufferSize) override;
	const char *InsertTimestampAsUtc() const override { return "DATETIME(?, 'utc')"; }
	const char *CollateNocase() const override { return "? COLLATE NOCASE"; }
	const char *InsertIgnore() const override { return "INSERT OR IGNORE"; }
	const char *Random() const override { return "RANDOM()"; }
	const char *MedianMapTime(char *pBuffer, int BufferSize) const override;
	const char *False() const override { return "0"; }
	const char *True() const override { return "1"; }

	bool Connect(char *pError, int ErrorSize) override;
	void Disconnect() override;

	bool PrepareStatement(const char *pStmt, char *pError, int ErrorSize) override;
	void BindString(int Idx, const char *pString) override;
	void BindBlob(int Idx, unsigned char *pBlob, int Size) override;
	void BindInt(int Idx, int Value) override;
	void BindInt64(int Idx, int64_t Value) override;
	void BindFloat(int Idx, float Value) override;
	void BindNull(int Idx) override;

	void Print() override;
	bool Step(bool *pEnd, char *pError, int ErrorSize) override;
	bool ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize) override;

	bool IsNull(int Col) override;
	float GetFloat(int Col) override;
	int GetInt(int Col) override;
	int64_t GetInt64(int Col) override;
	void GetString(int Col, char *pBuffer, int BufferSize) override;
	int GetBlob(int Col, unsigned char *pBuffer, int BufferSize) override;

	bool AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize) override;

private:
	bool ConnectImpl(char *pError, int ErrorSize);
	void CloseSocket();
	void ResetStatementState();
	void EnsureBindIndex(int Idx);
	std::string FormatBindValue(const CBindValue &Value) const;
	std::string BuildExpandedSql() const;
	bool SendAll(const char *pData, int Size);
	bool ReceiveProtocolLine(std::string &Line, char *pError, int ErrorSize);
	bool SendCommand(const char *pCmd, const std::string &Sql, std::string &Payload, char *pError, int ErrorSize);
	bool ExecuteRead(const std::string &Sql, char *pError, int ErrorSize);
	bool ExecuteWrite(const std::string &Sql, int *pChanges, char *pError, int ErrorSize);
	bool DecodeReadPayload(const std::string &Payload, char *pError, int ErrorSize);
	const CRemoteCell *GetCell(int Col) const;

	char m_aHost[128];
	char m_aBindAddr[128];
	char m_aAdminKey[128];
	int m_Port;
	int m_TimeoutMs;
	bool m_ReadOnlyMode;
	NETSOCKET m_pSocket;
	std::atomic_bool m_InUse;

	std::string m_RxBuffer;
	std::string m_Stmt;
	std::vector<CBindValue> m_vBinds;
	bool m_ResultLoaded;
	bool m_ResultWrite;
	bool m_Done;
	int m_ChangesFromLastWrite;
	int m_CurrentRow;
	std::vector<std::vector<CRemoteCell>> m_vReadRows;
};

CSqliteSyncConnection::CSqliteSyncConnection() :
	IDbConnection("record"),
	m_Port(g_Config.m_SvSqliteSyncPort),
	m_TimeoutMs(g_Config.m_SvSqliteSyncTimeoutMs),
	m_ReadOnlyMode(g_Config.m_SvSqliteSyncMode != 0),
	m_pSocket(nullptr),
	m_InUse(false),
	m_ResultLoaded(false),
	m_ResultWrite(false),
	m_Done(true),
	m_ChangesFromLastWrite(0),
	m_CurrentRow(-1)
{
	str_copy(m_aHost, g_Config.m_SvSqliteSyncHost, sizeof(m_aHost));
	str_copy(m_aBindAddr, g_Config.m_SvSqliteSyncBindaddr, sizeof(m_aBindAddr));
	str_copy(m_aAdminKey, g_Config.m_SvSqliteSyncAdminKey, sizeof(m_aAdminKey));
}

CSqliteSyncConnection::~CSqliteSyncConnection()
{
	CloseSocket();
}

void CSqliteSyncConnection::Print(IConsole *pConsole, const char *pMode)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf), "SQLiteSync-%s: Host: '%s' Port: %d Mode: %s", pMode, m_aHost, m_Port, m_ReadOnlyMode ? "read-only" : "read-write");
	pConsole->Print(IConsole::OUTPUT_LEVEL_STANDARD, "server", aBuf);
}

void CSqliteSyncConnection::ToUnixTimestamp(const char *pTimestamp, char *aBuf, unsigned int BufferSize)
{
	str_format(aBuf, BufferSize, "strftime('%%s', %s)", pTimestamp);
}

const char *CSqliteSyncConnection::MedianMapTime(char *pBuffer, int BufferSize) const
{
	str_format(pBuffer, BufferSize,
		"SELECT AVG("
		"  CASE counter %% 2 "
		"    WHEN 0 THEN CASE WHEN rn IN (counter / 2, counter / 2 + 1) THEN Time END "
		"    WHEN 1 THEN CASE WHEN rn = counter / 2 + 1 THEN Time END END) "
		"  OVER (PARTITION BY Map) AS Median "
		"FROM ("
		"  SELECT *, ROW_NUMBER() "
		"  OVER (PARTITION BY Map ORDER BY Time) rn, COUNT(*) "
		"  OVER (PARTITION BY Map) counter "
		"  FROM %s_race where Map = l.Map) as r",
		GetPrefix());
	return pBuffer;
}

void CSqliteSyncConnection::CloseSocket()
{
	if(m_pSocket != nullptr)
	{
		net_tcp_close(m_pSocket);
		m_pSocket = nullptr;
	}
}

void CSqliteSyncConnection::ResetStatementState()
{
	m_ResultLoaded = false;
	m_ResultWrite = false;
	m_Done = true;
	m_ChangesFromLastWrite = 0;
	m_CurrentRow = -1;
	m_vReadRows.clear();
}

bool CSqliteSyncConnection::Connect(char *pError, int ErrorSize)
{
	if(m_InUse.exchange(true))
	{
		dbg_assert_failed("Tried connecting while the connection is in use");
	}
	if(!ConnectImpl(pError, ErrorSize))
	{
		m_InUse.store(false);
		return false;
	}
	return true;
}

bool CSqliteSyncConnection::ConnectImpl(char *pError, int ErrorSize)
{
	if(m_pSocket != nullptr)
		return true;

	if(m_aHost[0] == '\0')
	{
		str_copy(pError, "sv_sqlite_sync_host is empty", ErrorSize);
		return false;
	}
	if(m_aAdminKey[0] == '\0')
	{
		str_copy(pError, "sv_sqlite_sync_admin_key is empty", ErrorSize);
		return false;
	}

	NETADDR BindAddr = NETADDR_ZEROED;
	BindAddr.type = NETTYPE_ALL;
	if(m_aBindAddr[0] != '\0' && net_host_lookup(m_aBindAddr, &BindAddr, NETTYPE_ALL) != 0)
	{
		str_format(pError, ErrorSize, "cannot resolve sqlite sync bind address '%s'", m_aBindAddr);
		return false;
	}

	NETADDR Addr = NETADDR_ZEROED;
	if(net_host_lookup(m_aHost, &Addr, NETTYPE_ALL) != 0)
	{
		str_format(pError, ErrorSize, "cannot resolve sqlite sync host '%s'", m_aHost);
		return false;
	}
	Addr.port = m_Port;

	m_pSocket = net_tcp_create(BindAddr);
	if(m_pSocket == nullptr || net_socket_type(m_pSocket) == NETTYPE_INVALID)
	{
		str_copy(pError, "failed to create TCP socket for sqlite sync", ErrorSize);
		CloseSocket();
		return false;
	}

	if(net_tcp_connect(m_pSocket, &Addr) != 0)
	{
		char aAddr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aAddr, sizeof(aAddr), true);
		str_format(pError, ErrorSize, "failed to connect sqlite sync service at %s", aAddr);
		CloseSocket();
		return false;
	}

	return true;
}

void CSqliteSyncConnection::Disconnect()
{
	ResetStatementState();
	m_Stmt.clear();
	m_vBinds.clear();
	m_InUse.store(false);
}

bool CSqliteSyncConnection::PrepareStatement(const char *pStmt, char *pError, int ErrorSize)
{
	if(pStmt == nullptr)
	{
		str_copy(pError, "empty statement", ErrorSize);
		return false;
	}
	m_Stmt = pStmt;
	m_vBinds.clear();
	ResetStatementState();
	return true;
}

void CSqliteSyncConnection::EnsureBindIndex(int Idx)
{
	if(Idx <= 0)
		return;
	if((int)m_vBinds.size() < Idx)
		m_vBinds.resize((size_t)Idx);
}

void CSqliteSyncConnection::BindString(int Idx, const char *pString)
{
	EnsureBindIndex(Idx);
	CBindValue &Value = m_vBinds[(size_t)Idx - 1];
	Value.m_Type = EBindValueType::STRING;
	Value.m_String = pString != nullptr ? pString : "";
}

void CSqliteSyncConnection::BindBlob(int Idx, unsigned char *pBlob, int Size)
{
	EnsureBindIndex(Idx);
	CBindValue &Value = m_vBinds[(size_t)Idx - 1];
	Value.m_Type = EBindValueType::BLOB;
	Value.m_Blob.clear();
	if(pBlob != nullptr && Size > 0)
		Value.m_Blob.assign(pBlob, pBlob + Size);
}

void CSqliteSyncConnection::BindInt(int Idx, int Value)
{
	EnsureBindIndex(Idx);
	CBindValue &Bind = m_vBinds[(size_t)Idx - 1];
	Bind.m_Type = EBindValueType::INT32;
	Bind.m_Int = Value;
}

void CSqliteSyncConnection::BindInt64(int Idx, int64_t Value)
{
	EnsureBindIndex(Idx);
	CBindValue &Bind = m_vBinds[(size_t)Idx - 1];
	Bind.m_Type = EBindValueType::INT64;
	Bind.m_Int64 = Value;
}

void CSqliteSyncConnection::BindFloat(int Idx, float Value)
{
	EnsureBindIndex(Idx);
	CBindValue &Bind = m_vBinds[(size_t)Idx - 1];
	Bind.m_Type = EBindValueType::FLOAT64;
	Bind.m_Float = Value;
}

void CSqliteSyncConnection::BindNull(int Idx)
{
	EnsureBindIndex(Idx);
	CBindValue &Bind = m_vBinds[(size_t)Idx - 1];
	Bind.m_Type = EBindValueType::NULL_VALUE;
	Bind.m_String.clear();
	Bind.m_Blob.clear();
}

std::string CSqliteSyncConnection::FormatBindValue(const CBindValue &Value) const
{
	switch(Value.m_Type)
	{
	case EBindValueType::NULL_VALUE:
		return "NULL";
	case EBindValueType::STRING:
	{
		std::string Escaped;
		Escaped.reserve(Value.m_String.size() + 4);
		Escaped.push_back('\'');
		for(char c : Value.m_String)
		{
			if(c == '\'')
				Escaped.append("''");
			else
				Escaped.push_back(c);
		}
		Escaped.push_back('\'');
		return Escaped;
	}
	case EBindValueType::BLOB:
	{
		static const char DIGITS[] = "0123456789ABCDEF";
		std::string Hex;
		Hex.reserve(Value.m_Blob.size() * 2 + 3);
		Hex.append("X'");
		for(unsigned char b : Value.m_Blob)
		{
			Hex.push_back(DIGITS[(b >> 4) & 0xF]);
			Hex.push_back(DIGITS[b & 0xF]);
		}
		Hex.push_back('\'');
		return Hex;
	}
	case EBindValueType::INT32:
		return std::to_string(Value.m_Int);
	case EBindValueType::INT64:
		return std::to_string(Value.m_Int64);
	case EBindValueType::FLOAT64:
		return std::to_string(Value.m_Float);
	}
	return "NULL";
}

std::string CSqliteSyncConnection::BuildExpandedSql() const
{
	std::string Expanded;
	Expanded.reserve(m_Stmt.size() + m_vBinds.size() * 8);
	size_t BindIndex = 0;
	bool InSingle = false;
	bool InDouble = false;
	for(size_t i = 0; i < m_Stmt.size(); ++i)
	{
		const char c = m_Stmt[i];
		if(c == '\'' && !InDouble)
		{
			if(InSingle && i + 1 < m_Stmt.size() && m_Stmt[i + 1] == '\'')
			{
				Expanded.push_back(c);
				Expanded.push_back(m_Stmt[i + 1]);
				++i;
				continue;
			}
			InSingle = !InSingle;
			Expanded.push_back(c);
			continue;
		}
		if(c == '"' && !InSingle)
		{
			InDouble = !InDouble;
			Expanded.push_back(c);
			continue;
		}
		if(c == '?' && !InSingle && !InDouble)
		{
			if(BindIndex < m_vBinds.size())
			{
				Expanded.append(FormatBindValue(m_vBinds[BindIndex]));
				++BindIndex;
			}
			else
			{
				Expanded.append("NULL");
			}
			continue;
		}
		Expanded.push_back(c);
	}
	return Expanded;
}

bool CSqliteSyncConnection::SendAll(const char *pData, int Size)
{
	int Sent = 0;
	while(Sent < Size)
	{
		const int Res = net_tcp_send(m_pSocket, pData + Sent, Size - Sent);
		if(Res <= 0)
			return false;
		Sent += Res;
	}
	return true;
}

bool CSqliteSyncConnection::ReceiveProtocolLine(std::string &Line, char *pError, int ErrorSize)
{
	const auto Timeout = std::chrono::milliseconds(m_TimeoutMs);
	for(;;)
	{
		const size_t NewlinePos = m_RxBuffer.find('\n');
		if(NewlinePos != std::string::npos)
		{
			Line = m_RxBuffer.substr(0, NewlinePos);
			m_RxBuffer.erase(0, NewlinePos + 1);
			return true;
		}

		if(!net_socket_read_wait(m_pSocket, Timeout))
		{
			str_format(pError, ErrorSize, "sqlite sync request timed out after %d ms", m_TimeoutMs);
			return false;
		}
		char aBuf[4096];
		const int Res = net_tcp_recv(m_pSocket, aBuf, sizeof(aBuf));
		if(Res <= 0)
		{
			str_copy(pError, "sqlite sync connection closed", ErrorSize);
			return false;
		}
		m_RxBuffer.append(aBuf, aBuf + Res);
	}
}

bool CSqliteSyncConnection::SendCommand(const char *pCmd, const std::string &Sql, std::string &Payload, char *pError, int ErrorSize)
{
	const std::string EncodedSql = Base64Encode(Sql);
	const std::string Request = std::string(pCmd) + " " + m_aAdminKey + " B64:" + EncodedSql + "\n";
	if(!SendAll(Request.c_str(), (int)Request.size()))
	{
		str_copy(pError, "failed sending sqlite sync request", ErrorSize);
		CloseSocket();
		return false;
	}

	std::string Line;
	if(!ReceiveProtocolLine(Line, pError, ErrorSize))
	{
		CloseSocket();
		return false;
	}

	if(str_startswith(Line.c_str(), "ERR "))
	{
		const size_t Pos = Line.find(' ', 4);
		if(Pos == std::string::npos || Pos + 1 >= Line.size())
			str_copy(pError, "sqlite sync returned ERR", ErrorSize);
		else
			str_copy(pError, Line.c_str() + Pos + 1, ErrorSize);
		return false;
	}
	if(!str_startswith(Line.c_str(), "OK "))
	{
		str_copy(pError, "sqlite sync returned malformed response", ErrorSize);
		return false;
	}
	const size_t Pos = Line.find(' ', 3);
	if(Pos == std::string::npos || Pos + 1 >= Line.size())
	{
		str_copy(pError, "sqlite sync returned malformed OK payload", ErrorSize);
		return false;
	}
	Payload = Line.substr(Pos + 1);
	return true;
}

bool CSqliteSyncConnection::DecodeReadPayload(const std::string &Payload, char *pError, int ErrorSize)
{
	if(!str_startswith(Payload.c_str(), "READBIN "))
	{
		str_copy(pError, "sqlite sync READ payload does not start with READBIN", ErrorSize);
		return false;
	}
	const std::string Encoded = Payload.substr(8);
	const int MaxDecoded = ((int)Encoded.size() * 3) / 4 + 8;
	std::vector<unsigned char> vDecoded((size_t)MaxDecoded);
	const int DecodedSize = str_base64_decode(vDecoded.data(), MaxDecoded, Encoded.c_str());
	if(DecodedSize < 0)
	{
		str_copy(pError, "sqlite sync READ payload is not valid base64", ErrorSize);
		return false;
	}
	vDecoded.resize((size_t)DecodedSize);

	size_t Offset = 0;
	auto ReadU8 = [&](unsigned char *pOut) -> bool {
		if(Offset + 1 > vDecoded.size())
			return false;
		*pOut = vDecoded[Offset];
		Offset += 1;
		return true;
	};
	auto ReadU32 = [&](uint32_t *pOut) -> bool {
		if(Offset + 4 > vDecoded.size())
			return false;
		*pOut = (uint32_t)vDecoded[Offset] | ((uint32_t)vDecoded[Offset + 1] << 8) | ((uint32_t)vDecoded[Offset + 2] << 16) | ((uint32_t)vDecoded[Offset + 3] << 24);
		Offset += 4;
		return true;
	};

	uint32_t RowCount = 0;
	uint32_t ColCount = 0;
	if(!ReadU32(&RowCount) || !ReadU32(&ColCount))
	{
		str_copy(pError, "sqlite sync READ payload truncated header", ErrorSize);
		return false;
	}
	if(RowCount > 1000000 || ColCount > 512)
	{
		str_copy(pError, "sqlite sync READ payload has unreasonable dimensions", ErrorSize);
		return false;
	}

	m_vReadRows.clear();
	m_vReadRows.resize(RowCount);
	for(uint32_t r = 0; r < RowCount; r++)
	{
		auto &vRow = m_vReadRows[r];
		vRow.resize(ColCount);
		for(uint32_t c = 0; c < ColCount; c++)
		{
			unsigned char Type = 0;
			if(!ReadU8(&Type))
			{
				str_copy(pError, "sqlite sync READ payload truncated cell type", ErrorSize);
				return false;
			}
			CRemoteCell &Cell = vRow[c];
			Cell.m_Type = (int)Type;
			if(Type == 0)
				continue;

			uint32_t Len = 0;
			if(!ReadU32(&Len) || Offset + Len > vDecoded.size())
			{
				str_copy(pError, "sqlite sync READ payload truncated cell data", ErrorSize);
				return false;
			}
			if(Type == 4)
			{
				Cell.m_Blob.assign(vDecoded.begin() + (std::ptrdiff_t)Offset, vDecoded.begin() + (std::ptrdiff_t)(Offset + Len));
			}
			else
			{
				Cell.m_Text.assign((const char *)vDecoded.data() + Offset, (size_t)Len);
			}
			Offset += Len;
		}
	}
	if(Offset != vDecoded.size())
	{
		str_copy(pError, "sqlite sync READ payload has trailing bytes", ErrorSize);
		return false;
	}

	m_CurrentRow = -1;
	m_Done = false;
	return true;
}

bool CSqliteSyncConnection::ExecuteRead(const std::string &Sql, char *pError, int ErrorSize)
{
	std::string Payload;
	if(!SendCommand("READ", Sql, Payload, pError, ErrorSize))
		return false;
	return DecodeReadPayload(Payload, pError, ErrorSize);
}

bool CSqliteSyncConnection::ExecuteWrite(const std::string &Sql, int *pChanges, char *pError, int ErrorSize)
{
	if(m_ReadOnlyMode)
	{
		*pChanges = 0;
		return true;
	}

	std::string Payload;
	if(!SendCommand("WRITE", Sql, Payload, pError, ErrorSize))
		return false;
	if(!str_startswith(Payload.c_str(), "WRITE "))
	{
		str_copy(pError, "sqlite sync WRITE payload is malformed", ErrorSize);
		return false;
	}
	*pChanges = str_toint(Payload.c_str() + 6);
	return true;
}

void CSqliteSyncConnection::Print()
{
	const std::string Expanded = BuildExpandedSql();
	dbg_msg("sql", "SQLiteSync statement: %s", Expanded.c_str());
}

bool CSqliteSyncConnection::Step(bool *pEnd, char *pError, int ErrorSize)
{
	if(!m_ResultLoaded)
	{
		const std::string Expanded = BuildExpandedSql();
		m_ResultWrite = IsWriteSql(Expanded);
		if(m_ResultWrite)
		{
			if(!ExecuteWrite(Expanded, &m_ChangesFromLastWrite, pError, ErrorSize))
				return false;
			m_Done = true;
		}
		else if(!ExecuteRead(Expanded, pError, ErrorSize))
		{
			return false;
		}
		m_ResultLoaded = true;
	}

	if(m_ResultWrite)
	{
		*pEnd = true;
		return true;
	}

	if(m_Done || m_CurrentRow + 1 >= (int)m_vReadRows.size())
	{
		m_Done = true;
		*pEnd = true;
		return true;
	}

	++m_CurrentRow;
	*pEnd = false;
	return true;
}

bool CSqliteSyncConnection::ExecuteUpdate(int *pNumUpdated, char *pError, int ErrorSize)
{
	const std::string Expanded = BuildExpandedSql();
	return ExecuteWrite(Expanded, pNumUpdated, pError, ErrorSize);
}

const CRemoteCell *CSqliteSyncConnection::GetCell(int Col) const
{
	const int ColIdx = Col - 1;
	if(m_CurrentRow < 0 || m_CurrentRow >= (int)m_vReadRows.size())
		return nullptr;
	const auto &vRow = m_vReadRows[(size_t)m_CurrentRow];
	if(ColIdx < 0 || ColIdx >= (int)vRow.size())
		return nullptr;
	return &vRow[(size_t)ColIdx];
}

bool CSqliteSyncConnection::IsNull(int Col)
{
	const CRemoteCell *pCell = GetCell(Col);
	return pCell == nullptr || pCell->m_Type == 0;
}

float CSqliteSyncConnection::GetFloat(int Col)
{
	const CRemoteCell *pCell = GetCell(Col);
	if(pCell == nullptr)
		return 0.0f;
	return str_tofloat(pCell->m_Text.c_str());
}

int CSqliteSyncConnection::GetInt(int Col)
{
	const CRemoteCell *pCell = GetCell(Col);
	if(pCell == nullptr)
		return 0;
	return str_toint(pCell->m_Text.c_str());
}

int64_t CSqliteSyncConnection::GetInt64(int Col)
{
	const CRemoteCell *pCell = GetCell(Col);
	if(pCell == nullptr)
		return 0;
	return str_toint64_base(pCell->m_Text.c_str(), 10);
}

void CSqliteSyncConnection::GetString(int Col, char *pBuffer, int BufferSize)
{
	const CRemoteCell *pCell = GetCell(Col);
	if(pCell == nullptr)
	{
		pBuffer[0] = '\0';
		return;
	}
	str_copy(pBuffer, pCell->m_Text.c_str(), BufferSize);
}

int CSqliteSyncConnection::GetBlob(int Col, unsigned char *pBuffer, int BufferSize)
{
	const CRemoteCell *pCell = GetCell(Col);
	if(pCell == nullptr || pCell->m_Type != 4)
		return 0;
	const int Size = minimum((int)pCell->m_Blob.size(), BufferSize);
	if(Size > 0)
		mem_copy(pBuffer, pCell->m_Blob.data(), Size);
	return Size;
}

bool CSqliteSyncConnection::AddPoints(const char *pPlayer, int Points, char *pError, int ErrorSize)
{
	char aBuf[512];
	str_format(aBuf, sizeof(aBuf),
		"INSERT INTO %s_points(Name, Points) "
		"VALUES (?, ?) "
		"ON CONFLICT(Name) DO UPDATE SET Points=Points+?",
		GetPrefix());
	if(!PrepareStatement(aBuf, pError, ErrorSize))
	{
		return false;
	}
	BindString(1, pPlayer);
	BindInt(2, Points);
	BindInt(3, Points);
	int Updated = 0;
	return ExecuteUpdate(&Updated, pError, ErrorSize);
}

std::unique_ptr<IDbConnection> CreateSqliteConnection(const char *pFilename, bool Setup)
{
	if(g_Config.m_SvSqliteSyncEnable && str_comp(pFilename, "sqlite-sync-remote") == 0)
	{
		return std::make_unique<CSqliteSyncConnection>();
	}
	return std::make_unique<CSqliteConnection>(pFilename, Setup);
}

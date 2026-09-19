// 请抬头享受阳光｜日子很好 我很我---------致咩子
#include "qmclient.h"

#include <base/hash.h>
#include <base/lock.h>
#include <base/log.h>
#include <base/str.h>
#include <base/system.h>
#include <base/windows.h>

#include <engine/client.h>
#include <engine/client/enums.h>
#include <engine/client/updater.h>
#include <engine/engine.h>
#include <engine/external/regex.h>
#include <engine/external/tinyexpr.h>
#include <engine/friends.h>
#include <engine/graphics.h>
#include <engine/keys.h>
#include <engine/map.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/shared/jobs.h>
#include <engine/shared/json.h>
#include <engine/shared/jsonwriter.h>
#include <engine/shared/localization.h>
#include <engine/storage.h>

#include <generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/components/chat.h>
#include <game/client/components/qmclient/qm_sponsors.h>
#include <game/client/components/qmclient/qm_title_style.h>
#include <game/client/components/qmclient/voice/voice_utils.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>
#include <game/client/ui.h>
#include <game/layers.h>
#include <game/localization.h>
#include <game/mapitems.h>
#include <game/version.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(CONF_FAMILY_WINDOWS)
#include <windows.h>
#endif

[[maybe_unused]] static constexpr const char *TCLIENT_UPDATE_EXE_URL = "https://github.com/wxj881027/QmClient/releases/latest/download/DDNet.exe";
[[maybe_unused]] static constexpr const char *MAP_CATEGORY_CACHE_FILE = "qmclient/map_categories.json";
[[maybe_unused]] static constexpr int64_t MAP_CATEGORY_CACHE_SAVE_DELAY_SEC = 5;

static void LogQmClientDistributionEvent(const char *pStage, int Users, int Dummies, int LocalMarks)
{
	log_info("qmclient", "distribution %s: users=%d dummies=%d local_marks=%d", pStage, Users, Dummies, LocalMarks);
}

static void LogQmClientDistributionFailureEvent(const char *pStage, const char *pDetail)
{
	log_warn("qmclient", "distribution %s: %s", pStage, pDetail ? pDetail : "");
}

// 广播发布为用户主动 HTTPS 操作，后台状态只走 WS。
static constexpr const char *QMCLIENT_DEVELOPER_TOKEN_FILE = "qmclient/developer_token.txt";
static constexpr const char *QMCLIENT_NEWS_PUBLISH_URL = "https://qmclient.icu/api/v1/news/publish";
static constexpr const char *QMCLIENT_NEWS_CACHE_FILE = "qmclient/news_cache.json";
static constexpr const char *QMCLIENT_NEWS_DRAFT_FILE = "qmclient/news_draft.md";
static constexpr int QMCLIENT_NEWS_CACHE_VERSION = 1;
static constexpr int QMCLIENT_NEWS_MAX_BYTES = 64 * 1024;
static constexpr const char *QMCLIENT_SPONSORS_PUBLISH_URL = "https://qmclient.icu/api/v1/sponsors/publish";
static constexpr const char *QMCLIENT_SPONSORS_CACHE_FILE = "qmclient/sponsors_cache.json";
static constexpr const char *QMCLIENT_SPONSORS_DRAFT_FILE = "qmclient/sponsors_draft.md";
static constexpr int QMCLIENT_DEVELOPER_SYNC_INTERVAL_SECONDS = 5;
static constexpr const char *QMCLIENT_LIFECYCLE_MARKER_FILE = "qmclient/lifecycle_pending.marker";
static constexpr const char *QMCLIENT_PLAYTIME_CLIENT_ID_FILE = "qmclient/playtime_client_id.txt";
static constexpr const char *QMCLIENT_MACHINE_ID_FALLBACK_FILE = "qmclient/voice_machine_id.txt";
static constexpr int QMCLIENT_MARKER_FLUSH_INTERVAL_SECONDS = 5;
static constexpr const char *DDNET_PLAYER_STATS_URL = "https://ddnet.org/players/?json2=";
static constexpr int QMCLIENT_DDNET_PLAYER_SYNC_INTERVAL_SECONDS = 120;
static constexpr int QMCLIENT_DDNET_PLAYER_RETRY_DELAY_SECONDS = 10;
static constexpr const char *QMCLIENT_FREEZE_WAKEUP_TEXT = "快醒醒!";
// 实时通道订阅协议版本，服务端可据此兼容老客户端。
static constexpr int QMCLIENT_REALTIME_PROTOCOL_VERSION = 2;

[[maybe_unused]] static bool TextContainsAny(const char *pText, const std::initializer_list<const char *> &Tokens)
{
	if(!pText || pText[0] == '\0')
		return false;

	for(const char *pToken : Tokens)
	{
		if(pToken && pToken[0] != '\0' && str_find_nocase(pText, pToken))
			return true;
	}
	return false;
}
[[maybe_unused]] static constexpr float QMCLIENT_TEXT_POPUP_FONT_SIZE = 30.0f;
[[maybe_unused]] static constexpr vec2 QMCLIENT_FREEZE_WAKEUP_POPUP_OFFSET = vec2(34.0f, -78.0f);
[[maybe_unused]] static constexpr vec2 QMCLIENT_FREEZE_WAKEUP_POPUP_DRIFT = vec2(18.0f, -16.0f);
[[maybe_unused]] static constexpr int QMCLIENT_COMBO_POPUP_WINDOW_SECONDS = 2;
[[maybe_unused]] static constexpr ColorRGBA QMCLIENT_POPUP_ROLL_COLOR_FROM = ColorRGBA(0.0f, 1.0f, 1.0f, 1.0f);
[[maybe_unused]] static constexpr ColorRGBA QMCLIENT_POPUP_ROLL_COLOR_TO = ColorRGBA(1.0f, 0.0f, 1.0f, 1.0f);
[[maybe_unused]] static constexpr const char *s_apKeywordNegationWords[] = {
	"不",
	"没",
	"無",
	"无",
	"別",
	"别",
	"勿",
	"莫",
	"非",
	"未",
	"沒",
};
[[maybe_unused]] static constexpr const char *s_apKeywordClauseContrastWords[] = {
	"但是",
	"但",
	"不过",
	"然而",
	"可是",
};
// 与 tclient 一致：默认英文 source key，空模板回退用
static constexpr const char *s_pFriendEnterBroadcastDefaultText = "%s joined this server";

[[maybe_unused]] static int AutoReplySeparatorLength(const char *pStr);
[[maybe_unused]] static bool AppendAutoReplyRuleBlock(char *pOutRules, size_t OutRulesSize, const char *pRules);
static const json_value *JsonObjectField(const json_value *pObject, const char *pName);
static bool JsonReadNonNegativeInt64(const json_value *pValue, int64_t &OutValue);

namespace
{
	enum class ETextPopupType
	{
		FREEZE_WAKEUP = 0,
		NUM_TYPES,
	};

	struct STextPopupDefinition
	{
		const char *m_pText;
	};

	[[maybe_unused]] static constexpr std::array<STextPopupDefinition, (int)ETextPopupType::NUM_TYPES> s_aTextPopupDefinitions = {{
		{QMCLIENT_FREEZE_WAKEUP_TEXT},
	}};

	class CQmClientUsersParseJob : public IJob
	{
	public:
		using SResult = SQmClientUsersParseResult;

	private:
		std::shared_ptr<const json_value> m_pPayload;
		char m_aServerAddress[NETADDR_MAXSTRSIZE] = "";
		int64_t m_ExpireTick = 0;
		CLock m_Lock;
		SResult m_Result;

	protected:
		void Run() override REQUIRES(!m_Lock)
		{
			SResult Result;
			if(m_pPayload)
				ParseQmClientUsersJson(m_pPayload.get(), m_aServerAddress, Result);

			{
				const CLockScope Lock(m_Lock);
				m_Result = std::move(Result);
			}
			m_pPayload.reset();
		}

	public:
		CQmClientUsersParseJob(std::shared_ptr<const json_value> pPayload, const char *pServerAddress, int64_t ExpireTick) :
			m_pPayload(std::move(pPayload)),
			m_ExpireTick(ExpireTick)
		{
			str_copy(m_aServerAddress, pServerAddress, sizeof(m_aServerAddress));
		}

		SResult TakeResult() REQUIRES(!m_Lock)
		{
			const CLockScope Lock(m_Lock);
			SResult Result = std::move(m_Result);
			m_Result = SResult();
			return Result;
		}

		const char *ServerAddress() const { return m_aServerAddress; }
		int64_t ExpireTick() const { return m_ExpireTick; }
	};

	class CQmDdnetPlayerStatsParseJob : public IJob
	{
	public:
		struct SResult
		{
			bool m_Parsed = false;
			std::string m_FavoritePartner;
			int m_TotalFinishes = -1;
		};

	private:
		std::shared_ptr<CHttpRequest> m_pTask;
		CLock m_Lock;
		SResult m_Result;

	protected:
		void Run() override REQUIRES(!m_Lock)
		{
			SResult Result;
			if(m_pTask && m_pTask->State() == EHttpState::DONE && m_pTask->StatusCode() == 200)
			{
				json_value *pRoot = m_pTask->ResultJson();
				if(pRoot && pRoot->type == json_object)
				{
					const json_value *pFavoritePartners = JsonObjectField(pRoot, "favorite_partners");
					if(pFavoritePartners->type == json_array)
					{
						const char *pBestPartner = nullptr;
						int BestPartnerFinishes = -1;
						for(unsigned i = 0; i < pFavoritePartners->u.array.length; ++i)
						{
							const json_value &Partner = (*pFavoritePartners)[i];
							if(Partner.type != json_object)
								continue;

							const json_value *pName = JsonObjectField(&Partner, "name");
							if(pName->type != json_string)
								continue;

							const char *pPartnerName = json_string_get(pName);
							if(!pPartnerName || pPartnerName[0] == '\0')
								continue;

							int PartnerFinishes = 0;
							const json_value *pFinishes = JsonObjectField(&Partner, "finishes");
							if(pFinishes->type == json_integer && pFinishes->u.integer > 0)
							{
								if(pFinishes->u.integer > std::numeric_limits<int>::max())
									PartnerFinishes = std::numeric_limits<int>::max();
								else
									PartnerFinishes = (int)pFinishes->u.integer;
							}

							if(!pBestPartner ||
								PartnerFinishes > BestPartnerFinishes ||
								(PartnerFinishes == BestPartnerFinishes && str_comp_nocase(pPartnerName, pBestPartner) < 0))
							{
								pBestPartner = pPartnerName;
								BestPartnerFinishes = PartnerFinishes;
							}
						}

						if(pBestPartner)
							Result.m_FavoritePartner = pBestPartner;
					}

					const json_value *pTypes = JsonObjectField(pRoot, "types");
					if(pTypes->type == json_object)
					{
						Result.m_Parsed = true;
						int64_t TotalFinishes = 0;
						for(unsigned i = 0; i < pTypes->u.object.length; ++i)
						{
							const json_value *pTypeObj = pTypes->u.object.values[i].value;
							if(!pTypeObj || pTypeObj->type != json_object)
								continue;

							const json_value *pMaps = JsonObjectField(pTypeObj, "maps");
							if(pMaps->type != json_object)
								continue;

							for(unsigned j = 0; j < pMaps->u.object.length; ++j)
							{
								const json_value *pMapObj = pMaps->u.object.values[j].value;
								if(!pMapObj || pMapObj->type != json_object)
									continue;

								const json_value *pFinishes = JsonObjectField(pMapObj, "finishes");
								if(pFinishes->type != json_integer || pFinishes->u.integer <= 0 || TotalFinishes >= std::numeric_limits<int>::max())
									continue;

								int64_t SafeAdd = pFinishes->u.integer;
								if(SafeAdd > std::numeric_limits<int>::max())
									SafeAdd = std::numeric_limits<int>::max();

								const int64_t MaxTotal = std::numeric_limits<int>::max();
								if(SafeAdd > MaxTotal - TotalFinishes)
									TotalFinishes = MaxTotal;
								else
									TotalFinishes += SafeAdd;
							}
						}
						Result.m_TotalFinishes = (int)TotalFinishes;
					}
					json_value_free(pRoot);
				}
			}

			{
				const CLockScope Lock(m_Lock);
				m_Result = std::move(Result);
			}
			m_pTask = nullptr;
		}

	public:
		explicit CQmDdnetPlayerStatsParseJob(std::shared_ptr<CHttpRequest> pTask) :
			m_pTask(std::move(pTask))
		{
		}

		SResult TakeResult() REQUIRES(!m_Lock)
		{
			const CLockScope Lock(m_Lock);
			SResult Result = std::move(m_Result);
			m_Result = SResult();
			return Result;
		}
	};

	class CQmClientLifecycleMarkerWriteJob : public IJob
	{
		IStorage *m_pStorage = nullptr;
		std::string m_Content;
		std::shared_ptr<std::mutex> m_pMutex;

	protected:
		void Run() override
		{
			if(m_pMutex == nullptr)
				return;
			std::lock_guard<std::mutex> Lock(*m_pMutex);
			if(m_pStorage == nullptr || State() == IJob::STATE_ABORTED)
				return;

			m_pStorage->CreateFolder("qmclient", IStorage::TYPE_SAVE);
			IOHANDLE File = m_pStorage->OpenFile(QMCLIENT_LIFECYCLE_MARKER_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
			if(!File)
				return;
			if(State() == IJob::STATE_ABORTED)
			{
				io_close(File);
				return;
			}

			io_write(File, m_Content.data(), m_Content.size());
			io_close(File);
		}

	public:
		CQmClientLifecycleMarkerWriteJob(IStorage *pStorage, std::string Content, std::shared_ptr<std::mutex> pMutex) :
			m_pStorage(pStorage),
			m_Content(std::move(Content)),
			m_pMutex(std::move(pMutex))
		{
			Abortable(true);
		}
	};

}

// NOLINTNEXTLINE(misc-use-internal-linkage)
struct SKeywordReplyRule
{
	std::string m_Keywords;
	std::string m_Reply;
	bool m_AutoRename = false;
	bool m_Regex = false;
	bool m_HasExplicitRenameFlag = false;
	bool m_HasExplicitRegexFlag = false;
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
const char *GetEffectiveQmVoiceServer()
{
	return VoiceUtils::EffectiveVoiceWebSocketUrl(g_Config.m_QmVoiceServer);
}

static void TrimQmClientTextInPlace(char *pText)
{
	if(!pText || pText[0] == '\0')
		return;
	char *pTrimmed = (char *)str_utf8_skip_whitespaces(pText);
	str_utf8_trim_right(pTrimmed);
	if(pTrimmed != pText)
		mem_move(pText, pTrimmed, str_length(pTrimmed) + 1);
}

static char *ParseAutoReplyRulePrefixes(char *pLine, bool &OutAutoRename, bool &OutRegex, bool &OutHasExplicitRenameFlag, bool &OutHasExplicitRegexFlag)
{
	OutAutoRename = false;
	OutRegex = false;
	OutHasExplicitRenameFlag = false;
	OutHasExplicitRegexFlag = false;

	char *pTrimmedLine = (char *)str_utf8_skip_whitespaces(pLine);
	while(true)
	{
		const char *pAfterPrefix = str_startswith_nocase(pTrimmedLine, "[rename]");
		if(!pAfterPrefix)
			pAfterPrefix = str_startswith_nocase(pTrimmedLine, "[r]");
		if(pAfterPrefix)
		{
			OutAutoRename = true;
			OutHasExplicitRenameFlag = true;
			pTrimmedLine = (char *)str_utf8_skip_whitespaces(pAfterPrefix);
			continue;
		}

		pAfterPrefix = str_startswith_nocase(pTrimmedLine, "[regex]");
		if(!pAfterPrefix)
			pAfterPrefix = str_startswith_nocase(pTrimmedLine, "[re]");
		if(!pAfterPrefix)
			pAfterPrefix = str_startswith_nocase(pTrimmedLine, "[rx]");
		if(pAfterPrefix)
		{
			OutRegex = true;
			OutHasExplicitRegexFlag = true;
			pTrimmedLine = (char *)str_utf8_skip_whitespaces(pAfterPrefix);
			continue;
		}

		break;
	}

	return pTrimmedLine;
}

[[maybe_unused]] static void ParseKeywordReplyRules(const char *pRules, std::vector<SKeywordReplyRule> &vOutRules)
{
	vOutRules.clear();
	if(!pRules || pRules[0] == '\0')
		return;

	const char *pCursor = pRules;
	while(*pCursor)
	{
		char aLine[sizeof(g_Config.m_QmKeywordReplyRules)];
		int LineLen = 0;
		while(*pCursor && *pCursor != '\n' && *pCursor != '\r')
		{
			if(LineLen < (int)sizeof(aLine) - 1)
				aLine[LineLen++] = *pCursor;
			++pCursor;
		}
		aLine[LineLen] = '\0';

		while(*pCursor == '\n' || *pCursor == '\r')
			++pCursor;

		char *pLine = (char *)str_utf8_skip_whitespaces(aLine);
		str_utf8_trim_right(pLine);
		if(pLine[0] == '\0' || pLine[0] == '#')
			continue;

		bool AutoRename = false;
		bool RegexRule = false;
		bool HasExplicitRenameFlag = false;
		bool HasExplicitRegexFlag = false;
		char *pRuleText = ParseAutoReplyRulePrefixes(pLine, AutoRename, RegexRule, HasExplicitRenameFlag, HasExplicitRegexFlag);
		const char *pArrowConst = str_find(pRuleText, "=>");
		if(!pArrowConst)
			continue;

		char *pArrow = pRuleText + (pArrowConst - pRuleText);
		*pArrow = '\0';
		pArrow += 2;

		char *pKeywords = (char *)str_utf8_skip_whitespaces(pRuleText);
		str_utf8_trim_right(pKeywords);
		char *pReply = (char *)str_utf8_skip_whitespaces(pArrow);
		str_utf8_trim_right(pReply);
		if(pKeywords[0] == '\0' || pReply[0] == '\0')
			continue;

		vOutRules.push_back({pKeywords, pReply, AutoRename, RegexRule, HasExplicitRenameFlag, HasExplicitRegexFlag});
	}
}

[[maybe_unused]] static void BuildKeywordReplyRules(const std::vector<SKeywordReplyRule> &vRules, char *pOutRules, size_t OutRulesSize)
{
	if(!pOutRules || OutRulesSize == 0)
		return;

	pOutRules[0] = '\0';
	for(const auto &Rule : vRules)
	{
		if(Rule.m_Keywords.empty() || Rule.m_Reply.empty())
			continue;

		if(pOutRules[0] != '\0')
			str_append(pOutRules, "\n", OutRulesSize);
		if(Rule.m_AutoRename)
			str_append(pOutRules, "[rename] ", OutRulesSize);
		if(Rule.m_Regex)
			str_append(pOutRules, "[regex] ", OutRulesSize);
		str_append(pOutRules, Rule.m_Keywords.c_str(), OutRulesSize);
		str_append(pOutRules, "=>", OutRulesSize);
		str_append(pOutRules, Rule.m_Reply.c_str(), OutRulesSize);
	}
}

[[maybe_unused]] static bool ReadQmClientAbsoluteTextFile(const char *pFilename, char *pBuf, size_t BufSize)
{
	if(!pFilename || !pBuf || BufSize == 0)
		return false;

	IOHANDLE File = io_open(pFilename, IOFLAG_READ);
	if(!File)
		return false;

	const int Read = io_read(File, pBuf, (unsigned)(BufSize - 1));
	io_close(File);
	if(Read <= 0)
		return false;

	pBuf[Read] = '\0';
	TrimQmClientTextInPlace(pBuf);
	return pBuf[0] != '\0';
}

static bool ReadPlatformMachineIdentity(std::string &OutIdentity)
{
#if defined(CONF_FAMILY_WINDOWS)
	HKEY Key = nullptr;
	LONG OpenResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &Key);
	if(OpenResult != ERROR_SUCCESS)
		OpenResult = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ, &Key);
	if(OpenResult == ERROR_SUCCESS && Key != nullptr)
	{
		wchar_t aValue[256] = {};
		DWORD Type = 0;
		DWORD Size = sizeof(aValue);
		const LONG QueryResult = RegQueryValueExW(Key, L"MachineGuid", nullptr, &Type, reinterpret_cast<LPBYTE>(aValue), &Size);
		RegCloseKey(Key);
		if(QueryResult == ERROR_SUCCESS && Type == REG_SZ)
		{
			const auto Utf8 = windows_wide_to_utf8(aValue);
			if(Utf8.has_value() && !Utf8->empty())
			{
				OutIdentity = *Utf8;
				return true;
			}
		}
	}
#elif defined(CONF_PLATFORM_LINUX)
	char aBuf[256];
	if(ReadQmClientAbsoluteTextFile("/etc/machine-id", aBuf, sizeof(aBuf)) ||
		ReadQmClientAbsoluteTextFile("/var/lib/dbus/machine-id", aBuf, sizeof(aBuf)))
	{
		OutIdentity = aBuf;
		return true;
	}
#endif

	return false;
}

static bool IsValidQmClientMachineHash(const char *pHash)
{
	if(!pHash || str_length(pHash) != SHA256_DIGEST_LENGTH * 2)
		return false;

	for(const char *p = pHash; *p; ++p)
	{
		if(!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')))
			return false;
	}
	return true;
}

[[maybe_unused]] static std::string BuildFriendEnterBroadcastText(const char *pTemplate, std::string_view FriendNames)
{
	const char *pFormat = pTemplate != nullptr && pTemplate[0] != '\0' ? pTemplate : s_pFriendEnterBroadcastDefaultText;
	std::string Result;
	Result.reserve(str_length(pFormat) + FriendNames.size() + 8);

	const std::string_view Placeholder = "%s";
	const std::string_view FormatView = pFormat;
	size_t Pos = 0;
	bool Replaced = false;
	while(true)
	{
		const size_t Match = FormatView.find(Placeholder, Pos);
		if(Match == std::string_view::npos)
		{
			Result.append(FormatView.substr(Pos));
			break;
		}

		Result.append(FormatView.substr(Pos, Match - Pos));
		Result.append(FriendNames);
		Pos = Match + Placeholder.size();
		Replaced = true;
	}

	if(!Replaced)
	{
		// Backward compatibility: if users remove '%s', keep friend names visible.
		Result.clear();
		Result.reserve(FriendNames.size() + FormatView.size());
		Result.append(FriendNames);
		Result.append(FormatView);
	}

	return Result;
}

static const json_value *JsonObjectField(const json_value *pObject, const char *pName)
{
	if(!pObject || pObject->type != json_object)
		return &json_value_none;
	return json_object_get(pObject, pName);
}

static bool JsonReadNonNegativeInt64(const json_value *pValue, int64_t &OutValue)
{
	if(!pValue)
		return false;

	if(pValue->type == json_integer)
	{
		if(pValue->u.integer < 0)
			return false;
		OutValue = pValue->u.integer;
		return true;
	}
	if(pValue->type == json_double)
	{
		if(pValue->u.dbl < 0.0)
			return false;
		OutValue = (int64_t)pValue->u.dbl;
		return true;
	}
	return false;
}

static bool IsValidQmClientPlaytimeId(const char *pClientId)
{
	if(!pClientId)
		return false;

	const int Len = str_length(pClientId);
	if(Len < 8 || Len > 64)
		return false;

	for(int i = 0; i < Len; ++i)
	{
		const unsigned char C = (unsigned char)pClientId[i];
		if(std::isalnum(C) || C == '_' || C == '-')
			continue;
		return false;
	}
	return true;
}

void CQmClient::OnInit()
{
	InitQmClientLifecycle();
	InitQmDeveloperAuthentication();
	InitTitleAuthentication();
	InitQmNews();
	InitQmSponsors();
	// 实时通道：worker 线程在 StartQmRealtime 里起，真正连接由 OnUpdate 驱动。
	StartQmRealtime();
	StartQmAnonymousEmotes();
}

void CQmClient::OnShutdown()
{
	if(!m_QmClientShutdownReported)
	{
		m_QmClientShutdownReported = true;
		TouchQmClientLifecycleMarker(true);
		SendQmRealtimeStop();
	}
	StopQmAnonymousEmotes();
	StopQmRealtime();
	for(auto *pTask : {&m_pQmDdnetPlayerTask, &m_pTitleOperation, &m_pQmNewsPublishTask, &m_pQmSponsorsPublishTask})
	{
		if(*pTask)
			(*pTask)->Abort();
		pTask->reset();
	}
	ResetTitlePresences();
	m_pQmClientUsersParseJob.reset();
	m_pQmRealtimeUsersPayload.reset();
	m_pQmDdnetPlayerParseJob.reset();
	m_QmRemoteEmoticonEvents.clear();
}

void CQmClient::OnUpdate()
{
	UpdateQmRealtime();
	UpdateQmAnonymousEmotes();
	UpdateQmClientRecognition();
	UpdateTitleAuthentication();
	UpdateQmClientLifecycleAndServerTime();
	UpdateQmDdnetPlayerStats();
	if(m_pQmNewsPublishTask && m_pQmNewsPublishTask->Done())
		FinishQmNewsPublish();
	if(m_pQmSponsorsPublishTask && m_pQmSponsorsPublishTask->Done())
		FinishQmSponsorsPublish();
}

void CQmClient::OnStateChange(int NewState, int OldState)
{
	if((NewState == IClient::STATE_QUITTING || NewState == IClient::STATE_RESTARTING) && !m_QmClientShutdownReported)
	{
		m_QmClientShutdownReported = true;
		TouchQmClientLifecycleMarker(true);
		SendQmRealtimeStop();
	}
	if(NewState != IClient::STATE_ONLINE && OldState == IClient::STATE_ONLINE)
		StopQmAnonymousEmotes();
	if(NewState == IClient::STATE_ONLINE && OldState != IClient::STATE_ONLINE)
		secure_random_password(m_aQmDeveloperSessionId, sizeof(m_aQmDeveloperSessionId), sizeof(m_aQmDeveloperSessionId) - 1);
	else if(NewState != IClient::STATE_ONLINE && OldState == IClient::STATE_ONLINE)
	{
		GameClient()->ClearQmDeveloperMarks();
		GameClient()->ClearQ1menGSyncMarks();
		GameClient()->ClearQmVoiceSyncMarks();
		ResetTitlePresences();
		m_aQmDeveloperSessionId[0] = '\0';
		m_QmRemoteEmoticonEvents.clear();
	}
	m_QmRealtimePresenceBody.clear();
	m_QmRealtimeNextPresenceCheck = 0;
}

bool CQmClient::ReadQmClientLifecycleMarker(int64_t &OutStartedAt, int64_t &OutLastSeenAt)
{
	OutStartedAt = 0;
	OutLastSeenAt = 0;

	void *pFileData = nullptr;
	unsigned FileSize = 0;
	if(!Storage()->ReadFile(QMCLIENT_LIFECYCLE_MARKER_FILE, IStorage::TYPE_SAVE, &pFileData, &FileSize))
		return false;

	std::string Marker;
	if(pFileData && FileSize > 0)
		Marker.assign(static_cast<const char *>(pFileData), FileSize);
	free(pFileData);
	if(Marker.empty())
		return true;

	char aLine[256];
	const char *pStr = Marker.c_str();
	while((pStr = str_next_token(pStr, "\n", aLine, sizeof(aLine))))
	{
		if(const char *pValue = str_startswith(aLine, "started_at="))
			OutStartedAt = maximum<int64_t>(0, str_toint(pValue));
		else if(const char *pLastSeenValue = str_startswith(aLine, "last_seen_at="))
			OutLastSeenAt = maximum<int64_t>(0, str_toint(pLastSeenValue));
	}
	return true;
}

void CQmClient::WriteQmClientLifecycleMarker()
{
	std::lock_guard<std::mutex> Lock(*m_pQmClientLifecycleMarkerMutex);
	if(m_QmClientMarkerStartedAt <= 0)
		m_QmClientMarkerStartedAt = time_timestamp();
	if(m_QmClientMarkerLastSeenAt <= 0)
		m_QmClientMarkerLastSeenAt = m_QmClientMarkerStartedAt;

	Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);

	IOHANDLE File = Storage()->OpenFile(QMCLIENT_LIFECYCLE_MARKER_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;

	char aLine[384];
	str_format(aLine, sizeof(aLine),
		"session=%s\nstarted_at=%d\nlast_seen_at=%d\nclient_id=%s\n",
		m_aQmClientLifecycleSessionId, (int)m_QmClientMarkerStartedAt, (int)m_QmClientMarkerLastSeenAt, m_aQmClientPlaytimeClientId);
	io_write(File, aLine, str_length(aLine));
	io_close(File);
}

void CQmClient::TouchQmClientLifecycleMarker(bool ForceWrite)
{
	const int64_t NowTick = time_get();
	const int64_t Interval = (int64_t)QMCLIENT_MARKER_FLUSH_INTERVAL_SECONDS * time_freq();
	if(!ForceWrite && m_QmClientMarkerLastFlushTick != 0 && NowTick - m_QmClientMarkerLastFlushTick < Interval)
		return;

	if(m_QmClientMarkerStartedAt <= 0)
		m_QmClientMarkerStartedAt = time_timestamp();
	m_QmClientMarkerLastSeenAt = time_timestamp();
	m_QmClientMarkerLastFlushTick = NowTick;

	if(ForceWrite)
	{
		if(m_pQmClientLifecycleMarkerWriteJob && !m_pQmClientLifecycleMarkerWriteJob->Done())
		{
			m_pQmClientLifecycleMarkerWriteJob->Abort();
			m_pQmClientLifecycleMarkerWriteJob = nullptr;
		}
		WriteQmClientLifecycleMarker();
		return;
	}

	if(m_pQmClientLifecycleMarkerWriteJob && !m_pQmClientLifecycleMarkerWriteJob->Done())
		return;

	char aLine[384];
	str_format(aLine, sizeof(aLine),
		"session=%s\nstarted_at=%d\nlast_seen_at=%d\nclient_id=%s\n",
		m_aQmClientLifecycleSessionId, (int)m_QmClientMarkerStartedAt, (int)m_QmClientMarkerLastSeenAt, m_aQmClientPlaytimeClientId);
	m_pQmClientLifecycleMarkerWriteJob = std::make_shared<CQmClientLifecycleMarkerWriteJob>(Storage(), aLine, m_pQmClientLifecycleMarkerMutex);
	Engine()->AddJob(m_pQmClientLifecycleMarkerWriteJob);
}

void CQmClient::ClearQmClientLifecycleMarker()
{
	Storage()->RemoveFile(QMCLIENT_LIFECYCLE_MARKER_FILE, IStorage::TYPE_SAVE);
}

void CQmClient::EnsureQmClientPlaytimeClientId()
{
	if(m_aQmClientPlaytimeClientId[0] != '\0')
		return;

	char aLoaded[128] = "";
	IOHANDLE File = Storage()->OpenFile(QMCLIENT_PLAYTIME_CLIENT_ID_FILE, IOFLAG_READ, IStorage::TYPE_SAVE);
	if(File)
	{
		const int Read = io_read(File, aLoaded, sizeof(aLoaded) - 1);
		io_close(File);
		if(Read > 0)
		{
			aLoaded[Read] = '\0';
			char *pTrimmed = (char *)str_utf8_skip_whitespaces(aLoaded);
			str_utf8_trim_right(pTrimmed);
			if(IsValidQmClientPlaytimeId(pTrimmed))
				str_copy(m_aQmClientPlaytimeClientId, pTrimmed, sizeof(m_aQmClientPlaytimeClientId));
		}
	}

	if(m_aQmClientPlaytimeClientId[0] == '\0')
	{
		unsigned char aRandom[16];
		secure_random_fill(aRandom, sizeof(aRandom));

		static constexpr const char HEX[] = "0123456789abcdef";
		char aHex[sizeof(aRandom) * 2 + 1];
		for(size_t i = 0; i < sizeof(aRandom); ++i)
		{
			aHex[i * 2] = HEX[aRandom[i] >> 4];
			aHex[i * 2 + 1] = HEX[aRandom[i] & 0x0f];
		}
		aHex[sizeof(aHex) - 1] = '\0';
		str_format(m_aQmClientPlaytimeClientId, sizeof(m_aQmClientPlaytimeClientId), "qm%s", aHex);
	}

	if(!IsValidQmClientPlaytimeId(m_aQmClientPlaytimeClientId))
	{
		m_aQmClientPlaytimeClientId[0] = '\0';
		return;
	}

	Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);
	IOHANDLE OutFile = Storage()->OpenFile(QMCLIENT_PLAYTIME_CLIENT_ID_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(OutFile)
	{
		io_write(OutFile, m_aQmClientPlaytimeClientId, str_length(m_aQmClientPlaytimeClientId));
		io_write(OutFile, "\n", 1);
		io_close(OutFile);
	}
}

void CQmClient::UpdateQmClientLifecycleAndServerTime()
{
	if(m_QmClientStartupSent && !m_QmClientShutdownReported)
		TouchQmClientLifecycleMarker(false);
}

void CQmClient::InitQmNews()
{
	m_QmNewsMarkdown.clear();
	m_QmNewsDraft.clear();
	m_QmNewsVersion = 0;
	m_QmNewsPublishing = false;
	m_QmNewsStatus = EQmNewsStatus::IDLE;
	LoadQmNewsCache();
}

void CQmClient::LoadQmNewsCache()
{
	void *pFileData = nullptr;
	unsigned FileSize = 0;
	if(!Storage()->ReadFile(QMCLIENT_NEWS_CACHE_FILE, IStorage::TYPE_SAVE, &pFileData, &FileSize) || !pFileData || FileSize == 0)
	{
		free(pFileData);
		return;
	}

	json_value *pRoot = json_parse(static_cast<const char *>(pFileData), FileSize);
	free(pFileData);
	if(!pRoot)
		return;
	const json_value *pCacheVersion = json_object_get(pRoot, "cache_version");
	const json_value *pMarkdown = json_object_get(pRoot, "markdown");
	const json_value *pVersion = json_object_get(pRoot, "version");
	if(pCacheVersion->type == json_integer && pCacheVersion->u.integer == QMCLIENT_NEWS_CACHE_VERSION &&
		pMarkdown->type == json_string && pMarkdown->u.string.length <= QMCLIENT_NEWS_MAX_BYTES)
	{
		m_QmNewsMarkdown.assign(pMarkdown->u.string.ptr, pMarkdown->u.string.length);
		m_QmNewsVersion = pVersion->type == json_integer ? (int)pVersion->u.integer : 0;
		m_QmNewsStatus = m_QmNewsMarkdown.empty() ? EQmNewsStatus::EMPTY : EQmNewsStatus::READY;
	}
	json_value_free(pRoot);
}

void CQmClient::SaveQmNewsCache()
{
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("cache_version");
	Writer.WriteIntValue(QMCLIENT_NEWS_CACHE_VERSION);
	Writer.WriteAttribute("version");
	Writer.WriteIntValue(m_QmNewsVersion);
	Writer.WriteAttribute("markdown");
	Writer.WriteStrValue(m_QmNewsMarkdown.c_str());
	Writer.EndObject();
	const std::string Output = Writer.GetOutputString();
	// IStorage 没有整文件写接口，统一走 OpenFile + io_write；缓存目录与其它 qmclient 凭证一致。
	Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(QMCLIENT_NEWS_CACHE_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	io_write(File, Output.c_str(), Output.size());
	io_close(File);
}

void CQmClient::ApplyQmNewsPayload(const char *pBody, size_t BodySize)
{
	if(pBody == nullptr || BodySize == 0)
		return;
	json_value *pRoot = json_parse(pBody, BodySize);
	if(!pRoot)
		return;
	const json_value *pMarkdown = json_object_get(pRoot, "markdown");
	const json_value *pVersion = json_object_get(pRoot, "version");
	if(pMarkdown->type == json_string && pMarkdown->u.string.length <= QMCLIENT_NEWS_MAX_BYTES)
	{
		m_QmNewsMarkdown.assign(pMarkdown->u.string.ptr, pMarkdown->u.string.length);
		m_QmNewsVersion = pVersion->type == json_integer ? (int)pVersion->u.integer : 0;
		m_QmNewsStatus = m_QmNewsMarkdown.empty() ? EQmNewsStatus::EMPTY : EQmNewsStatus::READY;
		SaveQmNewsCache();
	}
	json_value_free(pRoot);
	++m_QmNewsRevision;
}

void CQmClient::QmNewsRefresh(bool Force)
{
	if(!Force || !QmRealtimeConnected())
		return;
	const char *pMessage = "{\"type\":\"news\"}";
	m_pQmRealtime->SendText(pMessage, str_length(pMessage));
}

void CQmClient::QmNewsReloadDraft()
{
	m_QmNewsDraft.clear();
	char *pDraft = Storage()->ReadFileStr(QMCLIENT_NEWS_DRAFT_FILE, IStorage::TYPE_SAVE);
	if(pDraft)
	{
		if(str_length(pDraft) <= QMCLIENT_NEWS_MAX_BYTES)
			m_QmNewsDraft = pDraft;
		free(pDraft);
	}
	++m_QmNewsRevision;
}

void CQmClient::QmNewsPublishDraft()
{
	if(m_QmNewsPublishing || m_aQmDeveloperToken[0] == '\0')
		return;
	if(m_QmNewsDraft.empty())
		QmNewsReloadDraft();
	if(m_QmNewsDraft.empty())
	{
		m_QmNewsStatus = EQmNewsStatus::PUBLISH_FAILED;
		++m_QmNewsRevision;
		return;
	}

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("markdown");
	Writer.WriteStrValue(m_QmNewsDraft.c_str());
	Writer.EndObject();
	const std::string Output = Writer.GetOutputString();
	m_pQmNewsPublishTask = HttpPostJson(QMCLIENT_NEWS_PUBLISH_URL, Output.c_str());
	m_pQmNewsPublishTask->MaxResponseSize(8 * 1024);
	// 401/403/413 由发布结果处理，保留 HTTP 错误响应以区分权限和内容大小问题。
	m_pQmNewsPublishTask->FailOnErrorStatus(false);
	char aAuthorization[80];
	str_format(aAuthorization, sizeof(aAuthorization), "Bearer %s", m_aQmDeveloperToken);
	m_pQmNewsPublishTask->HeaderString("Authorization", aAuthorization);
	m_pQmNewsPublishTask->Timeout(CTimeout{3000, 5000, 500, 5});
	m_pQmNewsPublishTask->LogProgress(HTTPLOG::FAILURE);
	Http()->Run(m_pQmNewsPublishTask);
	m_QmNewsPublishing = true;
	m_QmNewsStatus = EQmNewsStatus::PUBLISHING;
	++m_QmNewsRevision;
}

void CQmClient::FinishQmNewsPublish()
{
	m_QmNewsPublishing = false;
	const int StatusCode = m_pQmNewsPublishTask->State() == EHttpState::DONE ? m_pQmNewsPublishTask->StatusCode() : 0;
	if(StatusCode == 200)
	{
		m_QmNewsMarkdown = m_QmNewsDraft;
		++m_QmNewsVersion;
		m_QmNewsStatus = EQmNewsStatus::PUBLISHED;
		SaveQmNewsCache();
	}
	else if(StatusCode == 401 || StatusCode == 403)
		m_QmNewsStatus = EQmNewsStatus::PUBLISH_DENIED;
	else if(StatusCode == 413)
		m_QmNewsStatus = EQmNewsStatus::PUBLISH_TOO_LARGE;
	else
		m_QmNewsStatus = EQmNewsStatus::PUBLISH_FAILED;
	m_pQmNewsPublishTask = nullptr;
	++m_QmNewsRevision;
}

void CQmClient::InitQmSponsors()
{
	m_QmSponsorsMarkdown.clear();
	m_QmSponsorsDraft.clear();
	m_vQmSponsorNames.clear();
	m_QmSponsorsVersion = -1;
	m_QmSponsorsStatus = EQmNewsStatus::IDLE;
	void *pFileData = nullptr;
	unsigned FileSize = 0;
	if(!Storage()->ReadFile(QMCLIENT_SPONSORS_CACHE_FILE, IStorage::TYPE_SAVE, &pFileData, &FileSize) || !pFileData)
	{
		free(pFileData);
		return;
	}
	json_value *pRoot = json_parse(static_cast<const char *>(pFileData), FileSize);
	free(pFileData);
	if(!pRoot)
		return;
	const json_value *pCacheVersion = json_object_get(pRoot, "cache_version");
	if(pCacheVersion->type == json_integer && pCacheVersion->u.integer == QMCLIENT_NEWS_CACHE_VERSION)
		ApplyQmSponsorsPayload(pRoot, false);
	json_value_free(pRoot);
}

void CQmClient::SaveQmSponsorsCache()
{
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("cache_version");
	Writer.WriteIntValue(QMCLIENT_NEWS_CACHE_VERSION);
	Writer.WriteAttribute("version");
	Writer.WriteIntValue(m_QmSponsorsVersion);
	Writer.WriteAttribute("markdown");
	Writer.WriteStrValue(m_QmSponsorsMarkdown.c_str());
	Writer.EndObject();
	const std::string Output = Writer.GetOutputString();
	Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);
	IOHANDLE File = Storage()->OpenFile(QMCLIENT_SPONSORS_CACHE_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
	if(!File)
		return;
	io_write(File, Output.c_str(), Output.size());
	io_close(File);
}

bool CQmClient::ApplyQmSponsorsPayload(const json_value *pPayload, bool SaveCache)
{
	if(!pPayload || pPayload->type != json_object)
		return false;
	const json_value *pMarkdown = json_object_get(pPayload, "markdown");
	const json_value *pVersion = json_object_get(pPayload, "version");
	if(pMarkdown->type != json_string || pMarkdown->u.string.length > QMCLIENT_NEWS_MAX_BYTES ||
		pVersion->type != json_integer || pVersion->u.integer < 0 || pVersion->u.integer > std::numeric_limits<int>::max() ||
		(size_t)str_length(pMarkdown->u.string.ptr) != pMarkdown->u.string.length || !str_utf8_check(pMarkdown->u.string.ptr))
		return false;

	// HTTP 发布结果可能晚于新一轮推送抵达；不让较旧版本覆盖当前名单。
	const int Version = (int)pVersion->u.integer;
	if(Version < m_QmSponsorsVersion)
		return true;
	const std::string Markdown(pMarkdown->u.string.ptr, pMarkdown->u.string.length);
	if(Version == m_QmSponsorsVersion && Markdown == m_QmSponsorsMarkdown)
		return true;
	m_QmSponsorsMarkdown = Markdown;
	m_vQmSponsorNames = qm_sponsors::ParseNames(m_QmSponsorsMarkdown.c_str());
	m_QmSponsorsVersion = Version;
	if(!QmSponsorsPublishing())
		m_QmSponsorsStatus = m_vQmSponsorNames.empty() ? EQmNewsStatus::EMPTY : EQmNewsStatus::READY;
	++m_QmSponsorsRevision;
	if(SaveCache)
		SaveQmSponsorsCache();
	return true;
}

void CQmClient::QmSponsorsRefresh()
{
	if(!QmRealtimeConnected())
		return;
	const char *pMessage = "{\"type\":\"sponsors\"}";
	m_pQmRealtime->SendText(pMessage, str_length(pMessage));
}

void CQmClient::QmSponsorsReloadDraft()
{
	if(QmSponsorsPublishing())
		return;
	m_QmSponsorsDraft.clear();
	char *pDraft = Storage()->ReadFileStr(QMCLIENT_SPONSORS_DRAFT_FILE, IStorage::TYPE_SAVE);
	if(pDraft)
	{
		if(str_length(pDraft) <= QMCLIENT_NEWS_MAX_BYTES)
			m_QmSponsorsDraft = pDraft;
		free(pDraft);
	}
	++m_QmSponsorsRevision;
}

void CQmClient::QmSponsorsPublishDraft()
{
	if(QmSponsorsPublishing() || !HasDeveloperCredential())
		return;
	if(m_QmSponsorsDraft.empty())
		QmSponsorsReloadDraft();
	if(m_QmSponsorsDraft.empty())
	{
		m_QmSponsorsStatus = EQmNewsStatus::PUBLISH_FAILED;
		++m_QmSponsorsRevision;
		return;
	}

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("markdown");
	Writer.WriteStrValue(m_QmSponsorsDraft.c_str());
	Writer.EndObject();
	const std::string Output = Writer.GetOutputString();
	m_pQmSponsorsPublishTask = HttpPostJson(QMCLIENT_SPONSORS_PUBLISH_URL, Output.c_str());
	// 成功响应带回服务端确认的正文和版本，允许 JSON 转义后的长度。
	m_pQmSponsorsPublishTask->MaxResponseSize(6 * QMCLIENT_NEWS_MAX_BYTES + 1024);
	m_pQmSponsorsPublishTask->FailOnErrorStatus(false);
	char aAuthorization[80];
	str_format(aAuthorization, sizeof(aAuthorization), "Bearer %s", m_aQmDeveloperToken);
	m_pQmSponsorsPublishTask->HeaderString("Authorization", aAuthorization);
	m_pQmSponsorsPublishTask->Timeout(CTimeout{3000, 5000, 500, 5});
	m_pQmSponsorsPublishTask->LogProgress(HTTPLOG::FAILURE);
	Http()->Run(m_pQmSponsorsPublishTask);
	m_QmSponsorsStatus = EQmNewsStatus::PUBLISHING;
	++m_QmSponsorsRevision;
}

void CQmClient::FinishQmSponsorsPublish()
{
	const int StatusCode = m_pQmSponsorsPublishTask->State() == EHttpState::DONE ? m_pQmSponsorsPublishTask->StatusCode() : 0;
	if(StatusCode == 200)
	{
		json_value *pRoot = m_pQmSponsorsPublishTask->ResultJson();
		const bool Applied = ApplyQmSponsorsPayload(pRoot, true);
		json_value_free(pRoot);
		m_QmSponsorsStatus = Applied ? EQmNewsStatus::PUBLISHED : EQmNewsStatus::PUBLISH_FAILED;
	}
	else if(StatusCode == 401 || StatusCode == 403)
		m_QmSponsorsStatus = EQmNewsStatus::PUBLISH_DENIED;
	else if(StatusCode == 413)
		m_QmSponsorsStatus = EQmNewsStatus::PUBLISH_TOO_LARGE;
	else
		m_QmSponsorsStatus = EQmNewsStatus::PUBLISH_FAILED;
	m_pQmSponsorsPublishTask = nullptr;
	++m_QmSponsorsRevision;
}

void CQmClient::UpdateQmDdnetPlayerStats()
{
	if(m_pQmDdnetPlayerParseJob && m_pQmDdnetPlayerParseJob->Done())
		FinishQmDdnetPlayerStats();
	if(m_pQmDdnetPlayerTask && m_pQmDdnetPlayerTask->Done())
		FinishQmDdnetPlayerStats();

	const char *pConfiguredName = g_Config.m_PlayerName;
	if(!pConfiguredName || pConfiguredName[0] == '\0')
	{
		if(m_pQmDdnetPlayerTask)
		{
			m_pQmDdnetPlayerTask->Abort();
			m_pQmDdnetPlayerTask = nullptr;
		}
		m_pQmDdnetPlayerParseJob = nullptr;
		if(m_aQmDdnetPlayerName[0] != '\0')
		{
			m_aQmDdnetPlayerName[0] = '\0';
			m_aQmDdnetFavoritePartner[0] = '\0';
			m_QmDdnetTotalFinishes = -1;
			m_QmDdnetPlayerLastSync = 0;
			m_QmDdnetPlayerNextRetry = 0;
		}
		return;
	}

	if(str_comp(m_aQmDdnetPlayerName, pConfiguredName) != 0)
	{
		if(m_pQmDdnetPlayerTask)
		{
			m_pQmDdnetPlayerTask->Abort();
			m_pQmDdnetPlayerTask = nullptr;
		}
		m_pQmDdnetPlayerParseJob = nullptr;

		str_copy(m_aQmDdnetPlayerName, pConfiguredName, sizeof(m_aQmDdnetPlayerName));
		m_aQmDdnetFavoritePartner[0] = '\0';
		m_QmDdnetTotalFinishes = -1;
		m_QmDdnetPlayerLastSync = 0;
		m_QmDdnetPlayerNextRetry = 0;
	}

	if(m_pQmDdnetPlayerParseJob)
		return;
	if(m_pQmDdnetPlayerTask)
		return;

	const int64_t Now = time_get();
	if(m_QmDdnetPlayerNextRetry != 0 && Now < m_QmDdnetPlayerNextRetry)
		return;

	if(m_QmDdnetPlayerNextRetry == 0)
	{
		const int64_t SyncIntervalTicks = (int64_t)QMCLIENT_DDNET_PLAYER_SYNC_INTERVAL_SECONDS * time_freq();
		if(m_QmDdnetPlayerLastSync != 0 && Now - m_QmDdnetPlayerLastSync < SyncIntervalTicks)
			return;
	}

	FetchQmDdnetPlayerStats(m_aQmDdnetPlayerName);
}

void CQmClient::FetchQmDdnetPlayerStats(const char *pPlayerName)
{
	if(!pPlayerName || pPlayerName[0] == '\0')
		return;
	if(m_pQmDdnetPlayerParseJob && !m_pQmDdnetPlayerParseJob->Done())
		return;
	if(m_pQmDdnetPlayerTask && !m_pQmDdnetPlayerTask->Done())
		return;

	char aEncodedName[256];
	EscapeUrl(aEncodedName, sizeof(aEncodedName), pPlayerName);

	char aUrl[512];
	str_format(aUrl, sizeof(aUrl), "%s%s", DDNET_PLAYER_STATS_URL, aEncodedName);

	m_pQmDdnetPlayerTask = HttpGet(aUrl);
	m_pQmDdnetPlayerTask->Timeout(CTimeout{10000, 30000, 100, 10});
	m_pQmDdnetPlayerTask->LogProgress(HTTPLOG::FAILURE);
	Http()->Run(m_pQmDdnetPlayerTask);
}

void CQmClient::FinishQmDdnetPlayerStats()
{
	if(m_pQmDdnetPlayerParseJob)
	{
		if(!m_pQmDdnetPlayerParseJob->Done())
			return;

		auto pParseJob = std::static_pointer_cast<CQmDdnetPlayerStatsParseJob>(m_pQmDdnetPlayerParseJob);
		CQmDdnetPlayerStatsParseJob::SResult Result = pParseJob->TakeResult();
		m_pQmDdnetPlayerParseJob = nullptr;

		const int64_t Now = time_get();
		if(Result.m_Parsed)
		{
			m_QmDdnetPlayerLastSync = Now;
			m_QmDdnetPlayerNextRetry = 0;
			str_copy(m_aQmDdnetFavoritePartner, Result.m_FavoritePartner.c_str(), sizeof(m_aQmDdnetFavoritePartner));
			m_QmDdnetTotalFinishes = Result.m_TotalFinishes;
		}
		else
		{
			m_QmDdnetPlayerLastSync = 0;
			m_QmDdnetPlayerNextRetry = Now + (int64_t)QMCLIENT_DDNET_PLAYER_RETRY_DELAY_SECONDS * time_freq();
		}
		return;
	}

	if(!m_pQmDdnetPlayerTask)
		return;

	if(m_pQmDdnetPlayerTask->State() != EHttpState::DONE || m_pQmDdnetPlayerTask->StatusCode() != 200)
	{
		m_QmDdnetPlayerLastSync = 0;
		m_QmDdnetPlayerNextRetry = time_get() + (int64_t)QMCLIENT_DDNET_PLAYER_RETRY_DELAY_SECONDS * time_freq();
		m_pQmDdnetPlayerTask = nullptr;
		return;
	}

	m_pQmDdnetPlayerParseJob = std::make_shared<CQmDdnetPlayerStatsParseJob>(m_pQmDdnetPlayerTask);
	Engine()->AddJob(m_pQmDdnetPlayerParseJob);
	m_pQmDdnetPlayerTask = nullptr;
}

void CQmClient::InitQmClientLifecycle()
{
	unsigned SessionRandom = 0;
	secure_random_fill(&SessionRandom, sizeof(SessionRandom));
	str_format(m_aQmClientLifecycleSessionId, sizeof(m_aQmClientLifecycleSessionId), "%08x%08x", (unsigned)time_timestamp(), SessionRandom);

	EnsureQmClientPlaytimeClientId();
	int64_t PreviousStartedAt = 0;
	int64_t PreviousLastSeenAt = 0;
	const bool HadPendingMarker = ReadQmClientLifecycleMarker(PreviousStartedAt, PreviousLastSeenAt);
	m_QmClientRecoveryStopAt = PreviousLastSeenAt > 0 ? PreviousLastSeenAt : PreviousStartedAt;
	if(m_QmClientRecoveryStopAt <= 0)
		m_QmClientRecoveryStopAt = time_timestamp();
	m_QmClientMarkerStartedAt = PreviousStartedAt;
	m_QmClientMarkerLastSeenAt = PreviousLastSeenAt;
	m_QmClientMarkerLastFlushTick = 0;

	m_QmClientShutdownReported = false;
	m_QmClientAwaitingRecoveryStop = HadPendingMarker;
	m_QmClientStartupSent = false;
	m_QmClientServerNow = 0;
	m_QmClientServerSessionStart = 0;
	m_QmClientServerTimeLastSync = 0;
	m_QmClientServerPlaytimeSeconds = -1;
	m_QmClientPlaytimeLastSync = 0;
}

void CQmClient::InitQmDeveloperAuthentication()
{
	m_aQmDeveloperToken[0] = '\0';
	char *pTokenText = Storage()->ReadFileStr(QMCLIENT_DEVELOPER_TOKEN_FILE, IStorage::TYPE_SAVE);
	if(!pTokenText)
		return;

	char *pToken = str_skip_whitespaces(pTokenText);
	char *pEnd = pToken + str_length(pToken);
	while(pEnd > pToken && std::isspace((unsigned char)pEnd[-1]))
		--pEnd;
	*pEnd = '\0';

	const int TokenLength = str_length(pToken);
	bool Valid = TokenLength == 64;
	for(int i = 0; Valid && i < TokenLength; ++i)
		Valid = std::isxdigit((unsigned char)pToken[i]) != 0;
	if(Valid)
		str_copy(m_aQmDeveloperToken, pToken, sizeof(m_aQmDeveloperToken));
	else
		log_warn("qmclient", "ignored invalid developer credential file");
	free(pTokenText);
}

void CQmClient::ApplyQmRealtimeDevelopers(const json_value *pPayload)
{
	char aServer[NETADDR_MAXSTRSIZE] = "";
	if(Client()->State() != IClient::STATE_ONLINE || !Client()->ServerAddress())
		return;
	net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);
	const json_value *pAddress = JsonObjectField(pPayload, "server_address");
	if(pAddress->type != json_string || str_comp(pAddress->u.string.ptr, aServer) != 0)
		return;
	SQmDeveloperPresenceParseResult Result;
	if(!ParseQmDeveloperPresencesJson(pPayload, aServer, Result))
		return;
	GameClient()->ClearQmDeveloperMarks();
	const char *pServerAddress = aServer;
	const int64_t NowTick = time_get();
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!GameClient()->m_aClients[ClientId].m_Active)
			continue;
		const SQmDeveloperPresence *pPresence = FindQmDeveloperPresence(
			Result.m_vPresences,
			pServerAddress,
			ClientId,
			GameClient()->m_aClients[ClientId].m_aName,
			Result.m_ServerTime);
		if(!pPresence)
			continue;
		const int64_t RemainingSeconds = std::min<int64_t>(pPresence->m_ExpiresAt - Result.m_ServerTime, QMCLIENT_DEVELOPER_SYNC_INTERVAL_SECONDS * 2);
		if(RemainingSeconds <= 0)
			continue;
		GameClient()->MarkQmDeveloperClient(
			ClientId,
			pPresence->m_PlayerName.c_str(),
			NowTick + RemainingSeconds * time_freq(),
			QmDeveloperBadgeStyleFromBucket(pPresence->m_StyleBucket) == EQmDeveloperBadgeStyle::RAINBOW);
	}
}

bool CQmClient::HasQmClientRecognitionService() const
{
	return QmRealtimeAvailable();
}

bool CQmClient::QmClientDistributionSyncing() const
{
	return m_QmClientDistribution.IsStale(time_get_impl());
}

bool CQmClient::EnsureQmClientMachineHash()
{
	if(IsValidQmClientMachineHash(m_aQmClientMachineHash))
		return true;

	std::string Identity;
	if(!ReadPlatformMachineIdentity(Identity))
	{
		char aLoaded[128] = "";
		IOHANDLE File = Storage()->OpenFile(QMCLIENT_MACHINE_ID_FALLBACK_FILE, IOFLAG_READ, IStorage::TYPE_SAVE);
		if(File)
		{
			const int Read = io_read(File, aLoaded, sizeof(aLoaded) - 1);
			io_close(File);
			if(Read > 0)
			{
				aLoaded[Read] = '\0';
				TrimQmClientTextInPlace(aLoaded);
				if(aLoaded[0] != '\0')
					Identity = aLoaded;
			}
		}

		if(Identity.empty())
		{
			unsigned char aRandom[32];
			secure_random_fill(aRandom, sizeof(aRandom));

			static constexpr const char HEX[] = "0123456789abcdef";
			char aHex[sizeof(aRandom) * 2 + 1];
			for(size_t i = 0; i < sizeof(aRandom); ++i)
			{
				aHex[i * 2] = HEX[aRandom[i] >> 4];
				aHex[i * 2 + 1] = HEX[aRandom[i] & 0x0f];
			}
			aHex[sizeof(aHex) - 1] = '\0';
			Identity = aHex;

			Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);
			IOHANDLE OutFile = Storage()->OpenFile(QMCLIENT_MACHINE_ID_FALLBACK_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
			if(OutFile)
			{
				io_write(OutFile, Identity.c_str(), Identity.size());
				io_write(OutFile, "\n", 1);
				io_close(OutFile);
			}
		}
	}

	if(Identity.empty())
		return false;

	const SHA256_DIGEST Digest = sha256(Identity.data(), Identity.size());
	sha256_str(Digest, m_aQmClientMachineHash, sizeof(m_aQmClientMachineHash));
	return IsValidQmClientMachineHash(m_aQmClientMachineHash);
}

void CQmClient::FinishQmClientUsers()
{
	if(m_pQmClientUsersParseJob)
	{
		if(!m_pQmClientUsersParseJob->Done())
			return;

		auto pParseJob = std::static_pointer_cast<CQmClientUsersParseJob>(m_pQmClientUsersParseJob);
		const int64_t ExpireTick = pParseJob->ExpireTick();
		char aCurrentServer[NETADDR_MAXSTRSIZE] = "";
		if(Client()->State() == IClient::STATE_ONLINE && Client()->ServerAddress())
			net_addr_str(Client()->ServerAddress(), aCurrentServer, sizeof(aCurrentServer), true);
		if(str_comp(aCurrentServer, pParseJob->ServerAddress()) != 0)
		{
			m_pQmClientUsersParseJob.reset();
			return;
		}
		CQmClientUsersParseJob::SResult Result = pParseJob->TakeResult();
		m_pQmClientUsersParseJob = nullptr;

		if(!m_QmClientDistribution.Apply(Result, ExpireTick))
		{
			m_QmClientDistributionSuccessLatched = false;
			LogQmClientDistributionFailureEvent("parse_failed", "users payload could not be parsed");
			return;
		}

		GameClient()->ClearQ1menGSyncMarks();
		GameClient()->ClearQmVoiceSyncMarks();
		if(!m_QmClientDistributionSuccessLatched)
			LogQmClientDistributionEvent("parse_ok", Result.m_OnlineUserCount, Result.m_OnlineDummyCount, (int)Result.m_vLocalServerMarks.size());
		m_QmClientDistributionSuccessLatched = true;
		for(const auto &Mark : Result.m_vLocalServerMarks)
		{
			for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
			{
				if(!GameClient()->m_aClients[ClientId].m_Active || str_comp(GameClient()->m_aClients[ClientId].m_aName, Mark.m_Name.c_str()) != 0)
					continue;

				GameClient()->MarkQ1menGSyncClient(ClientId, ExpireTick, Mark.m_Qid.c_str(), Mark.m_ClientBrand);
				if(Mark.m_VoiceSupported)
					GameClient()->MarkQmVoiceSupportedClient(ClientId, ExpireTick);
				break;
			}
		}
		return;
	}
}

void CQmClient::UpdateQmClientRecognition()
{
	if(m_pQmClientUsersParseJob && m_pQmClientUsersParseJob->Done())
		FinishQmClientUsers();
	if(!m_pQmClientUsersParseJob && m_pQmRealtimeUsersPayload)
	{
		m_pQmClientUsersParseJob = std::make_shared<CQmClientUsersParseJob>(std::move(m_pQmRealtimeUsersPayload), m_aQmRealtimeUsersServer, m_QmRealtimeUsersExpireTick);
		Engine()->AddJob(m_pQmClientUsersParseJob);
	}
}

void CQmClient::ApplyQmTitlePresences(const json_value *pRoot, const char *pServerAddress)
{
	if(pRoot == nullptr)
		return;
	int64_t ServerTime = 0;
	const std::vector<SQmTitlePresence> vPresences = ParseQmTitlePresences(pRoot, pServerAddress, &ServerTime);
	// 这一份是权威结果：缺席者视为已下线，因此在这里才丢弃旧名单。
	mem_zero(m_aTitleExpires, sizeof(m_aTitleExpires));
	for(const auto &Presence : vPresences)
	{
		str_copy(m_aaTitleNames[Presence.m_PlayerId], Presence.m_PlayerName.c_str());
		str_format(m_aaPlayerTitles[Presence.m_PlayerId], sizeof(m_aaPlayerTitles[Presence.m_PlayerId]), "[%s]", Presence.m_Title.c_str());
		str_copy(m_aaPlayerStyles[Presence.m_PlayerId], Presence.m_Style.c_str());
		m_aTitleExpires[Presence.m_PlayerId] = time_get_impl() + Presence.m_RemainingSeconds * time_freq();
	}
	// 用服务端时间对齐动画相位：各客户端据此得到一致的时间基准，
	// 否则同一时刻不同人看到的颜色相位会因本机时钟偏差而错开。
	if(ServerTime > 0)
	{
		const double Measured = (double)ServerTime - (double)Client()->GlobalTime();
		m_ServerTimeOffset = QmTitleUpdateServerTimeOffset(m_ServerTimeOffset, m_ServerTimeOffsetValid, Measured);
		m_ServerTimeOffsetValid = true;
	}
	m_TitleLastSync = time_get_impl();
}

void CQmClient::QmRealtimeRequestTitleRefresh()
{
	if(!QmRealtimeConnected())
		return;
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("type");
	Writer.WriteStrValue("subscribe_titles");
	if(QmRealtimeAllowsCredentials(m_aQmRealtimeUrl))
	{
		Writer.WriteAttribute("title_token");
		Writer.WriteStrValue(m_aTitleToken);
	}
	Writer.EndObject();
	const std::string Body = Writer.GetOutputString();
	m_pQmRealtime->SendText(Body.c_str(), Body.size());
	m_QmRealtimePresenceBody.clear();
	m_QmRealtimeNextPresenceCheck = 0;
}

void CQmClient::SendQmAnonymousEmoticon(int Emoticon, int PlayerId, bool LaunchMode, bool SuperLaunch)
{
	if(m_pQmAnonymousEmote == nullptr || m_pQmAnonymousEmote->State() != EQmWebSocketState::CONNECTED ||
		m_aQmAnonymousClientId[0] == '\0' || Emoticon < 0 || Emoticon >= NUM_EMOTICONS ||
		PlayerId < 0 || PlayerId >= MAX_CLIENTS || (!LaunchMode && !SuperLaunch))
		return;

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("type");
	Writer.WriteStrValue("emoticon");
	Writer.WriteAttribute("emoticon");
	Writer.WriteIntValue(Emoticon);
	Writer.WriteAttribute("player_id");
	Writer.WriteIntValue(PlayerId);
	Writer.WriteAttribute("launch_mode");
	Writer.WriteBoolValue(LaunchMode);
	Writer.WriteAttribute("super_launch");
	Writer.WriteBoolValue(SuperLaunch);
	Writer.EndObject();
	const std::string Body = Writer.GetOutputString();
	if(m_pQmAnonymousEmote->SendText(Body.c_str(), Body.size()))
		LogQmAnonymousEmoteEvent("emoticon_sent", Body.c_str());
	else
		LogQmAnonymousEmoteEvent("emoticon_send_failed", nullptr);
}

bool CQmClient::PollQmRemoteEmoticonEvent(SQmRemoteEmoticonEvent &OutEvent)
{
	if(m_QmRemoteEmoticonEvents.empty())
		return false;
	OutEvent = std::move(m_QmRemoteEmoticonEvents.front());
	m_QmRemoteEmoticonEvents.pop_front();
	return true;
}

void CQmClient::LogQmRealtimeEvent(const char *pStage, const char *pDetail) const
{
	if(!g_Config.m_QmWebSocketLog)
		return;
	log_info("qmclient", "realtime %s: %s", pStage, pDetail != nullptr ? pDetail : "");
}

void CQmClient::QueueQmRemoteEmoticonEvent(const SQmRealtimeMessage &Message)
{
	if(!Message.m_HasEmoticon || Client()->State() != IClient::STATE_ONLINE || !Client()->ServerAddress())
	{
		LogQmAnonymousEmoteEvent("event_dropped", "invalid state or payload");
		return;
	}

	char aServer[NETADDR_MAXSTRSIZE] = "";
	net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);
	const std::string CurrentServer = NormalizeQmServerAddress(aServer);
	const std::string EventServer = NormalizeQmServerAddress(Message.m_EmoticonServerAddress.c_str());
	if(EventServer.empty() || CurrentServer != EventServer)
	{
		LogQmAnonymousEmoteEvent("event_dropped", "server address mismatch");
		return;
	}
	if(Message.m_EmoticonPlayerName.size() > MAX_NAME_LENGTH || Message.m_EmoticonClientId.size() > 64)
	{
		LogQmAnonymousEmoteEvent("event_dropped", "identity field too long");
		return;
	}

	int PlayerId = Message.m_PlayerId;
	if(!GameClient()->m_aClients[PlayerId].m_Active)
	{
		PlayerId = -1;
		for(int Candidate = 0; Candidate < MAX_CLIENTS; ++Candidate)
		{
			if(GameClient()->m_aClients[Candidate].m_Active &&
				str_comp(GameClient()->m_aClients[Candidate].m_aName, Message.m_EmoticonPlayerName.c_str()) == 0)
			{
				PlayerId = Candidate;
				break;
			}
		}
		if(PlayerId < 0)
		{
			LogQmAnonymousEmoteEvent("event_dropped", "player not found");
			return;
		}
		LogQmAnonymousEmoteEvent("player_id_remapped", Message.m_EmoticonPlayerName.c_str());
	}

	SQmRemoteEmoticonEvent Event;
	Event.m_PlayerId = PlayerId;
	Event.m_Emoticon = Message.m_Emoticon;
	Event.m_LaunchMode = Message.m_LaunchMode;
	Event.m_SuperLaunch = Message.m_SuperLaunch;
	Event.m_Sequence = Message.m_EmoticonSequence;
	Event.m_ClientId = Message.m_EmoticonClientId;
	Event.m_PlayerName = Message.m_EmoticonPlayerName;
	Event.m_ServerAddress = Message.m_EmoticonServerAddress;
	if(m_QmRemoteEmoticonEvents.size() >= 128)
		m_QmRemoteEmoticonEvents.pop_front();
	m_QmRemoteEmoticonEvents.push_back(std::move(Event));
	LogQmAnonymousEmoteEvent("emoticon_received", Message.m_EmoticonPlayerName.c_str());
}

void CQmClient::QueueQmAnonymousEmoticonEvent(const SQmRealtimeMessage &Message)
{
	QueueQmRemoteEmoticonEvent(Message);
}

void CQmClient::StartQmRealtime()
{
	StopQmRealtime();
	// worker 不访问游戏状态；握手变化统一由主线程消费。
	m_pQmRealtime = CreateQmWebSocketClient({});
	if(m_pQmRealtime && m_pQmRealtime->Available())
	{
		IQmWebSocketClient::STuning Tuning;
		Tuning.m_HeartbeatMs = g_Config.m_QmWebSocketHeartbeat * 1000;
		Tuning.m_BackoffBaseMs = g_Config.m_QmWebSocketBackoffBaseMs;
		Tuning.m_BackoffMaxMs = g_Config.m_QmWebSocketBackoffMaxMs;
		m_pQmRealtime->SetTuning(Tuning);
	}
}

void CQmClient::StopQmRealtime()
{
	if(m_pQmRealtime)
	{
		m_pQmRealtime->Disconnect();
		m_pQmRealtime.reset();
	}
	m_QmRealtimeConnectedTick = 0;
	m_QmRealtimePresenceBody.clear();
	m_QmRealtimeNextPresenceCheck = 0;
}

void CQmClient::QmRealtimeRestart()
{
	StopQmRealtime();
	m_QmRealtimeFailureLogged = false;
	EnsureQmRealtimeConnection();
}

void CQmClient::EnsureQmRealtimeConnection()
{
	if(str_comp(m_aQmRealtimeUrl, QmRealtimeEffectiveUrl(g_Config.m_QmWebSocketUrl)) != 0)
	{
		// 地址变了：断开旧连接并记录新地址。
		StopQmRealtime();
		str_copy(m_aQmRealtimeUrl, QmRealtimeEffectiveUrl(g_Config.m_QmWebSocketUrl));
		m_QmRealtimeFailureLogged = false;
	}

	if(m_pQmRealtime == nullptr)
		StartQmRealtime();
	if(m_pQmRealtime == nullptr || !m_pQmRealtime->Available())
		return;
	if(m_pQmRealtime->Desired())
		return;

	SQmWebSocketConnectConfig Config;
	const std::string ParseError = ParseQmWebSocketUrl(m_aQmRealtimeUrl, Config);
	if(!ParseError.empty())
	{
		if(!m_QmRealtimeFailureLogged)
		{
			m_QmRealtimeFailureLogged = true;
			LogQmRealtimeEvent("invalid_url", ParseError.c_str());
		}
		return;
	}
	Config.m_Protocol = g_Config.m_QmWebSocketProtocol;
	Config.m_MaxMessageSize = 8 * 1024 * 1024;
	Config.m_AllowInsecureTls = g_Config.m_QmWebSocketAllowInsecureTls != 0;

	std::string Error;
	if(!m_pQmRealtime->Connect(Config, Error))
	{
		if(!m_QmRealtimeFailureLogged)
		{
			m_QmRealtimeFailureLogged = true;
			LogQmRealtimeEvent("connect_rejected", Error.c_str());
		}
		return;
	}
	m_QmRealtimeFailureLogged = false;
	LogQmRealtimeEvent("connecting", m_aQmRealtimeUrl);
}

void CQmClient::SendQmRealtimeHello()
{
	if(!QmRealtimeConnected() || !EnsureQmClientMachineHash())
		return;
	EnsureQmClientPlaytimeClientId();
	const std::string Body = BuildQmRealtimePresence(true);
	if(m_pQmRealtime->SendText(Body.c_str(), Body.size()))
	{
		m_QmRealtimePresenceBody = BuildQmRealtimePresence(false);
		m_QmRealtimeLastPresence = time_get_impl();
	}
}

void CQmClient::HandleQmRealtimeMessage(const SQmRealtimeMessage &Message)
{
	switch(Message.m_Event)
	{
	case EQmRealtimeEvent::PING:
		if(m_pQmRealtime != nullptr)
			m_pQmRealtime->SendText("{\"type\":\"pong\"}", 15);
		break;
	case EQmRealtimeEvent::PONG:
		// 往返时延由传输层用 RECEIVE_PONG 统计，这里无需处理。
		break;
	case EQmRealtimeEvent::STATE:
		ApplyQmRealtimeState(Message);
		break;
	case EQmRealtimeEvent::BROADCAST:
		ApplyQmRealtimeBroadcast(Message);
		break;
	case EQmRealtimeEvent::SPONSORS:
		if(Message.m_HasRealtimeData)
			ApplyQmSponsorsPayload(Message.m_pPayload.get(), true);
		break;
	case EQmRealtimeEvent::TITLES:
		ApplyQmRealtimeTitles(Message);
		break;
	case EQmRealtimeEvent::EMOTICON:
		// 表情已经迁移到匿名专用通道，账号/头衔通道不再承载该事件。
		break;
	case EQmRealtimeEvent::USERS:
	case EQmRealtimeEvent::DEVELOPERS:
	case EQmRealtimeEvent::PLAYTIME:
	case EQmRealtimeEvent::TIME:
	case EQmRealtimeEvent::TITLE_PROFILE:
	case EQmRealtimeEvent::TITLE_STATUS:
	case EQmRealtimeEvent::ERROR:
		ApplyQmRealtimeServices(Message);
		break;
	case EQmRealtimeEvent::UNKNOWN:
		LogQmRealtimeEvent("unknown_event", Message.m_Type.c_str());
		break;
	case EQmRealtimeEvent::INVALID:
		break;
	}
}

void CQmClient::ApplyQmRealtimeState(const SQmRealtimeMessage &Message)
{
	// state 是增量：只覆盖确实下发的字段，缺失字段保留上一次的值。
	if(!Message.m_StatePayloadValid)
		return;
	if(Message.m_HasOnlineUsers)
		m_QmClientDistribution.m_OnlineUserCount = Message.m_OnlineUsers;
	if(Message.m_HasOnlineDummies)
		m_QmClientDistribution.m_OnlineDummyCount = Message.m_OnlineDummies;
}

void CQmClient::ApplyQmRealtimeBroadcast(const SQmRealtimeMessage &Message)
{
	if(!Message.m_HasBroadcast)
		return;
	if(Message.m_BroadcastMarkdown.size() > (size_t)QMCLIENT_NEWS_MAX_BYTES)
	{
		LogQmRealtimeEvent("broadcast_too_large", Message.m_Type.c_str());
		return;
	}
	// 版本号不回退，避免旧推送覆盖新内容。
	if(Message.m_BroadcastVersion > 0 && Message.m_BroadcastVersion < m_QmNewsVersion)
		return;

	m_QmNewsMarkdown = Message.m_BroadcastMarkdown;
	if(Message.m_BroadcastVersion > 0)
		m_QmNewsVersion = Message.m_BroadcastVersion;
	m_QmNewsStatus = m_QmNewsMarkdown.empty() ? EQmNewsStatus::EMPTY : EQmNewsStatus::READY;
	++m_QmNewsRevision;
	SaveQmNewsCache();
	LogQmRealtimeEvent("broadcast", "已应用服务端推送");
}

void CQmClient::ApplyQmRealtimeTitles(const SQmRealtimeMessage &Message)
{
	if(!Message.m_HasTitles || !Message.m_pTitlePayload)
		return;
	if(Client()->State() != IClient::STATE_ONLINE || Client()->ServerAddress() == nullptr)
		return;

	char aServer[NETADDR_MAXSTRSIZE] = "";
	net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);

	// 直接复用消息首次解析的负载，应用规则仍与 HTTP 名单共用。
	const json_value *pAddress = JsonObjectField(Message.m_pTitlePayload.get(), "server_address");
	if(pAddress->type != json_string || str_comp(pAddress->u.string.ptr, aServer) != 0)
		return;
	ApplyQmTitlePresences(Message.m_pTitlePayload.get(), aServer);
}

void CQmClient::UpdateQmRealtime()
{
	if(m_QmClientShutdownReported)
		return;
	EnsureQmRealtimeConnection();
	if(!m_pQmRealtime || !m_pQmRealtime->Available())
		return;
	if(!QmRealtimeConnected())
	{
		m_QmRealtimeConnectedTick = 0;
		return;
	}
	const int64_t ConnectedTick = m_pQmRealtime->LastConnectedTick();
	if(m_QmRealtimeConnectedTick != ConnectedTick)
	{
		m_QmRealtimeConnectedTick = ConnectedTick;
		m_QmRealtimePresenceBody.clear();
		SendQmRealtimeHello();
		LogQmRealtimeEvent("connected", m_aQmRealtimeUrl);
	}
	SQmWebSocketMessage Message;
	// 限制单帧派发数量，剩余消息留到后续帧。
	for(int Count = 0; Count < 8 && m_pQmRealtime->PollMessage(Message); ++Count)
	{
		if(Message.m_Type != EQmWebSocketMessageType::TEXT)
			continue;
		SQmRealtimeMessage Parsed;
		if(ParseQmRealtimeMessage(Message.m_Data.c_str(), Message.m_Data.size(), Parsed))
			HandleQmRealtimeMessage(Parsed);
	}
	const int64_t Now = time_get_impl();
	if(Now >= m_QmRealtimeNextPresenceCheck)
	{
		m_QmRealtimeNextPresenceCheck = Now + time_freq() / 4;
		const std::string Body = BuildQmRealtimePresence(false);
		if(Body != m_QmRealtimePresenceBody || Now - m_QmRealtimeLastPresence >= 5 * time_freq())
		{
			if(m_pQmRealtime->SendText(Body.c_str(), Body.size()))
			{
				m_QmRealtimePresenceBody = Body;
				m_QmRealtimeLastPresence = Now;
			}
		}
	}
	if(m_QmRealtimeTitleRevision != m_TitleRevision)
	{
		m_QmRealtimeTitleRevision = m_TitleRevision;
		QmRealtimeRequestTitleRefresh();
	}
}

void CQmClient::LogQmAnonymousEmoteEvent(const char *pStage, const char *pDetail) const
{
	if(!g_Config.m_QmWebSocketLog)
		return;
	log_info("qmclient", "anonymous_emote %s: %s", pStage, pDetail != nullptr ? pDetail : "");
}

void CQmClient::StartQmAnonymousEmotes()
{
	StopQmAnonymousEmotes();
	str_copy(m_aQmAnonymousEmoteUrl, "wss://arghena.site/api/sync/anonymous/ws", sizeof(m_aQmAnonymousEmoteUrl));
	m_pQmAnonymousEmote = CreateQmWebSocketClient({});
	if(m_pQmAnonymousEmote && m_pQmAnonymousEmote->Available())
	{
		IQmWebSocketClient::STuning Tuning;
		Tuning.m_HeartbeatMs = g_Config.m_QmWebSocketHeartbeat * 1000;
		Tuning.m_BackoffBaseMs = g_Config.m_QmWebSocketBackoffBaseMs;
		Tuning.m_BackoffMaxMs = g_Config.m_QmWebSocketBackoffMaxMs;
		Tuning.m_OutgoingQueueCapacity = 16;
		m_pQmAnonymousEmote->SetTuning(Tuning);
	}
}

void CQmClient::StopQmAnonymousEmotes()
{
	if(m_pQmAnonymousEmote)
	{
		m_pQmAnonymousEmote->Disconnect();
		m_pQmAnonymousEmote.reset();
	}
	m_aQmAnonymousClientId[0] = '\0';
	m_aQmAnonymousSessionId[0] = '\0';
	m_QmAnonymousEmoteConnectedTick = 0;
	m_QmAnonymousEmoteNextHelloCheck = 0;
	m_QmAnonymousEmoteHelloBody.clear();
}

void CQmClient::EnsureQmAnonymousEmoteConnection()
{
	if(m_pQmAnonymousEmote == nullptr)
		StartQmAnonymousEmotes();
	if(m_pQmAnonymousEmote == nullptr || !m_pQmAnonymousEmote->Available() || m_pQmAnonymousEmote->Desired())
		return;

	SQmWebSocketConnectConfig Config;
	const std::string ParseError = ParseQmWebSocketUrl(m_aQmAnonymousEmoteUrl, Config);
	if(!ParseError.empty())
	{
		if(!m_QmAnonymousEmoteFailureLogged)
		{
			m_QmAnonymousEmoteFailureLogged = true;
			LogQmAnonymousEmoteEvent("invalid_url", ParseError.c_str());
		}
		return;
	}
	Config.m_Protocol = "qmclient-json";
	Config.m_MaxMessageSize = 128 * 1024;
	Config.m_AllowInsecureTls = false;

	std::string Error;
	if(!m_pQmAnonymousEmote->Connect(Config, Error))
	{
		if(!m_QmAnonymousEmoteFailureLogged)
		{
			m_QmAnonymousEmoteFailureLogged = true;
			LogQmAnonymousEmoteEvent("connect_rejected", Error.c_str());
		}
		return;
	}
	m_QmAnonymousEmoteFailureLogged = false;
	LogQmAnonymousEmoteEvent("connecting", m_aQmAnonymousEmoteUrl);
}

std::string CQmClient::BuildQmAnonymousEmoteHello() const
{
	char aServer[NETADDR_MAXSTRSIZE] = "";
	if(Client()->State() == IClient::STATE_ONLINE && Client()->ServerAddress())
		net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);

	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("type");
	Writer.WriteStrValue("hello");
	Writer.WriteAttribute("v");
	Writer.WriteIntValue(2);
	Writer.WriteAttribute("client_type");
	Writer.WriteStrValue("qm");
	Writer.WriteAttribute("client_id");
	Writer.WriteStrValue(m_aQmAnonymousClientId);
	Writer.WriteAttribute("player_name");
	Writer.WriteStrValue(g_Config.m_PlayerName);
	Writer.WriteAttribute("server_address");
	Writer.WriteStrValue(NormalizeQmServerAddress(aServer).c_str());
	Writer.WriteAttribute("session_id");
	Writer.WriteStrValue(m_aQmAnonymousSessionId);
	Writer.WriteAttribute("players");
	Writer.BeginArray();
	if(aServer[0])
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			const int Id = GameClient()->m_aLocalIds[Dummy];
			if((Dummy == 1 && !Client()->DummyConnected()) || Id < 0 || Id >= MAX_CLIENTS || !GameClient()->m_aClients[Id].m_Active)
				continue;
			Writer.BeginObject();
			Writer.WriteAttribute("player_id");
			Writer.WriteIntValue(Id);
			Writer.WriteAttribute("player_name");
			Writer.WriteStrValue(GameClient()->m_aClients[Id].m_aName);
			Writer.WriteAttribute("dummy");
			Writer.WriteBoolValue(Dummy == 1);
			Writer.EndObject();
		}
	}
	Writer.EndArray();
	Writer.EndObject();
	return Writer.GetOutputString();
}

void CQmClient::SendQmAnonymousEmoteHello()
{
	if(m_pQmAnonymousEmote == nullptr || m_pQmAnonymousEmote->State() != EQmWebSocketState::CONNECTED ||
		Client()->State() != IClient::STATE_ONLINE || !Client()->ServerAddress())
		return;
	const std::string Body = BuildQmAnonymousEmoteHello();
	if(m_pQmAnonymousEmote->SendText(Body.c_str(), Body.size()))
	{
		m_QmAnonymousEmoteHelloBody = Body;
		LogQmAnonymousEmoteEvent("hello_sent", Body.c_str());
	}
	else
		LogQmAnonymousEmoteEvent("hello_send_failed", nullptr);
}

void CQmClient::UpdateQmAnonymousEmotes()
{
	if(m_QmClientShutdownReported)
		return;
	if(Client()->State() != IClient::STATE_ONLINE || !Client()->ServerAddress())
	{
		if(m_pQmAnonymousEmote && m_pQmAnonymousEmote->Desired())
			StopQmAnonymousEmotes();
		return;
	}

	EnsureQmAnonymousEmoteConnection();
	if(!m_pQmAnonymousEmote || !m_pQmAnonymousEmote->Available())
		return;
	if(m_pQmAnonymousEmote->State() != EQmWebSocketState::CONNECTED)
	{
		m_QmAnonymousEmoteConnectedTick = 0;
		return;
	}

	const int64_t ConnectedTick = m_pQmAnonymousEmote->LastConnectedTick();
	if(m_QmAnonymousEmoteConnectedTick != ConnectedTick)
	{
		m_QmAnonymousEmoteConnectedTick = ConnectedTick;
		m_QmAnonymousEmoteHelloBody.clear();
		secure_random_password(m_aQmAnonymousClientId, sizeof(m_aQmAnonymousClientId), sizeof(m_aQmAnonymousClientId) - 1);
		secure_random_password(m_aQmAnonymousSessionId, sizeof(m_aQmAnonymousSessionId), sizeof(m_aQmAnonymousSessionId) - 1);
		SendQmAnonymousEmoteHello();
		LogQmAnonymousEmoteEvent("connected", m_aQmAnonymousEmoteUrl);
	}

	SQmWebSocketMessage Message;
	for(int Count = 0; Count < 8 && m_pQmAnonymousEmote->PollMessage(Message); ++Count)
	{
		if(Message.m_Type != EQmWebSocketMessageType::TEXT)
			continue;
		SQmRealtimeMessage Parsed;
		if(!ParseQmRealtimeMessage(Message.m_Data.c_str(), Message.m_Data.size(), Parsed))
			continue;
		if(Parsed.m_Type == "hello_ack")
			LogQmAnonymousEmoteEvent("hello_ack", Message.m_Data.c_str());
		else if(Parsed.m_Type == "error")
			LogQmAnonymousEmoteEvent("server_error", Message.m_Data.c_str());
		else if(Parsed.m_Event == EQmRealtimeEvent::PING)
			m_pQmAnonymousEmote->SendText("{\"type\":\"pong\"}", 15);
		else if(Parsed.m_Event == EQmRealtimeEvent::EMOTICON)
			QueueQmAnonymousEmoticonEvent(Parsed);
	}

	const int64_t Now = time_get_impl();
	if(Now >= m_QmAnonymousEmoteNextHelloCheck)
	{
		m_QmAnonymousEmoteNextHelloCheck = Now + time_freq() / 4;
		const std::string Body = BuildQmAnonymousEmoteHello();
		if(Body != m_QmAnonymousEmoteHelloBody)
			SendQmAnonymousEmoteHello();
	}
}

static bool IsTitleHex(const char *pText, int Length)
{
	if(str_length(pText) != Length)
		return false;
	for(int i = 0; i < Length; ++i)
		if(!((pText[i] >= '0' && pText[i] <= '9') || (pText[i] >= 'a' && pText[i] <= 'f')))
			return false;
	return true;
}

static const char *TitleJsonString(const json_value *pRoot, const char *pKey)
{
	if(!pRoot || pRoot->type != json_object)
		return "";
	const json_value *pValue = json_object_get(pRoot, pKey);
	return pValue->type == json_string ? pValue->u.string.ptr : "";
}

std::string CQmClient::BuildQmRealtimePresence(bool Hello) const
{
	char aServer[NETADDR_MAXSTRSIZE] = "";
	if(Client()->State() == IClient::STATE_ONLINE && Client()->ServerAddress())
		net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("type");
	Writer.WriteStrValue(Hello ? "hello" : "presence");
	if(Hello)
	{
		Writer.WriteAttribute("v");
		Writer.WriteIntValue(QMCLIENT_REALTIME_PROTOCOL_VERSION);
		Writer.WriteAttribute("client_version");
		Writer.WriteStrValue(QMCLIENT_VERSION);
		Writer.WriteAttribute("machine_hash");
		Writer.WriteStrValue(m_aQmClientMachineHash);
		Writer.WriteAttribute("client_id");
		Writer.WriteStrValue(m_aQmClientPlaytimeClientId);
		Writer.WriteAttribute("player_name");
		Writer.WriteStrValue(g_Config.m_PlayerName);
		if(m_QmClientAwaitingRecoveryStop)
		{
			Writer.WriteAttribute("recovery_stop_at");
			Writer.WriteIntValue((int)std::clamp<int64_t>(m_QmClientRecoveryStopAt, 0, std::numeric_limits<int>::max()));
		}
	}
	Writer.WriteAttribute("server_address");
	Writer.WriteStrValue(aServer);
	Writer.WriteAttribute("session_id");
	Writer.WriteStrValue(m_aQmDeveloperSessionId);
	if(QmRealtimeAllowsCredentials(m_aQmRealtimeUrl))
	{
		Writer.WriteAttribute("title_token");
		Writer.WriteStrValue(m_aTitleToken);
		Writer.WriteAttribute("developer_token");
		Writer.WriteStrValue(m_aQmDeveloperToken);
	}
	Writer.WriteAttribute("players");
	Writer.BeginArray();
	if(aServer[0] && m_aQmDeveloperSessionId[0])
	{
		for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
		{
			const int Id = GameClient()->m_aLocalIds[Dummy];
			if((Dummy == 1 && !Client()->DummyConnected()) || Id < 0 || Id >= MAX_CLIENTS || !GameClient()->m_aClients[Id].m_Active)
				continue;
			Writer.BeginObject();
			Writer.WriteAttribute("player_id");
			Writer.WriteIntValue(Id);
			Writer.WriteAttribute("player_name");
			Writer.WriteStrValue(GameClient()->m_aClients[Id].m_aName);
			Writer.WriteAttribute("dummy");
			Writer.WriteBoolValue(Dummy == 1);
			Writer.WriteAttribute("voice_supported");
			Writer.WriteBoolValue(true);
			Writer.EndObject();
		}
	}
	Writer.EndArray();
	Writer.EndObject();
	return Writer.GetOutputString();
}

void CQmClient::SendQmRealtimeStop()
{
	if(!QmRealtimeConnected())
		return;
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("type");
	Writer.WriteStrValue("stop");
	Writer.WriteAttribute("stop_at");
	Writer.WriteIntValue((int)std::clamp<int64_t>(time_timestamp(), 0, std::numeric_limits<int>::max()));
	Writer.EndObject();
	const std::string Body = Writer.GetOutputString();
	m_pQmRealtime->SendText(Body.c_str(), Body.size());
}

void CQmClient::ApplyQmRealtimeTitleProfile(const json_value *pPayload)
{
	if(TitleBusy())
		return;
	const char *pTitle = TitleJsonString(pPayload, "title");
	const char *pName = TitleJsonString(pPayload, "bound_name");
	const char *pStyle = TitleJsonString(pPayload, "style");
	const json_value *pStatus = JsonObjectField(pPayload, "status");
	const bool Authenticated = pStatus->type == json_integer && pStatus->u.integer == 200 && IsValidQmTitle(pTitle);
	const bool Changed = m_TitleAuthenticated != Authenticated || str_comp(m_aTitleText, pTitle) || str_comp(m_aTitleBoundName, pName) || str_comp(m_aTitleProfileStyle, pStyle);
	m_TitleAuthenticated = Authenticated;
	str_copy(m_aTitleText, pTitle);
	str_copy(m_aTitleBoundName, pName);
	str_copy(m_aTitleProfileStyle, pStyle);
	m_pTitleStatus = Authenticated ? Localizable("Permanent sponsor verified") : Localizable("Enter your sponsor code");
	if(Changed)
		++m_TitleRevision;
}

void CQmClient::ApplyQmRealtimeServices(const SQmRealtimeMessage &Message)
{
	const json_value *pPayload = Message.m_pPayload.get();
	if(!pPayload)
		return;
	if(Message.m_Event == EQmRealtimeEvent::USERS)
	{
		char aServer[NETADDR_MAXSTRSIZE] = "";
		if(Client()->State() == IClient::STATE_ONLINE && Client()->ServerAddress())
			net_addr_str(Client()->ServerAddress(), aServer, sizeof(aServer), true);
		const json_value *pAddress = JsonObjectField(pPayload, "server_address");
		if(pAddress->type != json_string || str_comp(pAddress->u.string.ptr, aServer) != 0)
			return;
		if(JsonObjectField(pPayload, "users")->type != json_array)
			return;
		m_pQmRealtimeUsersPayload = Message.m_pPayload;
		str_copy(m_aQmRealtimeUsersServer, aServer);
		m_QmRealtimeUsersExpireTick = time_get_impl() + 20 * time_freq();
	}
	else if(Message.m_Event == EQmRealtimeEvent::DEVELOPERS)
		ApplyQmRealtimeDevelopers(pPayload);
	else if(Message.m_Event == EQmRealtimeEvent::TITLE_PROFILE)
		ApplyQmRealtimeTitleProfile(pPayload);
	else if(Message.m_Event == EQmRealtimeEvent::TITLE_STATUS)
	{
		const json_value *pStatus = JsonObjectField(pPayload, "status");
		if(pStatus->type == json_integer && pStatus->u.integer == 409)
			m_pTitleStatus = Localizable("Four IP addresses are already online");
	}
	else if(Message.m_Event == EQmRealtimeEvent::TIME || Message.m_Event == EQmRealtimeEvent::PLAYTIME)
	{
		int64_t Value = 0;
		if(JsonReadNonNegativeInt64(JsonObjectField(pPayload, "ts"), Value) && Value > 0)
		{
			m_QmClientServerNow = Value;
			m_QmClientServerTimeLastSync = time_get();
		}
		if(Message.m_Event == EQmRealtimeEvent::PLAYTIME)
		{
			if(JsonReadNonNegativeInt64(JsonObjectField(pPayload, "total_seconds"), Value))
				m_QmClientServerPlaytimeSeconds = Value;
			if(JsonReadNonNegativeInt64(JsonObjectField(pPayload, "last_start_at"), Value) && Value > 0)
				m_QmClientServerSessionStart = Value;
			m_QmClientPlaytimeLastSync = time_get();
			if(str_comp(TitleJsonString(pPayload, "action"), "start") == 0)
			{
				if(!m_QmClientStartupSent)
				{
					m_QmClientMarkerStartedAt = time_timestamp();
					m_QmClientMarkerLastSeenAt = m_QmClientMarkerStartedAt;
					m_QmClientStartupSent = true;
					m_QmClientAwaitingRecoveryStop = false;
					WriteQmClientLifecycleMarker();
				}
			}
			else if(str_comp(TitleJsonString(pPayload, "action"), "stop") == 0)
				ClearQmClientLifecycleMarker();
		}
	}
	else if(Message.m_Event == EQmRealtimeEvent::ERROR)
		LogQmRealtimeEvent("service_error", TitleJsonString(pPayload, "error"));
}

static constexpr const char *TITLE_TOKEN_FILE = "qmclient/title_token.txt";

void CQmClient::InitTitleAuthentication()
{
	char *pToken = Storage()->ReadFileStr(TITLE_TOKEN_FILE, IStorage::TYPE_SAVE);
	if(!pToken)
		return;
	str_utf8_trim_right(pToken);
	if(IsTitleHex(pToken, 64))
		str_copy(m_aTitleToken, pToken);
	free(pToken);
}

void CQmClient::StartTitleRequest(const char *pPath, const char *pBody, std::shared_ptr<CHttpRequest> &pTask)
{
	char aUrl[512];
	str_format(aUrl, sizeof(aUrl), "https://qmclient.icu/api/v1/titles/%s", pPath);
	pTask = pBody ? HttpPostJson(aUrl, pBody) : HttpGet(aUrl);
	pTask->MaxResponseSize(128 * 1024);
	pTask->Timeout(CTimeout{3000, 5000, 500, 5});
	// 标题接口用 4xx 携带 error JSON，不能让 FAILONERROR 把响应变成 ERROR
	pTask->FailOnErrorStatus(false);
	if(m_aTitleToken[0] && (str_comp(pPath, "profile") == 0 || str_comp(pPath, "presence") == 0))
	{
		char aAuthorization[80];
		str_format(aAuthorization, sizeof(aAuthorization), "Bearer %s", m_aTitleToken);
		pTask->HeaderString("Authorization", aAuthorization);
	}
	Http()->Run(pTask);
}

void CQmClient::RedeemTitleCode(const char *pCode)
{
	if(TitleBusy() || m_TitleAuthenticated)
		return;
	if(!IsTitleHex(pCode, 48))
	{
		m_pTitleStatus = Localizable("Invalid sponsor code");
		return;
	}
	if(!m_aTitleToken[0])
	{
		unsigned char aRandom[32];
		secure_random_fill(aRandom, sizeof(aRandom));
		static constexpr const char HEX[] = "0123456789abcdef";
		for(size_t i = 0; i < sizeof(aRandom); ++i)
		{
			m_aTitleToken[i * 2] = HEX[aRandom[i] >> 4];
			m_aTitleToken[i * 2 + 1] = HEX[aRandom[i] & 15];
		}
		m_aTitleToken[64] = '\0';
		Storage()->CreateFolder("qmclient", IStorage::TYPE_SAVE);
		IOHANDLE File = Storage()->OpenFile(TITLE_TOKEN_FILE, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		bool Saved = false;
		if(File)
		{
			const unsigned Written = io_write(File, m_aTitleToken, 64);
			const int Closed = io_close(File);
			Saved = Written == 64 && Closed == 0;
		}
		if(!Saved)
		{
			m_aTitleToken[0] = '\0';
			m_pTitleStatus = Localizable("Could not save title credential");
			return;
		}
	}
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("code");
	Writer.WriteStrValue(pCode);
	Writer.WriteAttribute("token");
	Writer.WriteStrValue(m_aTitleToken);
	Writer.EndObject();
	StartTitleRequest("redeem", Writer.GetOutputString().c_str(), m_pTitleOperation);
	m_pTitleStatus = Localizable("Contacting title server");
}

void CQmClient::RefreshTitleProfile()
{
	if(TitleBusy() || !m_aTitleToken[0])
		return;
	StartTitleRequest("profile", nullptr, m_pTitleOperation);
	m_pTitleStatus = Localizable("Contacting title server");
}

void CQmClient::SaveTitleProfile(const char *pTitle, const char *pBoundName, const char *pStyle)
{
	if(TitleBusy() || !m_TitleAuthenticated)
		return;
	if(!IsValidQmTitle(pTitle))
	{
		m_pTitleStatus = Localizable("Title too long or contains unsupported characters");
		return;
	}
	// 只上传服务端认得的风格 id，避免把本地拼写错误写进账号。
	const char *pStyleId = pStyle != nullptr ? pStyle : "";
	if(pStyleId[0] != '\0' && QmTitleStyleById(pStyleId) == nullptr)
	{
		m_pTitleStatus = Localizable("Unknown title style");
		return;
	}
	CJsonStringWriter Writer;
	Writer.BeginObject();
	Writer.WriteAttribute("title");
	Writer.WriteStrValue(pTitle);
	Writer.WriteAttribute("bound_name");
	Writer.WriteStrValue(pBoundName);
	Writer.WriteAttribute("style");
	Writer.WriteStrValue(pStyleId);
	Writer.EndObject();
	StartTitleRequest("profile", Writer.GetOutputString().c_str(), m_pTitleOperation);
	m_pTitleStatus = Localizable("Contacting title server");
}

void CQmClient::ResetTitlePresences()
{
	mem_zero(m_aTitleExpires, sizeof(m_aTitleExpires));
	m_TitleLastSync = 0;
}

const char *CQmClient::PlayerTitle(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient()->m_aClients[ClientId].m_Active || GameClient()->ShouldHideStreamerIdentity(ClientId))
		return "";
	if(GameClient()->IsQmDeveloperAuthenticated(ClientId))
		return "[开发者]";
	if(m_aTitleExpires[ClientId] > time_get() && str_comp(m_aaTitleNames[ClientId], GameClient()->m_aClients[ClientId].m_aName) == 0)
		return m_aaPlayerTitles[ClientId];
	return "";
}

const char *CQmClient::PlayerTitleStyle(int ClientId) const
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient()->m_aClients[ClientId].m_Active || GameClient()->ShouldHideStreamerIdentity(ClientId))
		return "";
	// 与 PlayerTitle 使用同一套有效期与绑定名校验，避免出现「有风格但没有头衔」的状态。
	if(m_aTitleExpires[ClientId] <= time_get() || str_comp(m_aaTitleNames[ClientId], GameClient()->m_aClients[ClientId].m_aName) != 0)
		return "";
	return m_aaPlayerStyles[ClientId];
}

double CQmClient::TitleAnimationTime() const
{
	// 对齐服务端时间后再取模，保证所有客户端在同一时刻得到相同相位。
	return QmTitleAnimationTime((double)Client()->GlobalTime(), m_ServerTimeOffset, m_ServerTimeOffsetValid);
}

void CQmClient::UpdateTitleAuthentication()
{
	if(m_pTitleOperation && m_pTitleOperation->Done())
	{
		// libcurl failures also make Done() true, but they do not produce a
		// completed HTTP result.  ResultJson() asserts in that state.
		if(m_pTitleOperation->State() != EHttpState::DONE)
		{
			m_pTitleStatus = Localizable("Title server unavailable; retry");
			m_pTitleOperation.reset();
			return;
		}
		json_value *pRoot = m_pTitleOperation->ResultJson();
		const char *pTitle = TitleJsonString(pRoot, "title");
		const char *pName = TitleJsonString(pRoot, "bound_name");
		if(m_pTitleOperation->State() == EHttpState::DONE && m_pTitleOperation->StatusCode() == 200 && IsValidQmTitle(pTitle))
		{
			m_TitleAuthenticated = true;
			str_copy(m_aTitleText, pTitle);
			str_copy(m_aTitleBoundName, pName);
			str_copy(m_aTitleProfileStyle, TitleJsonString(pRoot, "style"));
			++m_TitleRevision;
			m_pTitleStatus = Localizable("Permanent sponsor verified");
			ResetTitlePresences();
		}
		else
		{
			const char *pError = TitleJsonString(pRoot, "error");
			m_pTitleStatus = Localizable("Title server unavailable; retry");
			if(str_comp(pError, "code_used") == 0)
				m_pTitleStatus = Localizable("Sponsor code already claimed");
			else if(str_comp(pError, "invalid_code") == 0)
				m_pTitleStatus = Localizable("Invalid sponsor code");
			else if(str_comp(pError, "invalid_title") == 0)
				m_pTitleStatus = Localizable("Title too long or contains unsupported characters");
			else if(str_comp(pError, "invalid_name") == 0)
				m_pTitleStatus = Localizable("Invalid bound name");
			else if(m_pTitleOperation->StatusCode() == 401)
			{
				m_TitleAuthenticated = false;
				m_pTitleStatus = Localizable("Enter your sponsor code");
			}
		}
		json_value_free(pRoot);
		m_pTitleOperation.reset();
	}
}

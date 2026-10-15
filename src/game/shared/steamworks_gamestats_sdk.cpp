//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Uploads KeyValue stats to the new SteamWorks gamestats system.
//
//=============================================================================

#include "cbase.h"
#include "cdll_int.h"
#include "tier2/tier2.h"
#include <time.h>

#ifdef	GAME_DLL
#include "gameinterface.h"
#elif	CLIENT_DLL
#include "c_playerresource.h"
#endif

#include "steam/isteamutils.h"

#include "steamworks_gamestats.h"
#include "achievementmgr.h"
#include "icommandline.h"

#include "steam/steam_gameserver.h"

// NOTE: This has to be the last file included!
#include "tier0/memdbgon.h"

#if defined(CLIENT_DLL) || defined(CSTRIKE_DLL)
ConVar	steamworks_sessionid_client("steamworks_sessionid_client", "0", FCVAR_HIDDEN, "The client session ID for the new steamworks gamestats.");
#endif

// This is used to replicate our server id to the client so that client data can be associated with the server's.
ConVar	steamworks_sessionid_server("steamworks_sessionid_server", "0", FCVAR_REPLICATED | FCVAR_HIDDEN, "The server session ID for the new steamworks gamestats.");

// This is used to show when the steam works is uploading stats
#define steamworks_show_uploads 0

// This is a stop gap to disable the steam works game stats from ever initializing in the event that we need it
#if defined(CSTRIKE_DLL)
#define steamworks_stats_disable 1
#else
#define steamworks_stats_disable 0
#endif

// This is used to control when the stats get uploaded. If we wait until the end of the session, we miss out on all the stats if the server crashes. If we upload as we go, then we will have the data
#define steamworks_immediate_upload 1

// If set to zero all cvars will be tracked.
#define steamworks_track_cvar 0

// If set to non zero only cvars that are don't have default values will be tracked, but only if steamworks_track_cvar is also not zero.
#define steamworks_track_cvar_diff_only 1

enum
{
	k_ClientJoinMethod_Unknown = -1,
	k_ClientJoinMethod_ListenServer = -2,
	k_ClientJoinMethod_ServerBrowser_UNKNOWN = -3, // server browser, unknown tab
	k_ClientJoinMethod_Steam = -4,
	k_ClientJoinMethod_Matchmaking = -5,
	k_ClientJoinMethod_Coaching = -6,
	k_ClientJoinMethod_Redirect = -7,
	k_ClientJoinMethod_ServerBrowserInternet = -10,
	k_ClientJoinMethod_ServerBrowserFriends = -11,
	k_ClientJoinMethod_ServerBrowserFavorites = -12,
	k_ClientJoinMethod_ServerBrowserHistory = -13,
	k_ClientJoinMethod_ServerBrowserLAN = -14,
	k_ClientJoinMethod_ServerBrowserSpectator = -15
};

extern ConVar developer;

static CSteamWorksGameStatsUploader g_SteamWorksGameStats;

//-----------------------------------------------------------------------------
// Purpose: Returns the time since the epoch
//-----------------------------------------------------------------------------
time_t CSteamWorksGameStatsUploader::GetTimeSinceEpoch( void )
{
	time_t aclock;
	time( &aclock );
	return aclock;
}

//-----------------------------------------------------------------------------
// Purpose: Returns a reference to the global object
//-----------------------------------------------------------------------------
CSteamWorksGameStatsUploader& GetSteamWorksSGameStatsUploader()
{
	return g_SteamWorksGameStats;
}

//-----------------------------------------------------------------------------
// Purpose: Constructor. Sets up the steam callbacks accordingly depending on client/server dll 
//-----------------------------------------------------------------------------
CSteamWorksGameStatsUploader::CSteamWorksGameStatsUploader() : CAutoGameSystemPerFrame( "CSteamWorksGameStatsUploader" )
#if !defined(NO_STEAM) && defined(GAME_DLL) 
, m_CallbackSteamSessionInfoIssued(this, &CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoIssued)
, m_CallbackSteamSessionInfoClosed(this, &CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoClosed)
#endif
{

#if !defined(NO_STEAM) && defined(CLIENT_DLL)
	m_CallbackSteamSessionInfoIssued.Register(this, &CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoIssued);
	m_CallbackSteamSessionInfoClosed.Register(this, &CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoClosed);
#endif

	Reset();

}

//-----------------------------------------------------------------------------
// Purpose: Checks to see if Cvar diff tracking is enabled and if so uploads the diffs
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::UploadCvars()
{
	if (steamworks_track_cvar)
	{
		bool bOnlyDiffCvars = steamworks_track_cvar_diff_only;

		// Get all the Cvar Differences...
		const ConCommandBase* var = g_pCVar->GetCommands();

		// Loop through vars and print out findings
		for (; var; var = var->GetNext())
		{
			if (var->IsCommand())
				continue;

			if (var->IsFlagSet(FCVAR_DEVELOPMENTONLY) || var->IsFlagSet(FCVAR_NEVER_AS_STRING))
				continue;

			const char* pDefValue = ((ConVar*)var)->GetDefault();
			const char* pValue = ((ConVar*)var)->GetString();
			if (bOnlyDiffCvars && !Q_stricmp(pDefValue, pValue))
				continue;

			KeyValues* pKV = new KeyValues("Cvars");
			pKV->SetUint64("TimeSubmitted", GetTimeSinceEpoch());
			pKV->SetString("CvarID", var->GetName());
			pKV->SetString("CvarDefValue", pDefValue);
			pKV->SetString("CvarValue", pValue);

			ParseKeyValuesAndSendStats(pKV, false);
			pKV->deleteThis();
		}

		static const char* csCLcvars[] =
		{
			"-threads",
			"-high",
			"-heap",
			"-dxlevel"
		};

		for (int hh = 0; hh < ARRAYSIZE(csCLcvars); ++hh)
		{
			const char* pszParamValue = CommandLine()->ParmValue(csCLcvars[hh]);
			if (pszParamValue)
			{
				KeyValues* pKV = new KeyValues("Cvars");
				pKV->SetUint64("TimeSubmitted", GetTimeSinceEpoch());
				pKV->SetString("CvarID", csCLcvars[hh]);
				pKV->SetString("CvarDefValue", "commandline");
				pKV->SetString("CvarValue", pszParamValue);

				ParseKeyValuesAndSendStats(pKV, false);
				pKV->deleteThis();
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Reset uploader state.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::Reset()
{
	ClearSessionID();

#ifdef	CLIENT_DLL
	steamworks_sessionid_client.SetValue(0);
	ClearServerSessionID();
#endif

	m_ServiceTicking = false;
	m_LastServiceTick = 0;
	m_SessionIDRequestUnsent = false;
	m_SessionIDRequestPending = false;
	m_UploadedStats = false;
	m_bCollectingAny = false;
	m_bCollectingDetails = false;
	m_UserID = 0;
	m_iAppID = 0;
	m_iServerIP = 0;
	m_nClientJoinMethod = k_ClientJoinMethod_Unknown;
	memset(m_pzServerIP, 0, ARRAYSIZE(m_pzServerIP));
	memset(m_pzMapStart, 0, ARRAYSIZE(m_pzMapStart));
	memset(m_pzHostName, 0, ARRAYSIZE(m_pzHostName));
	m_StartTime = 0;
	m_EndTime = 0;
	m_HumanCntInGame = 0;
	m_FriendCntInGame = 0;

	m_ActiveSession.Reset();
	m_iServerConnectCount = 0;

	for (int i = 0; i < m_StatsToSend.Count(); ++i)
	{
		m_StatsToSend[i]->deleteThis();
	}

	m_StatsToSend.RemoveAll();
}

//-----------------------------------------------------------------------------
// Purpose: Init function from CAutoGameSystemPerFrame and must return true.
//-----------------------------------------------------------------------------
bool CSteamWorksGameStatsUploader::Init()
{
	ListenForGameEvent( "hostname_changed" );
	ListenForGameEvent("server_spawn");

#ifdef CLIENT_DLL
	ListenForGameEvent("client_disconnect");
	ListenForGameEvent("client_beginconnect");
	steamworks_sessionid_server.InstallChangeCallback(ServerSessionIDChangeCallback);
#endif

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Event handler for gathering basic info as well as ending sessions.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::FireGameEvent( IGameEvent *event )
{
	if (!event)
	{
		return;
	}

	const char* pEventName = event->GetName();
	if (FStrEq("hostname_changed", pEventName))
	{
		const char* pzHostname = event->GetString("hostname");
		if (pzHostname)
		{
			V_strncpy(m_pzHostName, pzHostname, sizeof(m_pzHostName));
		}
		else
		{
			V_strncpy(m_pzHostName, "No Host Name", sizeof(m_pzHostName));
		}
	}
	else if (FStrEq("server_spawn", pEventName))
	{

#if 0		// the next three disabled if()'s are in the latest l4d2 branch, I'm not sure if they should be brought over
		if (!m_pzServerIP[0])
#endif
		{
			const char* pzAddress = event->GetString("address");
			if (pzAddress)
			{
				V_snprintf(m_pzServerIP, ARRAYSIZE(m_pzServerIP), "%s:%d", pzAddress, event->GetInt("port"));
				ServerAddressToInt();
			}
			else
			{
				V_strncpy(m_pzServerIP, "No Server Address", sizeof(m_pzServerIP));
				m_iServerIP = 0;
			}
		}

#if 0
		if (!m_pzHostName[0])
#endif
		{
			const char* pzHostname = event->GetString("hostname");
			if (pzHostname)
			{
				V_strncpy(m_pzHostName, pzHostname, sizeof(m_pzHostName));
			}
			else
			{
				V_strncpy(m_pzHostName, "No Host Name", sizeof(m_pzHostName));
			}
		}

#if 0
		if (!m_pzMapStart[0])
#endif
		{
			const char* pzMapName = event->GetString("mapname");
			if (pzMapName)
			{
				V_strncpy(m_pzMapStart, pzMapName, sizeof(m_pzMapStart));
			}
			else
			{
				V_strncpy(m_pzMapStart, "No Map Name", sizeof(m_pzMapStart));
			}
		}

		m_bPassword = event->GetBool("password");
	}
#ifdef CLIENT_DLL
	// Started attempting connection to gameserver
	else if (FStrEq("client_beginconnect", pEventName))
	{
		const char* pszSource = event->GetString("source", "");
		m_nClientJoinMethod = k_ClientJoinMethod_Unknown;
		if (pszSource[0] != '\0')
		{
			if (FStrEq("listenserver", pszSource))                 m_nClientJoinMethod = k_ClientJoinMethod_ListenServer;
			else if (FStrEq("serverbrowser", pszSource))           m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowser_UNKNOWN;
			else if (FStrEq("serverbrowser_internet", pszSource))  m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserInternet;
			else if (FStrEq("serverbrowser_friends", pszSource))   m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserFriends;
			else if (FStrEq("serverbrowser_favorites", pszSource)) m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserFavorites;
			else if (FStrEq("serverbrowser_history", pszSource))   m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserHistory;
			else if (FStrEq("serverbrowser_lan", pszSource))       m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserLAN;
			else if (FStrEq("serverbrowser_spectator", pszSource)) m_nClientJoinMethod = k_ClientJoinMethod_ServerBrowserSpectator;
			else if (FStrEq("steam", pszSource))                   m_nClientJoinMethod = k_ClientJoinMethod_Steam;
			else if (FStrEq("matchmaking", pszSource))             m_nClientJoinMethod = k_ClientJoinMethod_Matchmaking;
			else if (FStrEq("coaching", pszSource))                m_nClientJoinMethod = k_ClientJoinMethod_Coaching;
			else if (FStrEq("redirect", pszSource))                m_nClientJoinMethod = k_ClientJoinMethod_Redirect;
			else if (sscanf(pszSource, "quickplay_%d", &m_nClientJoinMethod) == 1)
				Assert(m_nClientJoinMethod > 0);
			else
				Warning("Unrecognized client_beginconnect event 'source' argument: '%s'\n", pszSource);
		}
	}
	else if (FStrEq("client_disconnect", pEventName))
	{
		ClientDisconnect();
	}
#endif
}

#ifdef CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose:	Sets the server session ID but ONLY if it's not 0. We are using this to avoid a race 
// 			condition where a server sends their session stats before a client does, thereby,
//			resetting the client's server session ID to 0.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::SetServerSessionID( uint64 serverSessionID )
{
}

//-----------------------------------------------------------------------------
// Purpose:	Writes the disconnect time to the current server session entry.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::ClientDisconnect()
{


}

#endif

//-----------------------------------------------------------------------------
// Purpose: Called when the level shuts down.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::LevelShutdown()
{
#ifdef CLIENT_DLL
	ClearServerSessionID();
#endif
}

//-----------------------------------------------------------------------------
// Purpose: Requests a session ID from steam.
//-----------------------------------------------------------------------------
EResult	CSteamWorksGameStatsUploader::RequestSessionID()
{
	// If we have disabled steam works game stats, don't request ids.
	if (steamworks_stats_disable)
	{
		DevMsg("Steamworks Stats: No stats collection because steamworks_stats_disable is set to 1.\n");
		return k_EResultAccessDenied;
	}

	if (developer.GetInt() == 1)
	{
		//		DevMsg( "Steamworks Stats: No stats collection because developer is set to 1.\n" );
		//		return k_EResultAccessDenied;
	}

	// Do not continue if we already have a session id.
	// We must end a session before we can begin a new one.
	if (m_SessionID)
	{
		return k_EResultOK;
	}

	// Do not continue if we are waiting a server response for this session request.
	if (m_SessionIDRequestPending)
	{
		return k_EResultPending;
	}

	// If a request is unsent, it will be sent as soon as the steam API is available.
	m_SessionIDRequestUnsent = true;

	// Turn on polling.
	m_ServiceTicking = true;

	// If we can't use Steam at the moment, we need to wait.
	if (!AccessToSteamAPI())
	{
		return k_EResultNoConnection;
	}

	m_SteamWorksInterface = GetInterface();
	if (m_SteamWorksInterface)
	{
		int accountType = k_EGameStatsAccountType_Steam;
#ifdef GAME_DLL
		if (engine->IsDedicatedServer())
		{
			accountType = k_EGameStatsAccountType_SteamGameServer;
		}
#endif

#ifdef CLIENT_DLL
		DevMsg("Steamworks Stats: Requesting CLIENT session id.\n");
#else
		DevMsg("Steamworks Stats: Requesting SERVER session id.\n");
#endif

		m_SessionIDRequestUnsent = false;
		m_SessionIDRequestPending = true;

		// This initiates a callback that will get us our session ID.
		// Callback: Steam_OnSteamSessionInfoIssued
		m_SteamWorksInterface->GetNewSession(accountType, m_UserID, m_iAppID, GetTimeSinceEpoch());
	}

	return k_EResultOK;
}

//-----------------------------------------------------------------------------
// Purpose: Clears our session id and session id convar.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::ClearSessionID()
{
	m_SessionID = 0;
	steamworks_sessionid_server.SetValue(0);
}

#ifndef	NO_STEAM

//-----------------------------------------------------------------------------
// Purpose: The steam callback used to get our session IDs.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoIssued( GameStatsSessionIssued_t *pGameStatsSessionInfo )
{
	if (!m_SessionIDRequestPending)
	{
		// There is no request outstanding.
		return;
	}

	m_SessionIDRequestPending = false;

	if (!pGameStatsSessionInfo)
	{
		// Empty callback!
		ClearSessionID();
		return;
	}

	if (pGameStatsSessionInfo->m_eResult != k_EResultOK)
	{
#ifdef CLIENT_DLL
		DevMsg("Steamworks Stats: CLIENT session id not available.\n");
#else
		DevMsg("Steamworks Stats: SERVER session id not available.\n");
#endif
		m_SessionIDRequestUnsent = true; // Queue to re-request a session ID.
		ClearSessionID();
		return;
	}

#ifdef CLIENT_DLL
	DevMsg("Steamworks Stats: Received CLIENT session id: %llu\n", pGameStatsSessionInfo->m_ulSessionID);
#else
	DevMsg("Steamworks Stats: Received SERVER session id: %llu\n", pGameStatsSessionInfo->m_ulSessionID);
#endif

	m_StartTime = GetTimeSinceEpoch();

	m_SessionID = pGameStatsSessionInfo->m_ulSessionID;
	m_bCollectingAny = pGameStatsSessionInfo->m_bCollectingAny;
	m_bCollectingDetails = pGameStatsSessionInfo->m_bCollectingDetails;

	char sessionIDString[32];
	Q_snprintf(sessionIDString, sizeof(sessionIDString), "%llu", m_SessionID);

#ifdef CLIENT_DLL
	steamworks_sessionid_client.SetValue(sessionIDString);
	m_FriendCntInGame = GetFriendCountInGame();
	m_HumanCntInGame = GetHumanCountInGame();
#else
	steamworks_sessionid_server.SetValue(sessionIDString);
#endif

	UploadCvars();
}

//-----------------------------------------------------------------------------
// Purpose: The steam callback to notify us that we've submitted stats.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::Steam_OnSteamSessionInfoClosed( GameStatsSessionClosed_t *pGameStatsSessionInfo )
{
	if (!m_UploadedStats)
		return;

	m_UploadedStats = false;
}

//-----------------------------------------------------------------------------
// Purpose: Per frame think. Used to periodically check if we have queued operations.
// For example: we may request a session id before steam is ready.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::FrameUpdatePostEntityThink()
{
	if (!m_ServiceTicking)
		return;

	if (gpGlobals->realtime - m_LastServiceTick < 3)
		return;
	m_LastServiceTick = gpGlobals->realtime;

	if (!AccessToSteamAPI())
		return;

	// Try to resend our request.
	if (m_SessionIDRequestUnsent)
	{
		RequestSessionID();
		return;
	}

	// If we had nothing to resend, stop ticking.
	m_ServiceTicking = false;
}

#endif

//-----------------------------------------------------------------------------
// Purpose: Opens a session: requests the session id, etc.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::StartSession()
{
	RequestSessionID();
}

//-----------------------------------------------------------------------------
// Purpose: Completes a session for the given type.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::EndSession()
{
	m_EndTime = GetTimeSinceEpoch();

	if (!m_SessionID)
	{
		// No session to end.
		return;
	}

	m_SteamWorksInterface = GetInterface();
	if (m_SteamWorksInterface)
	{
#ifdef CLIENT_DLL
		DevMsg("Steamworks Stats: Ending CLIENT session id: %llu\n", m_SessionID);
#else
		DevMsg("Steamworks Stats: Ending SERVER session id: %llu\n", m_SessionID);
#endif

		// Flush any stats that haven't been sent yet.
		FlushStats();

		// Always need some data in the session row or we'll crash steam.
		WriteSessionRow();
		m_SteamWorksInterface->EndSession(m_SessionID, m_EndTime, 0);
		Reset();
	}
}

//-----------------------------------------------------------------------------
// Purpose: Flush any unsent rows.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::FlushStats()
{
	for (int i = 0; i < m_StatsToSend.Count(); ++i)
	{
		ParseKeyValuesAndSendStats(m_StatsToSend[i]);
		m_StatsToSend[i]->deleteThis();
	}

	m_StatsToSend.RemoveAll();
}

//-----------------------------------------------------------------------------
// Purpose: Uploads any end of session rows.
//-----------------------------------------------------------------------------
void CSteamWorksGameStatsUploader::WriteSessionRow()
{
	m_SteamWorksInterface = GetInterface();
	if (!m_SteamWorksInterface)
		return;

	// The Session row is common to both client and server sessions.
	// It enables keying to other tables.

	// Don't send SessionID. It's provided by steam. Same with the account's id and type.
//	m_SteamWorksInterface->AddSessionAttributeInt64( m_SessionID, "SessionID", m_SessionID );

#ifdef CLIENT_DLL
//	m_SteamWorksInterface->AddSessionAttributeInt64( m_SessionID, "ServerSessionID", m_ServerSessionID );
#endif

	m_SteamWorksInterface->AddSessionAttributeInt(m_SessionID, "AppID", m_iAppID);
	m_SteamWorksInterface->AddSessionAttributeInt(m_SessionID, "StartTime", m_StartTime);
	m_SteamWorksInterface->AddSessionAttributeInt(m_SessionID, "EndTime", m_EndTime);

#ifndef CLIENT_DLL
	m_SteamWorksInterface->AddSessionAttributeString(m_SessionID, "ServerIP", m_pzServerIP);
	m_SteamWorksInterface->AddSessionAttributeString(m_SessionID, "ServerName", m_pzHostName);
	m_SteamWorksInterface->AddSessionAttributeString(m_SessionID, "StartMap", m_pzMapStart);
#endif

#ifdef CLIENT_DLL
	// TODO CAB: Need to get these added into the clientsessionlookup table
	m_SteamWorksInterface->AddSessionAttributeInt(m_SessionID, "PlayersInGame", m_HumanCntInGame);
	m_SteamWorksInterface->AddSessionAttributeInt(m_SessionID, "FriendsInGame", m_FriendCntInGame);
#endif
}

//-----------------------------------------------------------------------------
// DATA ACCESS UTILITIES
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose: Verifies that we have a valid interface and will attempt to obtain a new one if we don't.
//-----------------------------------------------------------------------------
bool CSteamWorksGameStatsUploader::VerifyInterface( void )
{
	if (!m_SteamWorksInterface)
	{
		m_SteamWorksInterface = GetInterface();
		if (!m_SteamWorksInterface)
		{
			return false;
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to write an int32 to a table given the row name
//-----------------------------------------------------------------------------
EResult CSteamWorksGameStatsUploader::WriteIntToTable( const int value, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	return m_SteamWorksInterface->AddRowAttributeInt(iTableID, pzRow, value);
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to write an int64 to a table given the row name
//-----------------------------------------------------------------------------
EResult CSteamWorksGameStatsUploader::WriteInt64ToTable( const uint64 value, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	return m_SteamWorksInterface->AddRowAttributeInt64(iTableID, pzRow, value);
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to write an float to a table given the row name
//-----------------------------------------------------------------------------
EResult CSteamWorksGameStatsUploader::WriteFloatToTable( const float value, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	return m_SteamWorksInterface->AddRowAttributeFloat(iTableID, pzRow, value);
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to write an string to a table given the row name
//-----------------------------------------------------------------------------
EResult CSteamWorksGameStatsUploader::WriteStringToTable( const char *value, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	return m_SteamWorksInterface->AddRowAtributeString(iTableID, pzRow, value);
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to search a KeyValues for a value with the given keyName and add the result to the 
// row. If the key isn't present, return ResultNoMatch to indicate such. 
//-----------------------------------------------------------------------------
EResult	CSteamWorksGameStatsUploader::WriteOptionalFloatToTable( KeyValues *pKV, const char* keyName, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	KeyValues* key = pKV->FindKey(keyName);
	if (!key)
		return k_EResultNoMatch;

	float value = key->GetFloat();

	return m_SteamWorksInterface->AddRowAttributeFloat(iTableID, pzRow, value);
}

//-----------------------------------------------------------------------------
// Purpose: Wrapper function to search a KeyValues for a value with the given keyName and add the result to the 
// row. If the key isn't present, return ResultNoMatch to indicate such. 
//-----------------------------------------------------------------------------
EResult	CSteamWorksGameStatsUploader::WriteOptionalIntToTable( KeyValues *pKV, const char* keyName, uint64 iTableID, const char *pzRow )
{
	if (!VerifyInterface())
		return k_EResultNoConnection;

	KeyValues* key = pKV->FindKey(keyName);
	if (!key)
		return k_EResultNoMatch;

	int value = key->GetInt();

	return m_SteamWorksInterface->AddRowAttributeInt(iTableID, pzRow, value);
}



//-----------------------------------------------------------------------------
// STEAM ACCESS UTILITIES
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Purpose: Determines if the system can connect to steam
//-----------------------------------------------------------------------------
bool CSteamWorksGameStatsUploader::AccessToSteamAPI( void )
{
#ifdef	GAME_DLL
	return (steamgameserverapicontext && steamgameserverapicontext->SteamGameServer() && g_pSteamClientGameServer && steamgameserverapicontext->SteamGameServerUtils());
#elif	CLIENT_DLL
	return (steamapicontext && steamapicontext->SteamUser() && steamapicontext->SteamUser()->BLoggedOn() && steamapicontext->SteamFriends() && steamapicontext->SteamMatchmaking());
#endif
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: There's no guarantee that your interface pointer will persist across level transitions,
//			so this function will update your interface.
//-----------------------------------------------------------------------------
ISteamGameStats* CSteamWorksGameStatsUploader::GetInterface( void )
{
	HSteamUser hSteamUser = 0;
	HSteamPipe hSteamPipe = 0;

#ifdef	GAME_DLL
	if (steamgameserverapicontext && steamgameserverapicontext->SteamGameServer() && steamgameserverapicontext->SteamGameServerUtils())
	{
		m_UserID = steamgameserverapicontext->SteamGameServer()->GetSteamID().ConvertToUint64();
		m_iAppID = steamgameserverapicontext->SteamGameServerUtils()->GetAppID();
		hSteamUser = SteamGameServer_GetHSteamUser();
		hSteamPipe = SteamGameServer_GetHSteamPipe();
	}

	// Now let's get the interface for dedicated servers
	if (g_pSteamClientGameServer && engine && engine->IsDedicatedServer())
	{
		return (ISteamGameStats*)g_pSteamClientGameServer->GetISteamGenericInterface(hSteamUser, hSteamPipe, STEAMGAMESTATS_INTERFACE_VERSION);
	}
#elif	CLIENT_DLL
	if (steamapicontext && steamapicontext->SteamUser() && steamapicontext->SteamUtils())
	{
		m_UserID = steamapicontext->SteamUser()->GetSteamID().ConvertToUint64();
		m_iAppID = steamapicontext->SteamUtils()->GetAppID();
		hSteamUser = steamapicontext->SteamUser()->GetHSteamUser();
		hSteamPipe = GetHSteamPipe();
	}
#endif

	// Listen server have access to SteamClient
	if (SteamClient())
	{
		return (ISteamGameStats*)SteamClient()->GetISteamGenericInterface(hSteamUser, hSteamPipe, STEAMGAMESTATS_INTERFACE_VERSION);
	}
	// If we haven't returned already, then we can't get access to the interface
	return NULL;
}

//-----------------------------------------------------------------------------
// Purpose: Creates a table from the KeyValue file. Do NOT send nested KeyValue objects into this function!
//-----------------------------------------------------------------------------
EResult CSteamWorksGameStatsUploader::AddStatsForUpload( KeyValues *pKV, bool bSendImmediately )
{
	if (pKV)
	{
		// Do we want to immediately upload the stats?
		if (bSendImmediately)
		{
			ParseKeyValuesAndSendStats(pKV);
			pKV->deleteThis();
		}
		else
		{
			m_StatsToSend.AddToTail(pKV);
		}
		return k_EResultOK;
	}

	return k_EResultFail;
}

//-----------------------------------------------------------------------------
// Purpose: Parses all the keyvalue files we've been sent and creates tables from them and uploads them
//-----------------------------------------------------------------------------
double g_rowCommitTime = 0.0f;
double g_rowWriteTime = 0.0f;
EResult CSteamWorksGameStatsUploader::ParseKeyValuesAndSendStats( KeyValues *pKV, bool bIncludeClientsServerSessionID )
{
	if (!pKV)
	{
		return k_EResultFail;
	}

	if (!IsCollectingAnyData())
	{
		return k_EResultFail;
	}

	// Refresh the interface in case steam has unloaded
	m_SteamWorksInterface = GetInterface();

	if (!m_SteamWorksInterface)
	{
		DevMsg("WARNING: Attempted to send a steamworks gamestats row when the steamworks interface was not available!");
		return k_EResultNoConnection;
	}

	const char* pzTable = pKV->GetName();
	/*
		if ( steamworks_show_uploads )
		{
	#ifdef	CLIENT_DLL
	//		DevMsg( "Client submitting row (%s).\n", pzTable );
	#elif	GAME_DLL
	//		DevMsg( "Server submitting row (%s).\n", pzTable );
	#endif
		}
	*/
	uint64 iTableID = 0;
	m_SteamWorksInterface->AddNewRow(&iTableID, m_SessionID, pzTable);

	if (!iTableID)
	{
		return k_EResultFail;
	}

	WriteInt64ToTable(m_SessionID, iTableID, "SessionID");
#ifdef	CLIENT_DLL
	if (bIncludeClientsServerSessionID)
	{
		WriteInt64ToTable(m_ServerSessionID, iTableID, "ServerSessionID");
	}
#endif

	// Now we need to loop over all the keys in pKV and add the name and value
	for (KeyValues* pData = pKV->GetFirstSubKey(); pData != NULL; pData = pData->GetNextKey())
	{
		const char* name = pData->GetName();

		CFastTimer writeTimer;
		writeTimer.Start();
		switch (pData->GetDataType())
		{
		case KeyValues::TYPE_STRING:	WriteStringToTable(pKV->GetString(name), iTableID, name);
			break;
		case KeyValues::TYPE_INT:		WriteIntToTable(pKV->GetInt(name), iTableID, name);
			break;
		case KeyValues::TYPE_FLOAT:		WriteFloatToTable(pKV->GetFloat(name), iTableID, name);
			break;
		case KeyValues::TYPE_UINT64:	WriteInt64ToTable(pKV->GetUint64(name), iTableID, name);
			break;
		};

		writeTimer.End();
		g_rowWriteTime += writeTimer.GetDuration().GetMillisecondsF();
	}

	CFastTimer commitTimer;
	commitTimer.Start();
	EResult res = m_SteamWorksInterface->CommitRow(iTableID);
	commitTimer.End();
	g_rowCommitTime += commitTimer.GetDuration().GetMillisecondsF();

	if (res != k_EResultOK)
	{
		AssertMsg(false, "Failed To Submit table %s", pzTable);
	}
	return res;
}

#ifdef	CLIENT_DLL
//-----------------------------------------------------------------------------
// Purpose: Reports client's perf data at the end of a client session.
//---------------------------------`--------------------------------------------
void CSteamWorksGameStatsUploader::AddClientPerfData( KeyValues *pKV )
{

}
#endif

//-------------------------------------------------------------------------------------------------
/**
*	Purpose:	Calculates the number of humans in the game
*/
int CSteamWorksGameStatsUploader::GetHumanCountInGame()
{
	int iHumansInGame = 0;
	// TODO: Need to add server/client code to count the number of connected humans.
	return iHumansInGame;
}

#ifdef	CLIENT_DLL
//-------------------------------------------------------------------------------------------------
/**
*	Purpose:	Calculates the number of friends in the game
*/
int CSteamWorksGameStatsUploader::GetFriendCountInGame()
{

	return 0;
}
#endif

void CSteamWorksGameStatsUploader::ServerAddressToInt()
{
	CUtlStringList IPs;
	V_SplitString(m_pzServerIP, ".", IPs);

	if (IPs.Count() < 4)
	{
		// Not an actual IP.
		m_iServerIP = 0;
		return;
	}

	byte ip[4];
	m_iServerIP = 0;
	for (int i = 0; i < IPs.Count() && i < 4; ++i)
	{
		ip[i] = (byte)Q_atoi(IPs[i]);
	}
	m_iServerIP = (ip[0] << 24) + (ip[1] << 16) + (ip[2] << 8) + ip[3];
}


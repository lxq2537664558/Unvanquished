/*
===========================================================================

Daemon GPL Source Code
Copyright (C) 2012 Unvanquished Developers

This file is part of the Daemon GPL Source Code (Daemon Source Code).

Daemon Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Daemon Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Daemon Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following the
terms and conditions of the GNU General Public License which accompanied the Daemon
Source Code.  If not, please request a copy in writing from id Software at the address
below.

If you have questions concerning this license or the applicable additional terms, you
may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville,
Maryland 20850 USA.

===========================================================================
*/

#include "cg_local.h"

rocketInfo_t rocketInfo;

vmCvar_t rocket_menuFiles;

typedef struct
{
	vmCvar_t   *vmCvar;
	const char *cvarName;
	const char *defaultString;
	int        cvarFlags;
} cvarTable_t;

static const cvarTable_t rocketCvarTable[] =
{
	{ &rocket_menuFiles, "rocket_menuFiles", "ui/rocket.txt", CVAR_ARCHIVE }
};

static const size_t rocketCvarTableSize = ARRAY_LEN( rocketCvarTable );

/*
=================
CG_RegisterRocketCvars
=================
*/
void CG_RegisterRocketCvars( void )
{
	int         i;
	const cvarTable_t *cv;

	for ( i = 0, cv = rocketCvarTable; i < rocketCvarTableSize; i++, cv++ )
	{
		trap_Cvar_Register( cv->vmCvar, cv->cvarName,
		                    cv->defaultString, cv->cvarFlags );
	}
}

void CG_Rocket_Init( void )
{
	int i, len;
	char *token, *text_p;
	char text[ 20000 ];
	fileHandle_t f;

	// Version check...
	trap_SyscallABIVersion( SYSCALL_ABI_VERSION_MAJOR, SYSCALL_ABI_VERSION_MINOR );

	// Init Rocket
	trap_Rocket_Init();

	// load overrides
	BG_InitClassConfigs();
	BG_InitBuildableConfigs();
	BG_InitAllowedGameElements();

	// Dynamic memory
	BG_InitMemory();

	// rocket cvars
	CG_RegisterRocketCvars();

	rocketInfo.rocketState = IDLE;

	// Preload all the menu files...
	len = trap_FS_FOpenFile( rocket_menuFiles.string, &f, FS_READ );
	if (  len <= 0 )
	{
		Com_Error( ERR_DROP, "Unable to load %s. No rocket menus loaded.", rocket_menuFiles.string );
	}

	if ( len >= sizeof( text ) - 1 )
	{
		trap_FS_FCloseFile( f );
		Com_Error( ERR_DROP, "File %s too long.", rocket_menuFiles.string );
	}

	trap_FS_Read( text, len, f );
	text[ len ] = 0;
	text_p = text;
	trap_FS_FCloseFile( f );

	// Parse files to load...
	while ( 1 )
	{
		token = COM_Parse2( &text_p );

		// Closing bracket. EOF
		if ( !*token || *token == '}' )
		{
			break;
		}

		// Ignore openning bracket
		if ( *token == '{' )
		{
			continue;
		}

		// Set the cursor
		if ( !Q_stricmp( token, "cursor" ) )
		{
			token = COM_Parse2( &text_p );

			// Skip non-RML files
			if ( Q_stricmp( token + strlen( token ) - 4, ".rml" ) )
			{
				continue;
			}

			trap_Rocket_LoadCursor( token );
			continue;
		}

		if ( !Q_stricmp( token, "main" ) )
		{
			token = COM_Parse2( &text_p );

			// Skip non-RML files
			if ( Q_stricmp( token + strlen( token ) - 4, ".rml" ) )
			{
				continue;
			}

			trap_Rocket_LoadDocument( token );
			continue;
		}

		if ( !Q_stricmp( token, "root" ) )
		{
			token = COM_Parse( &text_p );

			Q_strncpyz( rocketInfo.rootDir, token, sizeof( rocketInfo.rootDir ) );
			continue;
		}
	}

	// Intialize data sources...
	trap_Rocket_RegisterDataSource( "server_browser" );

	trap_Rocket_DocumentAction( "main", "open" );
}

static void CG_Rocket_EventOpen( const char *args )
{
	trap_Rocket_LoadDocument( va( "%s%s.rml", rocketInfo.rootDir, args ) );
}

static void CG_Rocket_EventClose( const char *args )
{
	trap_Rocket_DocumentAction( args, "close" );
}

static void CG_Rocket_EventGoto( const char *args )
{
	trap_Rocket_DocumentAction( args, "goto" );
}

static void CG_Rocket_EventShow( const char *args )
{
	trap_Rocket_DocumentAction( args, "show" );
}

static int CG_StringToNetSource( const char *src )
{
	if ( !Q_stricmp( src, "local" ) )
	{
		return AS_LOCAL;
	}
	else if ( !Q_stricmp( src, "favorites" ) )
	{
		return AS_FAVORITES;
	}
	else
	{
		return AS_GLOBAL;
	}
}

static void CG_Rocket_InitServers( const char *args )
{
	trap_LAN_ResetPings( CG_StringToNetSource( args ) );
	trap_LAN_ServerStatus( NULL, NULL, 0 );

	if ( !Q_stricmp( args, "internet" ) )
	{
		trap_Cmd_ExecuteText( EXEC_APPEND, "globalservers 0 86 full empty\n" );
	}

	else if ( !Q_stricmp( args, "local" ) )
	{
		trap_Cmd_ExecuteText( EXEC_APPEND, "localservers\n" );
	}

	trap_LAN_UpdateVisiblePings( CG_StringToNetSource( args ) );
}

static void CG_Rocket_BuildServerList( const char *args )
{
	char data[ MAX_INFO_STRING ] = { 0 };
	int i;


	// Only refresh once every second
	if ( trap_Milliseconds() < 1000 + rocketInfo.serversLastRefresh )
	{
		return;
	}

	Q_strncpyz( rocketInfo.currentNetSource, args, sizeof( rocketInfo.currentNetSource ) );
	rocketInfo.rocketState = RETRIEVING_SERVERS;

	if ( !Q_stricmp( args, "internet" ) )
	{
		int numServers;

		trap_Rocket_DSClearTable( "server_browser", args );

		trap_LAN_MarkServerVisible( CG_StringToNetSource( args ), -1, qtrue );

		numServers = trap_LAN_GetServerCount( CG_StringToNetSource( args ) );

		for ( i = 0; i < numServers; ++i )
		{
			char info[ MAX_STRING_CHARS ];
			int ping, bots, clients;

			Com_Memset( &data, 0, sizeof( data ) );

			if ( !trap_LAN_ServerIsVisible( CG_StringToNetSource( args ), i ) )
			{
				continue;
			}

			ping = trap_LAN_GetServerPing( CG_StringToNetSource( args ), i );

			if ( qtrue || !Q_stricmp( args, "favorites" ) )
			{
				trap_LAN_GetServerInfo( CG_StringToNetSource( args ), i, info, MAX_INFO_STRING );

				bots = atoi( Info_ValueForKey( info, "bots" ) );
				clients = atoi( Info_ValueForKey( info, "clients" ) );

				Info_SetValueForKey( data, "name", Info_ValueForKey( info, "hostname" ), qfalse );
				Info_SetValueForKey( data, "players", va( "%d + (%d)", clients, bots ), qfalse );
				Info_SetValueForKey( data, "ping", va( "%d", ping ), qfalse );

				if ( ping > 0 )
				{
					trap_Rocket_DSAddRow( "server_browser", args, data );
				}
			}
		}
	}

	rocketInfo.serversLastRefresh = trap_Milliseconds();
}

static void CG_Rocket_EventExec( const char *args )
{
	trap_Cmd_ExecuteText( EXEC_APPEND, args );
}

static void CG_Rocket_EventCvarForm( const char *args )
{
	static char params[ BIG_INFO_STRING ];
	static char key[BIG_INFO_VALUE], value[ BIG_INFO_VALUE ];
	const char *s;

	trap_Rocket_GetEventParameters( params, 0 );

	if ( !*params )
	{
		return;
	}

	s = params;

	while ( *s )
	{
		Info_NextPair( &s, key, value );
		if ( !Q_stricmpn( "cvar ", key, 5 ) )
		{

			trap_Cvar_Set( key + 5, value );
		}
	}
}


typedef struct
{
	const char *command;
	void ( *exec ) ( const char *args );
} eventCmd_t;

static const eventCmd_t eventCmdList[] =
{
	{ "build_list", &CG_Rocket_BuildServerList },
	{ "close", &CG_Rocket_EventClose },
	{ "cvarform", &CG_Rocket_EventCvarForm },
	{ "exec", &CG_Rocket_EventExec },
	{ "goto", &CG_Rocket_EventGoto },
	{ "init_servers", &CG_Rocket_InitServers },
	{ "open", &CG_Rocket_EventOpen },
	{ "show", &CG_Rocket_EventShow }
};

static const size_t eventCmdListCount = ARRAY_LEN( eventCmdList );

static int eventCmdCmp( const void *a, const void *b )
{
	return Q_stricmp( ( const char * ) a, ( ( eventCmd_t * ) b )->command );
}

void CG_Rocket_ProcessEvents( void )
{
	static char commands[ 2000 ];
	char *tail, *head;
	eventCmd_t *cmd;

	// Get the even command
	trap_Rocket_GetEvent( commands, sizeof( commands ) );

	head = commands;

	// No events to process
	if ( !*head )
	{
		return;
	}

	while ( 1 )
	{
		char *p, *args;

		// Parse it. Check for semicolons first
		tail = strchr( head, ';' );
		if ( tail )
		{
			*tail = '\0';
		}

		p = strchr( head, ' ' );
		if ( p )
		{
			*p = '\0';
		}

		// Special case for when head has no arguments
		args = head + strlen( head ) + ( head + strlen( head ) == tail ? 0 : 1 );

		cmd = bsearch( head, eventCmdList, eventCmdListCount, sizeof( eventCmd_t ), eventCmdCmp );

		if ( cmd )
		{
			cmd->exec( args );
		}

		head = args + strlen( args ) + 1;

		if ( !*head )
		{
			break;
		}

		// Skip whitespaces
		while ( *head == ' ' )
		{
			head++;
		}
	}

	trap_Rocket_DeleteEvent();
}

void CG_Rocket_Frame( void )
{
	switch ( rocketInfo.rocketState )
	{
		case RETRIEVING_SERVERS:
			if ( trap_LAN_UpdateVisiblePings( CG_StringToNetSource( rocketInfo.currentNetSource ) ) )
			{
				trap_Rocket_SetInnerRML( "serverbrowser", "status", "<strong>Updating...</strong>" );
				CG_Rocket_BuildServerList( rocketInfo.currentNetSource );
			}
			else
			{
				trap_Rocket_SetInnerRML( "serverbrowser", "status", "Updated" );
				rocketInfo.rocketState = IDLE;
			}
			break;
	}
	CG_Rocket_ProcessEvents();
}


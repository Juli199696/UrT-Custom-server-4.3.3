/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// sv_client.c -- server code for dealing with clients

#include "server.h"

static void SV_CloseDownload( client_t *cl );

/*
=================
SV_GetChallenge

A "getchallenge" OOB command has been received
Returns a challenge number that can be used
in a subsequent connectResponse command.
We do this to prevent denial of service attacks that
flood the server with invalid connection IPs.  With a
challenge, they must give a valid IP address.

If we are authorizing, a challenge request will cause a packet
to be sent to the authorize server.

When an authorizeip is returned, a challenge response will be
sent to that ip.
=================
*/
void SV_GetChallenge(netadr_t from) {
    int          i;
    int          oldest;
    int          oldestTime;
    challenge_t  *challenge;

    oldest = 0;
    oldestTime = 0x7fffffff;

    // see if we already have a challenge for this ip
    challenge = &svs.challenges[0];
    for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {

        if (!challenge->connected && NET_CompareAdr(from, challenge->adr)) {
            break;
        }

        if (challenge->time < oldestTime) {
            oldestTime = challenge->time;
            oldest = i;
        }

    }

    if (i == MAX_CHALLENGES) {

        // this is the first time this client has asked for a challenge
        challenge = &svs.challenges[oldest];

        challenge->challenge = ((rand() << 16) ^ rand()) ^ svs.time;
        challenge->adr = from;
        challenge->pingTime = -1;
        challenge->firstTime = svs.time;
        challenge->time = svs.time;
        challenge->connected = qfalse;
        i = oldest;

    }

    if (challenge->pingTime == -1) {
        challenge->pingTime = svs.time;
    }

    NET_OutOfBandPrint(NS_SERVER, from, "challengeResponse %i", challenge->challenge);
    return;
}

/*
====================
SV_AuthorizeIpPacket

A packet has been returned from the authorize server.
If we have a challenge adr for that ip, send the
challengeResponse to it
====================
*/
void SV_AuthorizeIpPacket( netadr_t from ) {
	int		challenge;
	int		i;
	char	*s;
	char	*r;

	if ( !NET_CompareBaseAdr( from, svs.authorizeAddress ) ) {
		Com_Printf( "SV_AuthorizeIpPacket: not from authorize server\n" );
		return;
	}

	challenge = atoi( Cmd_Argv( 1 ) );

	for (i = 0 ; i < MAX_CHALLENGES ; i++) {
		if ( svs.challenges[i].challenge == challenge ) {
			break;
		}
	}
	if ( i == MAX_CHALLENGES ) {
		Com_Printf( "SV_AuthorizeIpPacket: challenge not found\n" );
		return;
	}

	// send a packet back to the original client
	if (svs.challenges[i].pingTime == -1) {
		svs.challenges[i].pingTime = svs.time;
	}

	s = Cmd_Argv( 2 );
	r = Cmd_Argv( 3 );			// reason

	if ( !Q_stricmp( s, "demo" ) ) {
		// they are a demo client trying to connect to a real server
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nServer is not a demo server\n" );
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}
	if ( !Q_stricmp( s, "accept" ) ) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr,
			"challengeResponse %i", svs.challenges[i].challenge );
		return;
	}
	if ( !Q_stricmp( s, "unknown" ) ) {
		if (!r) {
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nAwaiting CD key authorization\n" );
		} else {
			NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\n%s\n", r);
		}
		// clear the challenge record so it won't timeout and let them through
		Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
		return;
	}

	// authorization failed
	if (!r) {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\nSomeone is using this CD Key\n" );
	} else {
		NET_OutOfBandPrint( NS_SERVER, svs.challenges[i].adr, "print\n%s\n", r );
	}

	// clear the challenge record so it won't timeout and let them through
	Com_Memset( &svs.challenges[i], 0, sizeof( svs.challenges[i] ) );
}

/*
==================
SV_DirectConnect

A "connect" OOB command has been received
==================
*/

#define PB_MESSAGE "PunkBuster Anti-Cheat software must be installed " \
                   "and Enabled in order to join this server. An updated game patch can be downloaded from " \
                   "www.idsoftware.com"

void SV_DirectConnect(netadr_t from) {

    char            userinfo[MAX_INFO_STRING];
    int             i, j;
    client_t        temp;
    client_t       *cl, *newcl;
    sharedEntity_t *ent;
    char           *password;
    int             clientNum;
    int             version;
    int             qport;
    int             challenge;
    int             startIndex;
    intptr_t        denied;
    int             count;
    int             numIpClients = 0;

    Com_DPrintf("SV_DirectConnect()\n");

    Q_strncpyz(userinfo, Cmd_Argv(1), sizeof(userinfo));

    version = atoi(Info_ValueForKey(userinfo, "protocol"));
    if (version != PROTOCOL_VERSION) {
        NET_OutOfBandPrint(NS_SERVER, from, "print\nServer uses protocol version %i.\n", PROTOCOL_VERSION);
        Com_DPrintf("    rejected connect from version %i\n", version);
        return;
    }

    challenge = atoi(Info_ValueForKey(userinfo, "challenge"));
    qport = atoi(Info_ValueForKey(userinfo, "qport"));

    // quick reject
    for (i = 0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

        if (cl->state == CS_FREE) {
            continue;
        }

        if (NET_CompareBaseAdr(from, cl->netchan.remoteAddress) && (cl->netchan.qport == qport || from.port == cl->netchan.remoteAddress.port)) {

            if ((svs.time - cl->lastConnectTime) < (sv_reconnectlimit->integer * 1000)) {
                Com_DPrintf("%s:reconnect rejected : too soon\n", NET_AdrToString (from));
                return;
            }

            break;
        }
    }

    // see if the challenge is valid (LAN clients don't need to challenge)
    if (!NET_IsLocalAddress (from)) {

        int ping;

        for (i = 0 ; i < MAX_CHALLENGES; i++) {
            if (NET_CompareAdr(from, svs.challenges[i].adr)) {
                if (challenge == svs.challenges[i].challenge) {
                    break;  // good
                }
            }
        }

        if (i == MAX_CHALLENGES) {
            NET_OutOfBandPrint(NS_SERVER, from, "print\nNo or bad challenge for address.\n");
            return;
        }

        // force the IP key/value pair so the game can filter based on ip
        Info_SetValueForKey(userinfo, "ip", NET_AdrToString(from));

        // Note that it is totally possible to flood the console and qconsole.log by being rejected
        // (high ping, ban, server full, or other) and repeatedly sending a connect packet against the same
        // challenge.  Prevent this situation by only logging the first time we hit SV_DirectConnect()
        // for this challenge.
        if (!svs.challenges[i].connected) {
            ping = svs.time - svs.challenges[i].pingTime;
            svs.challenges[i].challengePing = ping;
            Com_Printf("Client %i connecting with %i challenge ping\n", i, ping);
        }
        else {
            ping = svs.challenges[i].challengePing;
            Com_DPrintf("Client %i connecting again with %i challenge ping\n", i, ping);
        }

        svs.challenges[i].connected = qtrue;

        // never reject a LAN client based on ping
        if (!Sys_IsLANAddress(from)) {

            for (j=0,cl=svs.clients ; j < sv_maxclients->integer ; j++,cl++) {
                if ( cl->state == CS_FREE ) {
                    continue;
                }
                if ( NET_CompareBaseAdr( from, cl->netchan.remoteAddress )
                    && !( cl->netchan.qport == qport
                    || from.port == cl->netchan.remoteAddress.port ) ) {
                    numIpClients++;
                }
            }

            if (sv_clientsPerIp->integer && numIpClients >= sv_clientsPerIp->integer) {
                NET_OutOfBandPrint(NS_SERVER, from, "print\nToo many connections from the same IP\n");
                Com_DPrintf ("Client %i rejected due to too many connections from the same IP\n", i);
                return;
            }

            if (sv_minPing->value && ping < sv_minPing->value) {
                NET_OutOfBandPrint(NS_SERVER, from, va("print\n               Server is for high pings only                    \n"
                    "Your ping: " S_COLOR_RED "%i" S_COLOR_WHITE " Minimum ping: " S_COLOR_GREEN "%i\n",
                    ping, sv_minPing->integer));
                Com_DPrintf ("Client %i rejected on a too low ping\n", i);
                return;
            }

            if (sv_maxPing->value && ping > sv_maxPing->value) {
                NET_OutOfBandPrint(NS_SERVER, from, va("print\n               Server is for low pings only                    \n"
                    "Your ping: " S_COLOR_RED "%i" S_COLOR_WHITE " Maximum ping: " S_COLOR_GREEN "%i\n",
                    ping, sv_maxPing->integer));
                Com_DPrintf ("Client %i rejected on a too high ping\n", i);
                return;
            }

        }

    } else {
        // force the "ip" info key to "localhost"
        Info_SetValueForKey(userinfo, "ip", "localhost");
    }

    #ifdef USE_SQLITE_BANS
    #ifdef DEDICATED
    if (Bans_CheckIP(from)) {
        NET_OutOfBandPrint(NS_SERVER, from, "print\nYou are banned.\n");
        Com_Printf("%s(%i.%i.%i.%i) rejected: banned.\n", Info_ValueForKey(userinfo, "name"), from.ip[0], from.ip[1], from.ip[2], from.ip[3]);
        return;
    }
    #endif
    #endif

    newcl = &temp;
    Com_Memset(newcl, 0, sizeof(client_t));

    // if there is already a slot for this ip, reuse it
    for (i = 0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {

        if (cl->state == CS_FREE) {
            continue;
        }

        if (NET_CompareBaseAdr(from, cl->netchan.remoteAddress) && (cl->netchan.qport == qport || from.port == cl->netchan.remoteAddress.port)) {
            Com_Printf ("%s:reconnect\n", NET_AdrToString (from));
            newcl = cl;
            goto gotnewcl;
        }
    }

    // find a client slot
    // if "sv_privateClients" is set > 0, then that number
    // of client slots will be reserved for connections that
    // have "password" set to the value of "sv_privatePassword"
    // Info requests will report the maxclients as if the private
    // slots didn't exist, to prevent people from trying to connect
    // to a full server.
    // This is to allow us to reserve a couple slots here on our
    // servers so we can play without having to kick people.

    // check for privateClient password
    password = Info_ValueForKey(userinfo, "password");
    if (!strcmp(password, sv_privatePassword->string)) {
        startIndex = 0;
    } else {
        // skip past the reserved slots
        startIndex = sv_privateClients->integer;
    }

    newcl = NULL;
    for (i = startIndex; i < sv_maxclients->integer ; i++) {
        cl = &svs.clients[i];
        if (cl->state == CS_FREE) {
            newcl = cl;
            break;
        }
    }

    if (!newcl) {

        if (NET_IsLocalAddress(from)) {

            count = 0;
            for (i = startIndex; i < sv_maxclients->integer ; i++) {
                cl = &svs.clients[i];
                if (cl->netchan.remoteAddress.type == NA_BOT) {
                    count++;
                }
            }

            // if they're all bots
            if (count >= sv_maxclients->integer - startIndex) {
                SV_DropClient(&svs.clients[sv_maxclients->integer - 1], "only bots on server");
                newcl = &svs.clients[sv_maxclients->integer - 1];
            }
            else {
                Com_Error(ERR_FATAL, "server is full on local connect\n");
                return;
            }
        }
        else {
            NET_OutOfBandPrint(NS_SERVER, from, "print\nServer is full\n");
            Com_DPrintf("Rejected a connection.\n");
            return;
        }

    }

    // we got a newcl, so reset the reliableSequence and reliableAcknowledge
    cl->reliableAcknowledge = 0;
    cl->reliableSequence = 0;

gotnewcl:
    // build a new connection
    // accept the new client
    // this is the only place a client_t is ever initialized
    *newcl = temp;
    clientNum = newcl - svs.clients;
    ent = SV_GentityNum(clientNum);
    newcl->gentity = ent;

    // save the challenge
    newcl->challenge = challenge;

    // save the address
    Netchan_Setup (NS_SERVER, &newcl->netchan , from, qport);
    // init the netchan queue
    newcl->netchan_end_queue = &newcl->netchan_start_queue;

    // clear server-side demo recording
    newcl->demo_recording = qfalse;
    newcl->demo_file = -1;
    newcl->demo_waiting = qfalse;
    newcl->demo_backoff = 1;
    newcl->demo_deltas = 0;

    // save the userinfo
    Q_strncpyz(newcl->userinfo, userinfo, sizeof(newcl->userinfo));

    // get the game a chance to reject this connection or modify the userinfo
    denied = VM_Call(gvm, GAME_CLIENT_CONNECT, clientNum, qtrue, qfalse); // firstTime = qtrue
    if (denied) {
        // we can't just use VM_ArgPtr, because that is only valid inside a VM_Call
        char *str = VM_ExplicitArgPtr(gvm, denied);
        NET_OutOfBandPrint(NS_SERVER, from, "print\n%s\n", str);
        Com_DPrintf ("Game rejected a connection: %s.\n", str);
        return;
    }

    SV_UserinfoChanged(newcl);

    // send the connect packet to the client
    NET_OutOfBandPrint(NS_SERVER, from, "connectResponse");

    Com_DPrintf("Going from CS_FREE to CS_CONNECTED for %s\n", newcl->name);

    newcl->state = CS_CONNECTED;
    newcl->nextSnapshotTime = svs.time;
    newcl->lastPacketTime = svs.time;
    newcl->lastConnectTime = svs.time;
    newcl->numcmds = 0;

    // when we receive the first packet from the client, we will
    // notice that it is from a different serverid and that the
    // gamestate message was not just sent, forcing a retransmit
    newcl->gamestateMessageNum = -1;

    // if this was the first client on the server, or the last client
    // the server can hold, send a heartbeat to the master.
    count = 0;
    for (i=0,cl=svs.clients ; i < sv_maxclients->integer ; i++,cl++) {
        if (svs.clients[i].state >= CS_CONNECTED) {
            count++;
        }
    }

    if (count == 1 || count == sv_maxclients->integer) {
        SV_Heartbeat_f();
    }

}


/*
=====================
SV_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_DropClient( client_t *drop, const char *reason ) {
	int		i;
	challenge_t	*challenge;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if (drop->netchan.remoteAddress.type != NA_BOT) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
			if ( NET_CompareAdr( drop->netchan.remoteAddress, challenge->adr ) ) {
				challenge->connected = qfalse;
				break;
			}
		}
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	SV_SendServerCommand( NULL, "print \"%s" S_COLOR_WHITE " %s\n\"", drop->name, reason );

	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}

	if (drop->demo_recording) {
		// stop the server side demo if we were recording this client
	   Cbuf_ExecuteText(EXEC_NOW, va("stopserverdemo %d", (int)(drop - svs.clients)));
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", reason);

	if ( drop->netchan.remoteAddress.type == NA_BOT ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
	drop->state = CS_ZOMBIE;		// become free in a few seconds

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}

	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}

#ifdef USE_AUTH
/*
=====================
SV_Auth_DropClient

Called when the player is totally leaving the server, either willingly
or unwillingly.  This is NOT called if the entire server is quiting
or crashing -- SV_FinalMessage() will handle that
=====================
*/
void SV_Auth_DropClient( client_t *drop, const char *reason, const char *message ) {

	int		i;
	challenge_t	*challenge;

	if ( drop->state == CS_ZOMBIE ) {
		return;		// already dropped
	}

	if (drop->netchan.remoteAddress.type != NA_BOT) {
		// see if we already have a challenge for this ip
		challenge = &svs.challenges[0];

		for (i = 0 ; i < MAX_CHALLENGES ; i++, challenge++) {
			if ( NET_CompareAdr( drop->netchan.remoteAddress, challenge->adr ) ) {
				challenge->connected = qfalse;
				break;
			}
		}
	}

	// Kill any download
	SV_CloseDownload( drop );

	// tell everyone why they got dropped
	if( strlen( reason ) > 0 ) SV_SendServerCommand( NULL, "print \"%s\n\"", reason );

	if (drop->download)	{
		FS_FCloseFile( drop->download );
		drop->download = 0;
	}

	if (drop->demo_recording) {
		// stop the server side demo if we were recording this client
	   Cbuf_ExecuteText(EXEC_NOW, va("stopserverdemo %d", (int)(drop - svs.clients)));
	}

	// call the prog function for removing a client
	// this will remove the body, among other things
	VM_Call( gvm, GAME_CLIENT_DISCONNECT, drop - svs.clients );

	// add the disconnect command
	SV_SendServerCommand( drop, "disconnect \"%s\"", message);

	if ( drop->netchan.remoteAddress.type == NA_BOT ) {
		SV_BotFreeClient( drop - svs.clients );
	}

	// nuke user info
	SV_SetUserinfo( drop - svs.clients, "" );

	Com_DPrintf( "Going to CS_ZOMBIE for %s\n", drop->name );
	drop->state = CS_ZOMBIE;		// become free in a few seconds

	// if this was the last client on the server, send a heartbeat
	// to the master so it is known the server is empty
	// send a heartbeat now so the master will get up to date info
	// if there is already a slot for this ip, reuse it
	for (i=0 ; i < sv_maxclients->integer ; i++ ) {
		if ( svs.clients[i].state >= CS_CONNECTED ) {
			break;
		}
	}

	if ( i == sv_maxclients->integer ) {
		SV_Heartbeat_f();
	}
}
#endif

/*
================
SV_SendClientGameState

Sends the first message from the server to a connected client.
This will be sent on the initial connection and upon each new map load.

It will be resent if the client acknowledges a later message but has
the wrong gamestate.
================
*/
void SV_SendClientGameState( client_t *client ) {
	int			start;
	entityState_t	*base, nullstate;
	msg_t		msg;
	byte		msgBuffer[MAX_MSGLEN];

	Com_DPrintf ("SV_SendClientGameState() for %s\n", client->name);
	Com_DPrintf( "Going from CS_CONNECTED to CS_PRIMED for %s\n", client->name );
	client->state = CS_PRIMED;
	client->pureAuthentic = 0;
	client->gotCP = qfalse;

	// when we receive the first packet from the client, we will
	// notice that it is from a different serverid and that the
	// gamestate message was not just sent, forcing a retransmit
	client->gamestateMessageNum = client->netchan.outgoingSequence;

	MSG_Init( &msg, msgBuffer, sizeof( msgBuffer ) );

	// NOTE, MRE: all server->client messages now acknowledge
	// let the client know which reliable clientCommands we have received
	MSG_WriteLong( &msg, client->lastClientCommand );

	// send any server commands waiting to be sent first.
	// we have to do this cause we send the client->reliableSequence
	// with a gamestate and it sets the clc.serverCommandSequence at
	// the client side
	SV_UpdateServerCommandsToClient( client, &msg );

	// send the gamestate
	MSG_WriteByte( &msg, svc_gamestate );
	MSG_WriteLong( &msg, client->reliableSequence );

	// write the configstrings
	for ( start = 0 ; start < MAX_CONFIGSTRINGS ; start++ ) {
		if (sv.configstrings[start][0]) {
			MSG_WriteByte( &msg, svc_configstring );
			MSG_WriteShort( &msg, start );
			MSG_WriteBigString( &msg, sv.configstrings[start] );
		}
	}

	// write the baselines
	Com_Memset( &nullstate, 0, sizeof( nullstate ) );
	for ( start = 0 ; start < MAX_GENTITIES; start++ ) {
		base = &sv.svEntities[start].baseline;
		if ( !base->number ) {
			continue;
		}
		MSG_WriteByte( &msg, svc_baseline );
		MSG_WriteDeltaEntity( &msg, &nullstate, base, qtrue );
	}

	MSG_WriteByte( &msg, svc_EOF );

	MSG_WriteLong( &msg, client - svs.clients);

	// write the checksum feed
	MSG_WriteLong( &msg, sv.checksumFeed);

	// deliver this to the client
	SV_SendMessageToClient( &msg, client );
}


/*
==================
SV_ClientEnterWorld
==================
*/
void SV_ClientEnterWorld( client_t *client, usercmd_t *cmd ) {
	int		clientNum;
	sharedEntity_t *ent;

	Com_DPrintf( "Going from CS_PRIMED to CS_ACTIVE for %s\n", client->name );
	client->state = CS_ACTIVE;

	// resend all configstrings using the cs commands since these are
	// no longer sent when the client is CS_PRIMED
	SV_UpdateConfigstrings( client );

	// set up the entity for the client
	clientNum = client - svs.clients;
	ent = SV_GentityNum( clientNum );
	ent->s.number = clientNum;
	ent->r.svFlags &= ~SVF_NOCLIENT;
	client->gentity = ent;

	client->deltaMessage = -1;
	client->nextSnapshotTime = svs.time;	// generate a snapshot immediately

	if(cmd) {
		memcpy(&client->lastUsercmd, cmd, sizeof(client->lastUsercmd));
	} else {
		memset(&client->lastUsercmd, '\0', sizeof(client->lastUsercmd));
	}

	// call the game begin function
	VM_Call( gvm, GAME_CLIENT_BEGIN, client - svs.clients );
}

/*
============================================================

CLIENT COMMAND EXECUTION

============================================================
*/

/*
==================
SV_CloseDownload

clear/free any download vars
==================
*/
static void SV_CloseDownload( client_t *cl ) {
	int i;

	// EOF
	if (cl->download) {
		FS_FCloseFile( cl->download );
	}
	cl->download = 0;
	*cl->downloadName = 0;

	// Free the temporary buffer space
	for (i = 0; i < MAX_DOWNLOAD_WINDOW; i++) {
		if (cl->downloadBlocks[i]) {
			Z_Free( cl->downloadBlocks[i] );
			cl->downloadBlocks[i] = NULL;
		}
	}

}

/*
==================
SV_StopDownload_f

Abort a download if in progress
==================
*/
void SV_StopDownload_f( client_t *cl ) {
	if (*cl->downloadName)
		Com_DPrintf( "clientDownload: %d : file \"%s\" aborted\n", (int) (cl - svs.clients), cl->downloadName );

	SV_CloseDownload( cl );
}

/*
==================
SV_DoneDownload_f

Downloads are finished
==================
*/
void SV_DoneDownload_f( client_t *cl ) {
	if ( cl->state == CS_ACTIVE )
		return;

	Com_DPrintf( "clientDownload: %s Done\n", cl->name);
	// resend the game state to update any clients that entered during the download
	SV_SendClientGameState(cl);
}

/*
==================
SV_NextDownload_f

The argument will be the last acknowledged block from the client, it should be
the same as cl->downloadClientBlock
==================
*/
void SV_NextDownload_f( client_t *cl )
{
	int block = atoi( Cmd_Argv(1) );

	if (block == cl->downloadClientBlock) {
		Com_DPrintf( "clientDownload: %d : client acknowledge of block %d\n", (int) (cl - svs.clients), block );

		// Find out if we are done.  A zero-length block indicates EOF
		if (cl->downloadBlockSize[cl->downloadClientBlock % MAX_DOWNLOAD_WINDOW] == 0) {
			Com_Printf( "clientDownload: %d : file \"%s\" completed\n", (int) (cl - svs.clients), cl->downloadName );
			SV_CloseDownload( cl );
			return;
		}

		cl->downloadSendTime = svs.time;
		cl->downloadClientBlock++;
		return;
	}
	// We aren't getting an acknowledge for the correct block, drop the client
	// FIXME: this is bad... the client will never parse the disconnect message
	//			because the cgame isn't loaded yet
	SV_DropClient( cl, "broken download" );
}

/*
==================
SV_BeginDownload_f
==================
*/
void SV_BeginDownload_f( client_t *cl ) {

	// Kill any existing download
	SV_CloseDownload( cl );

	// cl->downloadName is non-zero now, SV_WriteDownloadToClient will see this and open
	// the file itself
	Q_strncpyz( cl->downloadName, Cmd_Argv(1), sizeof(cl->downloadName) );
}

/*
==================
SV_WriteDownloadToClient

Check to see if the client wants a file, open it if needed and start pumping the client
Fill up msg with data
==================
*/
void SV_WriteDownloadToClient( client_t *cl , msg_t *msg )
{
	int curindex;
	int rate;
	int blockspersnap;
	int idPack = 0, missionPack = 0, unreferenced = 1;
	char errorMessage[1024];
	char pakbuf[MAX_QPATH], *pakptr;
	int numRefPaks;

	if (!*cl->downloadName)
		return;	// Nothing being downloaded

	if (!cl->download) {
		// Chop off filename extension.
		Com_sprintf(pakbuf, sizeof(pakbuf), "%s", cl->downloadName);
		pakptr = Q_strrchr(pakbuf, '.');

		if(pakptr)
		{
			*pakptr = '\0';

			// Check for pk3 filename extension
			if(!Q_stricmp(pakptr + 1, "pk3"))
			{
				const char *referencedPaks = FS_ReferencedPakNames();

				// Check whether the file appears in the list of referenced
				// paks to prevent downloading of arbitrary files.
				Cmd_TokenizeStringIgnoreQuotes(referencedPaks);
				numRefPaks = Cmd_Argc();

				for(curindex = 0; curindex < numRefPaks; curindex++)
				{
					if(!FS_FilenameCompare(Cmd_Argv(curindex), pakbuf))
					{
						unreferenced = 0;

						// now that we know the file is referenced,
						// check whether it's legal to download it.
						missionPack = FS_idPak(pakbuf, "missionpack");
						idPack = missionPack || FS_idPak(pakbuf, BASEGAME);

						break;
					}
				}
			}
		}

		// We open the file here
		if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
			(sv_allowDownload->integer & DLF_NO_UDP) ||
			idPack || unreferenced ||
			( cl->downloadSize = FS_SV_FOpenFileRead( cl->downloadName, &cl->download ) ) <= 0 ) {
			// cannot auto-download file
			if(unreferenced)
			{
				Com_Printf("clientDownload: %d : \"%s\" is not referenced and cannot be downloaded.\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" is not referenced and cannot be downloaded.", cl->downloadName);
			}
			else if (idPack) {
				Com_Printf("clientDownload: %d : \"%s\" cannot download id pk3 files\n", (int) (cl - svs.clients), cl->downloadName);
				if (missionPack) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload Team Arena file \"%s\"\n"
									"The Team Arena mission pack can be found in your local game store.", cl->downloadName);
				}
				else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Cannot autodownload id pk3 file \"%s\"", cl->downloadName);
				}
			}
			else if ( !(sv_allowDownload->integer & DLF_ENABLE) ||
				(sv_allowDownload->integer & DLF_NO_UDP) ) {

				Com_Printf("clientDownload: %d : \"%s\" download disabled", (int) (cl - svs.clients), cl->downloadName);
				if (sv_pure->integer) {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
										"You will need to get this file elsewhere before you "
										"can connect to this pure server.\n", cl->downloadName);
				} else {
					Com_sprintf(errorMessage, sizeof(errorMessage), "Could not download \"%s\" because autodownloading is disabled on the server.\n\n"
					"The server you are connecting to is not a pure server, "
					"set autodownload to No in your settings and you might be "
					"able to join the game anyway.\n", cl->downloadName);
				}
			} else {
		// NOTE TTimo this is NOT supposed to happen unless bug in our filesystem scheme?
		//   if the pk3 is referenced, it must have been found somewhere in the filesystem
				Com_Printf("clientDownload: %d : \"%s\" file not found on server\n", (int) (cl - svs.clients), cl->downloadName);
				Com_sprintf(errorMessage, sizeof(errorMessage), "File \"%s\" not found on server for autodownloading.\n", cl->downloadName);
			}
			MSG_WriteByte( msg, svc_download );
			MSG_WriteShort( msg, 0 ); // client is expecting block zero
			MSG_WriteLong( msg, -1 ); // illegal file size
			MSG_WriteString( msg, errorMessage );

			*cl->downloadName = 0;
			return;
		}

		Com_Printf( "clientDownload: %d : beginning \"%s\"\n", (int) (cl - svs.clients), cl->downloadName );

		// Init
		cl->downloadCurrentBlock = cl->downloadClientBlock = cl->downloadXmitBlock = 0;
		cl->downloadCount = 0;
		cl->downloadEOF = qfalse;
	}

	// Perform any reads that we need to
	while (cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW &&
		cl->downloadSize != cl->downloadCount) {

		curindex = (cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW);

		if (!cl->downloadBlocks[curindex])
			cl->downloadBlocks[curindex] = Z_Malloc( MAX_DOWNLOAD_BLKSIZE );

		cl->downloadBlockSize[curindex] = FS_Read( cl->downloadBlocks[curindex], MAX_DOWNLOAD_BLKSIZE, cl->download );

		if (cl->downloadBlockSize[curindex] < 0) {
			// EOF right now
			cl->downloadCount = cl->downloadSize;
			break;
		}

		cl->downloadCount += cl->downloadBlockSize[curindex];

		// Load in next block
		cl->downloadCurrentBlock++;
	}

	// Check to see if we have eof condition and add the EOF block
	if (cl->downloadCount == cl->downloadSize &&
		!cl->downloadEOF &&
		cl->downloadCurrentBlock - cl->downloadClientBlock < MAX_DOWNLOAD_WINDOW) {

		cl->downloadBlockSize[cl->downloadCurrentBlock % MAX_DOWNLOAD_WINDOW] = 0;
		cl->downloadCurrentBlock++;

		cl->downloadEOF = qtrue;  // We have added the EOF block
	}

	// Loop up to window size times based on how many blocks we can fit in the
	// client snapMsec and rate

	// based on the rate, how many bytes can we fit in the snapMsec time of the client
	// normal rate / snapshotMsec calculation
	rate = cl->rate;
	if ( sv_maxRate->integer ) {
		if ( sv_maxRate->integer < 1000 ) {
			Cvar_Set( "sv_MaxRate", "1000" );
		}
		if ( sv_maxRate->integer < rate ) {
			rate = sv_maxRate->integer;
		}
	}
	if ( sv_minRate->integer ) {
		if ( sv_minRate->integer < 1000 )
			Cvar_Set( "sv_minRate", "1000" );
		if ( sv_minRate->integer > rate )
			rate = sv_minRate->integer;
	}

	if (!rate) {
		blockspersnap = 1;
	} else {
		blockspersnap = ( (rate * cl->snapshotMsec) / 1000 + MAX_DOWNLOAD_BLKSIZE ) /
			MAX_DOWNLOAD_BLKSIZE;
	}

	if (blockspersnap < 0)
		blockspersnap = 1;

	while (blockspersnap--) {

		// Write out the next section of the file, if we have already reached our window,
		// automatically start retransmitting

		if (cl->downloadClientBlock == cl->downloadCurrentBlock)
			return; // Nothing to transmit

		if (cl->downloadXmitBlock == cl->downloadCurrentBlock) {
			// We have transmitted the complete window, should we start resending?

			//FIXME:  This uses a hardcoded one second timeout for lost blocks
			//the timeout should be based on client rate somehow
			if (svs.time - cl->downloadSendTime > 1000)
				cl->downloadXmitBlock = cl->downloadClientBlock;
			else
				return;
		}

		// Send current block
		curindex = (cl->downloadXmitBlock % MAX_DOWNLOAD_WINDOW);

		MSG_WriteByte( msg, svc_download );
		MSG_WriteShort( msg, cl->downloadXmitBlock );

		// block zero is special, contains file size
		if ( cl->downloadXmitBlock == 0 )
			MSG_WriteLong( msg, cl->downloadSize );

		MSG_WriteShort( msg, cl->downloadBlockSize[curindex] );

		// Write the block
		if ( cl->downloadBlockSize[curindex] ) {
			MSG_WriteData( msg, cl->downloadBlocks[curindex], cl->downloadBlockSize[curindex] );
		}

		Com_DPrintf( "clientDownload: %d : writing block %d\n", (int) (cl - svs.clients), cl->downloadXmitBlock );

		// Move on to the next block
		// It will get sent with next snap shot.  The rate will keep us in line.
		cl->downloadXmitBlock++;

		cl->downloadSendTime = svs.time;
	}
}

/*
=================
SV_Disconnect_f

The client is going to disconnect, so remove the connection immediately  FIXME: move to game?
=================
*/
static void SV_Disconnect_f( client_t *cl ) {
	SV_DropClient( cl, "disconnected" );
}

/*
=================
SV_VerifyPaks_f

If we are pure, disconnect the client if they do no meet the following conditions:

1. the first two checksums match our view of cgame and ui
2. there are no any additional checksums that we do not have

This routine would be a bit simpler with a goto but i abstained

=================
*/
static void SV_VerifyPaks_f( client_t *cl ) {
	int nChkSum1, nChkSum2, nClientPaks, nServerPaks, i, j, nCurArg;
	int nClientChkSum[1024];
	int nServerChkSum[1024];
	const char *pPaks, *pArg;
	qboolean bGood = qtrue;

	// if we are pure, we "expect" the client to load certain things from
	// certain pk3 files, namely we want the client to have loaded the
	// ui and cgame that we think should be loaded based on the pure setting
	//
	if ( sv_pure->integer != 0 ) {

		bGood = qtrue;
		nChkSum1 = nChkSum2 = 0;
		// we run the game, so determine which cgame and ui the client "should" be running
		bGood = (FS_FileIsInPAK("vm/cgame.qvm", &nChkSum1) == 1);
		if (bGood)
			bGood = (FS_FileIsInPAK("vm/ui.qvm", &nChkSum2) == 1);

		nClientPaks = Cmd_Argc();

		// start at arg 2 ( skip serverId cl_paks )
		nCurArg = 1;

		pArg = Cmd_Argv(nCurArg++);
		if(!pArg) {
			bGood = qfalse;
		}
		else
		{
			// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=475
			// we may get incoming cp sequences from a previous checksumFeed, which we need to ignore
			// since serverId is a frame count, it always goes up
			if (atoi(pArg) < sv.checksumFeedServerId)
			{
				Com_DPrintf("ignoring outdated cp command from client %s\n", cl->name);
				return;
			}
		}

		// we basically use this while loop to avoid using 'goto' :)
		while (bGood) {

			// must be at least 6: "cl_paks cgame ui @ firstref ... numChecksums"
			// numChecksums is encoded
			if (nClientPaks < 6) {
				bGood = qfalse;
				break;
			}
			// verify first to be the cgame checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum1 ) {
				bGood = qfalse;
				break;
			}
			// verify the second to be the ui checksum
			pArg = Cmd_Argv(nCurArg++);
			if (!pArg || *pArg == '@' || atoi(pArg) != nChkSum2 ) {
				bGood = qfalse;
				break;
			}
			// should be sitting at the delimeter now
			pArg = Cmd_Argv(nCurArg++);
			if (*pArg != '@') {
				bGood = qfalse;
				break;
			}
			// store checksums since tokenization is not re-entrant
			for (i = 0; nCurArg < nClientPaks; i++) {
				nClientChkSum[i] = atoi(Cmd_Argv(nCurArg++));
			}

			// store number to compare against (minus one cause the last is the number of checksums)
			nClientPaks = i - 1;

			// make sure none of the client check sums are the same
			// so the client can't send 5 the same checksums
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nClientPaks; j++) {
					if (i == j)
						continue;
					if (nClientChkSum[i] == nClientChkSum[j]) {
						bGood = qfalse;
						break;
					}
				}
				if (bGood == qfalse)
					break;
			}
			if (bGood == qfalse)
				break;

			// get the pure checksums of the pk3 files loaded by the server
			pPaks = FS_LoadedPakPureChecksums();
			Cmd_TokenizeString( pPaks );
			nServerPaks = Cmd_Argc();
			if (nServerPaks > 1024)
				nServerPaks = 1024;

			for (i = 0; i < nServerPaks; i++) {
				nServerChkSum[i] = atoi(Cmd_Argv(i));
			}

			// check if the client has provided any pure checksums of pk3 files not loaded by the server
			for (i = 0; i < nClientPaks; i++) {
				for (j = 0; j < nServerPaks; j++) {
					if (nClientChkSum[i] == nServerChkSum[j]) {
						break;
					}
				}
				if (j >= nServerPaks) {
					bGood = qfalse;
					break;
				}
			}
			if ( bGood == qfalse ) {
				break;
			}

			// check if the number of checksums was correct
			nChkSum1 = sv.checksumFeed;
			for (i = 0; i < nClientPaks; i++) {
				nChkSum1 ^= nClientChkSum[i];
			}
			nChkSum1 ^= nClientPaks;
			if (nChkSum1 != nClientChkSum[nClientPaks]) {
				bGood = qfalse;
				break;
			}

			// break out
			break;
		}

		cl->gotCP = qtrue;

		if (bGood) {
			cl->pureAuthentic = 1;
		}
		else {
			cl->pureAuthentic = 0;
			cl->nextSnapshotTime = -1;
			cl->state = CS_ACTIVE;
			SV_SendClientSnapshot( cl );
			SV_DropClient( cl, "Unpure client detected. Invalid .PK3 files referenced!" );
		}
	}
}

/*
=================
SV_ResetPureClient_f
=================
*/
static void SV_ResetPureClient_f( client_t *cl ) {
	cl->pureAuthentic = 0;
	cl->gotCP = qfalse;
}

static char *SV_CleanName(char *name) {
	static char cleaned[MAX_NAME_LENGTH];
	int i, j = 0;

	for (i = 0; i < strlen(name); i++) {
		if (name[i] >= 32 && name[i] <= 126)
			cleaned[j++] = name[i];
	}

	cleaned[j] = 0;
	return cleaned;
}

/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl ) {
	char	*val;
	char	*ip;
	int		i;
	int	len;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );
	sprintf(cl->colourName, "%s^7", SV_CleanName(cl->name));

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && com_dedicated->integer != 2 && sv_lanForceRate->integer == 1) {
		cl->rate = 99999;	// lans should not rate limit
	} else {
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val)) {
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000) {
				cl->rate = 1000;
			} else if (cl->rate > 90000) {
				cl->rate = 90000;
			}
		} else {
			cl->rate = 3000;
		}
	}
	val = Info_ValueForKey (cl->userinfo, "handicap");
	if (strlen(val)) {
		i = atoi(val);
		if (i<=0 || i>100 || strlen(val) > 4) {
			Info_SetValueForKey( cl->userinfo, "handicap", "100" );
		}
	}

	// snaps command
	val = Info_ValueForKey (cl->userinfo, "snaps");
	if (strlen(val)) {
		i = atoi(val);
		if ( i < 1 ) {
			i = 1;
		} else if ( i > sv_fps->integer ) {
			i = sv_fps->integer;
		}
		cl->snapshotMsec = 1000/i;
	} else {
		cl->snapshotMsec = 50;
	}

	// TTimo
	// maintain the IP information
	// the banning code relies on this being consistently present
	if( NET_IsLocalAddress(cl->netchan.remoteAddress) )
		ip = "localhost";
	else
		ip = (char*)NET_AdrToString( cl->netchan.remoteAddress );

	val = Info_ValueForKey( cl->userinfo, "ip" );
	if( val[0] )
		len = strlen( ip ) - strlen( val ) + strlen( cl->userinfo );
	else
		len = strlen( ip ) + 4 + strlen( cl->userinfo );

	if( len >= MAX_INFO_STRING )
		SV_DropClient( cl, "userinfo string length exceeded" );
	else
		Info_SetValueForKey( cl->userinfo, "ip", ip );
}


/*
==================
SV_UpdateUserinfo_f
==================
*/
void SV_UpdateUserinfo_f( client_t *cl ) {
    gclient_t *gl;
	if ( (sv_floodProtect->integer) && (cl->state >= CS_ACTIVE) && (svs.time < cl->nextReliableUserTime) ) {
		Q_strncpyz( cl->userinfobuffer, Cmd_Argv(1), sizeof(cl->userinfobuffer) );
		SV_SendServerCommand(cl, "print \"^7Command ^1delayed^7 due to sv_floodprotect.\"");
		return;
	}
        gl = (gclient_t *)SV_GameClientNum(cl - svs.clients);
	cl->userinfobuffer[0]=0;
	cl->nextReliableUserTime = svs.time + 5000;

	Q_strncpyz( cl->userinfo, Cmd_Argv(1), sizeof(cl->userinfo) );


	SV_UserinfoChanged( cl );
	// call prog code to allow overrides
	VM_Call( gvm, GAME_CLIENT_USERINFO_CHANGED, cl - svs.clients );
        if (sv_colourNames->integer) 
            Q_strncpyz(gl->pers.netname, cl->colourName, MAX_NETNAME);
}

typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

	{NULL, NULL}
};

/*===============
SV_Maplist_f
==================
*/
void SV_Maplist_f(client_t *cl) {
	int numMaps, j;
	char **maplist;

	maplist = FS_ListFiles("maps", ".bsp", &numMaps);
	SV_SendServerCommand(cl, "print \"Maps:\"");
	for (j = 0; j < numMaps; j++) {
		maplist[j][strlen(maplist[j])-4] = 0;
		SV_SendServerCommand(cl, "print \"%s\"", maplist[j]);
	}
}

/*
==================
SV_FriendlyFire_f
==================
*/
void SV_FriendlyFire_f(client_t *cl) {
	if (!Cvar_VariableIntegerValue("g_friendlyfire")) {
		SV_SendServerCommand(cl, "chat \"^7Friendly fire is ^1OFF^7. Go crazy.\"");
	} else {
		SV_SendServerCommand(cl, "chat \"^7Friendly fire is ^2ON^7. DON'T SHOOT YOUR TEAMMATES.\"");
	}
}

/*
==================
SV_Mapcycle_f
==================
*/
void SV_Mapcycle_f(client_t *cl) {
	int s;
	fileHandle_t h;
	char *cycleFile;
	char *allMaps, *singleMap;

	cycleFile = Cvar_VariableString("g_mapcycle");

	s = FS_FOpenFileRead(cycleFile, &h, qtrue);
	if (s) {
		allMaps = Z_Malloc(s + 1);
		if (FS_Read(allMaps, s, h)) {
			SV_SendServerCommand(cl, "print \"Mapcycle:\"");
			singleMap = strtok(allMaps, "\r\n");
			while (singleMap) {
				if (!Q_stricmp(sv_mapname->string, singleMap)) {
					SV_SendServerCommand(cl, "print \"^2%s <- We are here\"", singleMap);
				} else {
					SV_SendServerCommand(cl, "print \"%s\"", singleMap);
				}
				singleMap = strtok(NULL, "\r\n");
			}
		}
	} else {
		SV_SendServerCommand(cl, "print \"Mapcycle retrieval failed.\"");
	}
}


/*
==================
SV_Say_f
An engine-side implementation of the `say` command for more customization

Currently missing:
	+ (SUB) indicator
	+ Chat variables
==================
*/
void SV_Say_f(client_t *cl) {
	/*
	-- to client:
	cchat "0" "6th|Clear^3: ^3asd"

	-- log:
	say: 0 6th|Clear: asd
	*/
	int team;
	char *message;
	playerState_t *ps;

	ps = SV_GameClientNum(cl - svs.clients);

	team = ps->persistant[PERS_TEAM];
	message = CopyString(Cmd_Args());

	/*
	If this ever gets finished, there should be a cvar for allowing coloured text
	For now, `csay` will leave coloured text

	Q_CleanStr(message);
	*/

	SV_SendServerCommand(NULL, "cchat \"%i\" \"%s%s%s^3: ^3%s\"",
		team,
		ps->pm_type == PM_DEAD ? "(DEAD) " : "",
		team == TEAM_SPECTATOR ? "(SPEC) " : "",
		cl->name, message);

	SV_LogPrintf("say: %i %s: %s\n", team, cl->name, message);
}

void SV_Tell_f(client_t *cl) {
	/*
	-- to client
	whatever I want

	-- log:
	saytell: from to fromname: message
	*/

	if (Cmd_Argc() < 3) {
		SV_SendServerCommand(cl, "print \"Usage: tell <player> <message>\"");
		return;
	}

	client_t *from, *to;
	char *cmd, *msg;

	from = cl;
	to = SV_BetterGetPlayerByHandle(Cmd_Argv(1));

	if (!to) {
		SV_SendServerCommand(cl, "print \"Player '%s' was not found\"", Cmd_Argv(1));
		return;
	}

	msg = Cmd_ArgsFrom(2);

	cmd = va("^8[pm] <- ^7%s: ^3%s", from->name, msg);
	SV_SendServerCommand(to, "chat \"%s\"", cmd);

	cmd = va("^8[pm] -> ^7%s: ^3%s", to->name, msg);
	SV_SendServerCommand(from, "chat \"%s\"", cmd);

	SV_LogPrintf("saytell: %i %i %s: %s\n", from - svs.clients, to - svs.clients, from->name, msg);
}



typedef struct {
	char	*name;
	void	(*func)( client_t *cl );
} ucmd_t;

static ucmd_t ucmds[] = {
	{"userinfo", SV_UpdateUserinfo_f},
	{"disconnect", SV_Disconnect_f},
	{"cp", SV_VerifyPaks_f},
	{"vdr", SV_ResetPureClient_f},
	{"download", SV_BeginDownload_f},
	{"nextdl", SV_NextDownload_f},
	{"stopdl", SV_StopDownload_f},
	{"donedl", SV_DoneDownload_f},

	{"maplist", SV_Maplist_f},
	{"ff?", SV_FriendlyFire_f},
	{"mapcycle", SV_Mapcycle_f},

	{"csay", SV_Say_f},
	{"tell", SV_Tell_f},

	{NULL, NULL}
};

/*
==================
SV_ExecuteClientCommand

Also called by bot code
==================
*/
void SV_ExecuteClientCommand( client_t *cl, const char *s, qboolean clientOK ) {

	ucmd_t		*u;
	int			argsFromOneMaxlen;
	int			charCount;
	int			dollarCount;
	int			i;
	char		*arg;
	qboolean 	bProcessed = qfalse;
	qboolean 	exploitDetected = qfalse;

	Cmd_TokenizeString( s );

	// see if it is a server level command
	for (u=ucmds ; u->name ; u++) {
		if (!strcmp (Cmd_Argv(0), u->name) ) {
			u->func( cl );
			bProcessed = qtrue;
			break;
		}
	}

	if (clientOK) {

		// pass unknown strings to the game
		if ((!u->name) && (sv.state == SS_GAME) && (cl->state == CS_ACTIVE)) {
			Cmd_Args_Sanitize();

			argsFromOneMaxlen = -1;
			if (Q_stricmp("say", Cmd_Argv(0)) == 0 || Q_stricmp("say_team", Cmd_Argv(0)) == 0) {
				argsFromOneMaxlen = MAX_SAY_STRLEN;
				
			 
					p = Cmd_Argv(1);
				while (*p == ' ') p++;
				if (strncmp("!pm", (char *)p, 3) == 0)
				{
					SV_SendServerCommand(cl, "chat \"^9[pm] ^7%s: ^3%s\"", cl->colourName, Cmd_Args());
							   SV_LogPrintf("say: %i %s: %s\n", ps->clientNum, cl->name, CopyString(Cmd_Args()));
							   return;
				}
				if (((*p == '!') || (*p == '@') || (*p == '&') || (*p == '/')) && sv_hideCmds->integer)
				{
							   if(sv_hideCmds->integer == 1)
								   SV_SendServerCommand(cl, "chat \"^9[pm] ^7%s: ^3%s\"", cl->colourName, Cmd_Args());
							   SV_LogPrintf("say: %i %s: %s\n", ps->clientNum, cl->name, CopyString(Cmd_Args()));
							   return;
				}

			} else if (Q_stricmp("tell", Cmd_Argv(0)) == 0) {
				// A command will look like "tell 12 hi" or "tell foo hi".  The "12"
				// and "foo" in the examples will be counted towards MAX_SAY_STRLEN,
				// plus the space.
				argsFromOneMaxlen = MAX_SAY_STRLEN;
			} else if (Q_stricmp("ut_radio", Cmd_Argv(0)) == 0) {
				// We add 4 to this value because in a command such as
				// "ut_radio 1 1 affirmative", the args at indices 1 and 2 each
				// have length 1 and there is a space after them.
				argsFromOneMaxlen = MAX_RADIO_STRLEN + 4;
			} else if (!sv_allowSuicide->integer && !Q_stricmp("kill", Cmd_Argv(0))) {
				SV_SendServerCommand(cl, "print \"This server doesn't allow suiciding.\n\"");
				return;
			} else if (!sv_allowItemdrop->integer && !Q_stricmp("ut_itemdrop", Cmd_Argv(0))) {
				SV_SendServerCommand(cl, "print \"This server doesn't allow dropping items.\n\"");
				return;
			} else if (!sv_allowWeapdrop->integer && !Q_stricmp("ut_weapdrop", Cmd_Argv(0))) {
				SV_SendServerCommand(cl, "print \"This server doesn't allow dropping weapons.\n\"");
				return;
			} else if (!sv_allowTell->integer && !Q_stricmp("tell", Cmd_Argv(0))) {
				SV_SendServerCommand(cl, "print \"This server doesn't allow telling.\n\"");
				return;
			} else if (!Q_stricmp("callvote", Cmd_Argv(0))) {
				if (!sv_allowVote->integer) {
					SV_SendServerCommand(cl, "print \"^8You are not allowed to vote, sorry.\n\"");
					return;
				}
			}
			if (argsFromOneMaxlen >= 0) {
				charCount = 0;
				dollarCount = 0;
				for (i = Cmd_Argc() - 1; i >= 1; i--) {
					arg = Cmd_Argv(i);
					while (*arg) {
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
						if (*arg == '$') {
							if (++dollarCount > MAX_DOLLAR_VARS) {
								exploitDetected = qtrue; break;
							}
							charCount += STRLEN_INCREMENT_PER_DOLLAR_VAR;
							if (charCount > argsFromOneMaxlen) {
								exploitDetected = qtrue; break;
							}
						}
						arg++;
					}
					if (exploitDetected) { break; }
					if (i != 1) { // Cmd_ArgsFrom() will add space
						if (++charCount > argsFromOneMaxlen) {
							exploitDetected = qtrue; break;
						}
					}
				}
			}
			if (exploitDetected) {
				Com_Printf("Buffer overflow exploit radio/say, possible attempt from %s\n", NET_AdrToString(cl->netchan.remoteAddress));
				SV_SendServerCommand(cl, "print \"Chat dropped due to message length constraints.\n\"");
				return;
			}

			VM_Call( gvm, GAME_CLIENT_COMMAND, cl - svs.clients );
		}
	}
	else if (!bProcessed) {
		Com_DPrintf( "client text ignored for %s: %s\n", cl->name, Cmd_Argv(0) );
	}
}

/*
===============
SV_ClientCommand
===============
*/
static qboolean SV_ClientCommand( client_t *cl, msg_t *msg ) {
	int		seq;
	const char	*s;
	qboolean clientOk = qtrue;

	seq = MSG_ReadLong( msg );
	s = MSG_ReadString( msg );

	// see if we have already executed it
	if ( cl->lastClientCommand >= seq ) {
		return qtrue;
	}

	Com_DPrintf( "clientCommand: %s : %i : %s\n", cl->name, seq, s );

	// drop the connection if we have somehow lost commands
	if ( seq > cl->lastClientCommand + 1 ) {
		Com_Printf( "Client %s lost %i clientCommands\n", cl->name,
			seq - cl->lastClientCommand + 1 );
		SV_DropClient( cl, "Lost reliable commands" );
		return qfalse;
	}

	// malicious users may try using too many string commands
	// to lag other players.  If we decide that we want to stall
	// the command, we will stop processing the rest of the packet,
	// including the usercmd.  This causes flooders to lag themselves
	// but not other people
	// We don't do this when the client hasn't been active yet since its
	// normal to spam a lot of commands when downloading
	if ( !com_cl_running->integer &&
		cl->state >= CS_ACTIVE &&
		sv_floodProtect->integer ) {
		if (svs.time < cl->nextReliableTime ) {
			if (++(cl->numcmds) > sv_floodProtect->integer ) {
				// ignore any other text messages from this client but let them keep playing
				// TTimo - moved the ignored verbose to the actual processing in SV_ExecuteClientCommand, only printing if the core doesn't intercept
				clientOk = qfalse;
			}
		} else {
			cl->numcmds = 1;
		}
	}

	// don't allow another command for one second
	cl->nextReliableTime = svs.time + 1000;

	SV_ExecuteClientCommand( cl, s, clientOk );

	cl->lastClientCommand = seq;
	Com_sprintf(cl->lastClientCommandString, sizeof(cl->lastClientCommandString), "%s", s);

	return qtrue;		// continue procesing
}


//==================================================================================


/*
==================
SV_ClientThink

Also called by bot code
==================
*/
void SV_ClientThink (client_t *cl, usercmd_t *cmd) {
	int i, j;
	playerState_t *ps;

	cl->lastUsercmd = *cmd;

	if ( cl->state != CS_ACTIVE ) {
		return;		// may have been kicked during the last usercmd
	}

	ps = SV_GameClientNum(cl - svs.clients);

	#ifdef USE_SERVER_EXTRAS
	if (sv_infiniteAmmo->integer) {
		for (i = 0; i < MAX_POWERUPS; i++) {
			cl->powerups[i] = ps->powerups[i];
		}
	}
	#endif

	VM_Call( gvm, GAME_CLIENT_THINK, cl - svs.clients );

	#ifdef USE_SERVER_EXTRAS
	if (sv_infiniteAmmo->integer) {
		if (cl->lastEventSequence < ps->eventSequence - MAX_PS_EVENTS) {
			cl->lastEventSequence = ps->eventSequence - MAX_PS_EVENTS;
		}

		for (j = cl->lastEventSequence; j < ps->eventSequence; j++) {
			if (ps->events[j & (MAX_PS_EVENTS - 1)] == 31 || ps->weaponstate == WEAPON_FIRING) {
				for (i = 0; i < MAX_POWERUPS; i++) {
					ps->powerups[i] = cl->powerups[i];
				}
			}
		}

		cl->lastEventSequence = ps->eventSequence;
	}
	#endif
}

/*
==================
SV_UserMove

The message usually contains all the movement commands
that were in the last three packets, so that the information
in dropped packets can be recovered.

On very fast clients, there may be multiple usercmd packed into
each of the backup packets.
==================
*/
static void SV_UserMove( client_t *cl, msg_t *msg, qboolean delta ) {
	int			i, key;
	int			cmdCount;
	usercmd_t	nullcmd;
	usercmd_t	cmds[MAX_PACKET_USERCMDS];
	usercmd_t	*cmd, *oldcmd;

	if ( delta ) {
		cl->deltaMessage = cl->messageAcknowledge;
	} else {
		cl->deltaMessage = -1;
	}

	cmdCount = MSG_ReadByte( msg );

	if ( cmdCount < 1 ) {
		Com_Printf( "cmdCount < 1\n" );
		return;
	}

	if ( cmdCount > MAX_PACKET_USERCMDS ) {
		Com_Printf( "cmdCount > MAX_PACKET_USERCMDS\n" );
		return;
	}

	// use the checksum feed in the key
	key = sv.checksumFeed;
	// also use the message acknowledge
	key ^= cl->messageAcknowledge;
	// also use the last acknowledged server command in the key
	key ^= Com_HashKey(cl->reliableCommands[ cl->reliableAcknowledge & (MAX_RELIABLE_COMMANDS-1) ], 32);

	Com_Memset( &nullcmd, 0, sizeof(nullcmd) );
	oldcmd = &nullcmd;
	for ( i = 0 ; i < cmdCount ; i++ ) {
		cmd = &cmds[i];
		MSG_ReadDeltaUsercmdKey( msg, key, oldcmd, cmd );
		oldcmd = cmd;
	}

	// save time for ping calculation
	cl->frames[ cl->messageAcknowledge & PACKET_MASK ].messageAcked = svs.time;

	// TTimo
	// catch the no-cp-yet situation before SV_ClientEnterWorld
	// if CS_ACTIVE, then it's time to trigger a new gamestate emission
	// if not, then we are getting remaining parasite usermove commands, which we should ignore
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0 && !cl->gotCP) {
		if (cl->state == CS_ACTIVE)
		{
			// we didn't get a cp yet, don't assume anything and just send the gamestate all over again
			Com_DPrintf( "%s: didn't get cp command, resending gamestate\n", cl->name);
			SV_SendClientGameState( cl );
		}
		return;
	}

	// if this is the first usercmd we have received
	// this gamestate, put the client into the world
	if ( cl->state == CS_PRIMED ) {
		SV_ClientEnterWorld( cl, &cmds[0] );
		// the moves can be processed normaly
	}

	// a bad cp command was sent, drop the client
	if (sv_pure->integer != 0 && cl->pureAuthentic == 0) {
		SV_DropClient( cl, "Cannot validate pure client!");
		return;
	}

	if ( cl->state != CS_ACTIVE ) {
		cl->deltaMessage = -1;
		return;
	}

	// usually, the first couple commands will be duplicates
	// of ones we have previously received, but the servertimes
	// in the commands will cause them to be immediately discarded
	for ( i =  0 ; i < cmdCount ; i++ ) {
		// if this is a cmd from before a map_restart ignore it
		if ( cmds[i].serverTime > cmds[cmdCount-1].serverTime ) {
			continue;
		}
		// extremely lagged or cmd from before a map_restart
		//if ( cmds[i].serverTime > svs.time + 3000 ) {
		//	continue;
		//}
		// don't execute if this is an old cmd which is already executed
		// these old cmds are included when cl_packetdup > 0
		if ( cmds[i].serverTime <= cl->lastUsercmd.serverTime ) {
			continue;
		}
		SV_ClientThink (cl, &cmds[ i ]);
	}
}


/*
===========================================================================

USER CMD EXECUTION

===========================================================================
*/

/*
===================
SV_ExecuteClientMessage

Parse a client packet
===================
*/
void SV_ExecuteClientMessage( client_t *cl, msg_t *msg ) {
	int			c;
	int			serverId;

	MSG_Bitstream(msg);

	serverId = MSG_ReadLong( msg );
	cl->messageAcknowledge = MSG_ReadLong( msg );

	if (cl->messageAcknowledge < 0) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		return;
	}

	cl->reliableAcknowledge = MSG_ReadLong( msg );

	// NOTE: when the client message is fux0red the acknowledgement numbers
	// can be out of range, this could cause the server to send thousands of server
	// commands which the server thinks are not yet acknowledged in SV_UpdateServerCommandsToClient
	if (cl->reliableAcknowledge < cl->reliableSequence - MAX_RELIABLE_COMMANDS) {
		// usually only hackers create messages like this
		// it is more annoying for them to let them hanging
#ifndef NDEBUG
		SV_DropClient( cl, "DEBUG: illegible client message" );
#endif
		cl->reliableAcknowledge = cl->reliableSequence;
		return;
	}
	// if this is a usercmd from a previous gamestate,
	// ignore it or retransmit the current gamestate
	//
	// if the client was downloading, let it stay at whatever serverId and
	// gamestate it was at.  This allows it to keep downloading even when
	// the gamestate changes.  After the download is finished, we'll
	// notice and send it a new game state
	//
	// https://zerowing.idsoftware.com/bugzilla/show_bug.cgi?id=536
	// don't drop as long as previous command was a nextdl, after a dl is done, downloadName is set back to ""
	// but we still need to read the next message to move to next download or send gamestate
	// I don't like this hack though, it must have been working fine at some point, suspecting the fix is somewhere else
	if ( serverId != sv.serverId && !*cl->downloadName && !strstr(cl->lastClientCommandString, "nextdl") ) {
		if ( serverId >= sv.restartedServerId && serverId < sv.serverId ) { // TTimo - use a comparison here to catch multiple map_restart
			// they just haven't caught the map_restart yet
			Com_DPrintf("%s : ignoring pre map_restart / outdated client message\n", cl->name);
			return;
		}
		// if we can tell that the client has dropped the last
		// gamestate we sent them, resend it
		if ( cl->messageAcknowledge > cl->gamestateMessageNum ) {
			Com_DPrintf( "%s : dropped gamestate, resending\n", cl->name );
			SV_SendClientGameState( cl );
		}
		return;
	}

	// this client has acknowledged the new gamestate so it's
	// safe to start sending it the real time again
	if( cl->oldServerTime && serverId == sv.serverId ){
		Com_DPrintf( "%s acknowledged gamestate\n", cl->name );
		cl->oldServerTime = 0;
	}

	// read optional clientCommand strings
	do {
		c = MSG_ReadByte( msg );
		if ( c == clc_EOF ) {
			break;
		}
		if ( c != clc_clientCommand ) {
			break;
		}
		if ( !SV_ClientCommand( cl, msg ) ) {
			return;	// we couldn't execute it because of the flood protection
		}
		if (cl->state == CS_ZOMBIE) {
			return;	// disconnect command
		}
	} while ( 1 );

	// read the usercmd_t
	if ( c == clc_move ) {
		SV_UserMove( cl, msg, qtrue );
	} else if ( c == clc_moveNoDelta ) {
		SV_UserMove( cl, msg, qfalse );
	} else if ( c != clc_EOF ) {
		Com_Printf( "WARNING: bad command byte for client %i\n", (int) (cl - svs.clients) );
	}
//	if ( msg->readcount != msg->cursize ) {
//		Com_Printf( "WARNING: Junk at end of packet for client %i\n", cl - svs.clients );
//	}
}

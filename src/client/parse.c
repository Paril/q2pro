/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// cl_parse.c  -- parse a message received from the server

#include "client.h"
#include "q2proto/q2proto.h"
#include "shared/m_flash.h"
#include "shared/game3_shared.h"
#include "common/loc.h"

/*
=====================================================================

  DELTA FRAME PARSING

=====================================================================
*/

static void apply_entity_delta(entity_state_t *to, int number, const q2proto_entity_state_delta_t *delta_state)
{
    Q_assert(to);
    Q_assert(number > 0 && number < MAX_EDICTS);

    to->number = number;
    to->event = 0;

    if (!delta_state->delta_bits && !delta_state->origin.read.value.delta_bits && !delta_state->angle.delta_bits) {
        return;
    }

    if (delta_state->delta_bits & Q2P_ESD_MODELINDEX)
        to->modelindex = delta_state->modelindex;
    if (delta_state->delta_bits & Q2P_ESD_MODELINDEX2)
        to->modelindex2 = delta_state->modelindex2;
    if (delta_state->delta_bits & Q2P_ESD_MODELINDEX3)
        to->modelindex3 = delta_state->modelindex3;
    if (delta_state->delta_bits & Q2P_ESD_MODELINDEX4)
        to->modelindex4 = delta_state->modelindex4;

    if (delta_state->delta_bits & Q2P_ESD_FRAME)
        to->frame = delta_state->frame;

    if (delta_state->delta_bits & Q2P_ESD_SKINNUM)
        to->skinnum = delta_state->skinnum;

    if (delta_state->delta_bits & Q2P_ESD_EFFECTS)
        to->effects = (to->effects & ~MASK_ULL(32)) | delta_state->effects;
    if (delta_state->delta_bits & Q2P_ESD_EFFECTS_MORE)
        to->effects = (to->effects & MASK_ULL(32)) | ((effects_t)delta_state->effects_more << 32);

    if (delta_state->delta_bits & Q2P_ESD_RENDERFX)
        to->renderfx = delta_state->renderfx;

    q2proto_maybe_read_diff_apply_float(&delta_state->origin, to->origin);

    if (delta_state->angle.delta_bits & BIT(0))
        to->angles[0] = q2proto_var_angles_get_float_comp(&delta_state->angle.values, 0);
    if (delta_state->angle.delta_bits & BIT(1))
        to->angles[1] = q2proto_var_angles_get_float_comp(&delta_state->angle.values, 1);
    if (delta_state->angle.delta_bits & BIT(2))
        to->angles[2] = q2proto_var_angles_get_float_comp(&delta_state->angle.values, 2);

    if (delta_state->delta_bits & Q2P_ESD_OLD_ORIGIN)
        q2proto_var_coords_get_float(&delta_state->old_origin, to->old_origin);

    if (delta_state->delta_bits & Q2P_ESD_SOUND)
        to->sound = delta_state->sound;

    if (delta_state->delta_bits & Q2P_ESD_EVENT)
        to->event = delta_state->event;

    if (delta_state->delta_bits & Q2P_ESD_SOLID)
        to->solid = delta_state->solid;

    if (delta_state->delta_bits & Q2P_ESD_ALPHA)
        to->alpha = delta_state->alpha / 255.f;

    if (delta_state->delta_bits & Q2P_ESD_SCALE)
        to->scale = delta_state->scale / 16.f;
}

/*
===================
CL_Configstrings_init

precondition: csr has been set. Initializes the array of configstrings so that they point to the right place in memory.
*/
static void CL_Configstrings_init(void) {
    for (size_t i = 0; i < MAX_CONFIGSTRINGS; i++) {
        cl.configstrings[i] = cl.configstring_mem + i * cl.csr.configstring_size;
    }
}

static void CL_ParseDeltaEntity(server_frame_t           *frame,
                                int                      newnum,
                                const entity_state_t     *old,
                                const q2proto_entity_state_delta_t *delta_state)
{
    entity_state_t     *state;

    // suck up to MAX_EDICTS for servers that don't cap at MAX_PACKET_ENTITIES
    if (frame->numEntities >= cl.csr.max_edicts) {
        Com_Error(ERR_DROP, "%s: too many entities", __func__);
    }

    state = &cl.entityStates[cl.numEntityStates & PARSE_ENTITIES_MASK];
    cl.numEntityStates++;
    frame->numEntities++;

    *state = *old;
    state->event = 0; // events reset automatically, unless explicitly given
    if (delta_state)
        apply_entity_delta(state, newnum, delta_state);

    // shuffle previous origin to old
    uint32_t bits = delta_state ? delta_state->delta_bits : 0;
    if (!(bits & Q2P_ESD_OLD_ORIGIN) && !(state->renderfx & RF_BEAM))
        VectorCopy(old->origin, state->old_origin);

    // make sure extended indices don't overflow
    if ((state->modelindex | state->modelindex2 | state->modelindex3 | state->modelindex4) >= cl.csr.max_models)
        Com_Error(ERR_DROP, "%s: bad modelindex", __func__);

    if (state->sound >= cl.csr.max_sounds)
        Com_Error(ERR_DROP, "%s: bad sound", __func__);

    // mask off high bits for non-extended servers
    if (!cl.csr.extended) {
        state->renderfx &= RF_SHELL_LITE_GREEN - 1;
        if (state->renderfx & RF_BEAM)
            state->renderfx &= ~RF_GLOW;
    }
}

static void CL_ParsePacketEntities(const server_frame_t *oldframe, server_frame_t *frame)
{
    const entity_state_t  *oldstate;
    int                   i, oldindex, oldnum;

    frame->firstEntity = cl.numEntityStates;
    frame->numEntities = 0;

    // delta from the entities present in oldframe
    oldindex = 0;
    oldstate = NULL;
    if (!oldframe) {
        oldnum = MAX_EDICTS;
    } else {
        if (oldindex >= oldframe->numEntities) {
            oldnum = MAX_EDICTS;
        } else {
            i = (oldframe->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
            oldstate = &cl.entityStates[i];
            oldnum = oldstate->number;
        }
    }

    while (1) {
        q2proto_svc_message_t svc_message;
        q2proto_client_read(&cls.q2proto_ctx, Q2PROTO_IOARG_CLIENT_READ, &svc_message);
        if (svc_message.type != Q2P_SVC_FRAME_ENTITY_DELTA)
            Com_Error(ERR_DROP, "%s: unexpected packet type %d", __func__, svc_message.type);

        int newnum = svc_message.frame_entity_delta.newnum;
        if (newnum == 0)
            break;

        if (newnum < 0 || newnum >= cl.csr.max_edicts) {
            Com_Error(ERR_DROP, "%s: bad number: %d", __func__, newnum);
        }

        while (oldnum < newnum) {
            // one or more entities from the old packet are unchanged
            SHOWNET(4, "   unchanged:%i\n", oldnum);
            CL_ParseDeltaEntity(frame, oldnum, oldstate, NULL);

            oldindex++;

            if (oldindex >= oldframe->numEntities) {
                oldnum = MAX_EDICTS;
            } else {
                i = (oldframe->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
                oldstate = &cl.entityStates[i];
                oldnum = oldstate->number;
            }
        }

        if (svc_message.frame_entity_delta.remove) {
            // the entity present in oldframe is not in the current frame
            if (oldnum != newnum) {
                Com_DPrintf("U_REMOVE: oldnum != newnum\n");
            }
            if (!oldframe) {
                Com_Error(ERR_DROP, "%s: U_REMOVE with NULL oldframe", __func__);
            }

            oldindex++;

            if (oldindex >= oldframe->numEntities) {
                oldnum = MAX_EDICTS;
            } else {
                i = (oldframe->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
                oldstate = &cl.entityStates[i];
                oldnum = oldstate->number;
            }
            continue;
        }

        if (oldnum == newnum) {
            // delta from previous state
            CL_ParseDeltaEntity(frame, newnum, oldstate, &svc_message.frame_entity_delta.entity_delta);

            oldindex++;

            if (oldindex >= oldframe->numEntities) {
                oldnum = MAX_EDICTS;
            } else {
                i = (oldframe->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
                oldstate = &cl.entityStates[i];
                oldnum = oldstate->number;
            }
            continue;
        }

        if (oldnum > newnum) {
            // delta from baseline
            CL_ParseDeltaEntity(frame, newnum, &cl.baselines[newnum], &svc_message.frame_entity_delta.entity_delta);
            continue;
        }
    }

    // any remaining entities in the old frame are copied over
    while (oldnum != MAX_EDICTS) {
        // one or more entities from the old packet are unchanged
        SHOWNET(4, "   unchanged:%i\n", oldnum);
        CL_ParseDeltaEntity(frame, oldnum, oldstate, NULL);

        oldindex++;

        if (oldindex >= oldframe->numEntities) {
            oldnum = MAX_EDICTS;
        } else {
            i = (oldframe->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
            oldstate = &cl.entityStates[i];
            oldnum = oldstate->number;
        }
    }
}

static void apply_playerstate(const q2proto_svc_playerstate_t *playerstate,
                              const player_state_t *from,
                              player_state_t *to)
{
    int         i;

    Q_assert(to);

    // clear to old value before delta parsing
    if (!from) {
        memset(to, 0, sizeof(*to));
    } else if (to != from) {
        memcpy(to, from, sizeof(*to));
    }

    //
    // parse the pmove_state_t
    //
    if (playerstate->delta_bits & Q2P_PSD_PM_TYPE) {
        // assume PM type & flags are from rerelease when using Q2rePRO protocol, vanilla otherwise
        if (cls.serverProtocol == PROTOCOL_VERSION_RERELEASE)
            to->pmove.pm_type = playerstate->pm_type;
        else
            to->pmove.pm_type = pmtype_from_game3(playerstate->pm_type);
    }

    q2proto_maybe_read_diff_apply_float(&playerstate->pm_origin, to->pmove.origin);
    q2proto_maybe_read_diff_apply_float(&playerstate->pm_velocity, to->pmove.velocity);

    if (playerstate->delta_bits & Q2P_PSD_PM_TIME)
        to->pmove.pm_time = playerstate->pm_time;

    if (playerstate->delta_bits & Q2P_PSD_PM_FLAGS) {
        if(cls.serverProtocol == PROTOCOL_VERSION_RERELEASE)
            to->pmove.pm_flags = playerstate->pm_flags;
        else
            to->pmove.pm_flags = pmflags_from_game3(playerstate->pm_flags, cls.q2proto_ctx.features.server_game_api != Q2PROTO_GAME_VANILLA);
    }

    if (playerstate->delta_bits & Q2P_PSD_PM_GRAVITY)
        to->pmove.gravity = playerstate->pm_gravity;

    if (playerstate->delta_bits & Q2P_PSD_PM_DELTA_ANGLES)
        q2proto_var_angles_get_float(&playerstate->pm_delta_angles, to->pmove.delta_angles);

    //
    // parse the rest of the player_state_t
    //
    if (playerstate->delta_bits & Q2P_PSD_VIEWOFFSET)
        q2proto_var_small_offsets_get_float(&playerstate->viewoffset, to->viewoffset);

    Q2PROTO_APPLY_ANGLES_DELTA(to->viewangles, playerstate->viewangles, float);

    if (playerstate->delta_bits & Q2P_PSD_KICKANGLES)
        q2proto_var_small_angles_get_float(&playerstate->kick_angles, to->kick_angles);

    if (playerstate->delta_bits & Q2P_PSD_GUNINDEX)
        to->gunindex = playerstate->gunindex;
    if (playerstate->delta_bits & Q2P_PSD_GUNSKIN)
        to->gunskin = playerstate->gunskin;

    if (playerstate->delta_bits & Q2P_PSD_GUNFRAME)
        to->gunframe = playerstate->gunframe;
    Q2PROTO_APPLY_SMALL_OFFSETS_DELTA(to->gunoffset, playerstate->gunoffset, float);
    Q2PROTO_APPLY_SMALL_ANGLES_DELTA(to->gunangles, playerstate->gunangles, float);

    for (int i = 0; i < 4; i++) {
        if(playerstate->blend.delta_bits & BIT(i))
            to->screen_blend[i] = q2proto_var_color_get_float_comp(&playerstate->blend.values, i);
    }
    for (int i = 0; i < 4; i++) {
        if(playerstate->damage_blend.delta_bits & BIT(i))
            to->damage_blend[i] = q2proto_var_color_get_float_comp(&playerstate->damage_blend.values, i);
    }

    if (playerstate->delta_bits & Q2P_PSD_FOV)
        to->fov = playerstate->fov;

    if (playerstate->delta_bits & Q2P_PSD_RDFLAGS)
        to->rdflags = playerstate->rdflags;

    // parse stats
    if (playerstate->statbits) {
        for (i = 0; i < MAX_STATS; i++)
            if (playerstate->statbits & BIT_ULL(i))
                to->stats[i] = playerstate->stats[i];
    }

    if (playerstate->delta_bits & Q2P_PSD_GUNRATE)
        to->gunrate = playerstate->gunrate;

    if (playerstate->delta_bits & Q2P_PSD_PM_VIEWHEIGHT)
        to->pmove.viewheight = playerstate->pm_viewheight;
}

static void CL_ParseFrame(const q2proto_svc_frame_t *frame_msg)
{
    int                     currentframe, deltaframe, suppressed;
    server_frame_t          frame;
    const server_frame_t    *oldframe;
    const player_state_t    *from;

    memset(&frame, 0, sizeof(frame));

    cl.frameflags = 0;

    currentframe = frame_msg->serverframe;
    if (currentframe < 0) {
        Com_Error(ERR_DROP, "%s: currentframe < 0", __func__);
    }
    deltaframe = frame_msg->deltaframe;
    suppressed = frame_msg->suppress_count;
    if (suppressed)
        cl.frameflags |= FF_SUPPRESSED;

    frame.number = currentframe;
    frame.delta = deltaframe;

    if (cls.netchan.dropped) {
        cl.frameflags |= FF_SERVERDROP;
    }

    // if the frame is delta compressed from data that we no longer have
    // available, we must suck up the rest of the frame, but not use it, then
    // ask for a non-compressed message
    if (deltaframe > 0) {
        oldframe = &cl.frames[deltaframe & UPDATE_MASK];
        from = &oldframe->ps;
        if (deltaframe == currentframe) {
            // old servers may cause this on map change
            Com_DPrintf("%s: delta from current frame\n", __func__);
            cl.frameflags |= FF_BADFRAME;
        } else if (oldframe->number != deltaframe) {
            // the frame that the server did the delta from
            // is too old, so we can't reconstruct it properly.
            Com_DPrintf("%s: delta frame was never received or too old\n", __func__);
            cl.frameflags |= FF_OLDFRAME;
        } else if (!oldframe->valid) {
            // should never happen
            Com_DPrintf("%s: delta from invalid frame\n", __func__);
            cl.frameflags |= FF_BADFRAME;
        } else if (cl.numEntityStates - oldframe->firstEntity >
                   MAX_PARSE_ENTITIES - MAX_PACKET_ENTITIES) {
            Com_DPrintf("%s: delta entities too old\n", __func__);
            cl.frameflags |= FF_OLDENT;
        } else {
            frame.valid = true; // valid delta parse
        }
        if (!frame.valid && cl.frame.valid && cls.demo.playback) {
            Com_DPrintf("%s: recovering broken demo\n", __func__);
            oldframe = &cl.frame;
            from = &oldframe->ps;
            frame.valid = true;
        }
    } else {
        oldframe = NULL;
        from = NULL;
        frame.valid = true; // uncompressed frame
        cl.frameflags |= FF_NODELTA;
    }

    // read areabits
    if(frame_msg->areabits_len) {
        if (frame_msg->areabits_len > sizeof(frame.areabits)) {
            Com_Error(ERR_DROP, "%s: invalid areabits length", __func__);
        }
        memcpy(frame.areabits, frame_msg->areabits, frame_msg->areabits_len);
    }
    frame.areabytes = frame_msg->areabits_len;

    // parse playerstate
    apply_playerstate(&frame_msg->playerstate, from, &frame.ps);
    if(frame_msg->playerstate.delta_bits & Q2P_PSD_CLIENTNUM) {
        frame.clientNum = frame_msg->playerstate.clientnum;
    } else if (cls.q2proto_ctx.features.has_clientnum && oldframe) {
        frame.clientNum = oldframe->clientNum;
    } else {
        frame.clientNum = cl.clientNum;
    }

    // parse packetentities
    CL_ParsePacketEntities(oldframe, &frame);

    // save the frame off in the backup array for later delta comparisons
    cl.frames[currentframe & UPDATE_MASK] = frame;

#if USE_DEBUG
    if (cl_shownet->integer >= 3) {
        int seq = cls.netchan.incoming_acknowledged & CMD_MASK;
        int rtt = cls.demo.playback ? 0 : cls.realtime - cl.history[seq].sent;
        Com_LPrintf(PRINT_DEVELOPER, "%3u:frame:%d  delta:%d  rtt:%d\n",
                    msg_read.readcount, frame.number, frame.delta, rtt);
    }
#endif

    if (!frame.valid) {
        cl.frame.valid = false;
#if USE_FPS
        cl.keyframe.valid = false;
#endif
        return; // do not change anything
    }

    if (!frame.ps.fov) {
        // fail out early to prevent spurious errors later
        Com_Error(ERR_DROP, "%s: bad fov", __func__);
    }

    if (cls.state < ca_precached)
        return;

    cl.oldframe = cl.frame;
    cl.frame = frame;

#if USE_FPS
    if (CL_FRAMESYNC) {
        cl.oldkeyframe = cl.keyframe;
        cl.keyframe = cl.frame;
    }
#endif

    cls.demo.frames_read++;

    if (!cls.demo.seeking)
        CL_DeltaFrame();
}

/*
=====================================================================

  SERVER CONNECTING MESSAGES

=====================================================================
*/

static void CL_ParseConfigstring(const q2proto_svc_configstring_t *configstring)
{
    size_t  maxlen;
    char    *s;

    if (configstring->index >= cl.csr.end) {
        Com_Error(ERR_DROP, "%s: bad index: %d", __func__, configstring->index);
    }

    s = cl.configstrings[configstring->index];
    maxlen = Com_ConfigstringSize(&cl.csr, configstring->index);
    q2pslcpy(s, maxlen, &configstring->value);

    SHOWNET(3, "    %d \"%s\"\n", configstring->index, Com_MakePrintable(configstring->value.str));

    if (configstring->value.len >= maxlen) {
        Com_WPrintf(
            "%s: index %d overflowed: %zu > %zu\n",
            __func__, configstring->index, configstring->value.len, maxlen - 1);
    }

    if (cls.demo.seeking) {
        Q_SetBit(cl.dcs, configstring->index);
        return;
    }

    if (cls.demo.recording && cls.demo.paused) {
        Q_SetBit(cl.dcs, configstring->index);
    }

    // do something appropriate
    CL_UpdateConfigstring(configstring->index);
}

static void CL_ParseBaseline(const q2proto_svc_spawnbaseline_t* spawnbaseline)
{
    entity_state_t *base;

    if (spawnbaseline->entnum < 1 || spawnbaseline->entnum >= cl.csr.max_edicts) {
        Com_Error(ERR_DROP, "%s: bad index: %d", __func__, spawnbaseline->entnum);
    }

    base = &cl.baselines[spawnbaseline->entnum];
    apply_entity_delta(base, spawnbaseline->entnum, &spawnbaseline->delta_state);
}

static void set_server_fps(int value)
{
    cl.frametime = Com_ComputeFrametime(value);
    cl.frametime_inv = cl.frametime.div * BASE_1_FRAMETIME;

    // fix time delta
    if (cls.state == ca_active) {
        int delta = cl.frame.number - cl.servertime / cl.frametime.time;
        cl.serverdelta = Q_align_down(delta, cl.frametime.div);
    }

    Com_DPrintf("client framediv=%d time=%d delta=%d\n",
                cl.frametime.div, cl.servertime, cl.serverdelta);
}

static void CL_ParseServerData(const q2proto_svc_serverdata_t *serverdata)
{
    char    levelname[MAX_QPATH];
    int     i, protocol, attractloop q_unused;
    bool    cinematic;

    Cbuf_Execute(&cl_cmdbuf);          // make sure any stuffed commands are done

    // wipe the client_state_t struct
    CL_ClearState();

    // parse protocol version number
    protocol = serverdata->protocol;
    cl.servercount = serverdata->servercount;
    attractloop = serverdata->attractloop;

    Com_DPrintf("Serverdata packet received "
                "(protocol=%d, servercount=%d, attractloop=%d)\n",
                protocol, cl.servercount, attractloop);

    cl.csr = cs_remap_old;

    // check protocol
    if (cls.serverProtocol != protocol) {
        if (!cls.demo.playback) {
            Com_Error(ERR_DROP, "Requested protocol version %d, but server returned %d.",
                      cls.serverProtocol, protocol);
        }
        // BIG HACK to let demos from release work with the 3.0x patch!!!
        if (!EXTENDED_SUPPORTED(protocol)
            && (protocol != PROTOCOL_VERSION_RERELEASE)
            && (protocol != PROTOCOL_VERSION_KEX_DEMOS)
            && (protocol != PROTOCOL_VERSION_KEX)
            && (protocol < PROTOCOL_VERSION_OLD || protocol > PROTOCOL_VERSION_DEFAULT)) {
            Com_Error(ERR_DROP, "Demo uses unsupported protocol version %d.", protocol);
        } else {
            cls.serverProtocol = protocol;
        }
    }

    // game directory
    if (serverdata->gamedir.len >= sizeof(cl.gamedir)) {
        Com_Error(ERR_DROP, "Oversize gamedir string");
    }
    q2pslcpy(cl.gamedir, sizeof(cl.gamedir), &serverdata->gamedir);

    // never allow demos to change gamedir
    // do not change gamedir if connected to local sever either,
    // as it was already done by SV_InitGame, and changing it
    // here will not work since server is now running
    if (!cls.demo.playback && !sv_running->integer) {
        // pretend it has been set by user, so that 'changed' hook
        // gets called and filesystem is restarted
        Cvar_UserSet("game", cl.gamedir);

        // protect it from modifications while we are connected
        fs_game->flags |= CVAR_ROM;
    }

    // parse player entity number
    cl.clientNum = serverdata->clientnum;

    // get the full level name
    q2pslcpy(levelname, sizeof(levelname), &serverdata->levelname);

    // setup default pmove parameters
    PmoveInit(&cl.pmp);

    // setup default frame times
    set_server_fps(BASE_FRAMERATE);

    // setup default server state
    cl.serverstate = ss_game;
    cinematic = cl.clientNum == -1;

    if (cls.serverProtocol == PROTOCOL_VERSION_R1Q2) {
        if (serverdata->r1q2.enhanced) {
            Com_Error(ERR_DROP, "'Enhanced' R1Q2 servers are not supported");
        }
        i = serverdata->protocol_version;
        // for some reason, R1Q2 servers always report the highest protocol
        // version they support, while still using the lower version
        // client specified in the 'connect' packet. oh well...
        if (!R1Q2_SUPPORTED(i)) {
            Com_WPrintf(
                "R1Q2 server reports unsupported protocol version %d.\n"
                "Assuming it really uses our current client version %d.\n"
                "Things will break if it does not!\n", i, PROTOCOL_VERSION_R1Q2_CURRENT);
            i = Q_clip(i, PROTOCOL_VERSION_R1Q2_MINIMUM, PROTOCOL_VERSION_R1Q2_CURRENT);
        }
        Com_DPrintf("Using minor R1Q2 protocol version %d\n", i);
        cls.protocolVersion = i;
        if (serverdata->strafejump_hack) {
            Com_DPrintf("R1Q2 strafejump hack enabled\n");
            cl.pmp.strafehack = true;
        }
        cl.esFlags |= MSG_ES_BEAMORIGIN;
        if (cls.q2proto_ctx.features.has_solid32) {
            cl.esFlags |= MSG_ES_LONGSOLID;
        }
        cl.pmp.speedmult = 2;
    } else if (cls.serverProtocol == PROTOCOL_VERSION_Q2PRO) {
        i = serverdata->protocol_version;
        if (!Q2PRO_SUPPORTED(i)) {
            Com_Error(ERR_DROP,
                      "Q2PRO server reports unsupported protocol version %d.\n"
                      "Current client version is %d.", i, PROTOCOL_VERSION_Q2PRO_CURRENT);
        }
        Com_DPrintf("Using minor Q2PRO protocol version %d\n", i);
        cls.protocolVersion = i;
        if (cls.protocolVersion >= PROTOCOL_VERSION_Q2PRO_SERVER_STATE) {
            i = serverdata->q2pro.server_state;
            Com_DPrintf("Q2PRO server state %d\n", i);
            cl.serverstate = i;
            cinematic = i == ss_pic || i == ss_cinematic;
        }
        if (serverdata->strafejump_hack) {
            Com_DPrintf("Q2PRO strafejump hack enabled\n");
            cl.pmp.strafehack = true;
        }
        if (serverdata->q2pro.qw_mode) {
            Com_DPrintf("Q2PRO QW mode enabled\n");
            PmoveEnableQW(&cl.pmp);
        }
        if (serverdata->q2pro.waterjump_hack) {
            Com_DPrintf("Q2PRO waterjump hack enabled\n");
            cl.pmp.waterhack = true;
        }
        if (serverdata->q2pro.extensions) {
            Com_DPrintf("Q2PRO protocol extensions enabled\n");
            cl.csr = cs_remap_q2pro_new;
        }
        if (serverdata->q2pro.extensions_v2) {
            if (!cl.csr.extended) {
                Com_Error(ERR_DROP, "Q2PRO_PF_EXTENSIONS_2 without Q2PRO_PF_EXTENSIONS");
            }
            Com_DPrintf("Q2PRO protocol extensions v2 enabled\n");
            cl.esFlags |= MSG_ES_EXTENSIONS_2;
            cl.psFlags |= MSG_PS_EXTENSIONS_2;
            if (cls.protocolVersion >= PROTOCOL_VERSION_Q2PRO_PLAYERFOG)
                cl.psFlags |= MSG_PS_MOREBITS;
            PmoveEnableExt(&cl.pmp);
        }
        cl.esFlags |= MSG_ES_UMASK | MSG_ES_LONGSOLID;
        if (cls.protocolVersion >= PROTOCOL_VERSION_Q2PRO_BEAM_ORIGIN) {
            cl.esFlags |= MSG_ES_BEAMORIGIN;
        }
        if (cls.protocolVersion >= PROTOCOL_VERSION_Q2PRO_SHORT_ANGLES) {
            cl.esFlags |= MSG_ES_SHORTANGLES;
        }
        cl.pmp.speedmult = 2;
        cl.pmp.flyhack = true; // fly hack is unconditionally enabled
        cl.pmp.flyfriction = 4;
    } else if (cls.serverProtocol == PROTOCOL_VERSION_RERELEASE) {
        cls.protocolVersion = serverdata->protocol_version;
        cl.serverstate = serverdata->q2pro.server_state;
        cinematic = cl.serverstate == ss_pic || cl.serverstate == ss_cinematic;
        cl.game_api = cls.q2proto_ctx.features.server_game_api;
        if (cl.game_api == Q2PROTO_GAME_RERELEASE)
            cl.csr = cs_remap_rerelease;
        else if (cl.game_api >= Q2PROTO_GAME_Q2PRO_EXTENDED)
            cl.csr = cs_remap_q2pro_new;

        /*
         * az: hack. rerelease protocol forces all configstrings to be 96 characters long.
         * setting this to the default in the given cs_remap (potentially 64) causes ParseConfigstring to
         * overwrite the last 32 bytes of the previous index on the CS_GENERAL + 1/CS_STATUSBAR + 1 ranges.
         * now this case is rare, it only happens if we're running a non-rerelease dll on repro.
        */
        cl.csr.configstring_size = CS_MAX_STRING_LENGTH;

        cl.psFlags |= MSG_PS_RERELEASE | MSG_PS_EXTENSIONS;
        if (cl.game_api == Q2PROTO_GAME_Q2PRO_EXTENDED_V2)
            cl.psFlags |= MSG_PS_EXTENSIONS_2;
        cl.esFlags |= MSG_ES_RERELEASE | CL_ES_EXTENDED_MASK;
        set_server_fps(serverdata->server_fps);
        /* Rerelease game assumes client & server framerate is in sync,
         * non-rerelease games w/ variable FPS (eg OpenFFA) seem to assume
         * certain things still happen at 10Hz.
         * (For one, view weapon looks janky if the divider is 1.)*/
        if (cl.game_api == Q2PROTO_GAME_RERELEASE)
            cl.frametime.div = 1;

        cl.pmp.speedmult = 2;
        cl.pmp.flyhack = true; // fly hack is unconditionally enabled
        cl.pmp.flyfriction = 4;
    } else if (cls.serverProtocol == PROTOCOL_VERSION_KEX_DEMOS || cls.serverProtocol == PROTOCOL_VERSION_KEX) {
        cl.game_api = cls.q2proto_ctx.features.server_game_api;
        cl.csr = cs_remap_rerelease;
        set_server_fps(serverdata->server_fps);
        cl.frametime.div = 1;
    } else {
        // Demo protocol, or vanilla
        if (serverdata->q2pro.extensions)
            cl.csr = cs_remap_q2pro_new;
        cls.protocolVersion = serverdata->protocol_version;
    }

    if (cl.csr.extended) {
        cl.esFlags |= CL_ES_EXTENDED_MASK;
        cl.psFlags |= MSG_PS_EXTENSIONS;

        // hack for demo playback
        if (EXTENDED_SUPPORTED(protocol)) {
            if (protocol >= PROTOCOL_VERSION_EXTENDED_LIMITS_2) {
                cl.esFlags |= MSG_ES_EXTENSIONS_2;
                cl.psFlags |= MSG_PS_EXTENSIONS_2;
            }
            if (protocol >= PROTOCOL_VERSION_EXTENDED_PLAYERFOG)
                cl.psFlags |= MSG_PS_MOREBITS;
        }

        cl.pmp.extended_server_ver = cl.psFlags & MSG_PS_EXTENSIONS_2 ? 2 : 1;
    } else
        cl.pmp.extended_server_ver = 0;

    if (cls.serverProtocol == PROTOCOL_VERSION_RERELEASE) {
        cls.demo.esFlags = cl.esFlags;
    } else {
        // use full extended flags unless writing backward compatible demo
        cls.demo.esFlags = cl.csr.extended ? CL_ES_EXTENDED_MASK_2 : 0;
        cls.demo.psFlags = cl.csr.extended ? CL_PS_EXTENDED_MASK_2 : 0;
    }

    cl.max_stats = (cl.game_api >= Q2PROTO_GAME_Q2PRO_EXTENDED_V2) ? MAX_STATS_NEW : MAX_STATS_OLD;

    CL_Configstrings_init();

    // Load cgame (after we know all the timings)
    CG_Load(cl.gamedir, cl.game_api == Q2PROTO_GAME_RERELEASE);
    cgame->Init();

    if (cinematic) {
        SCR_PlayCinematic(levelname);
    } else {
        // separate the printfs so the server message can have a color
        Con_Printf(
            "\n\n"
            "\35\36\36\36\36\36\36\36\36\36\36\36"
            "\36\36\36\36\36\36\36\36\36\36\36\36"
            "\36\36\36\36\36\36\36\36\36\36\36\37"
            "\n\n");

        Com_SetColor(COLOR_INDEX_ALT);
        Com_Printf("%s\n", levelname);
        Com_SetColor(COLOR_INDEX_NONE);
    }

    // make sure clientNum is in range
    if (!VALIDATE_CLIENTNUM(&cl.csr, cl.clientNum)) {
        Com_WPrintf("Serverdata has invalid playernum %d\n", cl.clientNum);
        cl.clientNum = -1;
    }
}

/*
=====================================================================

ACTION MESSAGES

=====================================================================
*/

tent_params_t   te;
mz_params_t     mz;
q2proto_sound_t snd;

static void CL_ParseTEntPacket(const q2proto_svc_temp_entity_t *temp_entity)
{
    te.type = temp_entity->type;
    VectorCopy(temp_entity->position1, te.pos1);
    VectorCopy(temp_entity->position2, te.pos2);
    VectorCopy(temp_entity->offset, te.offset);
    VectorCopy(temp_entity->direction, te.dir);
    te.count = temp_entity->count;
    te.color = temp_entity->color;
    te.entity1 = temp_entity->entity1;
    te.entity2 = temp_entity->entity2;
    te.time = temp_entity->time;
}

static void CL_ParseMuzzleFlashPacket(const q2proto_svc_muzzleflash_t *muzzleflash)
{
    mz.silenced = muzzleflash->silenced;
    mz.weapon = muzzleflash->weapon;
    mz.entity = muzzleflash->entity;
}

static void CL_ParseStartSoundPacket(const q2proto_svc_sound_t* sound)
{
    q2proto_sound_decode_message(sound, &snd);
    if (snd.index >= cl.csr.max_sounds)
        Com_Error(ERR_DROP, "%s: bad index: %d", __func__, snd.index);
    if (snd.entity >= cl.csr.max_edicts)
        Com_Error(ERR_DROP, "%s: bad entity: %d", __func__, snd.entity);
    SHOWNET(3, "    %s\n", cl.configstrings[cl.csr.sounds + snd.index]);
}

static void CL_ParseReconnect(void)
{
    if (cls.demo.playback) {
        Com_Error(ERR_DISCONNECT, "Server disconnected");
    }

    Com_Printf("Server disconnected, reconnecting\n");

    // close netchan now to prevent `disconnect'
    // message from being sent to server
    Netchan_Close(&cls.netchan);

    CL_Disconnect(ERR_RECONNECT);

    cls.state = ca_challenging;
    cls.connect_time -= CONNECT_FAST;
    cls.connect_count = 0;

    CL_CheckForResend();
}

#if USE_AUTOREPLY
static void CL_CheckForVersion(const char *s)
{
    const char* p = strstr(s, ": ");
    if (!p) {
        return;
    }

    if (strncmp(p + 2, "!version", 8)) {
        return;
    }

    if (cl.reply_time && cls.realtime - cl.reply_time < 120000) {
        return;
    }

    cl.reply_time = cls.realtime;
    cl.reply_delta = 1024 + (Q_rand() & 1023);
}
#endif

// attempt to scan out an IP address in dotted-quad notation and
// add it into circular array of recent addresses
static void CL_CheckForIP(const char *s)
{
    unsigned b1, b2, b3, b4, port;
    netadr_t *a;
    int n;

    while (*s) {
        n = sscanf(s, "%3u.%3u.%3u.%3u:%u", &b1, &b2, &b3, &b4, &port);
        if (n >= 4 && (b1 | b2 | b3 | b4) < 256) {
            if (n == 5) {
                if (port < 1024 || port > 65535) {
                    break; // privileged or invalid port
                }
            } else {
                port = PORT_SERVER;
            }

            a = &cls.recent_addr[cls.recent_head++ & RECENT_MASK];
            a->type = NA_IP;
            a->ip.u8[0] = b1;
            a->ip.u8[1] = b2;
            a->ip.u8[2] = b3;
            a->ip.u8[3] = b4;
            a->port = BigShort(port);
            break;
        }

        s++;
    }
}

/*
 * Replace ##P<n> tokens with player names from cl.clientinfo.
 * Per server game.c: "the client is supposed to translate ##P<n> to
 * player names".
 */
static void CL_TranslatePlayerNameTokens(char *s, size_t size)
{
    char out[MAX_STRING_CHARS];
    const char *src = s;
    char *dst = out;

    while (*src) {
        size_t space = sizeof(out) - (size_t)(dst - out) - 1;
        if (!space)
            break;

        if (!strncmp(src, "##P", 3) && Q_isdigit(src[3])) {
            src += 3;
            int playernum = 0;
            while (Q_isdigit(*src))
                playernum = playernum * 10 + (*src++ - '0');

            if (playernum < MAX_CLIENTS) {
                const char *name = cl.clientinfo[playernum].name;
                size_t len = min(strlen(name), space);
                memcpy(dst, name, len);
                dst += len;
            }
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
    Q_strlcpy(s, out, size);
}

static void CL_HandlePrint(int level, char *s)
{
    const char *fmt;

    /* Called here rather than CL_ParseLocPrint() because ##P tokens
     * arrive via svc_print broadcast messages, not localized prints. */
    CL_TranslatePlayerNameTokens(s, MAX_STRING_CHARS);

    if (level != PRINT_CHAT) {
        if (cl.csr.extended) {
            if (level == PRINT_TYPEWRITER || level == PRINT_CENTER) {
                Con_SkipNotify(true);
                cgame->ParseCenterPrint(s, 0, level == PRINT_CENTER);
                Con_SkipNotify(false);
            } else {
                if (cl_cgame_notify->integer) {
                    Con_SkipNotify(true);
                    cgame->NotifyMessage(0, s, level == PRINT_CHAT);
                }
                Com_Printf("%s", s);
            }

            Con_SkipNotify(false);
        }
        else
            Com_Printf("%s", s);
        if (!cls.demo.playback && cl.serverstate != ss_broadcast) {
            COM_strclr(s);
            Cmd_ExecTrigger(s);
        }
        return;
    }

    if (CL_CheckForIgnore(s)) {
        return;
    }

#if USE_AUTOREPLY
    if (!cls.demo.playback && cl.serverstate != ss_broadcast) {
        CL_CheckForVersion(s);
    }
#endif

    CL_CheckForIP(s);

    // disable notify
    bool use_cgame_notify = cl_cgame_notify->integer && cl.csr.extended;
    if (!cl_chat_notify->integer || use_cgame_notify) {
        Con_SkipNotify(true);
    }

    // filter text
    if (cl_chat_filter->integer) {
        COM_strclr(s);
        fmt = "%s\n";
    } else {
        fmt = "%s";
    }

    Com_LPrintf(PRINT_TALK, fmt, s);

    if (use_cgame_notify) {
        cgame->NotifyMessage(0, s, level == PRINT_CHAT);
    }

    Con_SkipNotify(false);

    SCR_AddToChatHUD(s);

    // silence MVD spectator chat
    if (cl.serverstate == ss_broadcast && !strncmp(s, "[MVD] ", 6))
        return;

    // play sound
    if (cl_chat_sound->integer > 1)
        S_StartLocalSoundOnce("misc/talk1.wav");
    else if (cl_chat_sound->integer > 0)
        S_StartLocalSoundOnce("misc/talk.wav");
}

static void CL_ParsePrint(const q2proto_svc_print_t *print)
{
    char s[MAX_STRING_CHARS];
    q2pslcpy(s, sizeof(s), &print->string);

    SHOWNET(2, "    %i \"%s\"\n", print->level, Com_MakePrintable(s));

    CL_HandlePrint(print->level, s);
}

static void CL_ParseCenterPrint(const q2proto_svc_centerprint_t *centerprint)
{
    char s[MAX_STRING_CHARS];

    q2pslcpy(s, sizeof(s), &centerprint->message);

    SHOWNET(3, "    \"%s\"\n", Com_MakePrintable(s));
    cgame->ParseCenterPrint(s, 0, false);

    if (!cls.demo.playback && cl.serverstate != ss_broadcast) {
        COM_strclr(s);
        Cmd_ExecTrigger(s);
    }
}

static void CL_ParseStuffText(const q2proto_svc_stufftext_t *stufftext)
{
    char s[MAX_STRING_CHARS];

    q2pslcpy(s, sizeof(s), &stufftext->string);
    SHOWNET(3, "    \"%s\"\n", Com_MakePrintable(s));
    Cbuf_AddText(&cl_cmdbuf, s);
}

static void CL_ParseLayout(const q2proto_svc_layout_t *layout)
{
    q2pslcpy(cl.cgame_data.layout, sizeof(cl.cgame_data.layout), &layout->layout_str);
    SHOWNET(3, "    \"%s\"\n", Com_MakePrintable(cl.cgame_data.layout));
}

static void CL_ParseInventory(const q2proto_svc_inventory_t *inventory)
{
    int        i;

    for (i = 0; i < MAX_ITEMS; i++) {
        cl.cgame_data.inventory[i] = inventory->inventory[i];
    }
}

static void CL_ParseDownload(const q2proto_svc_download_t *download)
{
    int size, percent;

    if (!cls.download.temp[0]) {
        Com_Error(ERR_DROP, "%s: no download requested", __func__);
    }

    // read the data
    size = download->size;
    percent = download->percent;
    if (size == -1) {
        CL_HandleDownload(NULL, size, percent);
        return;
    }

    if (size < 0) {
        Com_Error(ERR_DROP, "%s: bad size: %d", __func__, size);
    }

    CL_HandleDownload(download->data, size, percent);
}

static void CL_ParseSetting(const q2proto_svc_setting_t *setting)
{
    switch (setting->index) {
#if USE_FPS
    case SVS_FPS:
        set_server_fps(setting->value);
        break;
#endif
    default:
        break;
    }
}

static void CL_ParseDamage(const q2proto_svc_damage_t *damage)
{
    for (uint8_t i = 0; i < damage->count; i++)
    {
        vec3_t color = { 0.f, 0.f, 0.f };

        if (damage->damage[i].health)
            color[0] += 1.0f;
        if (damage->damage[i].shield)
            color[1] += 1.0f;
        if (damage->damage[i].armor)
        {
            color[0] += 1.0f;
            color[1] += 1.0f;
            color[2] += 1.0f;
        }

        VectorNormalize(color);

        SCR_AddToDamageDisplay(damage->damage[i].damage, color, damage->damage[i].direction);
    }
}

static void CL_ParseFog(const q2proto_svc_fog_t *fog)
{
    cl_fog_params_t params;
    int time = 0;

    if (fog->flags & Q2P_FOG_DENSITY_SKYFACTOR)
    {
        params.linear.density = q2proto_var_fraction_get_float(&fog->global.density);
        params.linear.sky_factor = q2proto_var_fraction_get_float(&fog->global.skyfactor);
    }
    if (fog->global.color.delta_bits & BIT(0))
        params.linear.color[0] = q2proto_var_color_get_float_comp(&fog->global.color.values, 0);
    if (fog->global.color.delta_bits & BIT(1))
        params.linear.color[1] = q2proto_var_color_get_float_comp(&fog->global.color.values, 1);
    if (fog->global.color.delta_bits & BIT(2))
        params.linear.color[2] = q2proto_var_color_get_float_comp(&fog->global.color.values, 2);
    if (fog->flags & Q2P_FOG_TIME)
        time = fog->global.time;

    if (fog->flags & Q2P_HEIGHTFOG_FALLOFF)
        params.height.falloff = q2proto_var_fraction_get_float(&fog->height.falloff);
    if (fog->flags & Q2P_HEIGHTFOG_DENSITY)
        params.height.density = q2proto_var_fraction_get_float(&fog->height.density);

    if (fog->height.start_color.delta_bits & BIT(0))
        params.height.start.color[0] = q2proto_var_color_get_float_comp(&fog->height.start_color.values, 0);
    if (fog->height.start_color.delta_bits & BIT(1))
        params.height.start.color[1] = q2proto_var_color_get_float_comp(&fog->height.start_color.values, 1);
    if (fog->height.start_color.delta_bits & BIT(2))
        params.height.start.color[2] = q2proto_var_color_get_float_comp(&fog->height.start_color.values, 2);
    if (fog->flags & Q2P_HEIGHTFOG_START_DIST)
        params.height.start.dist = q2proto_var_coord_get_float(&fog->height.start_dist);

    if (fog->height.end_color.delta_bits & BIT(0))
        params.height.end.color[0] = q2proto_var_color_get_float_comp(&fog->height.end_color.values, 0);
    if (fog->height.end_color.delta_bits & BIT(1))
        params.height.end.color[1] = q2proto_var_color_get_float_comp(&fog->height.end_color.values, 1);
    if (fog->height.end_color.delta_bits & BIT(2))
        params.height.end.color[2] = q2proto_var_color_get_float_comp(&fog->height.end_color.values, 2);
    if (fog->flags & Q2P_HEIGHTFOG_END_DIST)
        params.height.end.dist = q2proto_var_coord_get_float(&fog->height.end_dist);

    V_FogParamsChanged(fog->flags, fog->global.color.delta_bits, fog->height.start_color.delta_bits, fog->height.end_color.delta_bits, &params, time);
}

static void CL_ParsePOI(const q2proto_svc_poi_t *poi)
{
    // POIs can be "keyed" and "unkeyed".
    // Key'd POIs have a non-zero id and will replace others
    // on the same key if re-added.
    // POI IDs from 1 to (MAX_EDICTS - 1) inclusive are reserved
    // by the client, and attach to the given entity (can be used
    // for POIs that can follow an entity more accurately if it's
    // in your frame).
    // POI IDs above that are free for modders.
    int id = poi->key;
    // time == USHRT_MAX is a special value for "delete this POI".
    // it is only valid with a keyed POI.
    // time == 0 is a special value for "infinite", and can only
    // be used on reliable messages.
    int time = poi->time;

    if (time == USHRT_MAX)
        SCR_RemovePOI(id);
    else
        SCR_AddPOI(id, time, poi->pos, poi->image, poi->color, poi->flags);
}

static void CL_ParseLocPrint(const q2proto_svc_locprint_t *locprint)
{
    char base_str[MAX_STRING_CHARS];
    q2pslcpy(base_str, sizeof(base_str), &locprint->base);

    SHOWNET(2, "    %i \"%s\"\n", locprint->flags, Com_MakePrintable(base_str));

    Q_assert(locprint->num_args <= MAX_LOCALIZATION_ARGS);
    char loc_args[MAX_LOCALIZATION_ARGS][MAX_STRING_CHARS];
    const char *loc_args_p[MAX_LOCALIZATION_ARGS];
    for (int i = 0; i < locprint->num_args; i++)
    {
        q2pslcpy(loc_args[i], sizeof(loc_args[0]), &locprint->args[i]);
        loc_args_p[i] = loc_args[i];

        SHOWNET(2, "    ... arg %i: \"%s\"\n", i, Com_MakePrintable(loc_args[i]));
    }

    char string[MAX_STRING_CHARS];
    Loc_Localize(base_str, true, loc_args_p, locprint->num_args, string, sizeof(string));

    CL_HandlePrint(locprint->flags, string);
}


/*
=====================
CL_ParseServerMessage
=====================
*/
void CL_ParseServerMessage(void)
{
    uint32_t    readcount;

#if USE_DEBUG
    if (cl_shownet->integer == 1) {
        Com_LPrintf(PRINT_DEVELOPER, "%u ", msg_read.cursize);
    } else if (cl_shownet->integer >= 2) {
        Com_LPrintf(PRINT_DEVELOPER, "------------------\n");
    }
#endif

    msg_read.allowunderflow = false;

//
// parse the message
//
    while (1) {
        readcount = msg_read.readcount;

        q2proto_svc_message_t svc_msg;
        q2proto_error_t err = q2proto_client_read(&cls.q2proto_ctx, Q2PROTO_IOARG_CLIENT_READ, &svc_msg);
        if (err == Q2P_ERR_NO_MORE_INPUT) {
            SHOWNET(2, "%3u:END OF MESSAGE\n", readcount);
            break;
        }

        switch(svc_msg.type)
        {
        default:
            Com_Error(ERR_DROP, "%s: unknown message type: %d", __func__, svc_msg.type);
            break;

        case Q2P_SVC_NOP:
            break;

        case Q2P_SVC_DISCONNECT:
            Com_Error(ERR_DISCONNECT, "Server disconnected");
            break;

        case Q2P_SVC_RECONNECT:
            CL_ParseReconnect();
            return;

        case Q2P_SVC_PRINT:
            CL_ParsePrint(&svc_msg.print);
            break;

        case Q2P_SVC_CENTERPRINT:
            CL_ParseCenterPrint(&svc_msg.centerprint);
            break;

        case Q2P_SVC_STUFFTEXT:
            CL_ParseStuffText(&svc_msg.stufftext);
            break;

        case Q2P_SVC_SERVERDATA:
            CL_ParseServerData(&svc_msg.serverdata);
            continue;

        case Q2P_SVC_CONFIGSTRING:
            CL_ParseConfigstring(&svc_msg.configstring);
            break;

        case Q2P_SVC_SOUND:
            CL_ParseStartSoundPacket(&svc_msg.sound);
            S_ParseStartSound();
            break;

        case Q2P_SVC_SPAWNBASELINE:
            CL_ParseBaseline(&svc_msg.spawnbaseline);
            break;

        case Q2P_SVC_TEMP_ENTITY:
            CL_ParseTEntPacket(&svc_msg.temp_entity);
            CL_ParseTEnt();
            break;

        case Q2P_SVC_MUZZLEFLASH:
            CL_ParseMuzzleFlashPacket(&svc_msg.muzzleflash);
            CL_MuzzleFlash();
            break;

        case Q2P_SVC_MUZZLEFLASH2:
            CL_ParseMuzzleFlashPacket(&svc_msg.muzzleflash);
            CL_MuzzleFlash2();
            break;

        case Q2P_SVC_DOWNLOAD:
            CL_ParseDownload(&svc_msg.download);
            continue;

        case Q2P_SVC_FRAME:
            CL_ParseFrame(&svc_msg.frame);
            continue;

        case Q2P_SVC_INVENTORY:
            CL_ParseInventory(&svc_msg.inventory);
            break;

        case Q2P_SVC_LAYOUT:
            CL_ParseLayout(&svc_msg.layout);
            break;

        case Q2P_SVC_SETTING:
            CL_ParseSetting(&svc_msg.setting);
            break;

        // KEX
        case Q2P_SVC_DAMAGE:
            CL_ParseDamage(&svc_msg.damage);
            break;

        case Q2P_SVC_FOG:
            CL_ParseFog(&svc_msg.fog);
            break;

        case Q2P_SVC_POI:
            CL_ParsePOI(&svc_msg.poi);
            break;

        case Q2P_SVC_HELP_PATH:
            CL_AddHelpPath(svc_msg.help_path.pos, svc_msg.help_path.dir, svc_msg.help_path.start);
            break;

        case Q2P_SVC_ACHIEVEMENT:
            // ignore
            break;

        case Q2P_SVC_LOCPRINT:
            CL_ParseLocPrint(&svc_msg.locprint);
            break;
        // KEX
        }

        // if recording demos, copy off protocol invariant stuff
        if (cls.demo.recording && !cls.demo.paused) {
            uint32_t len = msg_read.readcount - readcount;

            // it is very easy to overflow standard 1390 bytes
            // demo frame with modern servers... attempt to preserve
            // reliable messages at least, assuming they come first
            bool written = false;
            if (cls.demo.buffer.cursize + len < cls.demo.buffer.maxsize) {
                uint32_t old_size = cls.demo.buffer.cursize;
                q2proto_error_t write_result = q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &svc_msg);
                written = write_result == Q2P_ERR_SUCCESS;
                if (!written) {
                    Com_WPrintf("%s: failed to write message of type %s: %s\n", __func__, q2proto_svc_message_str(svc_msg.type), q2proto_error_string(write_result));
                    // Roll back to previous message, in case of failure
                    cls.demo.buffer.cursize = old_size;
                    cls.demo.buffer.overflowed = false;
                }
            }
            if(!written) {
                cls.demo.others_dropped++;
            }
        }

        // if running GTV server, add current message
        CL_GTV_WriteMessage(msg_read.data + readcount,
                            msg_read.readcount - readcount);
    }
}

/*
=====================
CL_SeekDemoMessage

A variant of ParseServerMessage that skips over non-important action messages,
used for seeking in demos. Returns true if seeking should be aborted (got serverdata).
=====================
*/
bool CL_SeekDemoMessage(void)
{
    bool        serverdata = false;

#if USE_DEBUG
    if (cl_shownet->integer == 1) {
        Com_LPrintf(PRINT_DEVELOPER, "%u ", msg_read.cursize);
    } else if (cl_shownet->integer >= 2) {
        Com_LPrintf(PRINT_DEVELOPER, "------------------\n");
    }
#endif

    msg_read.allowunderflow = false;

//
// parse the message
//
    while (1) {
        q2proto_svc_message_t svc_msg;
        q2proto_error_t err = q2proto_client_read(&cls.q2proto_ctx, Q2PROTO_IOARG_CLIENT_READ, &svc_msg);
        if (err == Q2P_ERR_NO_MORE_INPUT) {
            SHOWNET(2, "%3u:END OF MESSAGE\n", msg_read.readcount);
            break;
        }
        switch(svc_msg.type)
        {
        default:
            Com_Error(ERR_DROP, "%s: illegible message type: %d", __func__, svc_msg.type);
            break;

        case Q2P_SVC_NOP:
            break;

        case Q2P_SVC_DISCONNECT:
        case Q2P_SVC_RECONNECT:
            Com_Error(ERR_DISCONNECT, "Server disconnected");
            break;

        case Q2P_SVC_PRINT:
        case Q2P_SVC_CENTERPRINT:
        case Q2P_SVC_STUFFTEXT:
        case Q2P_SVC_DAMAGE:
        case Q2P_SVC_POI:
        case Q2P_SVC_HELP_PATH:
        case Q2P_SVC_ACHIEVEMENT:
        case Q2P_SVC_LOCPRINT:
            // Ignore
            break;

        case Q2P_SVC_SERVERDATA:
            CL_ParseServerData(&svc_msg.serverdata);
            serverdata = true;
            break;

        case Q2P_SVC_CONFIGSTRING:
            CL_ParseConfigstring(&svc_msg.configstring);
            break;

        case Q2P_SVC_SOUND:
            CL_ParseStartSoundPacket(&svc_msg.sound);
            S_ParseStartSound();
            break;

        case Q2P_SVC_SPAWNBASELINE:
            CL_ParseBaseline(&svc_msg.spawnbaseline);
            break;

        case Q2P_SVC_TEMP_ENTITY:
            CL_ParseTEntPacket(&svc_msg.temp_entity);
            CL_ParseTEnt();
            break;

        case Q2P_SVC_MUZZLEFLASH:
            CL_ParseMuzzleFlashPacket(&svc_msg.muzzleflash);
            CL_MuzzleFlash();
            break;

        case Q2P_SVC_MUZZLEFLASH2:
            CL_ParseMuzzleFlashPacket(&svc_msg.muzzleflash);
            CL_MuzzleFlash2();
            break;

        case Q2P_SVC_FRAME:
            CL_ParseFrame(&svc_msg.frame);
            continue;

        case Q2P_SVC_INVENTORY:
            CL_ParseInventory(&svc_msg.inventory);
            break;

        case Q2P_SVC_LAYOUT:
            CL_ParseLayout(&svc_msg.layout);
            break;

        case Q2P_SVC_FOG:
            CL_ParseFog(&svc_msg.fog);
            break;
        }
    }

    return serverdata;
}

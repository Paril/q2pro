/*
Copyright (C) 2003-2006 Andrey Nazarov

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

//
// cl_demo.c - demo recording and playback
//

#include "client.h"

static byte     demo_buffer[MAX_MSGLEN];

static cvar_t   *cl_demosnaps;
static cvar_t   *cl_demomsglen;
static cvar_t   *cl_demowait;
static cvar_t   *cl_demosuspendtoggle;

q2protoio_ioarg_t demo_q2protoio_ioarg = {.sz_write = &cls.demo.buffer};

// =========================================================================

/*
====================
CL_WriteDemoMessage

Dumps the current demo message, prefixed by the length.
Stops demo recording and returns false on write error.
====================
*/
bool CL_WriteDemoMessage(sizebuf_t *buf)
{
    uint32_t msglen;
    int ret;

    if (buf->overflowed) {
        SZ_Clear(buf);
        Com_WPrintf("Demo message overflowed (should never happen).\n");
        return true;
    }

    if (!buf->cursize)
        return true;

    msglen = LittleLong(buf->cursize);
    ret = FS_Write(&msglen, 4, cls.demo.recording);
    if (ret != 4)
        goto fail;
    ret = FS_Write(buf->data, buf->cursize, cls.demo.recording);
    if (ret != buf->cursize)
        goto fail;

    Com_DDPrintf("%s: wrote %u bytes\n", __func__, buf->cursize);

    SZ_Clear(buf);
    return true;

fail:
    SZ_Clear(buf);
    Com_EPrintf("Couldn't write demo: %s\n", Q_ErrorString(ret));
    CL_Stop_f();
    return false;
}

void CL_PackEntity(entity_packed_t *out, const entity_state_t *in)
{
    MSG_PackEntity(out, in, cl.csr.extended);

    // repack solid 32 to 16
    if (!cl.csr.extended && cl.esFlags & MSG_ES_LONGSOLID && in->solid && in->solid != PACKED_BSP) {
        vec3_t mins, maxs;
        MSG_UnpackSolid32_Ver1(in->solid, mins, maxs);
        out->solid = MSG_PackSolid16(mins, maxs);
    }
}

static void CL_PackEntity_q2proto(q2proto_packed_entity_state_t *out, const entity_state_t *in)
{
    PackEntity(&cls.demo.q2proto_context, in, out);
    // repack solid 32 to 16
    if (!cl.csr.extended && in->solid && in->solid != PACKED_BSP) {
        vec3_t mins, maxs;
        q2proto_client_unpack_solid(&cls.q2proto_ctx, in->solid, mins, maxs);
        out->solid = q2proto_pack_solid_16(mins, maxs);
    }
}

static void write_delta_entity(const q2proto_packed_entity_state_t *oldpack, const q2proto_packed_entity_state_t *newpack, int newnum, msgEsFlags_t flags)
{
    q2proto_svc_message_t message_entity_delta = {.type = Q2P_SVC_FRAME_ENTITY_DELTA, .frame_entity_delta = {0}};
    bool entity_differs = Q2PROTO_MakeEntityDelta(&cls.demo.q2proto_context, &message_entity_delta.frame_entity_delta.entity_delta, oldpack, newpack, flags);
    message_entity_delta.frame_entity_delta.newnum = newnum;

    if (!(flags & MSG_ES_FORCE) && !entity_differs)
        return;
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message_entity_delta);
}

static void write_entity_remove(int num)
{
    q2proto_svc_message_t message_entity_delta = {.type = Q2P_SVC_FRAME_ENTITY_DELTA, .frame_entity_delta = {0}};
    message_entity_delta.frame_entity_delta.remove = true;
    message_entity_delta.frame_entity_delta.newnum = num;
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message_entity_delta);
}

// writes a delta update of an entity_state_t list to the message.
static void emit_packet_entities(const server_frame_t *from, const server_frame_t *to)
{
    q2proto_packed_entity_state_t oldpack, newpack;
    entity_state_t *oldent, *newent;
    int     oldindex, newindex;
    int     oldnum, newnum;
    int     i, from_num_entities;

    if (!from)
        from_num_entities = 0;
    else
        from_num_entities = from->numEntities;

    newindex = 0;
    oldindex = 0;
    oldent = newent = NULL;
    while (newindex < to->numEntities || oldindex < from_num_entities) {
        if (newindex >= to->numEntities) {
            newnum = MAX_EDICTS;
        } else {
            i = (to->firstEntity + newindex) & PARSE_ENTITIES_MASK;
            newent = &cl.entityStates[i];
            newnum = newent->number;
        }

        if (oldindex >= from_num_entities) {
            oldnum = MAX_EDICTS;
        } else {
            i = (from->firstEntity + oldindex) & PARSE_ENTITIES_MASK;
            oldent = &cl.entityStates[i];
            oldnum = oldent->number;
        }

        if (newnum == oldnum) {
            // Delta update from old position. Because the force param is false,
            // this will not result in any bytes being emitted if the entity has
            // not changed at all. Note that players are always 'newentities',
            // this updates their old_origin always and prevents warping in case
            // of packet loss.
            msgEsFlags_t flags = cls.demo.esFlags;
            if (newent->number <= cl.maxclients)
                flags |= MSG_ES_NEWENTITY;
            CL_PackEntity_q2proto(&oldpack, oldent);
            CL_PackEntity_q2proto(&newpack, newent);
            write_delta_entity(&oldpack, &newpack, newnum, flags);
            oldindex++;
            newindex++;
            continue;
        }

        if (newnum < oldnum) {
            // this is a new entity, send it from the baseline
            CL_PackEntity_q2proto(&oldpack, &cl.baselines[newnum]);
            CL_PackEntity_q2proto(&newpack, newent);
            write_delta_entity(&oldpack, &newpack, newnum, MSG_ES_FORCE | MSG_ES_NEWENTITY);
            newindex++;
            continue;
        }

        if (newnum > oldnum) {
            // the old entity isn't present in the new message
            write_entity_remove(oldnum);
            oldindex++;
            continue;
        }
    }

    // end of packetentities
    q2proto_svc_message_t message = {.type = Q2P_SVC_FRAME_ENTITY_DELTA, .frame_entity_delta = {0}};
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message);
}

static void emit_delta_frame(const server_frame_t *from, const server_frame_t *to,
                             int fromnum, int tonum)
{
    q2proto_svc_message_t message = {.type = Q2P_SVC_FRAME, .frame = {0}};

    message.frame.serverframe = tonum;
    message.frame.deltaframe = fromnum;
    message.frame.suppress_count = cl.suppress_count;   // rate dropped packets
    message.frame.q2pro_frame_flags = 0;

    message.frame.areabits_len = to->areabytes;
    message.frame.areabits = to->areabits;

    q2proto_packed_player_state_t newpack, oldpack;
    PackPlayerstate(&cls.demo.q2proto_context, &to->ps, &newpack);
    if (from) {
        PackPlayerstate(&cls.demo.q2proto_context, &from->ps, &oldpack);
        q2proto_server_make_player_state_delta(&cls.demo.q2proto_context, &oldpack, &newpack, &message.frame.playerstate);
    } else
        q2proto_server_make_player_state_delta(&cls.demo.q2proto_context, NULL, &newpack, &message.frame.playerstate);
    if ((from ? from->clientNum : 0) != to->clientNum) {
        message.frame.playerstate.clientnum = to->clientNum;
        message.frame.playerstate.delta_bits |= Q2P_PSD_CLIENTNUM;
    }

    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message);

    emit_packet_entities(from, to);
}

// frames_written counter starts at 0, but we add 1 to every frame number
// because frame 0 can't be used due to protocol limitation (hack).
#define FRAME_PRE   (cls.demo.frames_written)
#define FRAME_CUR   (cls.demo.frames_written + 1)

/*
====================
CL_EmitDemoFrame

Writes delta from the last frame we got to the current frame.
====================
*/
void CL_EmitDemoFrame(void)
{
    server_frame_t  *oldframe;
    int             lastframe;

    if (!cl.frame.valid)
        return;

    // the first frame is delta uncompressed
    if (cls.demo.last_server_frame == -1) {
        oldframe = NULL;
        lastframe = -1;
    } else {
        oldframe = &cl.frames[cls.demo.last_server_frame & UPDATE_MASK];
        lastframe = FRAME_PRE;
        if (oldframe->number != cls.demo.last_server_frame || !oldframe->valid ||
            cl.numEntityStates - oldframe->firstEntity > MAX_PARSE_ENTITIES) {
            oldframe = NULL;
            lastframe = -1;
        }
    }

    // emit and flush frame
    emit_delta_frame(oldframe, &cl.frame, lastframe, FRAME_CUR);

    if (msg_write.overflowed) {
        Com_WPrintf("%s: message buffer overflowed\n", __func__);
    } else if (cls.demo.buffer.cursize + msg_write.cursize > cls.demo.buffer.maxsize) {
        Com_DPrintf("Demo frame overflowed (%u + %u > %u)\n",
                    cls.demo.buffer.cursize, msg_write.cursize, cls.demo.buffer.maxsize);
        cls.demo.frames_dropped++;

        // warn the user if drop rate is too high
        if (cls.demo.frames_written < 10 && !(cls.demo.frames_dropped % 50)) {
            Com_WPrintf("Too many demo frames don't fit into %u bytes!\n", cls.demo.buffer.maxsize);
            if (cls.demo.frames_dropped == 50)
                Com_WPrintf("Try to increase 'cl_demomsglen' value and restart recording.\n");
        }
    } else {
        SZ_Write(&cls.demo.buffer, msg_write.data, msg_write.cursize);
        cls.demo.last_server_frame = cl.frame.number;
        cls.demo.frames_written++;
    }

    SZ_Clear(&msg_write);
}

static size_t format_demo_size(char *buffer, size_t size)
{
    return Com_FormatSizeLong(buffer, size, FS_Tell(cls.demo.recording));
}

static size_t format_demo_status(char *buffer, size_t size)
{
    size_t len = format_demo_size(buffer, size);
    int min, sec, frames = cls.demo.frames_written;

    sec = frames / BASE_FRAMERATE; frames %= BASE_FRAMERATE;
    min = sec / 60; sec %= 60;

    len += Q_scnprintf(buffer + len, size - len, ", %d:%02d.%d",
                       min, sec, frames);

    if (cls.demo.frames_dropped) {
        len += Q_scnprintf(buffer + len, size - len, ", %d frame%s dropped",
                           cls.demo.frames_dropped,
                           cls.demo.frames_dropped == 1 ? "" : "s");
    }

    if (cls.demo.others_dropped) {
        len += Q_scnprintf(buffer + len, size - len, ", %d message%s dropped",
                           cls.demo.others_dropped,
                           cls.demo.others_dropped == 1 ? "" : "s");
    }

    return len;
}

/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f(void)
{
    uint32_t msglen;
    char buffer[MAX_QPATH];

    if (!cls.demo.recording) {
        Com_Printf("Not recording a demo.\n");
        return;
    }

// finish up
    msglen = (uint32_t)-1;
    FS_Write(&msglen, 4, cls.demo.recording);

    format_demo_size(buffer, sizeof(buffer));

// close demofile
    FS_CloseFile(cls.demo.recording);
    cls.demo.recording = 0;
    cls.demo.paused = false;
    cls.demo.frames_written = 0;
    cls.demo.frames_dropped = 0;
    cls.demo.others_dropped = 0;

// print some statistics
    Com_Printf("Stopped demo (%s).\n", buffer);

// tell the server we finished recording
    CL_UpdateRecordingSetting();
}

static void fill_message_fog(q2proto_svc_fog_t *msg_fog, const cl_fog_params_t *client_fog)
{
    msg_fog->flags = 0;
    q2proto_var_color_set_float_comp(&msg_fog->global.color.values, 0, client_fog->linear.color[0]);
    q2proto_var_color_set_float_comp(&msg_fog->global.color.values, 1, client_fog->linear.color[1]);
    q2proto_var_color_set_float_comp(&msg_fog->global.color.values, 2, client_fog->linear.color[2]);
    msg_fog->global.color.delta_bits = BIT(0) | BIT(1) | BIT(2);
    q2proto_var_fraction_set_float(&msg_fog->global.density, client_fog->linear.density);
    q2proto_var_fraction_set_float(&msg_fog->global.skyfactor, client_fog->linear.sky_factor);
    msg_fog->flags |= Q2P_FOG_DENSITY_SKYFACTOR;

    q2proto_var_fraction_set_float(&msg_fog->height.falloff, client_fog->height.falloff);
    msg_fog->flags |= Q2P_HEIGHTFOG_FALLOFF;
    q2proto_var_fraction_set_float(&msg_fog->height.density, client_fog->height.density);
    msg_fog->flags |= Q2P_HEIGHTFOG_DENSITY;

    q2proto_var_color_set_float_comp(&msg_fog->height.start_color.values, 0, client_fog->height.start.color[0]);
    q2proto_var_color_set_float_comp(&msg_fog->height.start_color.values, 1, client_fog->height.start.color[1]);
    q2proto_var_color_set_float_comp(&msg_fog->height.start_color.values, 2, client_fog->height.start.color[2]);
    msg_fog->height.start_color.delta_bits = BIT(0) | BIT(1) | BIT(2);
    q2proto_var_coord_set_float(&msg_fog->height.start_dist, client_fog->height.start.dist);
    msg_fog->flags |= Q2P_HEIGHTFOG_START_DIST;

    q2proto_var_color_set_float_comp(&msg_fog->height.end_color.values, 0, client_fog->height.end.color[0]);
    q2proto_var_color_set_float_comp(&msg_fog->height.end_color.values, 1, client_fog->height.end.color[1]);
    q2proto_var_color_set_float_comp(&msg_fog->height.end_color.values, 2, client_fog->height.end.color[2]);
    msg_fog->height.end_color.delta_bits = BIT(0) | BIT(1) | BIT(2);
    q2proto_var_coord_set_float(&msg_fog->height.end_dist, client_fog->height.end.dist);
    msg_fog->flags |= Q2P_HEIGHTFOG_END_DIST;
}

static void write_current_fog(void)
{
    q2proto_svc_message_t fog_message = {.type = Q2P_SVC_FOG};

    if (cl.fog.lerp_time == 0 || cl.time > cl.fog.lerp_time_start + cl.fog.lerp_time) {
        // No fog lerping
        fill_message_fog(&fog_message.fog, &cl.fog.end);
        q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &fog_message);
    } else {
        cl_fog_params_t current_fog;

        int time_since_lerp_start = (cl.time - cl.fog.lerp_time_start);
        float fog_frontlerp = time_since_lerp_start / (float) cl.fog.lerp_time;
        float fog_backlerp = 1.0f - fog_frontlerp;

#define Q_FP(p) \
            current_fog.linear.p = LERP2(cl.fog.start.linear.p, cl.fog.end.linear.p, fog_backlerp, fog_frontlerp)
#define Q_HFP(p) \
            current_fog.height.p = LERP2(cl.fog.start.height.p, cl.fog.end.height.p, fog_backlerp, fog_frontlerp)

        Q_FP(color[0]);
        Q_FP(color[1]);
        Q_FP(color[2]);
        Q_FP(density);
        Q_FP(sky_factor);

        Q_HFP(start.color[0]);
        Q_HFP(start.color[1]);
        Q_HFP(start.color[2]);
        Q_HFP(start.dist);

        Q_HFP(end.color[0]);
        Q_HFP(end.color[1]);
        Q_HFP(end.color[2]);
        Q_HFP(end.dist);

        Q_HFP(density);
        Q_HFP(falloff);

#undef Q_FP
#undef Q_HFP

        // Write current fog, as perceived by user
        fill_message_fog(&fog_message.fog, &current_fog);
        q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &fog_message);

        // Write fog being lerped to
        fill_message_fog(&fog_message.fog, &cl.fog.end);
        fog_message.fog.global.time = cl.fog.lerp_time - time_since_lerp_start;
        fog_message.fog.flags |= Q2P_FOG_TIME;
        q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &fog_message);
    }
}

static const cmd_option_t o_record[] = {
    { "h", "help", "display this message" },
    { "z", "compress", "compress demo with gzip" },
    { "e", "extended", "use extended packet size" },
    { "s", "standard", "use standard packet size" },
    { NULL }
};

// Data needed for write_gamestate in CL_Record_f. Too large for stack, so store statically.
static q2proto_svc_configstring_t configstrings[MAX_CONFIGSTRINGS];
static q2proto_svc_spawnbaseline_t spawnbaselines[MAX_EDICTS];

/*
====================
CL_Record_f

record <demoname>

Begins recording a demo from the current position
====================
*/
static void CL_Record_f(void)
{
    char    buffer[MAX_OSPATH];
    int     i, c;
    qhandle_t       f;
    unsigned        mode = FS_MODE_WRITE;
    size_t          size = Cvar_ClampInteger(
                               cl_demomsglen,
                               MIN_PACKETLEN,
                               MAX_MSGLEN);

    while ((c = Cmd_ParseOptions(o_record)) != -1) {
        switch (c) {
        case 'h':
            Cmd_PrintUsage(o_record, "<filename>");
            Com_Printf("Begin client demo recording.\n");
            Cmd_PrintHelp(o_record);
            return;
        case 'z':
            mode |= FS_FLAG_GZIP;
            break;
        case 'e':
            size = MAX_PACKETLEN_WRITABLE;
            break;
        case 's':
            size = MAX_PACKETLEN_WRITABLE_DEFAULT;
            break;
        default:
            return;
        }
    }

    if (cls.demo.recording) {
        format_demo_status(buffer, sizeof(buffer));
        Com_Printf("Already recording (%s).\n", buffer);
        return;
    }

    if (!cmd_optarg[0]) {
        Com_Printf("Missing filename argument.\n");
        Cmd_PrintHint();
        return;
    }

    if (cls.state != ca_active) {
        Com_Printf("You must be in a level to record.\n");
        return;
    }

    if (cl.csr.extended)
        size = MAX_MSGLEN;

    // Set up q2proto structures
    memset(&cls.demo.server_info, 0, sizeof(cls.demo.server_info));
    cls.demo.server_info.game_api = cls.q2proto_ctx.features.server_game_api;
    cls.demo.server_info.default_packet_length = size;

    q2proto_error_t err = q2proto_init_servercontext_demo(&cls.demo.q2proto_context, &cls.demo.server_info, &size);
    if (err != Q2P_ERR_SUCCESS) {
        Com_EPrintf("Failed to start demo recording: %s.\n", q2proto_error_string(err));
        return;
    }
    demo_q2protoio_ioarg.max_msg_len = size;

    //
    // open the demo file
    //
    f = FS_EasyOpenFile(buffer, sizeof(buffer), mode,
                        "demos/", cmd_optarg, ".dm2");
    if (!f) {
        return;
    }

    Com_Printf("Recording client demo to %s.\n", buffer);

    cls.demo.recording = f;
    cls.demo.paused = false;

    // the first frame will be delta uncompressed
    cls.demo.last_server_frame = -1;

    SZ_InitWrite(&cls.demo.buffer, demo_buffer, size);

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));

    // tell the server we are recording
    CL_UpdateRecordingSetting();



    //
    // write out messages to hold the startup information
    //

    // send the serverdata
    q2proto_svc_message_t message_svcdata = {.type = Q2P_SVC_SERVERDATA, .serverdata = {0}};
    q2proto_server_fill_serverdata(&cls.demo.q2proto_context, &message_svcdata.serverdata);
    message_svcdata.serverdata.servercount = cl.servercount;
    message_svcdata.serverdata.attractloop = true; // demos are always attract loops
    message_svcdata.serverdata.gamedir = q2proto_make_string(cl.gamedir);
    message_svcdata.serverdata.clientnum = cl.clientNum;
    message_svcdata.serverdata.levelname = q2proto_make_string(cl.configstrings[CS_NAME]);
    message_svcdata.serverdata.q2repro.server_fps = cl.frametime_inv * 1000;
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message_svcdata);

    q2proto_gamestate_t gamestate = {.num_configstrings = 0, .configstrings = configstrings, .num_spawnbaselines = 0, .spawnbaselines = spawnbaselines};
    memset(spawnbaselines, 0, sizeof(spawnbaselines));

    // configstrings
    for (int i = 0; i < cl.csr.end; i++) {
        char* string = cl.configstrings[i];
        if (!string[0]) {
            continue;
        }
        q2proto_svc_configstring_t *cfgstr = &configstrings[gamestate.num_configstrings++];
        cfgstr->index = i;
        cfgstr->value.str = string;
        cfgstr->value.len = Q_strnlen(string, CS_MAX_STRING_LENGTH);
    }

    // baselines
    for (i = 1; i < cl.csr.max_edicts; i++) {
        entity_state_t *ent = &cl.baselines[i];
        if (!ent->number) {
            continue;
        }
        q2proto_svc_spawnbaseline_t *baseline = &spawnbaselines[gamestate.num_spawnbaselines++];
        baseline->entnum = ent->number;
        q2proto_packed_entity_state_t packed_entity;
        PackEntity(&cls.demo.q2proto_context, ent, &packed_entity);
        Q2PROTO_MakeEntityDelta(&cls.demo.q2proto_context, &baseline->delta_state, NULL, &packed_entity, 0);
    }

    int write_result;
    do {
        write_result = q2proto_server_write_gamestate(&cls.demo.q2proto_context, NULL, Q2PROTO_IOARG_DEMO_WRITE, &gamestate);;
        CL_WriteDemoMessage(&cls.demo.buffer);
    } while (write_result == Q2P_ERR_NOT_ENOUGH_PACKET_SPACE);

    // write fog
    write_current_fog();

    q2proto_svc_message_t message = {.type = Q2P_SVC_STUFFTEXT};
    message.stufftext.string = q2proto_make_string("precache\n");
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message);

    // write it to the demo file
    CL_WriteDemoMessage(&cls.demo.buffer);

    // the rest of the demo file will be individual frames
}

// resumes demo recording after pause or seek. tries to fit flushed
// configstrings and frame into single packet for seamless 'stitch'
static void resume_record(void)
{
    int i, j, index;
    size_t len;
    char *s;

    // write dirty configstrings
    for (i = 0; i < q_countof(cl.dcs); i++) {
        if (cl.dcs[i] == 0)
            continue;

        index = i * BC_BITS;
        for (j = 0; j < BC_BITS; j++, index++) {
            if (!Q_IsBitSet(cl.dcs, index))
                continue;

            s = cl.configstrings[index];

            len = Q_strnlen(s, CS_MAX_STRING_LENGTH);
            if (cls.demo.buffer.cursize + len + 4 > cls.demo.buffer.maxsize) {
                if (!CL_WriteDemoMessage(&cls.demo.buffer))
                    return;
                // multiple packets = not seamless
            }

            q2proto_svc_message_t message = {.type = Q2P_SVC_CONFIGSTRING};
            message.configstring.index = index;
            message.configstring.value.str = s;
            message.configstring.value.len = len;
            q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message);
        }
    }

    // write delta uncompressed frame
    //cls.demo.last_server_frame = -1;
    CL_EmitDemoFrame();

    // FIXME: write layout if it fits? most likely it won't

    // write it to the demo file
    CL_WriteDemoMessage(&cls.demo.buffer);
}

static void CL_Resume_f(void)
{
    if (!cls.demo.recording) {
        Com_Printf("Not recording a demo.\n");
        return;
    }

    if (!cls.demo.paused) {
        Com_Printf("Demo recording is already resumed.\n");
        return;
    }

    resume_record();

    if (!cls.demo.recording)
        // write failed
        return;

    Com_Printf("Resumed demo recording.\n");

    cls.demo.paused = false;

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));
}

static void CL_Suspend_f(void)
{
    if (!cls.demo.recording) {
        Com_Printf("Not recording a demo.\n");
        return;
    }

    if (!cls.demo.paused) {
        Com_Printf("Suspended demo recording.\n");
        cls.demo.paused = true;
        return;
    }

    // only resume if cl_demosuspendtoggle is enabled
    if (!cl_demosuspendtoggle->integer) {
        Com_Printf("Demo recording is already suspended.\n");
        return;
    }

    CL_Resume_f();
}

static int read_first_message(qhandle_t f)
{
    uint32_t    ul;
    uint16_t    us;
    size_t      msglen;
    int         read, type;

    // read magic/msglen
    read = FS_Read(&ul, 4, f);
    if (read != 4) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    // determine demo type
    if (ul == MVD_MAGIC) {
        read = FS_Read(&us, 2, f);
        if (read != 2) {
            return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
        }
        if (!us) {
            return Q_ERR_UNEXPECTED_EOF;
        }
        msglen = LittleShort(us);
        type = 1;
    } else {
        if (ul == (uint32_t)-1) {
            return Q_ERR_UNEXPECTED_EOF;
        }
        msglen = LittleLong(ul);
        type = 0;
    }

    if (msglen > sizeof(msg_read_buffer)) {
        return Q_ERR_INVALID_FORMAT;
    }

    // read packet data
    read = FS_Read(msg_read_buffer, msglen, f);
    if (read != msglen) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    SZ_InitRead(&msg_read, msg_read_buffer, msglen);
    return type;
}

static int read_next_message(qhandle_t f)
{
    uint32_t msglen;
    int read;

    // read msglen
    read = FS_Read(&msglen, 4, f);
    if (read != 4) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    // check for EOF packet
    if (msglen == (uint32_t)-1) {
        return 0;
    }

    msglen = LittleLong(msglen);
    if (msglen > sizeof(msg_read_buffer)) {
        return Q_ERR_INVALID_FORMAT;
    }

    // read packet data
    read = FS_Read(msg_read_buffer, msglen, f);
    if (read != msglen) {
        return read < 0 ? read : Q_ERR_UNEXPECTED_EOF;
    }

    SZ_InitRead(&msg_read, msg_read_buffer, msglen);
    return 1;
}

static void finish_demo(int ret)
{
    const char *s = Cvar_VariableString("nextserver");

    if (!s[0]) {
        if (ret == 0) {
            Com_Error(ERR_DISCONNECT, "Demo finished");
        } else {
            Com_Error(ERR_DROP, "Couldn't read demo: %s", Q_ErrorString(ret));
        }
    }

    CL_Disconnect(ERR_RECONNECT);

    Cbuf_AddText(&cmd_buffer, s);
    Cbuf_AddText(&cmd_buffer, "\n");

    Cvar_Set("nextserver", "");
}

static void update_status(void)
{
    if (cls.demo.file_size) {
        int64_t pos = FS_Tell(cls.demo.playback);

        if (pos > cls.demo.file_offset)
            cls.demo.file_progress = (float)(pos - cls.demo.file_offset) / cls.demo.file_size;
        else
            cls.demo.file_progress = 0.0f;
    }
}

static int parse_next_message(int wait)
{
    int ret;

    ret = read_next_message(cls.demo.playback);
    if (ret < 0 || (ret == 0 && wait == 0)) {
        finish_demo(ret);
        return -1;
    }

    update_status();

    if (ret == 0) {
        cls.demo.eof = true;
        return -1;
    }

    CL_ParseServerMessage();

    // if recording demo, write the message out
    if (cls.demo.recording && !cls.demo.paused && CL_FRAMESYNC) {
        CL_WriteDemoMessage(&cls.demo.buffer);
    }

    // if running GTV server, transmit to client
    CL_GTV_Transmit();

    // save a snapshot once the full packet is parsed
    CL_EmitDemoSnapshot();

    return 0;
}

/*
====================
CL_PlayDemo_f
====================
*/
static void CL_PlayDemo_f(void)
{
    char name[MAX_OSPATH];
    qhandle_t f;
    int type;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s <filename>\n", Cmd_Argv(0));
        return;
    }

    f = FS_EasyOpenFile(name, sizeof(name), FS_MODE_READ | FS_FLAG_GZIP,
                        "demos/", Cmd_Argv(1), ".dm2");
    if (!f) {
        return;
    }

    type = read_first_message(f);
    if (type < 0) {
        Com_Printf("Couldn't read %s: %s\n", name, Q_ErrorString(type));
        FS_CloseFile(f);
        return;
    }

    if (type == 1) {
#if USE_MVD_CLIENT
        Cbuf_InsertText(&cmd_buffer, va("mvdplay --replace @@ \"/%s\"\n", name));
#else
        Com_Printf("MVD support was not compiled in.\n");
#endif
        FS_CloseFile(f);
        return;
    }

    // if running a local server, kill it and reissue
    SV_Shutdown("Server was killed.\n", ERR_DISCONNECT);

    CL_Disconnect(ERR_RECONNECT);

    cls.demo.playback = f;
    cls.demo.compat = !strcmp(Cmd_Argv(2), "compat");
    cls.state = ca_connected;
    Q_strlcpy(cls.servername, COM_SkipPath(name), sizeof(cls.servername));
    cls.serverAddress.type = NA_LOOPBACK;
    cl.csr = cs_remap_old;
    cl.max_stats = MAX_STATS_OLD;

    Con_Popup(true);
    SCR_UpdateScreen();

    q2proto_init_clientcontext(&cls.q2proto_ctx);

    // parse the first message just read
    CL_ParseServerMessage();

    // Set up cls.demo.q2proto_context for demo snaps, with same protocol as server
    memset(&cls.demo.server_info, 0, sizeof(cls.demo.server_info));
    cls.demo.server_info.game_api = cls.q2proto_ctx.features.server_game_api;
    q2proto_connect_t connect_info;
    memset(&connect_info, 0, sizeof(connect_info));
    connect_info.protocol = q2proto_protocol_from_netver(cls.serverProtocol);
    connect_info.version = cls.protocolVersion;
    connect_info.q2pro_nctype = cls.netchan.type;
    q2proto_error_t err = q2proto_init_servercontext(&cls.demo.q2proto_context, &cls.demo.server_info, &connect_info);
    if (err != Q2P_ERR_SUCCESS) {
        Com_Error(ERR_DISCONNECT, "Couldn't init demo context: %s", q2proto_error_string(err));
        return;
    }
    SZ_InitWrite(&cls.demo.buffer, demo_buffer, MAX_MSGLEN);
    demo_q2protoio_ioarg.max_msg_len = MAX_MSGLEN;

#if USE_ZLIB
    if (!cls.demo.z_buffer) {
        cls.demo.z_buffer_size = deflateBound(NULL, MAX_MSGLEN) + 6 /* zlib header/footer */;
        cls.demo.z_buffer = Z_Malloc(cls.demo.z_buffer_size);
    }

    Q2PROTO_deflate_args_init(&cls.demo.q2proto_deflate, cls.demo.z_buffer, cls.demo.z_buffer_size, TAG_GENERAL);
#endif

    // read and parse messages util `precache' command
    for (int i = 0; cls.state == ca_connected && i < 1000; i++) {
        Cbuf_Execute(&cl_cmdbuf);
        parse_next_message(0);
    }
}

static void CL_Demo_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        FS_File_g("demos", ".dm2;.dm2.gz;.mvd2;.mvd2.gz", FS_SEARCH_RECURSIVE, ctx);
    }
}

#define MIN_SNAPSHOTS   64
#define MAX_SNAPSHOTS   250000000

/*
====================
CL_EmitDemoSnapshot

Periodically builds a fake demo packet used to reconstruct delta compression
state, configstrings and layouts at the given server frame.
====================
*/
void CL_EmitDemoSnapshot(void)
{
    demosnap_t *snap;
    int64_t pos;
    char *from, *to;
    server_frame_t *lastframe, *frame;
    int i, j, lastnum;

    if (cl_demosnaps->integer <= 0)
        return;

    if (cls.demo.frames_read < cls.demo.last_snapshot + cl_demosnaps->integer * BASE_FRAMERATE)
        return;

    if (cls.demo.numsnapshots >= MAX_SNAPSHOTS)
        return;

    if (!cl.frame.valid)
        return;

    if (!cls.demo.file_size)
        return;

    pos = FS_Tell(cls.demo.playback);
    if (pos < cls.demo.file_offset)
        return;

    q2proto_gamestate_t gamestate = {.num_configstrings = 0, .configstrings = configstrings, .num_spawnbaselines = 0, .spawnbaselines = spawnbaselines};
    memset(spawnbaselines, 0, sizeof(spawnbaselines));

    // configstrings
    for (i = 0; i < cl.csr.end; i++) {
        from = (char*)cl.baseconfigstrings + i * cl.csr.configstring_size;
        to = cl.configstrings[i];

        if (!strncmp(from, to, cl.csr.configstring_size))
            continue;

        char* string = cl.configstrings[i];
        q2proto_svc_configstring_t *cfgstr = &configstrings[gamestate.num_configstrings++];
        cfgstr->index = i;
        cfgstr->value.str = string;
        cfgstr->value.len = Q_strnlen(string, cl.csr.configstring_size);
    }

    /* baselines, for more robustness
     * Mainly for when the 2022 protocol is used, as "solid" values affect encoding,
     * so they really need to be right, and they need to be "seen" by q2proto
     * during both reading demo messages and writing snaps. */
    for (i = 1; i < cl.csr.max_edicts; i++) {
        entity_state_t *ent = &cl.baselines[i];
        if (!ent->number) {
            continue;
        }
        q2proto_svc_spawnbaseline_t *baseline = &spawnbaselines[gamestate.num_spawnbaselines++];
        baseline->entnum = ent->number;
        q2proto_packed_entity_state_t packed_entity;
        PackEntity(&cls.demo.q2proto_context, ent, &packed_entity);
        Q2PROTO_MakeEntityDelta(&cls.demo.q2proto_context, &baseline->delta_state, NULL, &packed_entity, 0);
    }

    q2protoio_deflate_args_t *deflate_args = NULL;
#if USE_ZLIB
    deflate_args = &cls.demo.q2proto_deflate;
#endif
    q2proto_server_write_gamestate(&cls.demo.q2proto_context, deflate_args, Q2PROTO_IOARG_DEMO_WRITE, &gamestate);;

    // write all the backups, since we can't predict what frame the next
    // delta will come from
    lastframe = NULL;
    lastnum = -1;
    for (i = 0; i < UPDATE_BACKUP; i++) {
        j = cl.frame.number - (UPDATE_BACKUP - 1) + i;
        frame = &cl.frames[j & UPDATE_MASK];
        if (frame->number != j || !frame->valid ||
            cl.numEntityStates - frame->firstEntity > MAX_PARSE_ENTITIES) {
            continue;
        }

        emit_delta_frame(lastframe, frame, lastnum, j);
        lastframe = frame;
        lastnum = frame->number;
    }

    // write layout
    q2proto_svc_message_t message = {.type = Q2P_SVC_LAYOUT};
    message.layout.layout_str = q2proto_make_string(cl.cgame_data.layout);
    q2proto_server_write(&cls.demo.q2proto_context, Q2PROTO_IOARG_DEMO_WRITE, &message);

    // write fog
    write_current_fog();

    if (cls.demo.buffer.overflowed) {
        Com_DWPrintf("%s: message buffer overflowed\n", __func__);
    } else {
        snap = Z_Malloc(sizeof(*snap) + cls.demo.buffer.cursize - 1);
        snap->framenum = cls.demo.frames_read;
        snap->filepos = pos;
        snap->msglen = cls.demo.buffer.cursize;
        memcpy(snap->data, cls.demo.buffer.data, cls.demo.buffer.cursize);

        cls.demo.snapshots = Z_Realloc(cls.demo.snapshots, sizeof(cls.demo.snapshots[0]) * Q_ALIGN(cls.demo.numsnapshots + 1, MIN_SNAPSHOTS));
        cls.demo.snapshots[cls.demo.numsnapshots++] = snap;

        Com_DPrintf("[%d] snaplen %u\n", cls.demo.frames_read, cls.demo.buffer.cursize);
    }

    SZ_Clear(&cls.demo.buffer);

    cls.demo.last_snapshot = cls.demo.frames_read;
}

static demosnap_t *find_snapshot(int64_t dest, bool byte_seek)
{
    int l = 0;
    int r = cls.demo.numsnapshots - 1;

    if (r < 0)
        return NULL;

    do {
        int m = (l + r) / 2;
        demosnap_t *snap = cls.demo.snapshots[m];
        int64_t pos = byte_seek ? snap->filepos : snap->framenum;
        if (pos < dest)
            l = m + 1;
        else if (pos > dest)
            r = m - 1;
        else
            return snap;
    } while (l <= r);

    return cls.demo.snapshots[max(r, 0)];
}

/*
====================
CL_FirstDemoFrame

Called after the first valid frame is parsed from the demo.
====================
*/
void CL_FirstDemoFrame(void)
{
    int64_t len, ofs;

    Com_DPrintf("[%d] first frame\n", cl.frame.number);

    // save base configstrings
    memcpy(cl.baseconfigstrings, cl.configstring_mem, sizeof cl.baseconfigstrings);

    // obtain file length and offset of the second frame
    len = FS_Length(cls.demo.playback);
    ofs = FS_Tell(cls.demo.playback);
    if (ofs > 0 && ofs < len) {
        cls.demo.file_offset = ofs;
        cls.demo.file_size = len - ofs;
    }

    // begin timedemo
    if (com_timedemo->integer) {
        cls.demo.time_frames = 0;
        cls.demo.time_start = Sys_Milliseconds();
    }

    // force initial snapshot
    cls.demo.last_snapshot = INT_MIN;
}

/*
====================
CL_FreeDemoSnapshots
====================
*/
void CL_FreeDemoSnapshots(void)
{
    for (int i = 0; i < cls.demo.numsnapshots; i++)
        Z_Free(cls.demo.snapshots[i]);
    cls.demo.numsnapshots = 0;

    Z_Freep(&cls.demo.snapshots);
}

/*
====================
CL_Seek_f
====================
*/
static void CL_Seek_f(void)
{
    demosnap_t *snap;
    int i, j, ret, index, frames, prev;
    int64_t dest;
    bool byte_seek, back_seek;
    char *from, *to;
    size_t maxlen;

    if (Cmd_Argc() < 2) {
        Com_Printf("Usage: %s [+-]<timespec|percent>[%%]\n", Cmd_Argv(0));
        return;
    }

#if USE_MVD_CLIENT
    if (sv_running->integer == ss_broadcast) {
        Cbuf_InsertText(&cmd_buffer, va("mvdseek \"%s\" @@\n", Cmd_Argv(1)));
        return;
    }
#endif

    if (!cls.demo.playback) {
        Com_Printf("Not playing a demo.\n");
        return;
    }

    to = Cmd_Argv(1);

    if (strchr(to, '%')) {
        char *suf;
        float percent = strtof(to, &suf);
        if (suf == to || strcmp(suf, "%") || !isfinite(percent)) {
            Com_Printf("Invalid percentage.\n");
            return;
        }

        if (!cls.demo.file_size) {
            Com_Printf("Unknown file size, can't seek.\n");
            return;
        }

        percent = Q_clipf(percent, 0, 100);
        dest = cls.demo.file_offset + cls.demo.file_size * percent / 100;

        byte_seek = true;
        back_seek = dest < FS_Tell(cls.demo.playback);
    } else {
        if (*to == '-' || *to == '+') {
            // relative to current frame
            if (!Com_ParseTimespec(to + 1, &frames)) {
                Com_Printf("Invalid relative timespec.\n");
                return;
            }
            if (*to == '-')
                frames = -frames;
            dest = cls.demo.frames_read + frames;
        } else {
            // relative to first frame
            if (!Com_ParseTimespec(to, &i)) {
                Com_Printf("Invalid absolute timespec.\n");
                return;
            }
            dest = i;
            frames = i - cls.demo.frames_read;
        }

        if (!frames)
            return; // already there

        byte_seek = false;
        back_seek = frames < 0;
    }

    if (!back_seek && cls.demo.eof && cl_demowait->integer)
        return; // already at end

    // disable effects processing
    cls.demo.seeking = true;

    // clear dirty configstrings
    memset(cl.dcs, 0, sizeof(cl.dcs));

    // stop sounds
    S_StopAllSounds();

    // save previous server frame number
    prev = cl.frame.number;

    Com_DPrintf("[%d] seeking to %"PRId64"\n", cls.demo.frames_read, dest);

    // seek to the previous most recent snapshot
    if (back_seek || cls.demo.last_snapshot > cls.demo.frames_read) {
        snap = find_snapshot(dest, byte_seek);

        if (snap) {
            Com_DPrintf("found snap at %d\n", snap->framenum);
            ret = FS_Seek(cls.demo.playback, snap->filepos, SEEK_SET);
            if (ret < 0) {
                Com_EPrintf("Couldn't seek demo: %s\n", Q_ErrorString(ret));
                goto done;
            }

            // clear end-of-file flag
            cls.demo.eof = false;

            // reset configstrings
            for (i = 0; i < cl.csr.end; i++) {
                from = (char*)cl.baseconfigstrings + i * cl.csr.configstring_size;
                to = cl.configstrings[i];

                if (!strncmp(from, to, cl.csr.configstring_size))
                    continue;

                Q_SetBit(cl.dcs, i);


                maxlen = Com_ConfigstringSize(&cl.csr, i);
                Q_strlcpy(to, from, maxlen);
            }

            SZ_InitRead(&msg_read, snap->data, snap->msglen);

            CL_SeekDemoMessage();
            cls.demo.frames_read = snap->framenum;
            Com_DPrintf("[%d] after snap parse %d\n", cls.demo.frames_read, cl.frame.number);
        } else if (back_seek) {
            Com_Printf("Couldn't seek backwards without snapshots!\n");
            goto done;
        }
    }

    // skip forward to destination frame/position
    while (1) {
        int64_t pos = byte_seek ? FS_Tell(cls.demo.playback) : cls.demo.frames_read;
        if (pos >= dest)
            break;

        ret = read_next_message(cls.demo.playback);
        if (ret == 0 && cl_demowait->integer) {
            cls.demo.eof = true;
            break;
        }
        if (ret <= 0) {
            finish_demo(ret);
            return;
        }

        if (CL_SeekDemoMessage())
            goto done;
        CL_EmitDemoSnapshot();
    }

    Com_DPrintf("[%d] after skip %d\n", cls.demo.frames_read, cl.frame.number);

    // update dirty configstrings
    for (i = 0; i < q_countof(cl.dcs); i++) {
        if (cl.dcs[i] == 0)
            continue;

        index = i * BC_BITS;
        for (j = 0; j < BC_BITS; j++, index++) {
            if (Q_IsBitSet(cl.dcs, index))
                CL_UpdateConfigstring(index);
        }
    }

    // don't lerp to old
    memset(&cl.oldframe, 0, sizeof(cl.oldframe));
#if USE_FPS
    memset(&cl.oldkeyframe, 0, sizeof(cl.oldkeyframe));
#endif

    // clear old effects
    CL_ClearEffects();
    CL_ClearTEnts();

    // fix time delta
    cl.serverdelta += cl.frame.number - prev;

    // fire up destination frame
    CL_DeltaFrame();

    if (cls.demo.recording && !cls.demo.paused)
        resume_record();

    update_status();

    cl.frameflags = 0;

done:
    cls.demo.seeking = false;
}

static void parse_info_string(demoInfo_t *info, int clientNum, int index, const char* string, const cs_remap_t *csr)
{
    char *p;

    if (index >= csr->playerskins && index < csr->playerskins + MAX_CLIENTS) {
        if (index - csr->playerskins == clientNum) {
            Q_strlcpy(info->pov, string, sizeof(info->pov));
            p = strchr(info->pov, '\\');
            if (p) {
                *p = 0;
            }
        }
    } else if (index == csr->models + 1) {
        Com_ParseMapName(info->map, string, sizeof(info->map));
    }
}

/*
====================
CL_GetDemoInfo
====================
*/
bool CL_GetDemoInfo(const char *path, demoInfo_t *info)
{
    qhandle_t f;
    int c, index, clientNum, flags, type;
    const cs_remap_t *csr = &cs_remap_old;
    bool res = false;

    FS_OpenFile(path, &f, FS_MODE_READ | FS_FLAG_GZIP);
    if (!f) {
        return false;
    }

    nonfatal_client_read_errors = true;

    type = read_first_message(f);
    if (type < 0) {
        goto fail;
    }

    info->mvd = type;

    if (type == 0) {
        q2proto_clientcontext_t demo_context;
        q2proto_init_clientcontext(&demo_context);

        q2proto_svc_message_t message;
        if (q2proto_client_read(&demo_context, Q2PROTO_IOARG_CLIENT_READ, &message) != Q2P_ERR_SUCCESS)
            goto fail;

        if (message.type != Q2P_SVC_SERVERDATA) {
            goto fail;
        }
        switch(demo_context.features.server_game_api)
        {
        case Q2PROTO_GAME_VANILLA:
            break;
        case Q2PROTO_GAME_Q2PRO_EXTENDED:
        case Q2PROTO_GAME_Q2PRO_EXTENDED_V2:
            csr = &cs_remap_q2pro_new;
            break;
        case Q2PROTO_GAME_RERELEASE:
            csr = &cs_remap_rerelease;
            break;
        }
        clientNum = message.serverdata.clientnum;

        while (1) {
            q2proto_error_t err = q2proto_client_read(&demo_context, Q2PROTO_IOARG_CLIENT_READ, &message);
            if (err == Q2P_ERR_NO_MORE_INPUT) {
                if (read_next_message(f) <= 0) {
                    break;
                }
                continue; // parse new message
            }
            if (message.type != Q2P_SVC_CONFIGSTRING) {
                break;
            }
            if (message.configstring.index < 0 || message.configstring.index >= csr->end) {
                goto fail;
            }
            parse_info_string(info, clientNum, message.configstring.index, message.configstring.value.str, csr);
        }
    } else {
        c = MSG_ReadByte();
        if ((c & SVCMD_MASK) != mvd_serverdata) {
            goto fail;
        }
        int mvd_protocol = MSG_ReadLong();
        if (mvd_protocol != PROTOCOL_VERSION_MVD) {
            goto fail;
        }
        int protocol_version = MSG_ReadWord();
        if (!MVD_SUPPORTED(protocol_version)) {
            goto fail;
        }
        if (protocol_version >= PROTOCOL_VERSION_MVD_EXTENDED_LIMITS_2) {
            flags = MSG_ReadWord();
        } else {
            flags = c >> SVCMD_BITS;
        }
        if (protocol_version == PROTOCOL_VERSION_MVD_RERELEASE) {
            csr = &cs_remap_rerelease;
        } else if (flags & MVF_EXTLIMITS) {
            csr = &cs_remap_q2pro_new;
        }
        MSG_ReadLong();
        MSG_ReadString(NULL, 0);
        clientNum = MSG_ReadShort();

        while (1) {
            index = MSG_ReadWord();
            if (index == csr->end) {
                break;
            }
            if (index < 0 || index >= csr->end) {
                goto fail;
            }
            char string[MAX_QPATH];
            MSG_ReadString(string, sizeof(string));
            parse_info_string(info, clientNum, index, string, csr);
        }
    }
    res = true;

fail:
    FS_CloseFile(f);
    nonfatal_client_read_errors = false;
    return res;
}

// =========================================================================

void CL_CleanupDemos(void)
{
    if (cls.demo.recording) {
        CL_Stop_f();
    }

    if (cls.demo.playback) {
        FS_CloseFile(cls.demo.playback);

        if (com_timedemo->integer && cls.demo.time_frames) {
            unsigned msec = Sys_Milliseconds();

            if (msec > cls.demo.time_start) {
                float sec = (msec - cls.demo.time_start) * 0.001f;
                float fps = cls.demo.time_frames / sec;

                Com_Printf("%u frames, %3.1f seconds: %3.1f fps\n",
                           cls.demo.time_frames, sec, fps);
            }
        }

        // clear whatever stufftext remains
        if (!cls.demo.compat)
            Cbuf_Clear(&cl_cmdbuf);
    }

    CL_FreeDemoSnapshots();

    memset(&cls.demo, 0, sizeof(cls.demo));
}

/*
====================
CL_DemoFrame
====================
*/
void CL_DemoFrame(void)
{
    if (!cls.demo.playback) {
        return;
    }

    if (cls.state != ca_active) {
        parse_next_message(0);
        return;
    }

    if (com_timedemo->integer) {
        parse_next_message(0);
        cl.time = cl.servertime;
        cls.demo.time_frames++;
        return;
    }

    // wait at the end of demo
    if (cls.demo.eof) {
        if (!cl_demowait->integer)
            finish_demo(0);
        return;
    }

    // cl.time has already been advanced for this client frame
    // read the next frame to start lerp cycle again
    while (cl.servertime < cl.time) {
        if (parse_next_message(cl_demowait->integer))
            break;
        if (cls.state != ca_active)
            break;
    }
}

static const cmdreg_t c_demo[] = {
    { "demo", CL_PlayDemo_f, CL_Demo_c },
    { "record", CL_Record_f, CL_Demo_c },
    { "stop", CL_Stop_f },
    { "suspend", CL_Suspend_f },
    { "resume", CL_Resume_f },
    { "seek", CL_Seek_f },

    { NULL }
};

/*
====================
CL_InitDemos
====================
*/
void CL_InitDemos(void)
{
    cl_demosnaps = Cvar_Get("cl_demosnaps", "10", 0);
    cl_demomsglen = Cvar_Get("cl_demomsglen", va("%d", MAX_PACKETLEN_WRITABLE_DEFAULT), 0);
    cl_demowait = Cvar_Get("cl_demowait", "0", 0);
    cl_demosuspendtoggle = Cvar_Get("cl_demosuspendtoggle", "1", 0);

    Cmd_Register(c_demo);
}

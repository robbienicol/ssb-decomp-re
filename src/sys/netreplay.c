#include <sys/netreplay.h>
#include <sys/netsync.h>

#include <ft/fighter.h>
#include <if/ifcommon.h>
#include <sc/scmanager.h>
#include <sys/netinput.h>
#include <sys/utils.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PORT
extern void port_log(const char *fmt, ...);
extern void port_exit_process(int code);
extern char *getenv(const char *name);
extern int atoi(const char *s);
extern int strncmp(const char *a, const char *b, __SIZE_TYPE__ n);
extern unsigned long strtoul(const char *s, char **end, int base);
extern int snprintf(char *buf, __SIZE_TYPE__ cap, const char *fmt, ...);
#endif

#define SYNETREPLAY_DEFAULT_RECORD_FRAMES 1800

typedef struct SYNetReplayFileHeader
{
	u32 magic;
	u32 version;
	u32 metadata_size;
	u32 frame_size;
	u32 frame_count;
	u32 player_count;
	u32 input_checksum;

} SYNetReplayFileHeader;

const char *sSYNetReplayRecordPath;
const char *sSYNetReplayPlayPath;
u32 sSYNetReplayRecordFrameLimit = SYNETREPLAY_DEFAULT_RECORD_FRAMES;
u32 sSYNetReplayLoadedFrameCount;
u32 sSYNetReplayLoadedInputChecksum;
sb32 sSYNetReplayIsRecording;
sb32 sSYNetReplayIsRecordWritten;
sb32 sSYNetReplayIsPlaybackLoaded;
sb32 sSYNetReplayIsPlaybackActive;
sb32 sSYNetReplayIsPlaybackVerified;
/* SSB64_RIG_EXIT=1: turn the playback verify verdict into the process exit
 * code (0 PASS, 1 FAIL, 2 playback ended before the replay did). */
sb32 sSYNetReplayRigExit;
SYNetInputReplayMetadata sSYNetReplayLoadedMetadata;
/* SSB64_NETPLAY_BATTLE: an online match set up by the website. */
sb32 sSYNetReplayIsNetplayBattle;
SYNetInputReplayMetadata sSYNetReplayNetplayMetadata;
SYNetInputFrame sSYNetReplayLoadedFrames[MAXCONTROLLERS][SYNETINPUT_REPLAY_MAX_FRAMES];

void syNetReplayClearLoadedFrames(void)
{
	s32 player;
	s32 tick;

	sSYNetReplayLoadedFrameCount = 0;
	sSYNetReplayLoadedInputChecksum = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		for (tick = 0; tick < SYNETINPUT_REPLAY_MAX_FRAMES; tick++)
		{
			memset(&sSYNetReplayLoadedFrames[player][tick], 0, sizeof(SYNetInputFrame));
		}
	}
}

void syNetReplayCaptureBattleMetadata(SCBattleState *battle_state, SYNetInputReplayMetadata *metadata)
{
	s32 player;

	memset(metadata, 0, sizeof(SYNetInputReplayMetadata));

	metadata->magic = SYNETINPUT_REPLAY_MAGIC;
	metadata->version = SYNETINPUT_REPLAY_VERSION;
	metadata->scene_kind = nSCKindVSBattle;
	metadata->rng_seed = syUtilsRandSeed();

	if (battle_state == NULL)
	{
		return;
	}
	metadata->player_count = battle_state->pl_count + battle_state->cp_count;
	metadata->stage_kind = battle_state->gkind;
	metadata->stocks = battle_state->stocks;
	metadata->time_limit = battle_state->time_limit;
	metadata->item_switch = battle_state->item_appearance_rate;
	metadata->item_toggles = battle_state->item_toggles;
	metadata->game_type = battle_state->game_type;
	metadata->game_rules = battle_state->game_rules;
	metadata->is_team_battle = battle_state->is_team_battle;
	metadata->handicap = battle_state->handicap;
	metadata->is_team_attack = battle_state->is_team_attack;
	metadata->is_stage_select = battle_state->is_stage_select;
	metadata->damage_ratio = battle_state->damage_ratio;
	metadata->item_appearance_rate = battle_state->item_appearance_rate;
	metadata->is_not_teamshadows = battle_state->is_not_teamshadows;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		metadata->player_kinds[player] = battle_state->players[player].pkind;
		metadata->fighter_kinds[player] = battle_state->players[player].fkind;
		metadata->costumes[player] = battle_state->players[player].costume;
		metadata->teams[player] = battle_state->players[player].team;
		metadata->handicaps[player] = battle_state->players[player].handicap;
		metadata->levels[player] = battle_state->players[player].level;
		metadata->shades[player] = battle_state->players[player].shade;
	}
}

void syNetReplayApplyBattleMetadata(const SYNetInputReplayMetadata *metadata)
{
	SCBattleState *battle_state = &gSCManagerTransferBattleState;
	s32 player;

	*battle_state = dSCManagerDefaultBattleState;
	battle_state->game_type = metadata->game_type;
	battle_state->gkind = metadata->stage_kind;
	battle_state->is_team_battle = metadata->is_team_battle;
	battle_state->game_rules = metadata->game_rules;
	battle_state->time_limit = metadata->time_limit;
	battle_state->stocks = metadata->stocks;
	battle_state->handicap = metadata->handicap;
	battle_state->is_team_attack = metadata->is_team_attack;
	battle_state->is_stage_select = FALSE;
	battle_state->damage_ratio = metadata->damage_ratio;
	battle_state->item_toggles = metadata->item_toggles;
	battle_state->item_appearance_rate = metadata->item_appearance_rate;
	battle_state->is_not_teamshadows = metadata->is_not_teamshadows;
	battle_state->pl_count = 0;
	battle_state->cp_count = 0;

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		u8 handicap = metadata->handicaps[player];

		/* A replay file is untrusted input: the per-player handicap indexes
		 * dFTCommonDataHandicapTable[handicap - 1] in ftParamGetCommonKnockback,
		 * so 0 (a zero-filled or corrupt file) reads one row BEFORE the table —
		 * caught by ASan as a global-buffer-overflow during hit processing. The
		 * game's own paths keep it in 1..9 (CSS default 9 = neutral); clamp to
		 * the same domain here. */
		if ((handicap < 1) || (handicap > 9))
		{
			handicap = 9;
		}

		battle_state->players[player].player = (metadata->is_team_battle != FALSE) ? metadata->teams[player] : player;
		battle_state->players[player].team = metadata->teams[player];
		battle_state->players[player].pkind = metadata->player_kinds[player];
		battle_state->players[player].fkind = metadata->fighter_kinds[player];
		battle_state->players[player].costume = metadata->costumes[player];
		battle_state->players[player].shade = metadata->shades[player];
		battle_state->players[player].handicap = handicap;
		battle_state->players[player].level = metadata->levels[player];
		battle_state->players[player].tag = (metadata->player_kinds[player] == nFTPlayerKindMan) ? player : GMCOMMON_PLAYERS_MAX;
		battle_state->players[player].is_single_stockicon = (metadata->game_rules & SCBATTLE_GAMERULE_TIME) ? TRUE : FALSE;

		if (metadata->player_kinds[player] == nFTPlayerKindMan)
		{
			battle_state->players[player].color =
			(metadata->is_team_battle == FALSE) ? player : dIFCommonPlayerTeamColorIDs[metadata->teams[player]];
			battle_state->pl_count++;
		}
		else if (metadata->player_kinds[player] == nFTPlayerKindCom)
		{
			battle_state->players[player].color =
			(metadata->is_team_battle == FALSE) ? GMCOMMON_PLAYERS_MAX : dIFCommonPlayerTeamColorIDs[metadata->teams[player]];
			battle_state->cp_count++;
		}
	}
	gSCManagerSceneData.gkind = metadata->stage_kind;
}

#ifdef PORT
/* Parses up to max comma-separated integers; stops at a space or ';'. */
static s32 syNetReplayParseList(const char *value, s32 *out, s32 max)
{
	s32 count = 0;

	while ((*value != '\0') && (*value != ' ') && (*value != ';') && (count < max))
	{
		out[count++] = atoi(value);

		while ((*value != '\0') && (*value != ',') && (*value != ' ') && (*value != ';'))
		{
			value++;
		}
		if (*value == ',')
		{
			value++;
		}
	}
	return count;
}

/* Online matches set up by the website boot straight into a VS battle. Every
 * peer gets the same string, e.g.
 *   SSB64_NETPLAY_BATTLE="stage=6 seed=1234 stocks=4 fighters=0,1 costumes=0,1"
 * Optional: time=<minutes, 100 = no limit; competitive ruleset only>
 * teams=<team per player>
 * items=<appearance rate 0-5> damage=<percent>. */
static sb32 syNetReplayParseBattleSpec(const char *spec, SYNetInputReplayMetadata *m)
{
	s32 fighters[MAXCONTROLLERS] = { 0 };
	s32 costumes[MAXCONTROLLERS] = { 0 };
	s32 teams[MAXCONTROLLERS] = { 0, 1, 2, 3 };
	s32 count = 0;
	sb32 is_teams = FALSE;
	s32 player;
	const char *p = spec;

	memset(m, 0, sizeof(*m));
	m->magic = SYNETINPUT_REPLAY_MAGIC;
	m->version = SYNETINPUT_REPLAY_VERSION;
	m->scene_kind = nSCKindVSBattle;
	m->stage_kind = 6; /* Dream Land */
	m->stocks = 3; /* four lives */
	m->time_limit = 100; /* no limit */
	m->rng_seed = 1;
	m->game_type = 1;
	m->game_rules = 0x2; /* stock */
	m->damage_ratio = 100;
	m->item_appearance_rate = nSCBattleItemSwitchNone;

	while (*p != '\0')
	{
		s32 value[1];

		while ((*p == ' ') || (*p == ';'))
		{
			p++;
		}
		if (strncmp(p, "stage=", 6) == 0)
		{
			m->stage_kind = atoi(p + 6);
		}
		else if (strncmp(p, "seed=", 5) == 0)
		{
			m->rng_seed = (u32)strtoul(p + 5, NULL, 10);
		}
		else if (strncmp(p, "stocks=", 7) == 0)
		{
			/* the game counts stocks from 0: 4 stocks are stored as 3 */
			syNetReplayParseList(p + 7, value, 1);
			m->stocks = (value[0] > 0) ? value[0] - 1 : 0;
		}
		else if (strncmp(p, "time=", 5) == 0)
		{
			m->time_limit = atoi(p + 5);
		}
		else if (strncmp(p, "items=", 6) == 0)
		{
			m->item_appearance_rate = atoi(p + 6);
		}
		else if (strncmp(p, "damage=", 7) == 0)
		{
			m->damage_ratio = atoi(p + 7);
		}
		else if (strncmp(p, "fighters=", 9) == 0)
		{
			count = syNetReplayParseList(p + 9, fighters, MAXCONTROLLERS);
		}
		else if (strncmp(p, "costumes=", 9) == 0)
		{
			syNetReplayParseList(p + 9, costumes, MAXCONTROLLERS);
		}
		else if (strncmp(p, "teams=", 6) == 0)
		{
			syNetReplayParseList(p + 6, teams, MAXCONTROLLERS);
			is_teams = TRUE;
		}
		while ((*p != '\0') && (*p != ' ') && (*p != ';'))
		{
			p++;
		}
	}
	if ((count < 2) || (count > MAXCONTROLLERS))
	{
		return FALSE;
	}
	m->player_count = count;
	m->item_switch = m->item_appearance_rate;
	m->is_team_battle = is_teams;
	m->game_rules = 0x2; /* stock (the time limit applies under the competitive ruleset) */

	for (player = 0; player < MAXCONTROLLERS; player++)
	{
		m->player_kinds[player] = (player < count) ? nFTPlayerKindMan : nFTPlayerKindNot;
		m->fighter_kinds[player] = (player < count) ? fighters[player] : 0;
		m->costumes[player] = costumes[player];
		m->teams[player] = teams[player];
		m->handicaps[player] = 9;
		m->levels[player] = 1;
	}
	return TRUE;
}

/* The battle's game_status (nSCBattleGameStatus*): Wait during the opening
 * countdown, Go while fighting, End then Wait again once it is decided. */
s32 syNetReplayGetGameStatus(void)
{
	return (gSCManagerBattleState != NULL) ? gSCManagerBattleState->game_status : -1;
}

/* Cheap per-frame checksum for rollback desync detection: the RNG plus every
 * player's score/damage counters. Reads only plain battle state (no object
 * lists), so it is safe while fighters are being removed. */
u32 syNetReplayQuickChecksum(void)
{
	SCBattleState *bs = gSCManagerBattleState;
	u32 h = 2166136261U;
	s32 i;

#define SYNETREPLAY_MIX(v) (h = (h ^ (u32)(v)) * 16777619U)
	SYNETREPLAY_MIX(syUtilsRandSeed());
	if (bs == NULL)
	{
		return h;
	}
	SYNETREPLAY_MIX(bs->game_status);
	SYNETREPLAY_MIX(bs->time_passed);
	for (i = 0; i < GMCOMMON_PLAYERS_MAX; i++)
	{
		SCPlayerData *pl = &bs->players[i];

		SYNETREPLAY_MIX(pl->stock_count);
		SYNETREPLAY_MIX(pl->falls);
		SYNETREPLAY_MIX(pl->score);
		SYNETREPLAY_MIX(pl->total_damage_given);
		SYNETREPLAY_MIX(pl->total_damage_all);
		SYNETREPLAY_MIX(pl->stock_damage_all);
		SYNETREPLAY_MIX(pl->combo_count_foe);
	}
#undef SYNETREPLAY_MIX
	return h;
}

/* Final standings of the VS battle as JSON, for the website. Stock battles
 * assign place as players are eliminated; the winner keeps place 0. */
s32 syNetReplayDescribeResults(char *buf, s32 cap)
{
	SCBattleState *bs = gSCManagerBattleState;
	const char *sep = "";
	s32 at;
	s32 i;
	s32 j;

	if (bs == NULL)
	{
		return snprintf(buf, cap, "{\"players\":[]}");
	}
	at = snprintf(buf, cap, "{\"stage\":%d,\"time\":%u,\"players\":[", bs->gkind, bs->time_passed);

	for (i = 0; (i < GMCOMMON_PLAYERS_MAX) && (at < cap); i++)
	{
		SCPlayerData *pl = &bs->players[i];
		s32 kos = 0;

		if (pl->pkind == nFTPlayerKindNot)
		{
			continue;
		}
		for (j = 0; j < GMCOMMON_PLAYERS_MAX; j++)
		{
			kos += pl->total_kos_players[j];
		}
		at += snprintf(buf + at, cap - at,
		               "%s{\"slot\":%d,\"fighter\":%d,\"place\":%d,\"stocks\":%d,\"falls\":%d,"
		               "\"kos\":%d,\"sds\":%d,\"damage\":%d}",
		               sep, i, pl->fkind, pl->place, pl->stock_count, pl->falls, kos, pl->total_selfdestructs,
		               pl->total_damage_given);
		sep = ",";
	}
	if (at < cap)
	{
		at += snprintf(buf + at, cap - at, "]}");
	}
	return (at < cap) ? at : cap - 1;
}
#endif

void syNetReplayInitDebugEnv(void)
{
#ifdef PORT
	const char *frame_limit_env;

	sSYNetReplayRecordPath = getenv("SSB64_REPLAY_RECORD");
	sSYNetReplayPlayPath = getenv("SSB64_REPLAY_PLAY");
	frame_limit_env = getenv("SSB64_REPLAY_RECORD_FRAMES");
	{
		const char *rig_exit_env = getenv("SSB64_RIG_EXIT");

		sSYNetReplayRigExit = ((rig_exit_env != NULL) && (atoi(rig_exit_env) != 0)) ? TRUE : FALSE;
	}

	if (frame_limit_env != NULL)
	{
		s32 frame_limit = atoi(frame_limit_env);

		if ((frame_limit > 0) && (frame_limit < SYNETINPUT_REPLAY_MAX_FRAMES))
		{
			sSYNetReplayRecordFrameLimit = frame_limit;
		}
	}
	if ((sSYNetReplayPlayPath == NULL) && (getenv("SSB64_NETPLAY_BATTLE") != NULL))
	{
		const char *spec = getenv("SSB64_NETPLAY_BATTLE");

		if (syNetReplayParseBattleSpec(spec, &sSYNetReplayNetplayMetadata) != FALSE)
		{
			sSYNetReplayIsNetplayBattle = TRUE;
			syNetReplayApplyBattleMetadata(&sSYNetReplayNetplayMetadata);
			syUtilsSetRandomSeed(sSYNetReplayNetplayMetadata.rng_seed);
			gSCManagerSceneData.scene_prev = nSCKindVSMode;
			gSCManagerSceneData.scene_curr = nSCKindVSBattle;
			port_log("SSB64 Netplay: battle spec \"%s\" stage=%u players=%u seed=%u\n", spec,
			         sSYNetReplayNetplayMetadata.stage_kind, sSYNetReplayNetplayMetadata.player_count,
			         sSYNetReplayNetplayMetadata.rng_seed);
		}
		else
		{
			port_log("SSB64 Netplay: bad battle spec \"%s\"\n", spec);
		}
	}
	if (sSYNetReplayPlayPath != NULL)
	{
		if (syNetReplayLoadDebugFile(sSYNetReplayPlayPath) != FALSE)
		{
			syNetReplayApplyBattleMetadata(&sSYNetReplayLoadedMetadata);
			syUtilsSetRandomSeed(sSYNetReplayLoadedMetadata.rng_seed);
			gSCManagerSceneData.scene_prev = nSCKindVSMode;
			gSCManagerSceneData.scene_curr = nSCKindVSBattle;
		}
		else
		{
			/* Nothing will arm playback now, so no verdict can ever be produced;
			 * without this a batch run would sit at the title screen forever. */
			port_log("SSB64 Replay: playback load failed path=%s result=LOADFAIL\n", sSYNetReplayPlayPath);

			if (sSYNetReplayRigExit != FALSE)
			{
				port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", 3);
				syNetSyncFinishVSSession();
				port_exit_process(3);
			}
		}
	}
#endif
}

void syNetReplayStartVSSession(SCBattleState *battle_state)
{
	SYNetInputReplayMetadata metadata;
	u32 tick;
	s32 player;

	if (sSYNetReplayIsNetplayBattle != FALSE)
	{
		/* Every peer starts the battle from the same seed, whatever ran before. */
		syUtilsSetRandomSeed(sSYNetReplayNetplayMetadata.rng_seed);
	}
	if (sSYNetReplayIsPlaybackLoaded != FALSE)
	{
		syNetInputClearReplayFrames();
		syNetInputSetReplayMetadata(&sSYNetReplayLoadedMetadata);
		syUtilsSetRandomSeed(sSYNetReplayLoadedMetadata.rng_seed);

		for (tick = 0; tick < sSYNetReplayLoadedFrameCount; tick++)
		{
			for (player = 0; player < MAXCONTROLLERS; player++)
			{
				syNetInputSetReplayFrame(player, tick, &sSYNetReplayLoadedFrames[player][tick]);
			}
		}
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			syNetInputSetSlotSource(player, nSYNetInputSourceSaved);
		}
		sSYNetReplayIsPlaybackActive = TRUE;
		sSYNetReplayIsPlaybackVerified = FALSE;
		syNetInputSetPublishedChecksumLimit(sSYNetReplayLoadedFrameCount);

#ifdef PORT
		port_log("SSB64 Replay: playback start path=%s frames=%u checksum=0x%08X stage=%u seed=%u\n",
		         sSYNetReplayPlayPath, sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum,
		         sSYNetReplayLoadedMetadata.stage_kind, sSYNetReplayLoadedMetadata.rng_seed);
#endif
		return;
	}
	if (sSYNetReplayRecordPath != NULL)
	{
		syNetReplayCaptureBattleMetadata(battle_state, &metadata);
		syNetInputClearReplayFrames();
		syNetInputSetReplayMetadata(&metadata);
		syNetInputSetRecordingEnabled(TRUE);
		sSYNetReplayIsRecording = TRUE;
		sSYNetReplayIsRecordWritten = FALSE;

#ifdef PORT
		port_log("SSB64 Replay: recording start path=%s limit=%u stage=%u seed=%u players=%u\n",
		         sSYNetReplayRecordPath, sSYNetReplayRecordFrameLimit, metadata.stage_kind,
		         metadata.rng_seed, metadata.player_count);
#endif
	}
}

void syNetReplayUpdate(void)
{
	if ((sSYNetReplayIsRecording != FALSE) && (sSYNetReplayIsRecordWritten == FALSE) &&
		(syNetInputGetRecordedFrameCount() >= sSYNetReplayRecordFrameLimit))
	{
		syNetReplayFinishVSSession();
	}
	if ((sSYNetReplayIsPlaybackActive != FALSE) && (sSYNetReplayIsPlaybackVerified == FALSE) &&
		(syNetInputGetPublishedTickCount() >= sSYNetReplayLoadedFrameCount))
	{
		/* netinput froze the published-input checksum at exactly frame_count
		 * advanced ticks (see syNetInputSetPublishedChecksumLimit), so this
		 * compares the same tick range the recorder hashed. */
		u32 checksum = syNetInputGetPublishedInputChecksum();
		sb32 is_pass = (checksum == sSYNetReplayLoadedInputChecksum) ? TRUE : FALSE;

#ifdef PORT
		port_log("SSB64 Replay: playback verify frames=%u expected=0x%08X actual=0x%08X result=%s\n",
		         sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum, checksum,
		         (is_pass != FALSE) ? "PASS" : "FAIL");
#endif
		sSYNetReplayIsPlaybackVerified = TRUE;
		sSYNetReplayIsPlaybackActive = FALSE;

#ifdef PORT
		if (sSYNetReplayRigExit != FALSE)
		{
			/* Batch/rig mode: report through the exit code. port_exit_process()
			 * terminates immediately - normal teardown from the game coroutine
			 * is not safe (render/audio threads are mid-frame). Inputs that
			 * replayed identically but a gameplay state trace that diverged
			 * (SSB64_SYNC_VERIFY) is its own code: the sim is nondeterministic. */
			s32 exit_code = (is_pass != FALSE) ? 0 : 1;

			if (is_pass != FALSE)
			{
				switch (syNetSyncGetVerifyResult())
				{
				case nSYNetSyncVerifyNotLoaded:
					/* an absent oracle must never read as PASS */
					port_log("SSB64 Replay: inputs PASS but the state trace to verify against did not load result=LOADFAIL\n");
					exit_code = 3;
					break;
				case nSYNetSyncVerifyDiverged:
					port_log("SSB64 Replay: inputs PASS but state trace diverged result=DESYNC\n");
					exit_code = 4;
					break;
				case nSYNetSyncVerifyShort:
					port_log("SSB64 Replay: inputs PASS but the state trace ended before the replay did result=DESYNC\n");
					exit_code = 4;
					break;
				default:
					break;
				}
			}
			port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", exit_code);
			syNetSyncFinishVSSession();
			port_exit_process(exit_code);
		}
#endif
	}
}

void syNetReplayFinishVSSession(void)
{
	if ((sSYNetReplayIsPlaybackActive != FALSE) && (sSYNetReplayIsPlaybackVerified == FALSE))
	{
		/* The match ended before the replay stream did (stocks/time ran out
		 * earlier than the recording expected), so there is nothing to verify
		 * against - report it rather than leaving a batch run waiting. */
#ifdef PORT
		port_log("SSB64 Replay: playback ended early ticks=%u of %u result=INCOMPLETE\n",
		         syNetInputGetPublishedTickCount(), sSYNetReplayLoadedFrameCount);
#endif
		sSYNetReplayIsPlaybackVerified = TRUE;
		sSYNetReplayIsPlaybackActive = FALSE;

#ifdef PORT
		if (sSYNetReplayRigExit != FALSE)
		{
			/* Batch/rig mode: report through the exit code. port_exit_process()
			 * terminates immediately - normal teardown from the game coroutine
			 * is not safe (render/audio threads are mid-frame). */
			/* the input stream could not be verified, but a state trace that
			 * already diverged is the stronger verdict and must not be masked */
			s32 exit_code = (syNetSyncGetVerifyResult() == nSYNetSyncVerifyDiverged) ? 4 : 2;

			if (exit_code == 4)
			{
				port_log("SSB64 Replay: match ended early and the state trace diverged result=DESYNC\n");
			}
			port_log("SSB64 Replay: SSB64_RIG_EXIT set, exiting with code %d\n", exit_code);
			syNetSyncFinishVSSession();
			port_exit_process(exit_code);
		}
#endif
	}
	if ((sSYNetReplayIsRecording != FALSE) && (sSYNetReplayIsRecordWritten == FALSE))
	{
		syNetReplayWriteDebugFile(sSYNetReplayRecordPath);
		syNetInputSetRecordingEnabled(FALSE);
		sSYNetReplayIsRecording = FALSE;
		sSYNetReplayIsRecordWritten = TRUE;
	}
}

sb32 syNetReplayWriteDebugFile(const char *path)
{
	SYNetReplayFileHeader header;
	SYNetInputReplayMetadata metadata;
	SYNetInputFrame frame;
	FILE *fp;
	u32 tick;
	s32 player;

	if ((path == NULL) || (syNetInputGetReplayMetadata(&metadata) == FALSE))
	{
		return FALSE;
	}
	fp = fopen(path, "wb");

	if (fp == NULL)
	{
#ifdef PORT
		port_log("SSB64 Replay: failed to open record path=%s\n", path);
#endif
		return FALSE;
	}
	header.magic = SYNETINPUT_REPLAY_MAGIC;
	header.version = SYNETINPUT_REPLAY_VERSION;
	header.metadata_size = sizeof(SYNetInputReplayMetadata);
	header.frame_size = sizeof(SYNetInputFrame);
	header.frame_count = syNetInputGetRecordedFrameCount();
	header.player_count = MAXCONTROLLERS;
	header.input_checksum = syNetInputGetReplayInputChecksum();

	fwrite(&header, sizeof(header), 1, fp);
	fwrite(&metadata, sizeof(metadata), 1, fp);

	for (tick = 0; tick < header.frame_count; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (syNetInputGetReplayFrame(player, tick, &frame) == FALSE)
			{
				memset(&frame, 0, sizeof(frame));
				frame.tick = tick;
			}
			fwrite(&frame, sizeof(frame), 1, fp);
		}
	}
	fclose(fp);

#ifdef PORT
	port_log("SSB64 Replay: wrote path=%s frames=%u checksum=0x%08X\n",
	         path, header.frame_count, header.input_checksum);
#endif
	return TRUE;
}

sb32 syNetReplayLoadDebugFile(const char *path)
{
	SYNetReplayFileHeader header;
	FILE *fp;
	u32 tick;
	s32 player;

	if (path == NULL)
	{
		return FALSE;
	}
	fp = fopen(path, "rb");

	if (fp == NULL)
	{
#ifdef PORT
		port_log("SSB64 Replay: failed to open playback path=%s\n", path);
#endif
		return FALSE;
	}
	if (fread(&header, sizeof(header), 1, fp) != 1)
	{
		fclose(fp);
		return FALSE;
	}
	if ((header.magic != SYNETINPUT_REPLAY_MAGIC) ||
		(header.version != SYNETINPUT_REPLAY_VERSION) ||
		(header.metadata_size != sizeof(SYNetInputReplayMetadata)) ||
		(header.frame_size != sizeof(SYNetInputFrame)) ||
		(header.frame_count == 0) || (header.frame_count > SYNETINPUT_REPLAY_MAX_FRAMES) ||
		(header.player_count != MAXCONTROLLERS))
	{
#ifdef PORT
		port_log("SSB64 Replay: rejected playback header path=%s magic=0x%08X version=%u frames=%u players=%u\n",
		         path, header.magic, header.version, header.frame_count, header.player_count);
#endif
		fclose(fp);
		return FALSE;
	}
	if (fread(&sSYNetReplayLoadedMetadata, sizeof(sSYNetReplayLoadedMetadata), 1, fp) != 1)
	{
		fclose(fp);
		return FALSE;
	}
	syNetReplayClearLoadedFrames();
	sSYNetReplayLoadedFrameCount = header.frame_count;
	sSYNetReplayLoadedInputChecksum = header.input_checksum;

	for (tick = 0; tick < header.frame_count; tick++)
	{
		for (player = 0; player < MAXCONTROLLERS; player++)
		{
			if (fread(&sSYNetReplayLoadedFrames[player][tick], sizeof(SYNetInputFrame), 1, fp) != 1)
			{
				fclose(fp);
				return FALSE;
			}
		}
	}
	fclose(fp);
	sSYNetReplayIsPlaybackLoaded = TRUE;

#ifdef PORT
	port_log("SSB64 Replay: loaded path=%s frames=%u checksum=0x%08X stage=%u seed=%u\n",
	         path, sSYNetReplayLoadedFrameCount, sSYNetReplayLoadedInputChecksum,
	         sSYNetReplayLoadedMetadata.stage_kind, sSYNetReplayLoadedMetadata.rng_seed);
#endif
	return TRUE;
}

/* Reads one frame of the loaded playback file (rollback tests drive the
 * local player from it). */
sb32 syNetReplayGetLoadedFrame(s32 player, u32 tick, SYNetInputFrame *out_frame)
{
	if ((sSYNetReplayIsPlaybackLoaded == FALSE) || (player < 0) || (player >= MAXCONTROLLERS) ||
		(tick >= sSYNetReplayLoadedFrameCount) || (out_frame == NULL))
	{
		return FALSE;
	}
	*out_frame = sSYNetReplayLoadedFrames[player][tick];
	return TRUE;
}

// The game's side of the touch screen's race display (3ds/bottom.h): once a
// frame, where the game draws its own map, it writes down who is racing, in
// which place and where on the map, and this console's speed, bananas and lap.
// Compiled as part of src/game_ui.c. Nothing here changes the race: it only
// reads, apart from loading each racer's portrait once per level, which every
// console of a session does at the same frame.

#include "../3ds/bottom.h"
#include "../3ds/characters.h"
#include "../3ds/netplay.h"

static BottomSnapshot sBottom;
static u8 sBottomMap[BOTTOM_MAP_MAX * BOTTOM_MAP_MAX];
static s32 sBottomMapOriginX, sBottomMapOriginY; // the map picture's corner in the sprite's own coordinates
static s32 sBottomMapStale = TRUE;               // minimap_init has loaded another level's map
static unsigned sBottomPortraits[BOTTOM_MAX_RACERS][BOTTOM_TEXTURE_MAX * BOTTOM_TEXTURE_MAX];
static s16 sBottomPortraitKey[BOTTOM_MAX_RACERS]; // the character each one was made for, + 1

// In the order of the game's character numbers (the Character enum in
// include/enums.h). The portrait textures follow the same numbers; the names
// of the gMenuPortrait* variables in src/menu.c do not, and are no guide.
static const char *const sBottomNames[NUMBER_OF_CHARACTERS] = {
    [CHARACTER_KRUNCH] = "KRUNCH",       [CHARACTER_BUMPER] = "BUMPER", [CHARACTER_TIPTUP] = "TIPTUP",
    [CHARACTER_CONKER] = "CONKER",       [CHARACTER_TIMBER] = "TIMBER", [CHARACTER_BANJO] = "BANJO",
    [CHARACTER_DRUMSTICK] = "DRUMSTICK", [CHARACTER_PIPSY] = "PIPSY",   [CHARACTER_TT] = "T.T.",
    [CHARACTER_DIDDY] = "DIDDY",
};

const BottomSnapshot *pc_bottom_snapshot(void) {
    return &sBottom;
}

/**
 * Whether the touch screen has this level's race display: then the banana
 * count and the map are shown there and left off the top screen
 * (hud_bananas, hud_render_general). Not with two players on one console,
 * who have one touch screen between them. The same on every console of a
 * session whatever its settings, as everything is that decides what is
 * drawn.
 */
static s32 bottom_has_race(void) {
    LevelHeader *header = level_header();

    if (header == NULL || (netplay_players() == 0 && gNumActivePlayers >= 2)) {
        return FALSE;
    }
    return header->race_type == RACETYPE_DEFAULT || header->race_type == RACETYPE_HORSESHOE_GULCH ||
           header->race_type == RACETYPE_BOSS || (header->race_type & RACETYPE_CHALLENGE);
}

/**
 * One texel of a loaded texture as 0xAARRGGBB. The intensity formats give
 * their intensity as alpha too, which is how the game draws its map.
 * Rows of a texture with RENDER_LINE_SWAP are stored the way the N64's
 * texture memory wants them: in odd rows the halves of each eight bytes are
 * exchanged (unswizzle_rows in 3ds/main.c has the whole story).
 */
static u32 bottom_texel(TextureHeader *tex, s32 x, s32 y) {
    const u8 *data = (const u8 *) (tex + 1);
    s32 format = TEX_FORMAT(tex->format);
    s32 texelBytes2 = 2; // twice the bytes per texel
    s32 rowBytes, at, i;
    u8 b[4];
    u32 v;

    switch (format) {
        case TEX_FORMAT_RGBA32:
            texelBytes2 = 8;
            break;
        case TEX_FORMAT_RGBA16:
        case TEX_FORMAT_IA16:
            texelBytes2 = 4;
            break;
        case TEX_FORMAT_I8:
        case TEX_FORMAT_IA8:
            texelBytes2 = 2;
            break;
        case TEX_FORMAT_I4:
        case TEX_FORMAT_IA4:
            texelBytes2 = 1;
            break;
        default:
            return 0; // colour-indexed: the game has none
    }
    rowBytes = (tex->width * texelBytes2 + 1) / 2;
    for (i = 0; i < 4 && i < (texelBytes2 + 1) / 2; i++) {
        at = y * rowBytes + (x * texelBytes2) / 2 + i;
        if ((tex->flags & RENDER_LINE_SWAP) && (y & 1)) {
            s32 from = at ^ (format == TEX_FORMAT_RGBA32 ? 8 : 4);

            if (from < rowBytes * tex->height) {
                at = from;
            }
        }
        b[i] = data[at];
    }
    switch (format) {
        case TEX_FORMAT_RGBA32:
            return ((u32) b[3] << 24) | ((u32) b[0] << 16) | ((u32) b[1] << 8) | b[2];
        case TEX_FORMAT_RGBA16:
            v = (b[0] << 8) | b[1];
            return ((v & 1) ? 0xFF000000u : 0) | ((((v >> 11) & 31) * 255 / 31) << 16) |
                   ((((v >> 6) & 31) * 255 / 31) << 8) | (((v >> 1) & 31) * 255 / 31);
        case TEX_FORMAT_IA16:
            return ((u32) b[1] << 24) | (b[0] * 0x010101u);
        case TEX_FORMAT_IA8:
            return ((u32) ((b[0] & 15) * 17) << 24) | (((b[0] >> 4) * 17) * 0x010101u);
        case TEX_FORMAT_I8:
            return ((u32) b[0] << 24) | (b[0] * 0x010101u);
        case TEX_FORMAT_IA4:
            v = (x & 1) ? (b[0] & 15) : (b[0] >> 4);
            return ((v & 1) ? 0xFF000000u : 0) | ((((v >> 1) * 255) / 7) * 0x010101u);
        default: // TEX_FORMAT_I4
            v = ((x & 1) ? (b[0] & 15) : (b[0] >> 4)) * 17;
            return (v << 24) | (v * 0x010101u);
    }
}

/**
 * The level's map sprite as one picture of coverage. The sprite is a few
 * textures side by side, each with its place in the sprite (sprite_init_frame
 * in src/textures_sprites.c lays the same ones out for drawing).
 */
static void bottom_build_map(LevelModel *model) {
    Sprite *sprite = (Sprite *) model->minimapSpriteIndex;
    s32 minX = 32767, minY = 32767, maxX = -32768, maxY = -32768;
    s32 i, x, y;

    sBottom.mapSerial++;
    sBottom.mapWidth = sBottom.mapHeight = 0;
    sBottom.map = sBottomMap;
    sBottom.mapColour[0] = gMinimapRed;
    sBottom.mapColour[1] = gMinimapGreen;
    sBottom.mapColour[2] = gMinimapBlue;
    if (sprite == NULL || sprite->numberOfTextures <= 0) {
        return;
    }
    for (i = 0; i < sprite->numberOfTextures; i++) {
        TextureHeader *tex = sprite->textures[i];

        if (tex == NULL) {
            return;
        }
        minX = MIN(minX, tex->posX);
        minY = MIN(minY, tex->posY);
        maxX = MAX(maxX, tex->posX + tex->width);
        maxY = MAX(maxY, tex->posY + tex->height);
    }
    if (maxX - minX > BOTTOM_MAP_MAX || maxY - minY > BOTTOM_MAP_MAX) {
        return;
    }
    sBottomMapOriginX = minX;
    sBottomMapOriginY = minY;
    sBottom.mapWidth = maxX - minX;
    sBottom.mapHeight = maxY - minY;
    bzero(sBottomMap, sizeof(sBottomMap));
    for (i = 0; i < sprite->numberOfTextures; i++) {
        TextureHeader *tex = sprite->textures[i];

        for (y = 0; y < tex->height; y++) {
            for (x = 0; x < tex->width; x++) {
                sBottomMap[(tex->posY - minY + y) * sBottom.mapWidth + (tex->posX - minX + x)] =
                    (u8) (bottom_texel(tex, x, y) >> 24);
            }
        }
    }
}

/**
 * Where a point of the track is on the map picture, in sixteenths of a
 * pixel: minimap_marker_pos without the place on the screen.
 */
static void bottom_map_point(LevelModel *model, f32 x, f32 z, int *outX, int *outY) {
    f32 aspect = (model->upperXBounds - model->lowerXBounds) / (f32) (model->upperZBounds - model->lowerZBounds);
    f32 angleCos = coss_f((model->minimapRotation * 0xFFFF) / 360);
    f32 angleSin = sins_f((model->minimapRotation * 0xFFFF) / 360);
    f32 scaledX = (60.0f * aspect * model->minimapXScale * (x - model->lowerXBounds)) /
                  (model->upperXBounds - model->lowerXBounds);
    f32 scaledY = (model->minimapYScale * -60.0f * (z - model->lowerZBounds)) /
                  (model->upperZBounds - model->lowerZBounds);
    f32 acrossX = (scaledX * angleCos) + (scaledY * angleSin);
    f32 acrossY = (scaledX * angleSin) - (scaledY * angleCos);
    f32 mapX, mapY;

    if (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) {
        // The game turns the sprite over about its anchor; the picture here
        // is turned over as a whole, so the point goes with it.
        mapX = (acrossX - model->minimapOffsetXAdv2 + gMinimapDotOffsetX) - sBottomMapOriginX;
        mapX = (sBottom.mapWidth - 1) - mapX;
        mapY = (acrossY - model->minimapOffsetYAdv2) - sBottomMapOriginY;
    } else {
        mapX = (acrossX + model->minimapOffsetXAdv1) - sBottomMapOriginX;
        mapY = (acrossY - model->minimapOffsetYAdv1) - sBottomMapOriginY;
    }
    *outX = (int) (mapX * 16.0f);
    *outY = (int) (mapY * 16.0f);
}

/**
 * A racer's portrait into its slot, once per character: the game's own are
 * menu textures, which a race does not keep loaded, so each is loaded, copied
 * out and let go again.
 */
static void bottom_portrait(s32 slot, BottomRacer *out) {
    static s16 sIds[10];
    static s32 sIdsRead;
    const ModCharacter *added = modchar_get(out->modCharacter);
    s16 key = (s16) ((added != NULL ? 100 + out->modCharacter : out->character) + 1);
    TextureHeader *tex;
    s32 id, x, y;

    out->portrait = sBottomPortraits[slot];
    if (sBottomPortraitKey[slot] == key) {
        return;
    }
    sBottomPortraitKey[slot] = key;
    out->portraitSerial++;
    out->portraitWidth = out->portraitHeight = 0;
    if (!sIdsRead) {
        // The table of what each menu picture is (menu_asset_load in src/menu.c).
        extern void pc_swap16_buf(void *buf, u32 numBytes);
        s16 *ids = (s16 *) asset_table_load(ASSET_MENU_ELEMENT_IDS);

        if (ids != NULL) {
            pc_swap16_buf(ids, asset_table_size(ASSET_MENU_ELEMENT_IDS));
            for (x = 0; x < 10; x++) {
                sIds[x] = ids[TEXTURE_ICON_PORTRAIT_KRUNCH + x];
            }
            mempool_free(ids);
            sIdsRead = TRUE;
        }
    }
    if (added != NULL) {
        id = added->portrait;
    } else if (sIdsRead && out->character >= 0 && out->character < 10 &&
               (sIds[out->character] & ASSET_MASK_TEXTURE) == ASSET_MASK_TEXTURE) {
        id = sIds[out->character] & 0x3FFF;
    } else {
        return;
    }
    tex = load_texture(id);
    if (tex == NULL) {
        return;
    }
    if (tex->width <= BOTTOM_TEXTURE_MAX && tex->height <= BOTTOM_TEXTURE_MAX) {
        for (y = 0; y < tex->height; y++) {
            for (x = 0; x < tex->width; x++) {
                sBottomPortraits[slot][y * tex->width + x] = bottom_texel(tex, x, y);
            }
        }
        out->portraitWidth = tex->width;
        out->portraitHeight = tex->height;
    }
    tex_free(tex);
}

/** The Adventure's collectables from the save being played. */
static void bottom_collectables(void) {
    Settings *settings = get_settings();
    s32 i, bits;

    bzero(&sBottom.collect, sizeof(sBottom.collect));
    // A session between consoles plays on a complete save that is nobody's.
    sBottom.adventure = settings != NULL && !is_in_tracks_mode() && netplay_players() == 0;
    if (!sBottom.adventure) {
        return;
    }
    sBottom.collect.balloons = settings->balloonsPtr != NULL ? settings->balloonsPtr[0] : 0;
    for (bits = settings->keys & 0x1E; bits != 0; bits &= bits - 1) {
        sBottom.collect.keys++;
    }
    sBottom.collect.wizpigAmulet = settings->wizpigAmulet;
    sBottom.collect.ttAmulet = settings->ttAmulet;
    for (i = 0; i < 5; i++) {
        s32 trophy = (settings->trophies >> (i * 2)) & 3;

        sBottom.collect.trophies += trophy != 0;
        sBottom.collect.goldTrophies += trophy == 3;
    }
    for (bits = settings->bosses & 0x7BF; bits != 0; bits &= bits - 1) {
        sBottom.collect.bosses++;
    }
}

/**
 * Fills in the snapshot. Called from hud_render_general, so once for every
 * frame in which a level is played and never under a menu.
 */
static void bottom_update(void) {
    LevelHeader *header = level_header();
    LevelModel *model = get_current_level_model();
    s32 mine = netplay_players() != 0 ? netplay_slot() : PLAYER_ONE;
    Object **racers;
    s32 count, i;

    sBottom.frame++;
    sBottom.scene = BOTTOM_SCENE_NONE;
    sBottom.racerCount = 0;
    sBottom.me = -1;
    if (header == NULL || model == NULL) {
        return;
    }
    racers = get_racer_objects(&count);
    if (racers == NULL || count <= 0) {
        return;
    }
    if (header->race_type == RACETYPE_HUBWORLD) {
        sBottom.scene = BOTTOM_SCENE_HUB;
    } else if (header->race_type == RACETYPE_DEFAULT || header->race_type == RACETYPE_HORSESHOE_GULCH ||
               header->race_type == RACETYPE_BOSS || (header->race_type & RACETYPE_CHALLENGE)) {
        // Also with two players on one console: the standings and the map
        // serve both (bottom_has_race is about what leaves the top screen).
        sBottom.scene = BOTTOM_SCENE_RACE;
    } else {
        return;
    }
    bottom_collectables();
    if (sBottomMapStale) {
        sBottomMapStale = FALSE;
        bottom_build_map(model);
        bzero(sBottomPortraitKey, sizeof(sBottomPortraitKey));
    }
    sBottom.twoPlayer = netplay_players() == 0 && gNumActivePlayers >= 2;
    sBottom.mapMirrored = (get_filtered_cheats() & CHEAT_MIRRORED_TRACKS) != 0;
    sBottom.laps = header->race_type == RACETYPE_DEFAULT ? header->laps : 0;
    sBottom.silverCoins = -1;
    for (i = 0; i < count && i < BOTTOM_MAX_RACERS; i++) {
        Object *obj = racers[i];
        Object_Racer *racer = obj != NULL ? obj->racer : NULL;
        BottomRacer *out = &sBottom.racers[sBottom.racerCount];
        const ModCharacter *added;

        if (racer == NULL) {
            continue;
        }
        added = modchar_get(racer->modCharacter);
        out->place = racer->raceFinished && racer->finishPosition >= 1 ? racer->finishPosition : racer->racePosition;
        out->character = racer->characterId;
        out->modCharacter = added != NULL ? racer->modCharacter : 0;
        out->player = racer->playerIndex;
        out->finished = racer->raceFinished;
        out->name = added != NULL ? added->name
                                  : (racer->characterId >= 0 && racer->characterId < 10
                                         ? sBottomNames[racer->characterId]
                                         : "");
        out->colour[0] = gHudMinimapColours[racer->characterId].red;
        out->colour[1] = gHudMinimapColours[racer->characterId].green;
        out->colour[2] = gHudMinimapColours[racer->characterId].blue;
        bottom_map_point(model, obj->trans.x_position, obj->trans.z_position, &out->mapX, &out->mapY);
        bottom_portrait(sBottom.racerCount, out);
        if (racer->playerIndex == mine) {
            // The game's own dial (hud_speedometre): its needle is at the
            // end of the scale, marked 150, at a speed of 25.
            f32 vel = racer->vehicleID == VEHICLE_PLANE
                          ? sqrtf((obj->x_velocity * obj->x_velocity) + (obj->y_velocity * obj->y_velocity) +
                                  (obj->z_velocity * obj->z_velocity))
                          : sqrtf((obj->x_velocity * obj->x_velocity) + (obj->z_velocity * obj->z_velocity));

            sBottom.me = sBottom.racerCount;
            sBottom.speed = (s32) (vel * 6.0f);
            sBottom.speed = MIN(sBottom.speed, 150);
            sBottom.bananas = racer->bananas;
            sBottom.lap = MIN(racer->countLap + 1, sBottom.laps);
            if (gHudSilverCoinRace) {
                sBottom.silverCoins = racer->silverCoinCount;
            }
        }
        sBottom.racerCount++;
    }
}

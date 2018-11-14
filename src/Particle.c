#include "Particle.h"
#include "Block.h"
#include "World.h"
#include "ExtMath.h"
#include "Lighting.h"
#include "Entity.h"
#include "TerrainAtlas.h"
#include "Graphics.h"
#include "GraphicsCommon.h"
#include "Funcs.h"
#include "Game.h"
#include "Event.h"
#include "GameStructs.h"


/*########################################################################################################################*
*------------------------------------------------------Particle base------------------------------------------------------*
*#########################################################################################################################*/
static GfxResourceID Particles_TexId, Particles_VB;
#define PARTICLES_MAX 600
static RNGState rnd;
static bool particle_hitTerrain;

void Particle_DoRender(Vector2* size, Vector3* pos, TextureRec* rec, PackedCol col, VertexP3fT2fC4b* vertices) {
	float sX = size->X * 0.5f, sY = size->Y * 0.5f;
	Vector3 centre = *pos; centre.Y += sY;

	struct Matrix* view = &Gfx_View;
	float aX, aY, aZ, bX, bY, bZ;
	aX = view->Row0.X * sX; aY = view->Row1.X * sX; aZ = view->Row2.X * sX; /* right * size.X * 0.5f */
	bX = view->Row0.Y * sY; bY = view->Row1.Y * sY; bZ = view->Row2.Y * sY; /* up    * size.Y * 0.5f */
	VertexP3fT2fC4b v; v.Col = col;

	v.X = centre.X - aX - bX; v.Y = centre.Y - aY - bY; v.Z = centre.Z - aZ - bZ;
	v.U = rec->U1; v.V = rec->V2; vertices[0] = v;

	v.X = centre.X - aX + bX; v.Y = centre.Y - aY + bY; v.Z = centre.Z - aZ + bZ;
				   v.V = rec->V1; vertices[1] = v;

	v.X = centre.X + aX + bX; v.Y = centre.Y + aY + bY; v.Z = centre.Z + aZ + bZ;
	v.U = rec->U2;                vertices[2] = v;

	v.X = centre.X + aX - bX; v.Y = centre.Y + aY - bY; v.Z = centre.Z + aZ - bZ;
				   v.V = rec->V2; vertices[3] = v;
}

static void Particle_Reset(struct Particle* p, Vector3 pos, Vector3 velocity, float lifetime) {
	p->LastPos = pos; p->NextPos = pos;
	p->Velocity = velocity;
	p->Lifetime = lifetime;
}

static bool Particle_CanPass(BlockID block, bool throughLiquids) {
	uint8_t draw = Block_Draw[block];
	return draw == DRAW_GAS || draw == DRAW_SPRITE || (throughLiquids && Block_IsLiquid[block]);
}

static bool Particle_CollideHor(Vector3* nextPos, BlockID block) {
	Vector3 horPos = Vector3_Create3((float)Math_Floor(nextPos->X), 0.0f, (float)Math_Floor(nextPos->Z));
	Vector3 min, max;
	Vector3_Add(&min, &Block_MinBB[block], &horPos);
	Vector3_Add(&max, &Block_MaxBB[block], &horPos);
	return nextPos->X >= min.X && nextPos->Z >= min.Z && nextPos->X < max.X && nextPos->Z < max.Z;
}

static BlockID Particle_GetBlock(int x, int y, int z) {
	if (World_IsValidPos(x, y, z)) { return World_GetBlock(x, y, z); }

	if (y >= Env_EdgeHeight) return BLOCK_AIR;
	if (y >= Env_SidesHeight) return Env_EdgeBlock;
	return Env_SidesBlock;
}

static bool Particle_TestY(struct Particle* p, int y, bool topFace, bool throughLiquids) {
	BlockID block;
	Vector3 minBB, maxBB;
	float collideY;
	bool collideVer;

	if (y < 0) {
		p->NextPos.Y = ENTITY_ADJUSTMENT; p->LastPos.Y = ENTITY_ADJUSTMENT;
		p->Velocity  = Vector3_Zero;
		particle_hitTerrain = true;
		return false;
	}

	block = Particle_GetBlock((int)p->NextPos.X, y, (int)p->NextPos.Z);
	if (Particle_CanPass(block, throughLiquids)) return true;
	minBB = Block_MinBB[block]; maxBB = Block_MaxBB[block];

	collideY   = y + (topFace ? maxBB.Y : minBB.Y);
	collideVer = topFace ? (p->NextPos.Y < collideY) : (p->NextPos.Y > collideY);

	if (collideVer && Particle_CollideHor(&p->NextPos, block)) {
		float adjust = topFace ? ENTITY_ADJUSTMENT : -ENTITY_ADJUSTMENT;
		p->LastPos.Y = collideY + adjust;
		p->NextPos.Y = p->LastPos.Y;
		p->Velocity  = Vector3_Zero;
		particle_hitTerrain = true;
		return false;
	}
	return true;
}

static bool Particle_PhysicsTick(struct Particle* p, float gravity, bool throughLiquids, double delta) {
	BlockID cur;
	float minY, maxY;
	Vector3 velocity;
	int y, begY, endY;

	p->LastPos = p->NextPos;
	cur  = Particle_GetBlock((int)p->NextPos.X, (int)p->NextPos.Y, (int)p->NextPos.Z);
	minY = Math_Floor(p->NextPos.Y) + Block_MinBB[cur].Y;
	maxY = Math_Floor(p->NextPos.Y) + Block_MaxBB[cur].Y;

	if (!Particle_CanPass(cur, throughLiquids) && p->NextPos.Y >= minY
		&& p->NextPos.Y < maxY && Particle_CollideHor(&p->NextPos, cur)) {
		return true;
	}

	p->Velocity.Y -= gravity * (float)delta;
	begY = Math_Floor(p->NextPos.Y);
	
	Vector3_Mul1(&velocity, &p->Velocity, (float)delta * 3.0f);
	Vector3_Add(&p->NextPos, &p->NextPos, &velocity);
	endY = Math_Floor(p->NextPos.Y);

	if (p->Velocity.Y > 0.0f) {
		/* don't test block we are already in */
		for (y = begY + 1; y <= endY && Particle_TestY(p, y, false, throughLiquids); y++) {}
	} else {
		for (y = begY; y >= endY && Particle_TestY(p, y, true, throughLiquids); y--) {}
	}

	p->Lifetime -= (float)delta;
	return p->Lifetime < 0.0f;
}


/*########################################################################################################################*
*-------------------------------------------------------Rain particle-----------------------------------------------------*
*#########################################################################################################################*/
struct RainParticle { struct Particle Base; };

static struct RainParticle Rain_Particles[PARTICLES_MAX];
static int Rain_Count;

static bool RainParticle_Tick(struct RainParticle* p, double delta) {
	particle_hitTerrain = false;
	return Particle_PhysicsTick(&p->Base, 3.5f, false, delta) || particle_hitTerrain;
}

TextureRec Rain_Rec = { 2.0f/128.0f, 14.0f/128.0f, 5.0f/128.0f, 16.0f/128.0f };
static void RainParticle_Render(struct RainParticle* p, float t, VertexP3fT2fC4b* vertices) {
	Vector3 pos;
	Vector2 size;
	PackedCol col;
	int x, y, z;

	Vector3_Lerp(&pos, &p->Base.LastPos, &p->Base.NextPos, t);
	size.X = (float)p->Base.Size * 0.015625f; size.Y = size.X;

	x = Math_Floor(pos.X); y = Math_Floor(pos.Y); z = Math_Floor(pos.Z);
	col = World_IsValidPos(x, y, z) ? Lighting_Col(x, y, z) : Env_SunCol;
	Particle_DoRender(&size, &pos, &Rain_Rec, col, vertices);
}

static void Rain_Render(float t) {
	VertexP3fT2fC4b vertices[PARTICLES_MAX * 4];
	VertexP3fT2fC4b* ptr;
	int i;
	if (!Rain_Count) return;
	
	ptr = vertices;
	for (i = 0; i < Rain_Count; i++) {
		RainParticle_Render(&Rain_Particles[i], t, ptr);
		ptr += 4;
	}

	Gfx_BindTexture(Particles_TexId);
	GfxCommon_UpdateDynamicVb_IndexedTris(Particles_VB, vertices, Rain_Count * 4);
}

static void Rain_RemoveAt(int index) {
	struct RainParticle removed = Rain_Particles[index];
	int i;

	for (i = index; i < Rain_Count - 1; i++) {
		Rain_Particles[i] = Rain_Particles[i + 1];
	}
	Rain_Particles[Rain_Count - 1] = removed;
	Rain_Count--;
}

static void Rain_Tick(double delta) {
	int i;
	for (i = 0; i < Rain_Count; i++) {
		if (RainParticle_Tick(&Rain_Particles[i], delta)) {
			Rain_RemoveAt(i); i--;
		}
	}
}


/*########################################################################################################################*
*------------------------------------------------------Terrain particle---------------------------------------------------*
*#########################################################################################################################*/
struct TerrainParticle {
	struct Particle Base;
	TextureRec Rec;
	TextureLoc TexLoc;
	BlockID Block;
};

static struct TerrainParticle Terrain_Particles[PARTICLES_MAX];
static int Terrain_Count;
static uint16_t Terrain_1DCount[ATLAS1D_MAX_ATLASES];
static uint16_t Terrain_1DIndices[ATLAS1D_MAX_ATLASES];

static bool TerrainParticle_Tick(struct TerrainParticle* p, double delta) {
	return Particle_PhysicsTick(&p->Base, 5.4f, true, delta);
}

static void TerrainParticle_Render(struct TerrainParticle* p, float t, VertexP3fT2fC4b* vertices) {
	PackedCol col = PACKEDCOL_WHITE;
	Vector3 pos;
	Vector2 size;
	int x, y, z;

	Vector3_Lerp(&pos, &p->Base.LastPos, &p->Base.NextPos, t);
	size.X = (float)p->Base.Size * 0.015625f; size.Y = size.X;
	
	if (!Block_FullBright[p->Block]) {
		x = Math_Floor(pos.X); y = Math_Floor(pos.Y); z = Math_Floor(pos.Z);
		col = World_IsValidPos(x, y, z) ? Lighting_Col_XSide(x, y, z) : Env_SunXSide;
	}

	if (Block_Tinted[p->Block]) {
		PackedCol tintCol = Block_FogCol[p->Block];
		col.R = (uint8_t)(col.R * tintCol.R / 255);
		col.G = (uint8_t)(col.G * tintCol.G / 255);
		col.B = (uint8_t)(col.B * tintCol.B / 255);
	}
	Particle_DoRender(&size, &pos, &p->Rec, col, vertices);
}

static void Terrain_Update1DCounts(void) {
	int i, index;

	for (i = 0; i < ATLAS1D_MAX_ATLASES; i++) {
		Terrain_1DCount[i]   = 0;
		Terrain_1DIndices[i] = 0;
	}
	for (i = 0; i < Terrain_Count; i++) {
		index = Atlas1D_Index(Terrain_Particles[i].TexLoc);
		Terrain_1DCount[index] += 4;
	}
	for (i = 1; i < Atlas1D_Count; i++) {
		Terrain_1DIndices[i] = Terrain_1DIndices[i - 1] + Terrain_1DCount[i - 1];
	}
}

static void Terrain_Render(float t) {
	VertexP3fT2fC4b vertices[PARTICLES_MAX * 4];
	VertexP3fT2fC4b* ptr;
	int offset = 0;
	int i, index;
	if (!Terrain_Count) return;

	Terrain_Update1DCounts();
	for (i = 0; i < Terrain_Count; i++) {
		index = Atlas1D_Index(Terrain_Particles[i].TexLoc);
		ptr   = &vertices[Terrain_1DIndices[index]];

		TerrainParticle_Render(&Terrain_Particles[i], t, ptr);
		Terrain_1DIndices[index] += 4;
	}

	Gfx_SetDynamicVbData(Particles_VB, vertices, Terrain_Count * 4);
	for (i = 0; i < Atlas1D_Count; i++) {
		int partCount = Terrain_1DCount[i];
		if (!partCount) continue;

		Gfx_BindTexture(Atlas1D_TexIds[i]);
		Gfx_DrawVb_IndexedTris_Range(partCount, offset);
		offset += partCount;
	}
}

static void Terrain_RemoveAt(int index) {
	struct TerrainParticle removed = Terrain_Particles[index];
	int i;

	for (i = index; i < Terrain_Count - 1; i++) {
		Terrain_Particles[i] = Terrain_Particles[i + 1];
	}
	Terrain_Particles[Terrain_Count - 1] = removed;
	Terrain_Count--;
}

static void Terrain_Tick(double delta) {
	int i;
	for (i = 0; i < Terrain_Count; i++) {
		if (TerrainParticle_Tick(&Terrain_Particles[i], delta)) {
			Terrain_RemoveAt(i); i--;
		}
	}
}


/*########################################################################################################################*
*--------------------------------------------------------Particles--------------------------------------------------------*
*#########################################################################################################################*/
static void Particles_FileChanged(void* obj, struct Stream* stream, const String* name) {
	if (String_CaselessEqualsConst(name, "particles.png")) {
		Game_UpdateTexture(&Particles_TexId, stream, name, NULL);
	}
}

static void Particles_ContextLost(void* obj) {
	Gfx_DeleteVb(&Particles_VB); 
}
static void Particles_ContextRecreated(void* obj) {
	Particles_VB = Gfx_CreateDynamicVb(VERTEX_FORMAT_P3FT2FC4B, PARTICLES_MAX * 4);
}

static void Particles_BreakBlockEffect_Handler(void* obj, Vector3I coords, BlockID old, BlockID now) {
	Particles_BreakBlockEffect(coords, old, now);
}

static void Particles_Init(void) {
	Random_InitFromCurrentTime(&rnd);
	Particles_ContextRecreated(NULL);

	Event_RegisterBlock(&UserEvents_BlockChanged,   NULL, Particles_BreakBlockEffect_Handler);
	Event_RegisterEntry(&TextureEvents_FileChanged, NULL, Particles_FileChanged);
	Event_RegisterVoid(&GfxEvents_ContextLost,      NULL, Particles_ContextLost);
	Event_RegisterVoid(&GfxEvents_ContextRecreated, NULL, Particles_ContextRecreated);
}

static void Particles_Reset(void) { Rain_Count = 0; Terrain_Count = 0; }

static void Particles_Free(void) {
	Gfx_DeleteTexture(&Particles_TexId);
	Particles_ContextLost(NULL);

	Event_UnregisterBlock(&UserEvents_BlockChanged,   NULL, Particles_BreakBlockEffect_Handler);
	Event_UnregisterEntry(&TextureEvents_FileChanged, NULL, Particles_FileChanged);
	Event_UnregisterVoid(&GfxEvents_ContextLost,      NULL, Particles_ContextLost);
	Event_UnregisterVoid(&GfxEvents_ContextRecreated, NULL, Particles_ContextRecreated);
}

void Particles_MakeComponent(struct IGameComponent* comp) {
	comp->Init     = Particles_Init;
	comp->Reset    = Particles_Reset;
	comp->OnNewMap = Particles_Reset;
	comp->Free     = Particles_Free;
}

void Particles_Render(double delta, float t) {
	if (!Terrain_Count && !Rain_Count) return;
	if (Gfx_LostContext) return;

	Gfx_SetTexturing(true);
	Gfx_SetAlphaTest(true);

	Gfx_SetBatchFormat(VERTEX_FORMAT_P3FT2FC4B);
	Terrain_Render(t);
	Rain_Render(t);

	Gfx_SetAlphaTest(false);
	Gfx_SetTexturing(false);
}

void Particles_Tick(struct ScheduledTask* task) {
	Terrain_Tick(task->Interval);
	Rain_Tick(task->Interval);
}

void Particles_BreakBlockEffect(Vector3I coords, BlockID old, BlockID now) {
	struct TerrainParticle* p;
	Vector3 origin;
	Vector3 minBB, maxBB;

	if (now != BLOCK_AIR || Block_Draw[old] == DRAW_GAS) return;

	Vector3I_ToVector3(&origin, &coords);
	TextureLoc texLoc = Block_GetTex(old, FACE_XMIN);
	int texIndex;
	TextureRec baseRec = Atlas1D_TexRec(texLoc, 1, &texIndex);
	float uScale = (1.0f/16.0f), vScale = (1.0f/16.0f) * Atlas1D_InvTileSize;

	minBB = Block_MinBB[old]; maxBB = Block_MaxBB[old];
	int minX = (int)(minBB.X * 16), minZ = (int)(minBB.Z * 16);
	int maxX = (int)(maxBB.X * 16), maxZ = (int)(maxBB.Z * 16);

	int minU = min(minX, minZ), minV = (int)(16 - maxBB.Y * 16);
	int maxU = min(maxX, maxZ), maxV = (int)(16 - minBB.Y * 16);
	int maxUsedU = maxU, maxUsedV = maxV;
	/* This way we can avoid creating particles which outside the bounds and need to be clamped */
	if (minU < 12 && maxU > 12) maxUsedU = 12;
	if (minV < 12 && maxV > 12) maxUsedV = 12;

	#define GRID_SIZE 4
	/* gridOffset gives the centre of the cell on a grid */
	#define CELL_CENTRE ((1.0f / GRID_SIZE) * 0.5f)

	int x, y, z;
	float maxU2 = baseRec.U1 + maxU * uScale;
	float maxV2 = baseRec.V1 + maxV * vScale;
	for (x = 0; x < GRID_SIZE; x++) {
		for (y = 0; y < GRID_SIZE; y++) {
			for (z = 0; z < GRID_SIZE; z++) {
				float cellX = (float)x / GRID_SIZE, cellY = (float)y / GRID_SIZE, cellZ = (float)z / GRID_SIZE;
				Vector3 cell = Vector3_Create3(CELL_CENTRE + cellX, CELL_CENTRE / 2 + cellY, CELL_CENTRE + cellZ);
				if (cell.X < minBB.X || cell.X > maxBB.X || cell.Y < minBB.Y
					|| cell.Y > maxBB.Y || cell.Z < minBB.Z || cell.Z > maxBB.Z) continue;

				Vector3 velocity; /* centre random offset around [-0.2, 0.2] */
				velocity.X = CELL_CENTRE + (cellX - 0.5f) + (Random_Float(&rnd) * 0.4f - 0.2f);
				velocity.Y = CELL_CENTRE + (cellY - 0.0f) + (Random_Float(&rnd) * 0.4f - 0.2f);
				velocity.Z = CELL_CENTRE + (cellZ - 0.5f) + (Random_Float(&rnd) * 0.4f - 0.2f);

				TextureRec rec = baseRec;
				rec.U1 = baseRec.U1 + Random_Range(&rnd, minU, maxUsedU) * uScale;
				rec.V1 = baseRec.V1 + Random_Range(&rnd, minV, maxUsedV) * vScale;
				rec.U2 = rec.U1 + 4 * uScale;
				rec.V2 = rec.V1 + 4 * vScale;
				rec.U2 = min(rec.U2, maxU2) - 0.01f * uScale;
				rec.V2 = min(rec.V2, maxV2) - 0.01f * vScale;

				if (Terrain_Count == PARTICLES_MAX) Terrain_RemoveAt(0);
				p = &Terrain_Particles[Terrain_Count++];
				float life = 0.3f + Random_Float(&rnd) * 1.2f;

				Vector3 pos;
				Vector3_Add(&pos, &origin, &cell);
				Particle_Reset(&p->Base, pos, velocity, life);
				p->Rec = rec;
				p->TexLoc = texLoc;
				p->Block = old;
				int type = Random_Range(&rnd, 0, 30);
				p->Base.Size = (uint8_t)(type >= 28 ? 12 : (type >= 25 ? 10 : 8));
			}
		}
	}
}

void Particles_RainSnowEffect(Vector3 pos) {
	struct RainParticle* p;
	Vector3 origin = pos;
	Vector3 offset, velocity;
	int i, type;

	for (i = 0; i < 2; i++) {
		velocity.X = Random_Float(&rnd) * 0.8f - 0.4f; /* [-0.4, 0.4] */
		velocity.Z = Random_Float(&rnd) * 0.8f - 0.4f;
		velocity.Y = Random_Float(&rnd) + 0.4f;

		offset.X = Random_Float(&rnd); /* [0.0, 1.0] */
		offset.Y = Random_Float(&rnd) * 0.1f + 0.01f;
		offset.Z = Random_Float(&rnd);

		if (Rain_Count == PARTICLES_MAX) Rain_RemoveAt(0);
		p = &Rain_Particles[Rain_Count++];

		Vector3_Add(&pos, &origin, &offset);
		Particle_Reset(&p->Base, pos, velocity, 40.0f);

		type = Random_Range(&rnd, 0, 30);
		p->Base.Size = (uint8_t)(type >= 28 ? 2 : (type >= 25 ? 4 : 3));
	}
}

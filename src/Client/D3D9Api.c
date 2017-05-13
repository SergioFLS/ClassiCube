#include "GraphicsAPI.h"
#include "D3D9Api.h"
#include "ErrorHandler.h"
#include "GraphicsEnums.h"
#include "Platform.h"

#ifdef USE_DX

IDirect3D9* d3d;
IDirect3DDevice9* device;
MatrixStack* curStack;
MatrixStack viewStack, projStack, texStack;


#define D3D9_SetRenderState(raw, state, name) \
ReturnCode hresult = IDirect3DDevice9_SetRenderState(device, state, raw); \
ErrorHandler_CheckOrFail(hresult, name)

#define D3D9_SetRenderState2(raw, state, name) \
hresult = IDirect3DDevice9_SetRenderState(device, state, raw); \
ErrorHandler_CheckOrFail(hresult, name)

#define D3D9_LogLeakedResource(msg, i) \
logMsg.length = 0;\
String_AppendConstant(&logMsg, msg);\
String_AppendInt32(&logMsg, i);\
Platform_Log(logMsg);

#define D3D9_NumPrimitives(mode, vertices) (mode == DrawMode_Triangles ? vertices / 3 : vertices / 2)


/* We only ever create a single index buffer internally. */
#define d3d9_iBuffersExpSize 2
IDirect3DIndexBuffer9* d3d9_ibuffers[d3d9_iBuffersExpSize];
Int32 d3d9_ibuffersCapacity = d3d9_iBuffersExpSize;

/* TODO: This number's probably not big enough... */
#define d3d9_vBuffersExpSize 2048
IDirect3DVertexBuffer9* d3d9_vbuffers[d3d9_vBuffersExpSize];
Int32 d3d9_vbuffersCapacity = d3d9_vBuffersExpSize;

/* At most we can have 256 entities with their own texture each.
Adding another 128 gives us a lot of leeway. */
#define d3d9_texturesExpSize 384
IDirect3DTexture9* d3d9_textures[d3d9_texturesExpSize];
Int32 d3d9_texturesCapacity = d3d9_texturesExpSize;


void Gfx_Init() {
	/* TODO: EVERYTHING ELSE */
	viewStack.Type = D3DTS_VIEW;
	projStack.Type = D3DTS_PROJECTION;
	texStack.Type = D3DTS_TEXTURE0;
}

void Gfx_Free() {
	UInt8 logMsgBuffer[String_BufferSize(63)];
	String logMsg = String_FromRawBuffer(logMsgBuffer, 63);

	Int32 i;
	for (i = 0; i < d3d9_texturesCapacity; i++) {
		if (d3d9_textures[i] == NULL) continue;
		Int32 texId = i;
		Gfx_DeleteTexture(&texId);
		D3D9_LogLeakedResource("Texture leak! ID: ", i);
	}

	for (i = 0; i < d3d9_vbuffersCapacity; i++) {
		if (d3d9_vbuffers[i] == NULL) continue;
		Int32 vb = i;
		Gfx_DeleteVb(&vb);
		D3D9_LogLeakedResource("Vertex buffer leak! ID: ", i);
	}

	for (i = 0; i < d3d9_ibuffersCapacity; i++) {
		if (d3d9_ibuffers[i] == NULL) continue;
		Int32 ib = i;
		Gfx_DeleteIb(&ib);
		D3D9_LogLeakedResource("Index buffer leak! ID: ", i);
	}

	if (d3d9_ibuffersCapacity != d3d9_iBuffersExpSize) {
		Platform_MemFree(d3d9_ibuffers);
	}
	if (d3d9_vbuffersCapacity != d3d9_vBuffersExpSize) {
		Platform_MemFree(d3d9_vbuffers);
	}
	if (d3d9_texturesCapacity != d3d9_texturesExpSize) {
		Platform_MemFree(d3d9_textures);
	}
}


void Gfx_BindTexture(Int32 texId) {
	ReturnCode hresult = IDirect3DDevice9_SetTexture(device, 0, d3d9_textures[texId]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_BindTexture");
}

void Gfx_DeleteTexture(Int32* texId) {
	D3D9_DeleteResource((void**)d3d9_textures, d3d9_texturesCapacity, texId);
}

void Gfx_SetTexturing(bool enabled) {
	if (enabled) return;
	ReturnCode hresult = IDirect3DDevice9_SetTexture(device, 0, NULL);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetTexturing");
}



bool d3d9_fogEnable = false;
void Gfx_SetFog(bool enabled) {
	if (d3d9_fogEnable == enabled) return;

	d3d9_fogEnable = enabled;
	D3D9_SetRenderState((UInt32)enabled, D3DRS_FOGENABLE, "D3D9_SetFog");
}

UInt32 d3d9_fogCol = 0xFF000000; /* black */
void Gfx_SetFogColour(PackedCol col) {
	if (col.Packed == d3d9_fogCol) return;

	d3d9_fogCol = col.Packed;
	D3D9_SetRenderState(col.Packed, D3DRS_FOGCOLOR, "D3D9_SetFogColour");
}

Real32 d3d9_fogDensity = -1.0f;
void Gfx_SetFogDensity(Real32 value) {
	if (value == d3d9_fogDensity) return;

	d3d9_fogDensity = value;
	UInt32 raw = *(UInt32*)&value;
	D3D9_SetRenderState(raw, D3DRS_FOGDENSITY, "D3D9_SetFogDensity");
}

void Gfx_SetFogStart(Real32 value) {
	UInt32 raw = *(UInt32*)&value;
	D3D9_SetRenderState(raw, D3DRS_FOGSTART, "D3D9_SetFogStart");
}

Real32 d3d9_fogEnd = -1.0f;
void Gfx_SetFogEnd(Real32 value) {
	if (value == d3d9_fogEnd) return;

	d3d9_fogEnd = value;
	UInt32 raw = *(UInt32*)&value;
	D3D9_SetRenderState(raw, D3DRS_FOGEND, "D3D9_SetFogEnd");
}

D3DFOGMODE fogTableMode = D3DFOG_NONE;
void Gfx_SetFogMode(Int32 fogMode) {
	D3DFOGMODE mode = d3d9_modes[fogMode];
	if (mode == fogTableMode) return;

	fogTableMode = mode;
	D3D9_SetRenderState(mode, D3DRS_FOGTABLEMODE, "D3D9_SetFogMode");
}


void Gfx_SetFaceCulling(bool enabled) {
	D3DCULL mode = enabled ? D3DCULL_CW : D3DCULL_NONE;
	D3D9_SetRenderState(mode, D3DRS_CULLMODE, "D3D9_SetFaceCulling");
}

bool d3d9_alphaTest = false;
void Gfx_SetAlphaTest(bool enabled) {
	if (d3d9_alphaTest == enabled) return;

	d3d9_alphaTest = enabled;
	D3D9_SetRenderState((UInt32)enabled, D3DRS_ALPHATESTENABLE, "D3D9_SetAlphaTest");
}

D3DCMPFUNC d3d9_alphaTestFunc = 0;
Int32 d3d9_alphaTestRef = 0;
void Gfx_SetAlphaTestFunc(Int32 compareFunc, Real32 refValue) {
	d3d9_alphaTestFunc = d3d9_compareFuncs[compareFunc];
	D3D9_SetRenderState(d3d9_alphaTestFunc, D3DRS_ALPHAFUNC, "D3D9_SetAlphaTestFunc");
	d3d9_alphaTestRef = (Int32)(refValue * 255);
	D3D9_SetRenderState2(d3d9_alphaTestRef, D3DRS_ALPHAREF, "D3D9_SetAlphaTestFunc2");
}

bool d3d9_alphaBlend = false;
void Gfx_SetAlphaBlending(bool enabled) {
	if (d3d9_alphaBlend == enabled) return;

	d3d9_alphaBlend = enabled;
	D3D9_SetRenderState((UInt32)enabled, D3DRS_ALPHABLENDENABLE, "D3D9_SetAlphaBlend");
}

D3DBLEND d3d9_srcBlendFunc = 0;
D3DBLEND d3d9_dstBlendFunc = 0;
void Gfx_SetAlphaBlendFunc(Int32 srcBlendFunc, Int32 dstBlendFunc) {
	d3d9_srcBlendFunc = d3d9_blendFuncs[srcBlendFunc];
	D3D9_SetRenderState(d3d9_srcBlendFunc, D3DRS_SRCBLEND, "D3D9_SetAlphaBlendFunc");
	d3d9_dstBlendFunc = d3d9_blendFuncs[dstBlendFunc];
	D3D9_SetRenderState2(d3d9_dstBlendFunc, D3DRS_DESTBLEND, "D3D9_SetAlphaBlendFunc2");
}

void Gfx_SetAlphaArgBlend(bool enabled) {
	D3DTEXTUREOP op = enabled ? D3DTOP_MODULATE : D3DTOP_SELECTARG1;
	ReturnCode hresult = IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, op);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetAlphaArgBlend");
}

UInt32 d3d9_clearCol = 0xFF000000;
void Gfx_Clear() {
	DWORD flags = D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER;
	ReturnCode hresult = IDirect3DDevice9_Clear(device, 0, NULL, flags, d3d9_clearCol, 1.0f, 0);
	ErrorHandler_CheckOrFail(hresult, "D3D9_Clear");
}

void Gfx_ClearColour(PackedCol col) {
	d3d9_clearCol = col.Packed;
}



bool d3d9_depthTest = false;
void Gfx_SetDepthTest(bool enabled) {
	d3d9_depthTest = enabled;
	D3D9_SetRenderState((UInt32)enabled, D3DRS_ZENABLE, "D3D9_SetDepthTest");
}

D3DCMPFUNC d3d9_depthTestFunc = 0;
void Gfx_SetDepthTestFunc(Int32 compareFunc) {
	d3d9_depthTestFunc = d3d9_compareFuncs[compareFunc];
	D3D9_SetRenderState(d3d9_alphaTestFunc, D3DRS_ZFUNC, "D3D9_SetDepthTestFunc");
}

void Gfx_SetColourWrite(bool enabled) {
	UInt32 channels = enabled ? 0xF : 0x0;
	D3D9_SetRenderState(channels, D3DRS_COLORWRITEENABLE, "D3D9_SetColourWrite");
}

bool d3d9_depthWrite = false;
void Gfx_SetDepthWrite(bool enabled) {
	d3d9_depthWrite = enabled;
	D3D9_SetRenderState((UInt32)enabled, D3DRS_ZWRITEENABLE, "D3D9_SetDepthWrite");
}



Int32 Gfx_CreateDynamicVb(Int32 vertexFormat, Int32 maxVertices) {
	Int32 size = maxVertices * Gfx_strideSizes[vertexFormat];
	IDirect3DVertexBuffer9* vbuffer;
	ReturnCode hresult = IDirect3DDevice9_CreateVertexBuffer(device, size, D3DUSAGE_DYNAMIC, 
		d3d9_formatMappings[vertexFormat], D3DPOOL_DEFAULT, &vbuffer, NULL);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateDynamicVb");

	return D3D9_GetOrExpand(&d3d9_vbuffers, &d3d9_vbuffersCapacity, vbuffer, d3d9_vBuffersExpSize);
}

Int32 Gfx_CreateVb(void* vertices, Int32 vertexFormat, Int32 count) {
	Int32 size = count * Gfx_strideSizes[vertexFormat];
	IDirect3DVertexBuffer9* vbuffer;
	ReturnCode hresult = IDirect3DDevice9_CreateVertexBuffer(device, size, 0,
		d3d9_formatMappings[vertexFormat], D3DPOOL_DEFAULT, &vbuffer, NULL);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateVb");

	void* dst;
	hresult = IDirect3DVertexBuffer9_Lock(vbuffer, 0, size, dst, 0);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateVb - Lock");
	Platform_MemCpy(dst, vertices, size);
	hresult = IDirect3DVertexBuffer9_Unlock(vbuffer);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateVb - Unlock");

	return D3D9_GetOrExpand(&d3d9_vbuffers, &d3d9_vbuffersCapacity, vbuffer, d3d9_vBuffersExpSize);
}

Int32 Gfx_CreateIb(void* indices, Int32 indicesCount) {
	Int32 size = indicesCount * sizeof(UInt16);
	IDirect3DIndexBuffer9* ibuffer;
	ReturnCode hresult = IDirect3DDevice9_CreateIndexBuffer(device, size, 0, 
		D3DFMT_INDEX16, D3DPOOL_MANAGED, &ibuffer, NULL);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateIb");

	void* dst;
	hresult = IDirect3DIndexBuffer9_Lock(ibuffer, 0, size, dst, 0);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateIb - Lock");
	Platform_MemCpy(dst, indices, size);
	hresult = IDirect3DIndexBuffer9_Unlock(ibuffer);
	ErrorHandler_CheckOrFail(hresult, "D3D9_CreateIb - Unlock");

	return D3D9_GetOrExpand(&d3d9_ibuffers, &d3d9_ibuffersCapacity, ibuffer, d3d9_iBuffersExpSize);
}

Int32 d3d9_batchStride;
void Gfx_BindVb(Int32 vb) {
	ReturnCode hresult = IDirect3DDevice9_SetStreamSource(device, 0, d3d9_vbuffers[vb], 0, d3d9_batchStride);
	ErrorHandler_CheckOrFail(hresult, "D3D9_BindVb");
}

void Gfx_BindIb(Int32 ib) {
	ReturnCode hresult = IDirect3DDevice9_SetIndices(device, d3d9_ibuffers[ib]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_BindIb");
}

void Gfx_DeleteVb(Int32* vb) {
	D3D9_DeleteResource((void**)d3d9_vbuffers, d3d9_vbuffersCapacity, vb);
}

void Gfx_DeleteIb(Int32* ib) {
	D3D9_DeleteResource((void**)d3d9_ibuffers, d3d9_ibuffersCapacity, ib);
}

void Gfx_SetBatchFormat(Int32 vertexFormat) {
	ReturnCode hresult = IDirect3DDevice9_SetFVF(device, d3d9_formatMappings[vertexFormat]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetBatchFormat");
	d3d9_batchStride = Gfx_strideSizes[vertexFormat];
}

void Gfx_DrawVb(Int32 drawMode, Int32 startVertex, Int32 vCount) {
	Int32 numPrims = D3D9_NumPrimitives(drawMode, vCount);
	ReturnCode hresult = IDirect3DDevice9_DrawPrimitive(device, d3d9_modeMappings[drawMode],
		startVertex, numPrims);
	ErrorHandler_CheckOrFail(hresult, "D3D9_DrawVb");
}

void Gfx_SetDynamicVbData(Int32 vb, void* vertices, Int32 vCount) {
	Int32 size = vCount * d3d9_batchStride;
	IDirect3DVertexBuffer9* vbuffer = d3d9_vbuffers[vb];

	void* dst;
	ReturnCode hresult = IDirect3DVertexBuffer9_Lock(vbuffer, 0, size, dst, D3DLOCK_DISCARD);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetDynamicVbData - Lock");
	Platform_MemCpy(dst, vertices, size);
	hresult = IDirect3DVertexBuffer9_Unlock(vbuffer);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetDynamicVbData - Unlock");

	hresult = IDirect3DDevice9_SetStreamSource(device, 0, d3d9_vbuffers[vb], 0, d3d9_batchStride);
	ErrorHandler_CheckOrFail(hresult, "D3D9_SetDynamicVbData - Bind");
}

void Gfx_DrawIndexedVb(Int32 drawMode, Int32 indicesCount, Int32 startIndex) {
	Int32 numPrims = D3D9_NumPrimitives(drawMode, indicesCount);
	ReturnCode hresult = IDirect3DDevice9_DrawIndexedPrimitive(device, d3d9_modeMappings[drawMode], 0,
		startIndex / 6 * 4, indicesCount / 6 * 4, startIndex, numPrims);
	ErrorHandler_CheckOrFail(hresult, "D3D9_DrawIndexedVb");
}

void Gfx_DrawIndexedVb_TrisT2fC4b_Range(Int32 indicesCount, Int32 offsetVertex, Int32 startIndex) {
	ReturnCode hresult = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, offsetVertex,
		0, indicesCount / 6 * 4, startIndex, indicesCount / 3);
	ErrorHandler_CheckOrFail(hresult, "D3D9_DrawIndexedVb_TrisT2fC4b_Range");
}

void Gfx_DrawIndexedVb_TrisT2fC4b(Int32 indicesCount, Int32 startIndex) {
	ReturnCode hresult = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST, 0,
		startIndex / 6 * 4, indicesCount / 6 * 4, startIndex, indicesCount / 3);
	ErrorHandler_CheckOrFail(hresult, "D3D9_DrawIndexedVb_TrisT2fC4b");
}



void Gfx_SetMatrixMode(Int32 matrixType) {
	if (matrixType == MatrixType_Projection) {
		curStack = &projStack;
	} else if (matrixType == MatrixType_Modelview) {
		curStack = &viewStack;
	} else if (matrixType == MatrixType_Texture) {
		curStack = &texStack;
	}
}

void Gfx_LoadMatrix(Matrix* matrix) {
	if (curStack == &texStack) {
		matrix->Row2.X = matrix->Row3.X; /* NOTE: this hack fixes the texture movements. */
		IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	}

	Int32 idx = curStack->Index;
	curStack->Stack[idx] = *matrix;
	
	ReturnCode hresult = IDirect3DDevice9_SetTransform(device, curStack->Type, &curStack->Stack[idx]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_LoadMatrix");
}

void Gfx_LoadIdentityMatrix() {
	if (curStack == &texStack) {
		IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	}

	Int32 idx = curStack->Index;
	curStack->Stack[idx] = Matrix_Identity;

	ReturnCode hresult = IDirect3DDevice9_SetTransform(device, curStack->Type, &curStack->Stack[idx]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_LoadIdentityMatrix");
}

void Gfx_MultiplyMatrix(Matrix* matrix) {
	Int32 idx = curStack->Index;
	Matrix_Mul(matrix, &curStack->Stack[idx], &curStack->Stack[idx]);

	ReturnCode hresult = IDirect3DDevice9_SetTransform(device, curStack->Type, &curStack->Stack[idx]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_MultiplyMatrix");
}

void Gfx_PushMatrix() {
	Int32 idx = curStack->Index;
	if (idx == MatrixStack_Capacity) {
		ErrorHandler_Fail("Unable to push matrix, at capacity already");
	}

	curStack->Stack[idx + 1] = curStack->Stack[idx]; /* mimic GL behaviour */
	curStack->Index++; /* exact same, we don't need to update DirectX state. */
}

void Gfx_PopMatrix() {
	Int32 idx = curStack->Index;
	if (idx == 0) {
		ErrorHandler_Fail("Unable to pop matrix, at 0 already");
	}

	curStack->Index--; idx--;
	ReturnCode hresult = IDirect3DDevice9_SetTransform(device, curStack->Type, &curStack->Stack[idx]);
	ErrorHandler_CheckOrFail(hresult, "D3D9_PopMatrix");
}

#define d3d9_zN -10000.0f
#define d3d9_zF 10000.0f
void Gfx_LoadOrthoMatrix(Real32 width, Real32 height) {
	Matrix matrix;
	Matrix_OrthographicOffCenter(0, width, height, 0, d3d9_zN, d3d9_zF, &matrix);

	matrix.Row2.Y = 1.0f / (d3d9_zN - d3d9_zF);
	matrix.Row2.Z = d3d9_zN / (d3d9_zN - d3d9_zF);
	matrix.Row3.Z = 1.0f;
	Gfx_LoadMatrix(&matrix);
}



void D3D9_DeleteResource(void** resources, Int32 capacity, Int32* id) {
	Int32 resourceID = *id;
	if (resourceID <= 0 || resourceID >= capacity) return;

	void* value = resources[resourceID];
	*id = -1;
	if (value == NULL) return;

	IUnknown* unk = (IUnknown*)value;
	UInt32 refCount = unk->lpVtbl->Release(unk);
	resources[resourceID] = NULL;
	if (refCount <= 0) return;

	UInt8 logMsgBuffer[String_BufferSize(127)];
	String logMsg = String_FromRawBuffer(logMsgBuffer, 127);
	String_AppendConstant(&logMsg, "D3D9 Resource has outstanding references! ID: ");
	String_AppendInt32(&logMsg, resourceID);
	Platform_Log(logMsg);
}

/* TODO: I have no clue if this even works. */
Int32 D3D9_GetOrExpand(void*** resourcesPtr, Int32* capacity, void* resource, Int32 expSize) {
	Int32 i;
	void** resources = *resourcesPtr;
	for (i = 1; i < *capacity; i++) {
		if (resources[i] == NULL) {
			resources[i] = resource;
			return i;
		}
	}

	/* Otherwise resize and add more elements */
	Int32 oldLength = *capacity;
	(*capacity) += expSize;

	/*  Allocate resized pointers table */
	void** newResources = Platform_MemAlloc(*capacity * sizeof(void*));
	if (newResources == NULL) {
		ErrorHandler_Fail("D3D9 - failed to resize pointers table");
	}
	*resourcesPtr = newResources;

	/* Update elements in new table */
	for (i = 0; i < oldLength; i++) {
		newResources[i] = resources[i];
	}
	/* Free old allocated memory if necessary */
	if (oldLength != expSize) Platform_MemFree(resources);

	newResources[oldLength] = resource;
	return oldLength;
}
#endif
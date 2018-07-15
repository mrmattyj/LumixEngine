#include "ffr.h"
#include "engine/blob.h"
#include "engine/crc32.h"
#include "engine/hash_map.h"
#include "engine/log.h"
#include "engine/math_utils.h"
#include "engine/mt/sync.h"
#include <Windows.h>
#include <gl/GL.h>
#include "renderdoc_app.h"

#define FFR_GL_IMPORT(prototype, name) static prototype name;
#define FFR_GL_IMPORT_TYPEDEFS

#include "gl_ext.h"

#undef FFR_GL_IMPORT_TYPEDEFS
#undef FFR_GL_IMPORT

namespace Lumix
{


namespace ffr {

struct Buffer
{
	enum { MAX_COUNT = 4096 };
	
	GLuint handle;
};


struct Uniform
{
	enum { MAX_COUNT = 256 };

	UniformType type;
	uint count;
	void* data; 
	#ifdef _DEBUG
		StaticString<32> debug_name;
	#endif
};


struct Texture
{
	enum { MAX_COUNT = 4096 };

	GLuint handle;
	bool cubemap;
};


struct Program
{
	enum { MAX_COUNT = 1024 };
	GLuint handle;

	struct {
		int loc;
		UniformHandle uniform;
	} uniforms[32];
	int uniforms_count;
};


template <typename T, int MAX_COUNT>
struct Pool
{
	void create(IAllocator& allocator)
	{
		values = (T*)allocator.allocate(sizeof(T) * MAX_COUNT);
		for(int i = 0; i < MAX_COUNT; ++i) {
			*((int*)&values[i]) = i + 1;
		}
		*((int*)&values[MAX_COUNT - 1]) = -1;	
		first_free = 0;
	}

	void destroy(IAllocator& allocator)
	{
		allocator.deallocate(values);
	}

	int alloc()
	{
		if(first_free == -1) return -1;

		const int id = first_free;
		first_free = *((int*)&values[id]);
		return id;
	}

	void dealloc(uint idx)
	{
		*((int*)&values[idx]) = first_free;
		first_free = idx;
	}

	T* values;
	int first_free;

	T& operator[](int idx) { return values[idx]; }
	bool isFull() const { return first_free == -1; }
};

static struct {
	RENDERDOC_API_1_1_2* rdoc_api;
	GLuint vao;
	GLuint tex_buffers[32];
	IAllocator* allocator;
	void* device_context;
	Pool<Buffer, Buffer::MAX_COUNT> buffers;
	Pool<Texture, Texture::MAX_COUNT> textures;
	Pool<Uniform, Uniform::MAX_COUNT> uniforms;
	Pool<Program, Program::MAX_COUNT> programs;
	HashMap<u32, uint>* uniforms_hash_map;
	MT::SpinMutex handle_mutex {false};
	DWORD thread;
} g_ffr;


namespace DDS
{

static const uint DDS_MAGIC = 0x20534444; //  little-endian
static const uint DDSD_CAPS = 0x00000001;
static const uint DDSD_HEIGHT = 0x00000002;
static const uint DDSD_WIDTH = 0x00000004;
static const uint DDSD_PITCH = 0x00000008;
static const uint DDSD_PIXELFORMAT = 0x00001000;
static const uint DDSD_MIPMAPCOUNT = 0x00020000;
static const uint DDSD_LINEARSIZE = 0x00080000;
static const uint DDSD_DEPTH = 0x00800000;
static const uint DDPF_ALPHAPIXELS = 0x00000001;
static const uint DDPF_FOURCC = 0x00000004;
static const uint DDPF_INDEXED = 0x00000020;
static const uint DDPF_RGB = 0x00000040;
static const uint DDSCAPS_COMPLEX = 0x00000008;
static const uint DDSCAPS_TEXTURE = 0x00001000;
static const uint DDSCAPS_MIPMAP = 0x00400000;
static const uint DDSCAPS2_CUBEMAP = 0x00000200;
static const uint DDSCAPS2_CUBEMAP_POSITIVEX = 0x00000400;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEX = 0x00000800;
static const uint DDSCAPS2_CUBEMAP_POSITIVEY = 0x00001000;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEY = 0x00002000;
static const uint DDSCAPS2_CUBEMAP_POSITIVEZ = 0x00004000;
static const uint DDSCAPS2_CUBEMAP_NEGATIVEZ = 0x00008000;
static const uint DDSCAPS2_VOLUME = 0x00200000;
static const uint D3DFMT_ATI1 = '1ITA';
static const uint D3DFMT_ATI2 = '2ITA';
static const uint D3DFMT_DXT1 = '1TXD';
static const uint D3DFMT_DXT2 = '2TXD';
static const uint D3DFMT_DXT3 = '3TXD';
static const uint D3DFMT_DXT4 = '4TXD';
static const uint D3DFMT_DXT5 = '5TXD';

struct PixelFormat {
	uint dwSize;
	uint dwFlags;
	uint dwFourCC;
	uint dwRGBBitCount;
	uint dwRBitMask;
	uint dwGBitMask;
	uint dwBBitMask;
	uint dwAlphaBitMask;
};

struct Caps2 {
	uint dwCaps1;
	uint dwCaps2;
	uint dwDDSX;
	uint dwReserved;
};

struct Header {
	uint dwMagic;
	uint dwSize;
	uint dwFlags;
	uint dwHeight;
	uint dwWidth;
	uint dwPitchOrLinearSize;
	uint dwDepth;
	uint dwMipMapCount;
	uint dwReserved1[11];

	PixelFormat pixelFormat;
	Caps2 caps2;

	uint dwReserved2;
};

struct LoadInfo {
	bool compressed;
	bool swap;
	bool palette;
	uint blockBytes;
	GLenum internalFormat;
	GLenum internalSRGBFormat;
	GLenum externalFormat;
	GLenum type;
};

static uint sizeDXTC(uint w, uint h, GLuint format) {
    const bool is_dxt1 = format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT || format == GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT;
	const bool is_ati = format == GL_COMPRESSED_RED_RGTC1;
	return ((w + 3) / 4) * ((h + 3) / 4) * (is_dxt1 || is_ati ? 8 : 16);
}

static bool isDXT1(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT1));
}

static bool isATI1(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_ATI1));
}

static bool isATI2(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_ATI2));
}

static bool isDXT3(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT3));

}

static bool isDXT5(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_FOURCC) && (pf.dwFourCC == D3DFMT_DXT5));
}

static bool isBGRA8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 32)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff)
		&& (pf.dwAlphaBitMask == 0xff000000U));
}

static bool isBGR8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_ALPHAPIXELS)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 24)
		&& (pf.dwRBitMask == 0xff0000)
		&& (pf.dwGBitMask == 0xff00)
		&& (pf.dwBBitMask == 0xff));
}

static bool isBGR5A1(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& (pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x00007c00)
		&& (pf.dwGBitMask == 0x000003e0)
		&& (pf.dwBBitMask == 0x0000001f)
		&& (pf.dwAlphaBitMask == 0x00008000));
}

static bool isBGR565(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_RGB)
		&& !(pf.dwFlags & DDPF_ALPHAPIXELS)
		&& (pf.dwRGBBitCount == 16)
		&& (pf.dwRBitMask == 0x0000f800)
		&& (pf.dwGBitMask == 0x000007e0)
		&& (pf.dwBBitMask == 0x0000001f));
}

static bool isINDEX8(PixelFormat& pf)
{
	return ((pf.dwFlags & DDPF_INDEXED) && (pf.dwRGBBitCount == 8));
}

static LoadInfo loadInfoDXT1 = {
	true, false, false, 8, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT
};
static LoadInfo loadInfoDXT3 = {
	true, false, false, 16, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT
};
static LoadInfo loadInfoDXT5 = {
	true, false, false, 16, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT
};
static LoadInfo loadInfoATI1 = {
	true, false, false, 8, GL_COMPRESSED_RED_RGTC1, GL_ZERO
};
static LoadInfo loadInfoATI2 = {
	true, false, false, 16, GL_COMPRESSED_RG_RGTC2, GL_ZERO
};
static LoadInfo loadInfoBGRA8 = {
	false, false, false, 4, GL_RGBA8, GL_SRGB8_ALPHA8, GL_BGRA, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR8 = {
	false, false, false, 3, GL_RGB8, GL_SRGB8, GL_BGR, GL_UNSIGNED_BYTE
};
static LoadInfo loadInfoBGR5A1 = {
	false, true, false, 2, GL_RGB5_A1, GL_ZERO, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV
};
static LoadInfo loadInfoBGR565 = {
	false, true, false, 2, GL_RGB5, GL_ZERO, GL_RGB, GL_UNSIGNED_SHORT_5_6_5
};
static LoadInfo loadInfoIndex8 = {
	false, false, true, 1, GL_RGB8, GL_SRGB8, GL_BGRA, GL_UNSIGNED_BYTE
};

struct DXTColBlock
{
	uint16_t col0;
	uint16_t col1;
	u8 row[4];
};

struct DXT3AlphaBlock
{
	uint16_t row[4];
};

struct DXT5AlphaBlock
{
	u8 alpha0;
	u8 alpha1;
	u8 row[6];
};

static LUMIX_FORCE_INLINE void swapMemory(void* mem1, void* mem2, int size)
{
	if(size < 2048)
	{
		u8 tmp[2048];
		memcpy(tmp, mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, tmp, size);
	}
	else
	{
		Array<u8> tmp(*g_ffr.allocator);
		tmp.resize(size);
		memcpy(&tmp[0], mem1, size);
		memcpy(mem1, mem2, size);
		memcpy(mem2, &tmp[0], size);
	}
}

static void flipBlockDXTC1(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;

	for (int i = 0; i < numBlocks; i++)
	{
		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipBlockDXTC3(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT3AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT3AlphaBlock*)curblock;

		swapMemory(&alphablock->row[0], &alphablock->row[3], sizeof(uint16_t));
		swapMemory(&alphablock->row[1], &alphablock->row[2], sizeof(uint16_t));
		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));
		++curblock;
	}
}

static void flipDXT5Alpha(DXT5AlphaBlock *block)
{
	u8 tmp_bits[4][4];

	const uint mask = 0x00000007;
	uint bits = 0;
	memcpy(&bits, &block->row[0], sizeof(u8) * 3);

	tmp_bits[0][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[0][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[1][3] = (u8)(bits & mask);

	bits = 0;
	memcpy(&bits, &block->row[3], sizeof(u8) * 3);

	tmp_bits[2][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[2][3] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][0] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][1] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][2] = (u8)(bits & mask);
	bits >>= 3;
	tmp_bits[3][3] = (u8)(bits & mask);

	uint *out_bits = (uint*)&block->row[0];

	*out_bits = *out_bits | (tmp_bits[3][0] << 0);
	*out_bits = *out_bits | (tmp_bits[3][1] << 3);
	*out_bits = *out_bits | (tmp_bits[3][2] << 6);
	*out_bits = *out_bits | (tmp_bits[3][3] << 9);

	*out_bits = *out_bits | (tmp_bits[2][0] << 12);
	*out_bits = *out_bits | (tmp_bits[2][1] << 15);
	*out_bits = *out_bits | (tmp_bits[2][2] << 18);
	*out_bits = *out_bits | (tmp_bits[2][3] << 21);

	out_bits = (uint*)&block->row[3];

	*out_bits &= 0xff000000;

	*out_bits = *out_bits | (tmp_bits[1][0] << 0);
	*out_bits = *out_bits | (tmp_bits[1][1] << 3);
	*out_bits = *out_bits | (tmp_bits[1][2] << 6);
	*out_bits = *out_bits | (tmp_bits[1][3] << 9);

	*out_bits = *out_bits | (tmp_bits[0][0] << 12);
	*out_bits = *out_bits | (tmp_bits[0][1] << 15);
	*out_bits = *out_bits | (tmp_bits[0][2] << 18);
	*out_bits = *out_bits | (tmp_bits[0][3] << 21);
}

static void flipBlockDXTC5(DXTColBlock *line, int numBlocks)
{
	DXTColBlock *curblock = line;
	DXT5AlphaBlock *alphablock;

	for (int i = 0; i < numBlocks; i++)
	{
		alphablock = (DXT5AlphaBlock*)curblock;

		flipDXT5Alpha(alphablock);

		++curblock;

		swapMemory(&curblock->row[0], &curblock->row[3], sizeof(u8));
		swapMemory(&curblock->row[1], &curblock->row[2], sizeof(u8));

		++curblock;
	}
}

/// from gpu gems
static void flipCompressedTexture(int w, int h, int format, void* surface)
{
	void (*flipBlocksFunction)(DXTColBlock*, int);
	int xblocks = w >> 2;
	int yblocks = h >> 2;
	int blocksize;

	switch (format)
	{
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
			blocksize = 8;
			flipBlocksFunction = &flipBlockDXTC1;
			break;
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC3;
			break;
		case GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT:
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
			blocksize = 16;
			flipBlocksFunction = &flipBlockDXTC5;
			break;
		default:
			ASSERT(false);
			return;
	}

	int linesize = xblocks * blocksize;

	DXTColBlock *top = (DXTColBlock*)surface;
	DXTColBlock *bottom = (DXTColBlock*)((u8*)surface + ((yblocks - 1) * linesize));

	while (top < bottom)
	{
		(*flipBlocksFunction)(top, xblocks);
		(*flipBlocksFunction)(bottom, xblocks);
		swapMemory(bottom, top, linesize);

		top = (DXTColBlock*)((u8*)top + linesize);
		bottom = (DXTColBlock*)((u8*)bottom - linesize);
	}
}


} // namespace DDS

#ifdef _DEBUG
	#define CHECK_GL(gl) \
		do { \
			gl; \
			GLenum err = glGetError(); \
			if (err != GL_NO_ERROR) { \
				g_log_error.log("Renderer") << "OpenGL error " << err; \
			} \
		} while(false)
#else
	#define CHECK_GL(gl) do { gl; } while(false)
#endif

void checkThread()
{
	ASSERT(g_ffr.thread == GetCurrentThreadId());
}


static void try_load_renderdoc()
{
	HMODULE lib = LoadLibrary("renderdoc.dll");
	if (!lib) return;
	pRENDERDOC_GetAPI RENDERDOC_GetAPI = (pRENDERDOC_GetAPI)GetProcAddress(lib, "RENDERDOC_GetAPI");
	if (RENDERDOC_GetAPI) {
		RENDERDOC_GetAPI(eRENDERDOC_API_Version_1_1_2, (void **)&g_ffr.rdoc_api);
		g_ffr.rdoc_api->MaskOverlayBits(~RENDERDOC_OverlayBits::eRENDERDOC_Overlay_Enabled, 0);
	}
	/**/
	//FreeLibrary(lib);
}


static int load_gl(void* device_contex)
{
	HDC hdc = (HDC)device_contex;
	const HGLRC dummy_context = wglCreateContext(hdc);
	wglMakeCurrent(hdc, dummy_context);

	typedef HGLRC (WINAPI * PFNWGLCREATECONTEXTATTRIBSARBPROC) (HDC hDC, HGLRC hShareContext, const int *attribList);
	PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
	#define WGL_CONTEXT_DEBUG_BIT_ARB 0x00000001
	#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB 0x00000002
	#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
	#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
	#define WGL_CONTEXT_LAYER_PLANE_ARB 0x2093
	#define WGL_CONTEXT_FLAGS_ARB 0x2094
	#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
	#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
	
	const int32_t contextAttrs[] = {
		WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
		WGL_CONTEXT_MINOR_VERSION_ARB, 5,
		#ifdef _DEBUG
			WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
		#endif
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
		0
	};
	HGLRC hglrc = wglCreateContextAttribsARB(hdc, 0, contextAttrs);
	wglMakeCurrent(hdc, hglrc);
	wglDeleteContext(dummy_context);

	#define FFR_GL_IMPORT(prototype, name) \
		do { \
			name = (prototype)wglGetProcAddress(#name); \
			if (!name) { \
				g_log_error.log("Renderer") << "Failed to load GL function " #name "."; \
				return 0; \
			} \
		} while(0)

	#include "gl_ext.h"

	#undef FFR_GL_IMPORT

	return 1;
}


static int getSize(AttributeType type)
{
	switch(type) {
		case AttributeType::FLOAT: return 4;
		case AttributeType::U8: return 1;
		case AttributeType::I16: return 2;
		default: ASSERT(false); return 0;
	}
}


void VertexDecl::addAttribute(uint components_num, AttributeType type, bool normalized, bool as_int)
{
	if((int)attributes_count >= lengthOf(attributes)) {
		ASSERT(false);
		return;
	}

	Attribute& attr = attributes[attributes_count];
	attr.components_num = components_num;
	attr.as_int = as_int;
	attr.normalized = normalized;
	attr.type = type;
	attr.offset = 0;
	if(attributes_count > 0) {
		const Attribute& prev = attributes[attributes_count - 1];
		attr.offset = prev.offset + prev.components_num * getSize(prev.type);
	}
	size = attr.offset + attr.components_num * getSize(attr.type);
	++attributes_count;
}


void viewport(uint x,uint y,uint w,uint h)
{
	checkThread();
	glViewport(x, y, w, h);
}


void blending(int mode)
{
	checkThread();
	if (mode) {
		glEnable(GL_BLEND);
	}
	else {
		glDisable(GL_BLEND);
	}
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}


void scissor(uint x,uint y,uint w,uint h)
{
	checkThread();
	glScissor(x, y, w, h);
}

int getUniformLocation(ProgramHandle program_handle, UniformHandle uniform)
{
	const Program& prg = g_ffr.programs.values[program_handle.value];
	const Uniform& u = g_ffr.uniforms[uniform.value];
	for(int i = 0; i < prg.uniforms_count; ++i) {
		const auto& pu = prg.uniforms[i];
		if (pu.uniform.value == uniform.value) {
			return pu.loc;
		}
	}
	return -1;
}

void applyUniformMatrix4f(int location, const float* value)
{
	glUniformMatrix4fv(location, 1, false, value);
}

void applyUniformMatrix4x3f(int location, const float* value)
{
	glUniformMatrix4x3fv(location, 1, false, value);
}

void applyUniformMatrix3x4f(int location, const float* value)
{
	glUniformMatrix3x4fv(location, 1, false, value);
}

void useProgram(ProgramHandle handle)
{
	if (!handle.isValid()) return;

	const Program& prg = g_ffr.programs.values[handle.value];
	CHECK_GL(glUseProgram(prg.handle));
	
	for(int i = 0; i < prg.uniforms_count; ++i) {
		const auto& pu = prg.uniforms[i];
		const Uniform& u = g_ffr.uniforms[pu.uniform.value];
		switch(u.type) {
			case UniformType::MAT4:
				glUniformMatrix4fv(pu.loc, u.count, false, (float*)u.data);
				break;
			case UniformType::MAT4X3:
				glUniformMatrix4x3fv(pu.loc, u.count, false, (float*)u.data);
				break;
			case UniformType::MAT3X4:
				glUniformMatrix3x4fv(pu.loc, u.count, false, (float*)u.data);
				break;
			case UniformType::VEC4:
				glUniform4fv(pu.loc, u.count, (float*)u.data);
				break;
			case UniformType::VEC3:
				glUniform3fv(pu.loc, u.count, (float*)u.data);
				break;
			case UniformType::VEC2:
				glUniform2fv(pu.loc, u.count, (float*)u.data);
				break;
			case UniformType::FLOAT:
				glUniform1fv(pu.loc, u.count, (float*)u.data);
				break;
			case UniformType::INT:
				glUniform1i(pu.loc, *(int*)u.data);
				break;
			default: ASSERT(false); break;
		}
	}
}


void bindTexture(uint unit, TextureHandle handle)
{
	if(handle.isValid()) {
		const Texture& t = g_ffr.textures[handle.value];
		CHECK_GL(glActiveTexture(GL_TEXTURE0 + unit));
		CHECK_GL(glBindTexture(t.cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D, t.handle));
	}
	else {
		CHECK_GL(glActiveTexture(GL_TEXTURE0 + unit));
		CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
	}
}


void setVertexBuffer(const VertexDecl* decl, BufferHandle vertex_buffer, uint buffer_offset_bytes, const int* attribute_map)
{
	if (decl) {
		const GLsizei stride = decl->size;
		const GLuint vb = g_ffr.buffers[vertex_buffer.value].handle;
		const uint vb_offset = buffer_offset_bytes;
		CHECK_GL(glBindBuffer(GL_ARRAY_BUFFER, vb));

		for (uint i = 0; i < decl->attributes_count; ++i) {
			const Attribute* attr = &decl->attributes[i];
			const void* offset = (void*)(intptr_t)(attr->offset + vb_offset);
			GLenum gl_attr_type;
			switch (attr->type) {
				case AttributeType::I16: gl_attr_type = GL_SHORT; break;
				case AttributeType::FLOAT: gl_attr_type = GL_FLOAT; break;
				case AttributeType::U8: gl_attr_type = GL_UNSIGNED_BYTE; break;
			}
			const int index = attribute_map ? attribute_map[i] : i;

			if(index >= 0) {
				CHECK_GL(glEnableVertexAttribArray(index));
				CHECK_GL(glVertexAttribPointer(index, attr->components_num, gl_attr_type, attr->normalized, stride, offset));
			}
			else {
				glDisableVertexAttribArray(i);
			}
		}
	}
	else {
		GLint n;
		glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &n);
		for (int i = 0; i < n; ++i) {
			glDisableVertexAttribArray(i);
		}
	}
}


void setState(u64 state)
{
	if (state & u64(StateFlags::DEPTH_TEST)) CHECK_GL(glEnable(GL_DEPTH_TEST));
	else CHECK_GL(glDisable(GL_DEPTH_TEST));
	/*
	if( dc.state & u64(StateFlags::SCISSOR_TEST)) CHECK_GL(glEnable(GL_SCISSOR_TEST));
	else CHECK_GL(glDisable(GL_SCISSOR_TEST));
	*/
	// TODO
	if (state & u64(StateFlags::CULL_BACK)) {
		CHECK_GL(glEnable(GL_CULL_FACE));
		CHECK_GL(glCullFace(GL_BACK));
	}
	else if(state & u64(StateFlags::CULL_FRONT)) {
		CHECK_GL(glEnable(GL_CULL_FACE));
		CHECK_GL(glCullFace(GL_FRONT));
	}
	else {
		CHECK_GL(glDisable(GL_CULL_FACE));
	}

	CHECK_GL(glPolygonMode(GL_FRONT_AND_BACK, state & u64(StateFlags::WIREFRAME) ? GL_LINE : GL_FILL));
	
}


void setIndexBuffer(BufferHandle handle)
{
	if(handle.isValid()) {	
		const GLuint ib = g_ffr.buffers[handle.value].handle;
		CHECK_GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib));
		return;
	}

	CHECK_GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
}


void drawElements(uint offset, uint count, PrimitiveType type)
{
	checkThread();
	
	GLuint pt;
	switch (type) {
		case PrimitiveType::TRIANGLES: pt = GL_TRIANGLES; break;
		case PrimitiveType::TRIANGLE_STRIP: pt = GL_TRIANGLE_STRIP; break;
		case PrimitiveType::LINES: pt = GL_LINES; break;
		default: ASSERT(0); break;
	}

	CHECK_GL(glDrawElements(pt, count, GL_UNSIGNED_SHORT, (void*)(intptr_t)(offset * sizeof(short))));
}


void drawTriangles(uint indices_count)
{
	checkThread();
	CHECK_GL(glDrawElements(GL_TRIANGLES, indices_count, GL_UNSIGNED_SHORT, 0));
}


void drawArrays(uint offset, uint count, PrimitiveType type)
{
	checkThread();
	
	GLuint pt;
	switch (type) {
		case PrimitiveType::TRIANGLES: pt = GL_TRIANGLES; break;
		case PrimitiveType::TRIANGLE_STRIP: pt = GL_TRIANGLE_STRIP; break;
		case PrimitiveType::LINES: pt = GL_LINES; break;
		default: ASSERT(0); break;
	}

	CHECK_GL(glDrawArrays(pt, offset, count));
}


void uniformBlockBinding(ProgramHandle program, const char* block_name, uint binding)
{
	checkThread();
	const GLuint handle = g_ffr.programs.values[program.value].handle;
	const GLint index = glGetUniformBlockIndex(handle, block_name);
	CHECK_GL(glUniformBlockBinding(handle, index, binding));
}


void bindUniformBuffer(uint index, BufferHandle buffer, size_t offset, size_t size)
{
	checkThread();
	const GLuint buf = g_ffr.buffers[buffer.value].handle;
	glBindBufferRange(GL_UNIFORM_BUFFER, index, buf, offset, size);
}


void update(BufferHandle buffer, const void* data, size_t offset, size_t size)
{
	checkThread();
	const GLuint buf = g_ffr.buffers[buffer.value].handle;
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, buf));
	CHECK_GL(glBufferSubData(GL_UNIFORM_BUFFER, offset, size, data));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, 0));
}


void swapBuffers()
{
	checkThread();
	HDC hdc = (HDC)g_ffr.device_context;
	SwapBuffers(hdc);
}


void createBuffer(BufferHandle buffer, size_t size, const void* data)
{
	checkThread();
	GLuint buf;
	CHECK_GL(glGenBuffers(1, &buf));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, buf));
	CHECK_GL(glBufferData(GL_UNIFORM_BUFFER, size, data, GL_STATIC_DRAW));
	CHECK_GL(glBindBuffer(GL_UNIFORM_BUFFER, 0));

	g_ffr.buffers[buffer.value].handle = buf;
}


void destroy(ProgramHandle program)
{
	checkThread();
	if (program.isValid()) {
		glDeleteProgram(program.value);
	}
}

static struct {
	TextureFormat format;
	GLenum gl_internal;
	GLenum gl_format;
	GLenum type;
} s_texture_formats[] =
{ 
	{TextureFormat::D24, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
	{TextureFormat::D24S8, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8},
	{TextureFormat::D32, GL_DEPTH_COMPONENT32, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT},
	
	{TextureFormat::SRGB, GL_SRGB8, GL_RGBA, GL_UNSIGNED_BYTE},
	{TextureFormat::SRGBA, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE},
	{TextureFormat::RGBA8, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE},
	{TextureFormat::RGBA16F, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT},
	{TextureFormat::R16F, GL_R16F, GL_RED, GL_HALF_FLOAT},
	{TextureFormat::R16, GL_R16, GL_RED, GL_UNSIGNED_SHORT},
	{TextureFormat::R32F, GL_R32F, GL_RED, GL_FLOAT}
};


TextureInfo getTextureInfo(const void* data)
{
	const DDS::Header* hdr = (const DDS::Header*)data;
	const uint mips = (hdr->dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr->dwMipMapCount : 1;
	const bool is_cubemap = (hdr->caps2.dwCaps2 & DDS::DDSCAPS2_CUBEMAP) != 0;
	TextureInfo info;
	info.width = hdr->dwWidth;
	info.height = hdr->dwHeight;
	info.depth = 1;
	info.layers = 1;
	info.mips = mips;
	info.is_cubemap = is_cubemap;
	return info;
}


bool loadTexture(TextureHandle handle, const void* input, int input_size, uint flags, TextureInfo* info)
{
	checkThread();
	DDS::Header hdr;

	InputBlob blob(input, input_size);
	blob.read(&hdr, sizeof(hdr));

	if (hdr.dwMagic != DDS::DDS_MAGIC || hdr.dwSize != 124 ||
		!(hdr.dwFlags & DDS::DDSD_PIXELFORMAT) || !(hdr.dwFlags & DDS::DDSD_CAPS))
	{
		g_log_error.log("renderer") << "Wrong dds format or corrupted dds.";
		return false;
	}

	DDS::LoadInfo* li;

	if (isDXT1(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT1;
	}
	else if (isDXT3(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT3;
	}
	else if (isDXT5(hdr.pixelFormat)) {
		li = &DDS::loadInfoDXT5;
	}
	else if (isATI1(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI1;
	}
	else if (isATI2(hdr.pixelFormat)) {
		li = &DDS::loadInfoATI2;
	}
	else if (isBGRA8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGRA8;
	}
	else if (isBGR8(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR8;
	}
	else if (isBGR5A1(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR5A1;
	}
	else if (isBGR565(hdr.pixelFormat)) {
		li = &DDS::loadInfoBGR565;
	}
	else if (isINDEX8(hdr.pixelFormat)) {
		li = &DDS::loadInfoIndex8;
	}
	else {
		return false;
	}

	const bool is_cubemap = (hdr.caps2.dwCaps2 & DDS::DDSCAPS2_CUBEMAP) != 0;

	GLuint texture;
	glGenTextures(1, &texture);
	if (texture == 0) {
		return false;
	}

	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	const GLenum internal_format = is_srgb ? li->internalSRGBFormat : li->internalFormat;

	const uint mipMapCount = (hdr.dwFlags & DDS::DDSD_MIPMAPCOUNT) ? hdr.dwMipMapCount : 1;
	for(int side = 0; side < (is_cubemap ? 6 : 1); ++side) {

		uint width = hdr.dwWidth;
		uint height = hdr.dwHeight;

		const GLenum tex_img_target =  is_cubemap ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + side : GL_TEXTURE_2D;
		const GLenum texture_target = is_cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
		CHECK_GL(glBindTexture(texture_target, texture));

		if (li->compressed) {
			uint size = DDS::sizeDXTC(width, height, internal_format);
			if (size != hdr.dwPitchOrLinearSize || (hdr.dwFlags & DDS::DDSD_LINEARSIZE) == 0) {
				CHECK_GL(glDeleteTextures(1, &texture));
				return false;
			}
			Array<u8> data(*g_ffr.allocator);
			data.resize(size);
			for (uint ix = 0; ix < mipMapCount; ++ix) {
				blob.read(&data[0], size);
				//DDS::flipCompressedTexture(width, height, internal_format, &data[0]);
				CHECK_GL(glCompressedTexImage2D(tex_img_target, ix, internal_format, width, height, 0, size, &data[0]));
				CHECK_GL(glTexParameteri(texture_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));
				CHECK_GL(glTexParameteri(texture_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
				width = Math::maximum(1, width >> 1);
				height = Math::maximum(1, height >> 1);
				size = DDS::sizeDXTC(width, height, internal_format);
			}
		}
		else if (li->palette) {
			if ((hdr.dwFlags & DDS::DDSD_PITCH) == 0 || hdr.pixelFormat.dwRGBBitCount != 8) {
				CHECK_GL(glDeleteTextures(1, &texture));
				return false;
			}
			uint size = hdr.dwPitchOrLinearSize * height;
			if (size != width * height * li->blockBytes) {
				CHECK_GL(glDeleteTextures(1, &texture));
				return false;
			}
			Array<u8> data(*g_ffr.allocator);
			data.resize(size);
			uint palette[256];
			Array<uint> unpacked(*g_ffr.allocator);
			unpacked.resize(size);
			blob.read(palette, 4 * 256);
			for (uint ix = 0; ix < mipMapCount; ++ix) {
				blob.read(&data[0], size);
				for (uint zz = 0; zz < size; ++zz) {
					unpacked[zz] = palette[data[zz]];
				}
				//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
				CHECK_GL(glTexImage2D(tex_img_target, ix, internal_format, width, height, 0, li->externalFormat, li->type, &unpacked[0]));
				width = Math::maximum(1, width >> 1);
				height = Math::maximum(1, height >> 1);
				size = width * height * li->blockBytes;
			}
		}
		else {
			if (li->swap) {
				CHECK_GL(glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE));
			}
			uint size = width * height * li->blockBytes;
			Array<u8> data(*g_ffr.allocator);
			data.resize(size);
			for (uint ix = 0; ix < mipMapCount; ++ix) {
				blob.read(&data[0], size);
				//glPixelStorei(GL_UNPACK_ROW_LENGTH, height);
				CHECK_GL(glTexImage2D(tex_img_target, ix, internal_format, width, height, 0, li->externalFormat, li->type, &data[0]));
				width = Math::maximum(1, width >> 1);
				height = Math::maximum(1, height >> 1);
				size = width * height * li->blockBytes;
			}
			CHECK_GL(glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_FALSE));
		}
		CHECK_GL(glTexParameteri(texture_target, GL_TEXTURE_MAX_LEVEL, mipMapCount - 1));
	}

	if(info) {
		info->width = hdr.dwWidth;
		info->height = hdr.dwHeight;
		info->depth = 1;
		info->layers = 1;
		info->mips = mipMapCount;
		info->is_cubemap = is_cubemap;
	}

	Texture& t = g_ffr.textures[handle.value];
	t.handle = texture;
	t.cubemap = is_cubemap;
	return true;
}


BufferHandle allocBufferHandle()
{
	MT::SpinLock lock(g_ffr.handle_mutex);

	if(g_ffr.buffers.isFull()) {
		g_log_error.log("Renderer") << "FFR is out of free buffer slots.";
		return INVALID_BUFFER;
	}
	const int id = g_ffr.buffers.alloc();
	Buffer& t = g_ffr.buffers[id];
	t.handle = 0;
	return { (uint)id };
}


TextureHandle allocTextureHandle()
{
	MT::SpinLock lock(g_ffr.handle_mutex);

	if(g_ffr.textures.isFull()) {
		g_log_error.log("Renderer") << "FFR is out of free texture slots.";
		return INVALID_TEXTURE;
	}
	const int id = g_ffr.textures.alloc();
	Texture& t = g_ffr.textures[id];
	t.handle = 0;
	return { (uint)id };
}


bool createTexture(TextureHandle handle, uint w,uint h, TextureFormat format, uint flags, const void* data)
{
	checkThread();
	const bool is_srgb = flags & (u32)TextureFlags::SRGB;
	ASSERT(!is_srgb); // use format argument to enable srgb

	GLuint texture;
	CHECK_GL(glGenTextures(1, &texture));
	CHECK_GL(glBindTexture(GL_TEXTURE_2D, texture));
	int found_format = 0;
	for (int i = 0; i < sizeof(s_texture_formats) / sizeof(s_texture_formats[0]); ++i) {
		if(s_texture_formats[i].format == format) {
			CHECK_GL(glTexImage2D(GL_TEXTURE_2D
				, 0
				, s_texture_formats[i].gl_internal
				, w
				, h
				, 0
				, s_texture_formats[i].gl_format
				, s_texture_formats[i].type
				, data));
			found_format = 1;
			break;
		}
	}

	if(!found_format) {
		CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
		CHECK_GL(glDeleteTextures(1, &texture));
		return false;	
	}

	CHECK_GL(glGenerateMipmap(GL_TEXTURE_2D));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
	CHECK_GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR));

	CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));

	Texture& t = g_ffr.textures[handle.value];
	t.handle = texture;
	t.cubemap = false;

	return true;
}


void destroy(TextureHandle texture)
{
	checkThread();
	Texture& t = g_ffr.textures[texture.value];
	const GLuint handle = t.handle;
	CHECK_GL(glDeleteTextures(1, &handle));

	MT::SpinLock lock(g_ffr.handle_mutex);
	g_ffr.textures.dealloc(texture.value);
}


void destroy(BufferHandle buffer)
{
	checkThread();
	
	Buffer& t = g_ffr.buffers[buffer.value];
	const GLuint handle = t.handle;
	CHECK_GL(glDeleteBuffers(1, &handle));

	MT::SpinLock lock(g_ffr.handle_mutex);
	g_ffr.buffers.dealloc(buffer.value);

}


void clear(uint flags, const float* color, float depth)
{
	checkThread();
	GLbitfield gl_flags = 0;
	if (flags & (uint)ClearFlags::COLOR) {
		CHECK_GL(glClearColor(color[0], color[1], color[2], color[3]));
		gl_flags |= GL_COLOR_BUFFER_BIT;
	}
	if (flags & (uint)ClearFlags::DEPTH) {
		CHECK_GL(glClearDepth(depth));
		gl_flags |= GL_DEPTH_BUFFER_BIT;
	}
	CHECK_GL(glUseProgram(0));
	CHECK_GL(glClear(gl_flags));
}

static const char* shaderTypeToString(ShaderType type)
{
	switch(type) {
		case ShaderType::FRAGMENT: return "fragment shader";
		case ShaderType::VERTEX: return "vertex shader";
		default: return "unknown shader type";
	}
}


static uint getSize(UniformType type)
{
	switch(type)
	{
	case UniformType::INT: return sizeof(int);
	case UniformType::FLOAT: return sizeof(float);
	case UniformType::VEC2: return sizeof(float) * 2;
	case UniformType::VEC3: return sizeof(float) * 3;
	case UniformType::VEC4: return sizeof(float) * 4;
	case UniformType::MAT4: return sizeof(float) * 16;
	case UniformType::MAT4X3: return sizeof(float) * 12;
	case UniformType::MAT3X4: return sizeof(float) * 12;
	default:
		ASSERT(false);
		return 4;
	}
}


UniformHandle allocUniform(const char* name, UniformType type, int count)
{
	const u32 name_hash = crc32(name);
	
	MT::SpinLock lock(g_ffr.handle_mutex);

	auto iter = g_ffr.uniforms_hash_map->find(name_hash);
	if(iter.isValid()) {
		return { iter.value() };
	}

	if(g_ffr.uniforms.isFull()) {
		g_log_error.log("Renderer") << "FFR is out of free uniform slots.";
		return INVALID_UNIFORM;
	}
	const int id = g_ffr.uniforms.alloc();
	Uniform& u = g_ffr.uniforms[id];
	u.count = count;
	u.type = type;
	#ifdef _DEBUG
		u.debug_name = name;
	#endif
	size_t byte_size = getSize(type) * count;
	u.data = g_ffr.allocator->allocate(byte_size);
	setMemory(u.data, 0, byte_size);
	g_ffr.uniforms_hash_map->insert(name_hash, id);
	return { (uint)id };
}


ProgramHandle createProgram(const char** srcs, const ShaderType* types, int num, const char** prefixes, int prefixes_count, const char* name)
{
	checkThread();
	if(g_ffr.programs.isFull()) {
		g_log_error.log("Renderer") << "FFR is out of free program slots.";
		return INVALID_PROGRAM;
	}

	const char* combined_srcs[16];
	ASSERT(prefixes_count < lengthOf(combined_srcs) - 1); 
	enum { MAX_SHADERS_PER_PROGRAM = 16 };

	if (num > MAX_SHADERS_PER_PROGRAM) {
		g_log_error.log("Renderer") << "Too many shaders per program in " << name;
		return INVALID_PROGRAM;
	}

	const GLuint prg = glCreateProgram();

	for (int i = 0; i < num; ++i) {
		GLenum shader_type;
		switch (types[i]) {
			case ShaderType::FRAGMENT: shader_type = GL_FRAGMENT_SHADER; break;
			case ShaderType::VERTEX: shader_type = GL_VERTEX_SHADER; break;
			default: ASSERT(0); break;
		}
		const GLuint shd = glCreateShader(shader_type);
		combined_srcs[prefixes_count] = srcs[i];
		for (int j = 0; j < prefixes_count; ++j) {
			combined_srcs[j] = prefixes[j];
		}

		CHECK_GL(glShaderSource(shd, 1 + prefixes_count, combined_srcs, 0));
		CHECK_GL(glCompileShader(shd));

		GLint compile_status;
		CHECK_GL(glGetShaderiv(shd, GL_COMPILE_STATUS, &compile_status));
		if (compile_status == GL_FALSE) {
			GLint log_len = 0;
			CHECK_GL(glGetShaderiv(shd, GL_INFO_LOG_LENGTH, &log_len));
			if (log_len > 0) {
				Array<char> log_buf(*g_ffr.allocator);
				log_buf.resize(log_len);
				CHECK_GL(glGetShaderInfoLog(shd, log_len, &log_len, &log_buf[0]));
				g_log_error.log("Renderer") << name << " - " << shaderTypeToString(types[i]) << ": " << &log_buf[0];
			}
			else {
				g_log_error.log("Renderer") << "Failed to compile shader " << name << " - " << shaderTypeToString(types[i]);
			}
			CHECK_GL(glDeleteShader(shd));
			return INVALID_PROGRAM;
		}

		CHECK_GL(glAttachShader(prg, shd));
		CHECK_GL(glDeleteShader(shd));
	}

	CHECK_GL(glLinkProgram(prg));
	GLint linked;
	CHECK_GL(glGetProgramiv(prg, GL_LINK_STATUS, &linked));

	if (linked == GL_FALSE) {
		GLint log_len = 0;
		CHECK_GL(glGetProgramiv(prg, GL_INFO_LOG_LENGTH, &log_len));
		if (log_len > 0) {
			Array<char> log_buf(*g_ffr.allocator);
			log_buf.resize(log_len);
			CHECK_GL(glGetProgramInfoLog(prg, log_len, &log_len, &log_buf[0]));
			g_log_error.log("Renderer") << name << ": " << &log_buf[0];
		}
		else {
			g_log_error.log("Renderer") << "Failed to link program " << name;
		}
		CHECK_GL(glDeleteProgram(prg));
		return INVALID_PROGRAM;
	}

	const int id = g_ffr.programs.alloc();
	g_ffr.programs[id].handle = prg;
	GLint uniforms_count;
	CHECK_GL(glGetProgramiv(prg, GL_ACTIVE_UNIFORMS, &uniforms_count));
	if(uniforms_count > lengthOf(g_ffr.programs[id].uniforms)) {
		uniforms_count = lengthOf(g_ffr.programs[id].uniforms);
		g_log_error.log("Renderer") << "Too many uniforms per program, not all will be used.";
	}
	g_ffr.programs[id].uniforms_count = 0;
	for(int i = 0; i < uniforms_count; ++i) {
		char name[32];
		GLint size;
		GLenum type;
		UniformType ffr_type;
		glGetActiveUniform(prg, i, sizeof(name), nullptr, &size, &type, name);
		switch(type) {
			case GL_SAMPLER_CUBE:
			case GL_SAMPLER_2D:
			case GL_INT: ffr_type = UniformType::INT; break;
			case GL_FLOAT: ffr_type = UniformType::FLOAT; break;
			case GL_FLOAT_VEC2: ffr_type = UniformType::VEC2; break;
			case GL_FLOAT_VEC3: ffr_type = UniformType::VEC3; break;
			case GL_FLOAT_VEC4: ffr_type = UniformType::VEC4; break;
			case GL_FLOAT_MAT4: ffr_type = UniformType::MAT4; break;
			case GL_FLOAT_MAT4x3: ffr_type = UniformType::MAT4X3; break;
			case GL_FLOAT_MAT3x4: ffr_type = UniformType::MAT3X4; break;
			default: ASSERT(false); ffr_type = UniformType::VEC4; break;
		}

		const int loc = glGetUniformLocation(prg, name);

		if(loc >= 0) {
			auto& u = g_ffr.programs[id].uniforms[g_ffr.programs[id].uniforms_count];
			u.loc = loc;
			u.uniform = allocUniform(name, ffr_type, size);
			++g_ffr.programs[id].uniforms_count;
		}
	}

	return { (uint)id };
}


static void gl_debug_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *userParam)
{
	if(GL_DEBUG_TYPE_PUSH_GROUP == type || type == GL_DEBUG_TYPE_POP_GROUP) return;
	if (type == GL_DEBUG_TYPE_ERROR || type == GL_DEBUG_TYPE_PERFORMANCE) {
		g_log_error.log("GL") << message;
	}
	else {
		g_log_info.log("GL") << message;
	}
}


void preinit(IAllocator& allocator)
{
	try_load_renderdoc();
	g_ffr.allocator = &allocator;
	g_ffr.textures.create(*g_ffr.allocator);
	g_ffr.buffers.create(*g_ffr.allocator);
	g_ffr.uniforms.create(*g_ffr.allocator);
	g_ffr.programs.create(*g_ffr.allocator);
	g_ffr.uniforms_hash_map = LUMIX_NEW(*g_ffr.allocator, HashMap<u32, uint>)(*g_ffr.allocator);
}


bool init(void* window_handle)
{
	g_ffr.device_context = GetDC((HWND)window_handle);
	g_ffr.thread = GetCurrentThreadId();

	if (!load_gl(g_ffr.device_context)) return false;

/*	int extensions_count;
	glGetIntegerv(GL_NUM_EXTENSIONS, &extensions_count);
	for(int i = 0; i < extensions_count; ++i) {
		const char* ext = (const char*)glGetStringi(GL_EXTENSIONS, i);
		OutputDebugString(ext);
		OutputDebugString("\n");
	}
	const unsigned char* extensions = glGetString(GL_EXTENSIONS);
	const unsigned char* version = glGetString(GL_VERSION);*/

	CHECK_GL(glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE));
	CHECK_GL(glDepthFunc(GL_GREATER));

	#ifdef _DEBUG
		CHECK_GL(glEnable(GL_DEBUG_OUTPUT));
		CHECK_GL(glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS));
		CHECK_GL(glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, 0, GL_TRUE));
		CHECK_GL(glDebugMessageCallback(gl_debug_callback, 0));
	#endif

	CHECK_GL(glGenVertexArrays(1, &g_ffr.vao));
	CHECK_GL(glBindVertexArray(g_ffr.vao));
	CHECK_GL(glGenTextures(_countof(g_ffr.tex_buffers), g_ffr.tex_buffers));

	return true;
}


bool isHomogenousDepth() { return false; }


bool isOriginBottomLeft() { return true; }


void getTextureImage(ffr::TextureHandle texture, uint size, void* buf)
{
	checkThread();
	glGetTextureImage(texture.value, 0, GL_RGBA, GL_UNSIGNED_BYTE, size, buf);
}


void popDebugGroup()
{
	checkThread();
	glPopDebugGroup();
}


void pushDebugGroup(const char* msg)
{
	checkThread();
	glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, msg);
}


void destroy(FramebufferHandle fb)
{
	checkThread();
	CHECK_GL(glDeleteFramebuffers(1, &fb.value));
}


int getAttribLocation(ProgramHandle program, const char* uniform_name)
{
	checkThread();
	return glGetAttribLocation(g_ffr.programs.values[program.value].handle, uniform_name);
}


void setUniform1i(UniformHandle uniform, int value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::INT);
	memcpy(g_ffr.uniforms[uniform.value].data, &value, sizeof(value)); 
}


void setUniform2f(UniformHandle uniform, const float* value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::VEC2);
	memcpy(g_ffr.uniforms[uniform.value].data, value, sizeof(value[0]) * 2); 
}


void setUniform4f(UniformHandle uniform, const float* value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::VEC4);
	memcpy(g_ffr.uniforms[uniform.value].data, value, sizeof(value[0]) * 4); 
}


void setUniformMatrix4f(UniformHandle uniform, const float* value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::MAT4);
	memcpy(g_ffr.uniforms[uniform.value].data, value, sizeof(value[0]) * 16); 
}


void setUniformMatrix4x3f(UniformHandle uniform, const float* value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::MAT4X3);
	memcpy(g_ffr.uniforms[uniform.value].data, value, sizeof(value[0]) * 12); 
}


void setUniformMatrix3x4f(UniformHandle uniform, const float* value)
{
	checkThread();
	ASSERT(g_ffr.uniforms[uniform.value].type == UniformType::MAT3X4);
	memcpy(g_ffr.uniforms[uniform.value].data, value, sizeof(value[0]) * 12); 
}


void update(FramebufferHandle fb, uint renderbuffers_count, const TextureHandle* renderbuffers)
{
	checkThread();

	int color_attachment_idx = 0;
	bool depth_bound = false;
	for (uint i = 0; i < renderbuffers_count; ++i) {
		const GLuint t = g_ffr.textures[renderbuffers[i].value].handle;
		CHECK_GL(glBindTexture(GL_TEXTURE_2D, t));
		GLint internal_format;
		CHECK_GL(glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal_format));

		CHECK_GL(glBindTexture(GL_TEXTURE_2D, 0));
		switch(internal_format) {
			case GL_DEPTH24_STENCIL8:
			case GL_DEPTH_COMPONENT24:
			case GL_DEPTH_COMPONENT32:
				CHECK_GL(glNamedFramebufferTexture(fb.value, GL_DEPTH_ATTACHMENT, t, 0));
				depth_bound = true;
				break;
			default:
				CHECK_GL(glNamedFramebufferTexture(fb.value, GL_COLOR_ATTACHMENT0 + color_attachment_idx, t, 0));
				++color_attachment_idx;
				break;
		}
	}

	GLint max_attachments = 0;
	glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_attachments);
	for(int i = color_attachment_idx; i < max_attachments; ++i) {
		glNamedFramebufferRenderbuffer(fb.value, GL_COLOR_ATTACHMENT0 + i, GL_RENDERBUFFER, 0);
	}
	if (!depth_bound) {
		glNamedFramebufferRenderbuffer(fb.value, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
	}
}


QueryHandle createQuery()
{
	GLuint q;
	CHECK_GL(glGenQueries(1, &q));
	return {q};
}


u64 getQueryResult(QueryHandle query)
{
	u64 time;
	glGetQueryObjectui64v(query.value, GL_QUERY_RESULT, &time);
	return time;
}


void destroy(QueryHandle query)
{
	glDeleteQueries(1, &query.value);
}


void queryTimestamp(QueryHandle query)
{
	glQueryCounter(query.value, GL_TIMESTAMP);
}


FramebufferHandle createFramebuffer(uint renderbuffers_count, const TextureHandle* renderbuffers)
{
	checkThread();
	GLuint fb;
	CHECK_GL(glGenFramebuffers(1, &fb));
	CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, fb));
	CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

	update({fb}, renderbuffers_count, renderbuffers);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
	if (status != GL_FRAMEBUFFER_COMPLETE) {
		CHECK_GL(glDeleteFramebuffers(1, &fb));
		return INVALID_FRAMEBUFFER;
	}

	return {fb};
}


void setFramebuffer(FramebufferHandle fb, bool srgb)
{
	checkThread();
	if(fb.value == 0xffFFffFF) {
		CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
	}
	else {
		CHECK_GL(glBindFramebuffer(GL_FRAMEBUFFER, fb.value));
		const GLenum db[] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
		CHECK_GL(glDrawBuffers(2, db));
	}
	if(srgb) {	
		CHECK_GL(glEnable(GL_FRAMEBUFFER_SRGB));
	}
	else {
		CHECK_GL(glDisable(GL_FRAMEBUFFER_SRGB));
	}
}


void shutdown()
{
	checkThread();
	g_ffr.textures.destroy(*g_ffr.allocator);
	g_ffr.buffers.destroy(*g_ffr.allocator);
	g_ffr.uniforms.destroy(*g_ffr.allocator);
	g_ffr.programs.destroy(*g_ffr.allocator);
	LUMIX_DELETE(*g_ffr.allocator, g_ffr.uniforms_hash_map);
}

} // ns ffr 

} // ns Lumix
#pragma once

#include "shader.h"
#include "shader_code.h"
#include "shader_pdz.h"

inline constexpr char SWIZZLES[] = { 'x', 'y', 'z', 'w', '0', '1', '_', '_' };

// pdz branch: per-shader state collected while translating without a constant table
struct PdzVertexInput
{
    uint32_t address = 0, slot = 0, offset = 0, stride = 0, format = 0, srcRegister = 0, srcSwizzle = 0;
    int32_t expAdjust = 0;
    bool isSigned = false, normalized = false, isMini = false, bound = false;
    uint32_t usage = 0xFFFFFFFF, usageIndex = 0;
    std::string name, semantic, hlslType, dxgiFormat, convert, convertedName;
};

// A vertex fetch whose index is not the vertex index (r0.x as the hardware loaded it): the vertex is
// computed by the shader, so the input assembler cannot deliver it.  Read from a raw buffer instead
// (SDK dxbc_translator_fetch.cpp ProcessVertexFetchInstruction: index = floor(src) or floor(src + 0.5),
// address = index * stride + offset dwords; vfetch_mini reuses the address of the previous full fetch).
struct PdzRawVertexFetch
{
    uint32_t address = 0, slot = 0, offset = 0, stride = 0, format = 0, srcRegister = 0, srcSwizzle = 0;
    int32_t expAdjust = 0;
    bool isSigned = false, normalized = false, isMini = false, indexRounded = false;
};

struct PdzSamplerVariant
{
    uint32_t slot, magFilter, minFilter, mipFilter, anisoFilter, volMagFilter, volMinFilter, dimension;
};

struct PdzTextureFetchSite
{
    uint32_t slot, dimension;
    bool unnormalized, computedLod, registerLod, offsets;
    float lodBias;
};

struct PdzLoopSite
{
    uint32_t constant;
    bool repeat;
    bool predicatedBreak;
};

// pdz: per-shader helper functions emitted only when an instruction uses them (shader_common_pdz.hlsli stays
// identical across shaders)
enum : uint32_t
{
    PDZ_HELPER_RCPF = 1u << 0,
    PDZ_HELPER_RCPC = 1u << 1,
    PDZ_HELPER_RSQF = 1u << 2,
    PDZ_HELPER_RSQC = 1u << 3,
    PDZ_HELPER_LOGC = 1u << 4,
};

struct PdzState
{
    bool simpleControlFlow = true;
    bool usesLoopInSwitch = false;
    bool usesRelativeConstants = false;
    bool writesA0 = false;
    bool usesKill = false;
    bool writesDepth = false;
    bool writesPosition = false;
    bool writesPointSize = false;
    bool vertexTextureFetch = false;
    bool usesRegisterLod = false;
    uint32_t scalarHelpers = 0; // PDZ_HELPER_*
    uint32_t cfBytes = 0;
    uint32_t colorOutputs = 0;
    uint32_t interpolatorsWritten = 0;
    uint32_t interpolatorsRead = 0;
    std::set<uint32_t> liveInRegisters;
    std::set<uint32_t> floatConstantsUsed;
    std::set<uint32_t> boolConstantsUsed;
    std::set<uint32_t> loopConstantsUsed;
    std::map<uint32_t, uint32_t> textureSlots; // slot -> dimension mask
    std::unordered_map<uint32_t, const pdz::LiteralRecord*> literalRegisters;
    std::vector<PdzVertexInput> vertexInputs;
    std::unordered_map<uint32_t, uint32_t> vertexInputsByAddress;
    std::vector<PdzRawVertexFetch> rawVertexFetches;            // computed-index fetches (raw buffer path)
    bool vfetchIndexFromHardwareRegister = false;               // a computed index the shader never wrote (domain shader input)
    std::unordered_map<uint32_t, uint32_t> rawVertexFetchesByAddress;
    std::vector<PdzSamplerVariant> samplerVariants;
    std::vector<PdzLoopSite> loopSites;
    std::vector<PdzTextureFetchSite> textureFetchSites;
    std::vector<std::string> unsupported;
};

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    uint32_t specConstantsMask = 0;

    // pdz branch
    bool pdzMode = false;
    // pdz: XENOSRECOMP_PDZ_CLAMP_LOG_RCP=1 keeps the old +-FLT_MAX clamp on the unclamped log / rcp / rsq.  The SDK
    // leaves them unclamped but multiplies with the SM3 rule (0 * anything = 0, ORACLE_DIFF 4.1), which this
    // translator does not, so 0 * log2(0) is NaN here where the hardware gives 0 (pow(0, 0) via exp/log).
    bool pdzClampLogRcp = false;
    PdzState pdz;
    std::string pdzJson;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    static bool hasDstComponents(uint32_t dstSwizzle)
    {
        for (uint32_t i = 0; i < 4; i++)
            if (((dstSwizzle >> (i * 3)) & 0x7) <= 3)
                return true;
        return false;
    }
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);

    // pdz branch
    void recompilePdz(const pdz::Shader& shader, const std::string_view& include);
    void recompilePdzVertexFetch(const VertexFetchInstruction& instr, uint32_t address);
    void recompilePdzTextureFetch(const TextureFetchInstruction& instr);
    uint32_t pdzSamplerVariant(const TextureFetchInstruction& instr);
    static uint32_t pdzSamplerCode(const PdzSamplerVariant& v);
    std::string pdzConstantRef(uint32_t reg, bool relative, bool useA0);
    void pdzBuildJson(const pdz::Shader& shader);
};

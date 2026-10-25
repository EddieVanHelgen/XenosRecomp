// Command-line front end of the pdz branch (legacy 0x102A0E0x containers and raw microcode).
#include "shader_pdz.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

static std::vector<uint8_t> pdzReadFile(const char* path)
{
    std::vector<uint8_t> data;
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
        return data;
    fseek(file, 0, SEEK_END);
    data.resize(ftell(file));
    fseek(file, 0, SEEK_SET);
    fread(data.data(), 1, data.size(), file);
    fclose(file);
    return data;
}

static bool pdzWriteFile(const char* path, const void* data, size_t size)
{
    FILE* file = fopen(path, "wb");
    if (file == nullptr)
        return false;
    fwrite(data, 1, size, file);
    fclose(file);
    return true;
}

static float pdzAsFloat(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static const char* USAGE_NAMES[] =
{
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
    "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE", "?14", "?15"
};

static bool pdzLoadShader(const char* path, const char* typeArg, std::vector<uint8_t>& data, pdz::Shader& shader)
{
    data = pdzReadFile(path);
    if (data.empty())
    {
        fmt::println(stderr, "cannot read {}", path);
        return false;
    }
    std::string error;
    if (pdz::isLegacyContainer(data.data(), data.size()))
    {
        if (!pdz::parseLegacyContainer(data.data(), data.size(), shader, error))
        {
            fmt::println(stderr, "{}: {}", path, error);
            return false;
        }
        return true;
    }
    if (typeArg == nullptr)
    {
        fmt::println(stderr, "{}: raw microcode needs --type vs|ps", path);
        return false;
    }
    const bool isPixel = strcmp(typeArg, "ps") == 0;
    if (!isPixel && strcmp(typeArg, "vs") != 0)
    {
        fmt::println(stderr, "--type must be vs or ps");
        return false;
    }
    if (!pdz::parseRawUcode(data.data(), data.size(), isPixel, shader, error))
    {
        fmt::println(stderr, "{}: {}", path, error);
        return false;
    }
    return true;
}

static int pdzDump(const char* path, const char* typeArg)
{
    std::vector<uint8_t> data;
    pdz::Shader shader;
    if (!pdzLoadShader(path, typeArg, data, shader))
        return 1;

    fmt::println("{}: {} {} ucode dwords {}", path, shader.fromContainer ? "container" : "raw",
        shader.isPixelShader ? "ps" : "vs", shader.ucodeDwords);
    if (shader.fromContainer)
        fmt::println("  program word0 {:#x} word4 {:#x} outputMask {:#x}", shader.programWord0, shader.programWord4, shader.outputMaskRaw);
    for (auto& lit : shader.literals)
    {
        fmt::print("  literal c{}..c{}:", lit.registerIndex, lit.registerIndex + (lit.dwords.size() + 3) / 4 - 1);
        for (size_t i = 0; i < lit.dwords.size(); i++)
            fmt::print(" {}{:g}", (i % 4) == 0 ? "| " : "", pdzAsFloat(lit.dwords[i]));
        fmt::println("");
    }
    for (auto& c : shader.constantNames)
        fmt::println("  ctab {} : {}{}+{}", c.name, "bics"[uint32_t(c.set) & 3], c.registerIndex, c.registerCount);
    if (!shader.ctabCreator.empty())
        fmt::println("  ctab creator \"{}\" target {}", shader.ctabCreator, shader.ctabTarget);
    for (auto& e : shader.vertexElements)
        fmt::println("  vertex element: vfetch at {} -> {}{} (raw {:#010x})", e.address, USAGE_NAMES[e.usage], e.usageIndex, e.raw);
    for (auto& i : shader.interpolators)
        fmt::println("  interpolator: {}{} = {}{} mask {:#x} (raw {:#010x})", shader.isPixelShader ? "r" : "o", i.reg, USAGE_NAMES[i.usage], i.usageIndex, i.mask, i.raw);
    for (auto p : shader.patchList)
        fmt::println("  patch: {:#x}", p);
    return 0;
}

static int pdzTranslate(const char* input, const char* output, const char* typeArg, const char* includePath,
    const char* jsonPath, const char* dxilPath, const char* exePath)
{
    std::vector<uint8_t> data;
    pdz::Shader shader;
    if (!pdzLoadShader(input, typeArg, data, shader))
        return 1;

    std::string includeFile = includePath != nullptr ? includePath
        : (std::filesystem::path(exePath).parent_path() / "shader_common_pdz.hlsli").string();
    std::vector<uint8_t> includeData = pdzReadFile(includeFile.c_str());
    if (includeData.empty())
    {
        fmt::println(stderr, "cannot read the include file {}", includeFile);
        return 1;
    }
    std::string_view include(reinterpret_cast<const char*>(includeData.data()), includeData.size());

    ShaderRecompiler recompiler;
    recompiler.recompilePdz(shader, include);

    if (!pdzWriteFile(output, recompiler.out.data(), recompiler.out.size()))
    {
        fmt::println(stderr, "cannot write {}", output);
        return 1;
    }
    if (jsonPath != nullptr && !pdzWriteFile(jsonPath, recompiler.pdzJson.data(), recompiler.pdzJson.size()))
    {
        fmt::println(stderr, "cannot write {}", jsonPath);
        return 1;
    }

    int result = 0;
    if (!recompiler.pdz.unsupported.empty())
    {
        std::set<std::string> unique(recompiler.pdz.unsupported.begin(), recompiler.pdz.unsupported.end());
        for (auto& u : unique)
            fmt::println(stderr, "{}: unsupported: {}", input, u);
        result = 3;
    }

    if (dxilPath != nullptr)
    {
        DxcCompiler compiler;
        IDxcBlob* dxil = compiler.compile(recompiler.out, recompiler.isPixelShader, false, false);
        if (dxil == nullptr)
        {
            fmt::println(stderr, "{}: dxc failed", input);
            return 4;
        }
        pdzWriteFile(dxilPath, dxil->GetBufferPointer(), dxil->GetBufferSize());
        dxil->Release();
    }
    return result;
}

static void pdzUsage()
{
    fmt::println("pdz branch usage:");
    fmt::println("  XenosRecomp --pdz-dump <container.bin | ucode.bin> [--type vs|ps]");
    fmt::println("  XenosRecomp --pdz <container.bin | ucode.bin> <out.hlsl> [--type vs|ps] [--include shader_common_pdz.hlsli]");
    fmt::println("                    [--json out.json] [--dxil out.dxil]");
    fmt::println("  A 0x102A0E0x container is recognised by its magic; anything else is raw guest-order microcode and needs --type.");
}

int pdzMain(int argc, char** argv)
{
    if (argc < 3)
    {
        pdzUsage();
        return 1;
    }

    const char* command = argv[1];
    const char* input = argv[2];
    const char* output = nullptr;
    const char* typeArg = nullptr;
    const char* includePath = nullptr;
    const char* jsonPath = nullptr;
    const char* dxilPath = nullptr;

    int positional = 0;
    for (int i = 3; i < argc; i++)
    {
        auto takeValue = [&](const char*& dst) -> bool
        {
            if (i + 1 >= argc)
            {
                fmt::println(stderr, "{} needs a value", argv[i]);
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (strcmp(argv[i], "--type") == 0) { if (!takeValue(typeArg)) return 1; }
        else if (strcmp(argv[i], "--include") == 0) { if (!takeValue(includePath)) return 1; }
        else if (strcmp(argv[i], "--json") == 0) { if (!takeValue(jsonPath)) return 1; }
        else if (strcmp(argv[i], "--dxil") == 0) { if (!takeValue(dxilPath)) return 1; }
        else if (argv[i][0] == '-') { fmt::println(stderr, "unknown option {}", argv[i]); pdzUsage(); return 1; }
        else if (positional == 0) { output = argv[i]; positional++; }
        else { fmt::println(stderr, "unexpected argument {}", argv[i]); return 1; }
    }

    if (strcmp(command, "--pdz-dump") == 0)
        return pdzDump(input, typeArg);

    if (strcmp(command, "--pdz") == 0)
    {
        if (output == nullptr)
        {
            pdzUsage();
            return 1;
        }
        return pdzTranslate(input, output, typeArg, includePath, jsonPath, dxilPath, argv[0]);
    }

    pdzUsage();
    return 1;
}

// Exercise the app-local shader compilers without requiring a graphics device.
#include <windows.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;

static void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

int main()
{
    try
    {
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        Check(length && length < 32768, "Cannot locate the test executable");
        const auto directory = std::filesystem::path(executable).parent_path();
        auto load = [&](const wchar_t* name) {
            const auto path = directory / name;
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            Check(module != nullptr, "Cannot load an app-local shader compiler DLL");
            wchar_t loaded[32768]{};
            Check(GetModuleFileNameW(module, loaded, 32768) != 0, "Cannot locate the loaded DLL");
            Check(std::filesystem::equivalent(path, loaded), "Shader compiler came from outside the package");
            std::wcout << loaded << L'\n';
            return module;
        };
        load(L"dxil.dll"); // Signing library must be loaded before DXC.
        auto dxc = load(L"dxcompiler.dll");
        auto fxc = load(L"d3dcompiler_47.dll");
        auto create = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance"));
        auto compile = reinterpret_cast<pD3DCompile>(GetProcAddress(fxc, "D3DCompile"));
        Check(create && compile, "Shader compiler entry points are missing");

        constexpr char shader[] = "float4 main(float4 p : SV_Position) : SV_Target { return float4(p.xy * 0.01, 0.5, 1); }";
        ComPtr<IDxcCompiler3> compiler;
        Check(SUCCEEDED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))), "Cannot create DXC");
        DxcBuffer input{shader, sizeof(shader) - 1, DXC_CP_UTF8};
        const wchar_t* arguments[] = {L"-E", L"main", L"-T", L"ps_6_0"};
        ComPtr<IDxcResult> result;
        Check(SUCCEEDED(compiler->Compile(&input, arguments, 4, nullptr, IID_PPV_ARGS(&result))), "DXC call failed");
        HRESULT status = E_FAIL;
        Check(SUCCEEDED(result->GetStatus(&status)) && SUCCEEDED(status), "DXC shader compilation failed");
        ComPtr<IDxcBlob> dxil;
        Check(SUCCEEDED(result->GetResult(&dxil)) && dxil->GetBufferSize() > 0, "DXC returned no shader");
        ComPtr<ID3DBlob> dxbc, errors;
        Check(SUCCEEDED(compile(shader, sizeof(shader) - 1, "test", nullptr, nullptr,
            "main", "ps_5_1", D3DCOMPILE_ENABLE_STRICTNESS, 0, &dxbc, &errors)), "FXC shader compilation failed");
        Check(dxbc && dxbc->GetBufferSize() > 0, "FXC returned no shader");
        std::cout << "App-local DXC and FXC shader compilation passed\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << " (Windows error " << GetLastError() << ")\n";
        return 1;
    }
}

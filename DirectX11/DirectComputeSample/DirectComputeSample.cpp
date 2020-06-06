#include <assert.h>
#include <stdio.h>
#include <tchar.h>

#include <vector>

#include <shlwapi.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#include <unknwn.h>
#include <winrt\base.h>
#include <winrt\Windows.Foundation.h>
#include <winrt\Windows.Foundation.Diagnostics.h>
#include <winrt\Windows.Foundation.Collections.h>

#pragma comment(lib, "windowsapp.lib")

VOID CompileFile(TCHAR const* FileName, LPCSTR SourceName, D3D_SHADER_MACRO const* Macros, CHAR* EntryPoint, CHAR* Target, DWORD Flags1, DWORD Flags2, ID3DBlob** CodeBlob, ID3DBlob** ErrorBlob)
{
    assert(FileName);
    assert(EntryPoint && Target);
    assert(CodeBlob && ErrorBlob);
    TCHAR Path[MAX_PATH];
    GetModuleFileName(nullptr, Path, static_cast<DWORD>(std::size(Path)));
    PathRemoveFileSpec(Path);
    PathCombine(Path, Path, FileName);
    winrt::file_handle File { CreateFile(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
    winrt::check_bool(!!File);
    BYTE Data[8 << 10];
    DWORD DataSize;
    winrt::check_bool(ReadFile(File.get(), Data, static_cast<DWORD>(std::size(Data)), &DataSize, nullptr));
	winrt::check_hresult(D3DCompile(Data, DataSize, SourceName, Macros, nullptr, EntryPoint, Target, Flags1, Flags2, CodeBlob, ErrorBlob));
}

// TODO: This sample is not careful to clean up resources before exiting if 
// something fails.  If you use it for something important, it's up to you 
// to include proper error checks and cleanup code.

int _tmain(int /*argc*/, _TCHAR* /*argv[]*/)
{
    winrt::init_apartment();
    try
    {
        // GROUP_SIZE_X defined in kernel.hlsl must match the 
        // groupSize declared here.
        size_t constexpr groupSize = 512;
        size_t constexpr numGroups = 16;
        size_t constexpr dimension = numGroups * groupSize;

        // Create a D3D11 device and immediate context. 
        // TODO: The code below uses the default video adapter, with the
        // default set of feature levels.  Please see the MSDN docs if 
        // you wish to control which adapter and feature level are used.
        DWORD flags = 0;
        #if defined(_DEBUG)
            flags |= D3D11_CREATE_DEVICE_DEBUG;
        #endif
        D3D_FEATURE_LEVEL featureLevel;
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, device.put(), &featureLevel, context.put()));

        // Create system memory and fill it with our initial data.  Note that
        // these data structures aren't really necessary , it's just a demonstration
        // of how you can take existing data structures you might have and copy
        // their data to/from GPU computations.
        std::vector<float> x(dimension);
        std::vector<float> y(dimension);
        std::vector<float> z(dimension);
        float const a = 2.0f;
        for(size_t i = 0; i < dimension; ++ i)
        {
            x[i] = static_cast<float>(i);
            y[i] = 100 - static_cast<float>(i);
        }

        // Create structured buffers for the "x" and "y" vectors.
        CD3D11_BUFFER_DESC inputBufferDesc(sizeof(float) * dimension, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float));
        // The buffers are read-only by the GPU, writeable by the CPU.
        // TODO: If you will never again upate the data in a GPU buffer,
        // you might want to look at using a D3D11_SUBRESOURCE_DATA here to
        // provide the initialization data instead of doing the mapping 
        // and copying that happens below.
        winrt::com_ptr<ID3D11Buffer> xBuffer;
        winrt::check_hresult(device->CreateBuffer(&inputBufferDesc, nullptr, xBuffer.put()));

        // We can re-use inputBufferDesc here because the layout and usage of the x
        // and y buffers is exactly the same.
        winrt::com_ptr<ID3D11Buffer> yBuffer;
        winrt::check_hresult(device->CreateBuffer(&inputBufferDesc, nullptr, yBuffer.put()));

        // Create shader resource views for the "x" and "y" buffers.
        // TODO: You can optionally provide a D3D11_SHADER_RESOURCE_VIEW_DESC
        // as the second parameter if you need to use only part of the buffer
        // inside the compute shader.
        winrt::com_ptr<ID3D11ShaderResourceView> xSRV;
        winrt::check_hresult(device->CreateShaderResourceView(xBuffer.get(), nullptr, xSRV.put()));

        winrt::com_ptr<ID3D11ShaderResourceView> ySRV;
        winrt::check_hresult(device->CreateShaderResourceView(yBuffer.get(), nullptr, ySRV.put()));

        // Create a structured buffer for the "z" vector.  This buffer needs to be 
        // writeable by the GPU, so we can't create it with CPU read/write access.
        CD3D11_BUFFER_DESC outputBufferDesc(sizeof(float) * dimension, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float));
        winrt::com_ptr<ID3D11Buffer> zBuffer;
        winrt::check_hresult(device->CreateBuffer(&outputBufferDesc, nullptr, zBuffer.put()));

        // Create an unordered access view for the "z" vector.  
        CD3D11_UNORDERED_ACCESS_VIEW_DESC outputUAVDesc(D3D11_UAV_DIMENSION_BUFFER, DXGI_FORMAT_UNKNOWN, 0, dimension);
        winrt::com_ptr<ID3D11UnorderedAccessView> zBufferUAV;
        winrt::check_hresult(device->CreateUnorderedAccessView(zBuffer.get(), &outputUAVDesc, zBufferUAV.put()));

        // Create a staging buffer, which will be used to copy back from zBuffer.
        CD3D11_BUFFER_DESC stagingBufferDesc(sizeof(float) * dimension, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof(float));
        winrt::com_ptr<ID3D11Buffer> stagingBuffer;
        winrt::check_hresult(device->CreateBuffer(&stagingBufferDesc, nullptr, stagingBuffer.put()));

        // Create a constant buffer (this buffer is used to pass the constant 
        // value 'a' to the kernel as cbuffer Constants).
        CD3D11_BUFFER_DESC cbDesc(sizeof(float)*4, D3D11_BIND_CONSTANT_BUFFER, D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
        // Even though the constant buffer only has one float, DX expects
        // ByteWidth to be a multiple of 4 floats (i.e., one 128-bit register).
        winrt::com_ptr<ID3D11Buffer> constantBuffer;
        winrt::check_hresult(device->CreateBuffer( &cbDesc, nullptr, constantBuffer.put()));

        // Map the constant buffer and set the constant value 'a'.
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        winrt::check_hresult(context->Map(constantBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));
        float* constants = reinterpret_cast<float*>(mappedResource.pData);
        constants[0] = a;
        constants = nullptr;
        context->Unmap(constantBuffer.get(), 0);

        // Map the x buffer and copy our data into it.
        winrt::check_hresult(context->Map(xBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));
        float* xvalues = reinterpret_cast<float*>(mappedResource.pData);
        memcpy(xvalues, &x[0], sizeof(float)*x.size());
        xvalues = nullptr;
        context->Unmap(xBuffer.get(), 0);

        // Map the y buffer and copy our data into it.
        winrt::check_hresult(context->Map(yBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource));
        float* yvalues = reinterpret_cast<float*>(mappedResource.pData);
        memcpy(yvalues, &y[0], sizeof(float)*y.size());
        yvalues = nullptr;
        context->Unmap(yBuffer.get(), 0);

        // Compile the compute shader into a blob.
        winrt::com_ptr<ID3DBlob> errorBlob;
        winrt::com_ptr<ID3DBlob> shaderBlob;
        try
        {
            CompileFile(L"kernel.hlsl", nullptr, nullptr, "saxpy", "cs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, nullptr, shaderBlob.put(), errorBlob.put());
        }
        catch(winrt::hresult_error Exception)
        {
            // Print out the error message if there is one.
            if (errorBlob)
            {
                char const* message = (char*)errorBlob->GetBufferPointer();
                printf("kernel.hlsl failed to compile; error message:\n");
                printf("%s\n", message);
                errorBlob->Release();
            }
            return static_cast<int>(Exception.code());
        }

        // Create a shader object from the compiled blob.
        winrt::com_ptr<ID3D11ComputeShader> computeShader;
        winrt::check_hresult(device->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, computeShader.put()));

        // Make the shader active.
        context->CSSetShader(computeShader.get(), nullptr, 0);

        // Attach the z buffer to the output via its unordered access view.
        ID3D11UnorderedAccessView* UnorderedAccessViews[] { zBufferUAV.get() };
        UINT initCounts = static_cast<UINT>(-1);
        context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(UnorderedAccessViews)), UnorderedAccessViews, &initCounts);

        // Attach the input buffers via their shader resource views.
        ID3D11ShaderResourceView* ShaderResourceViews0[] { xSRV.get() };
        context->CSSetShaderResources(0, static_cast<UINT>(std::size(ShaderResourceViews0)), ShaderResourceViews0);
        ID3D11ShaderResourceView* ShaderResourceViews1[] { ySRV.get() };
        context->CSSetShaderResources(1, static_cast<UINT>(std::size(ShaderResourceViews1)), ShaderResourceViews1);

        // Attach the constant buffer
        ID3D11Buffer* ConstantBuffers[] { constantBuffer.get() };
        context->CSSetConstantBuffers(0, static_cast<UINT>(std::size(ConstantBuffers)), ConstantBuffers);

        // Execute the shader, in 'numGroups' groups of 'groupSize' threads each.
        context->Dispatch(numGroups, 1, 1);

        // Copy the z buffer to the staging buffer so that we can 
        // retrieve the data for accesss by the CPU.
        context->CopyResource(stagingBuffer.get(), zBuffer.get());

        // Map the staging buffer for reading.
        winrt::check_hresult(context->Map(stagingBuffer.get(), 0, D3D11_MAP_READ, 0, &mappedResource));
        float* zData = reinterpret_cast<float*>(mappedResource.pData);
        memcpy(&z[0], zData, sizeof(float)*z.size());
        zData = nullptr;
        context->Unmap(stagingBuffer.get(), 0);

        // Now compare the GPU results against expected values.
        bool resultOK = true;
        for (size_t i = 0; i < x.size(); ++ i)
        {
            // NOTE: This comparison assumes the GPU produces *exactly* the 
            // same result as the CPU.  In general, this will not be the case
            // with floating-point calculations.
            float const expected = a*x[i] + y[i];
            if (z[i] != expected)
            {
                printf("Unexpected result at position %lu: expected %.7e, got %.7e\n",
                    i, expected, z[i]);
                resultOK = false;
            }
        }

        if (!resultOK)
        {
            printf("GPU results differed from the CPU results.\n");
            OutputDebugStringA("GPU results differed from the CPU results.\n");
            return 1;
        }

        printf("GPU output matched the CPU results.\n");
        OutputDebugStringA("GPU output matched the CPU results.\n");

        // Disconnect everything from the pipeline.
        {
            ID3D11UnorderedAccessView* UnorderedAccessViews[] { nullptr };
            context->CSSetUnorderedAccessViews(0, static_cast<UINT>(std::size(UnorderedAccessViews)), UnorderedAccessViews, &initCounts);
            ID3D11ShaderResourceView* ShaderResourceViews[] { nullptr };
            context->CSSetShaderResources(0, static_cast<UINT>(std::size(ShaderResourceViews0)), ShaderResourceViews0);
            context->CSSetShaderResources(1, static_cast<UINT>(std::size(ShaderResourceViews0)), ShaderResourceViews0);
            ID3D11Buffer* ConstantBuffers[] { nullptr };
            context->CSSetConstantBuffers(0, 0, nullptr);
        }

        // Release resources.  Again, note that none of the error checks above
        // release resources that have been allocated up to this point, so the 
        // sample doesn't clean up after itself correctly unless everything succeeds.

    }
    catch(winrt::hresult_error Exception)
    {
        return static_cast<int>(Exception.code());
    }
    return 0;
}

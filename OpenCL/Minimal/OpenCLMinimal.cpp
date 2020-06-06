#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>

#include <vector>
#include <functional>

#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")

#include <unknwn.h>
#include <winrt\base.h>
#include <winrt\Windows.Foundation.h>
#include <winrt\Windows.Foundation.Diagnostics.h>
#include <winrt\Windows.Foundation.Collections.h>

#pragma comment(lib, "windowsapp.lib")

#include <cl/opencl.h>

namespace OCL
{
    inline VOID Check(cl_int Result)
    {
        if(Result == CL_SUCCESS)
            return;
        WCHAR Message[1 << 10];
        swprintf_s(Message, L"OpenCL error %d", Result);
        throw winrt::hresult_error(E_FAIL, static_cast<winrt::hstring>(Message));
    }

    inline VOID CreateProgram(TCHAR const* FileName, cl_context Context, cl_program& Program)
    {
        // Read the kernel source.
        // TODO: If you want to open a different
        // kernel, you need to change the filename here.  It might also be
        // convenient to factor the kernel loading and compilation into a
        // separate function.
        TCHAR Path[MAX_PATH];
        GetModuleFileName(nullptr, Path, static_cast<DWORD>(std::size(Path)));
        PathRemoveFileSpec(Path);
        PathCombine(Path, Path, FileName);
        winrt::file_handle File { CreateFile(Path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        winrt::check_bool(!!File);
        BYTE Data[8 << 10];
        DWORD DataSize;
        winrt::check_bool(ReadFile(File.get(), Data, static_cast<DWORD>(std::size(Data)), &DataSize, nullptr));
        assert(DataSize < std::size(Data));
        Data[DataSize] = 0;
        // Create a program from kernel source text.
        char const* sourceLines[] { reinterpret_cast<char const*>(Data) };
        cl_int Result;
        Program = clCreateProgramWithSource(Context, static_cast<cl_uint>(std::size(sourceLines)), sourceLines, nullptr, &Result);
        Check(Result);
        assert(Program);
    }

    class Buffer
    {
    private:
        cl_mem m_MemObject = nullptr;

    public:
        Buffer(cl_context Context, cl_mem_flags Flags, VOID* Data, size_t DataSize)
        {
            cl_int Result;
            m_MemObject = clCreateBuffer(Context, Flags, DataSize, Data, &Result);
            OCL::Check(Result);
            assert(m_MemObject);
        }
        Buffer(cl_context Context, cl_mem_flags Flags, size_t DataSize)
        {
            cl_int Result;
            m_MemObject = clCreateBuffer(Context, Flags, DataSize, nullptr, &Result);
            OCL::Check(Result);
            assert(m_MemObject);
        }
        ~Buffer()
        {
            if(m_MemObject)
                OCL::Check(clReleaseMemObject(std::exchange(m_MemObject, nullptr)));
        }
        cl_mem const& get() const
        {
            return m_MemObject;
        }
    };
}

int main()
{
    winrt::init_apartment();
    try
    {
        // This example operates on buffers of size 32k elements, processing
        // blocks of 512 elements at a time.

        // TODO: Depending on your hardware and the amount of work the kernel
        // does for each element, you may want to alter the blocksize.  Of
        // course, if you aren't processing 32k elements, you need to adjust
        // the number of blocks and block size accordingly.
        size_t const blockSize = 512;
        size_t const blocks = 64;
        size_t const dimension = blocks * blockSize;

        // Get the list of platforms.
        cl_uint constexpr maxPlatformCount = 8;
        cl_platform_id platforms[maxPlatformCount];
        cl_uint numPlatforms = 0;
        OCL::Check(clGetPlatformIDs(maxPlatformCount, &platforms[0], &numPlatforms));

        // TODO: You may want to look at the list of platforms that are
        // returned, and choose the most appropriate one for your needs.
        int const platformToUse = 0;

        // Get the devices available for the chosen platform.
        cl_uint deviceCount = 0;
        size_t constexpr maxDeviceCount = 8;
        cl_device_id devices[maxDeviceCount];
        OCL::Check(clGetDeviceIDs(platforms[platformToUse], CL_DEVICE_TYPE_GPU, maxDeviceCount, &devices[0], &deviceCount));

        // Create the context, using all devices.
        cl_int CreateContextResult;
        cl_context Context = clCreateContext(0, deviceCount, &devices[0], nullptr, nullptr, &CreateContextResult);
        OCL::Check(CreateContextResult);
        assert(Context);

        // TODO: If you have multiple devices, you may specify which one
        // you'd like to use by changing this variable.
        int const deviceToUse = 0;

        // Create a command queue for the selected device.
        cl_int CreateCommandQueueResult;
        cl_command_queue CommandQueue = clCreateCommandQueue(Context, devices[deviceToUse], 0, &CreateCommandQueueResult);
        OCL::Check(CreateCommandQueueResult);
        assert(CommandQueue);

        cl_program Program;
        OCL::CreateProgram(_T("kernel.cl"), Context, Program);

        // Build the program
        try
        {
            OCL::Check(clBuildProgram(Program, 0, 0, 0, 0, 0));
        }
        catch(winrt::hresult_error Exception)
        {
            printf("clBuildProgram failed; error log:\n");
            // Get the build log and write to stdout
            char buildLog[16 << 10];
            if(clGetProgramBuildInfo(Program, devices[0], CL_PROGRAM_BUILD_LOG, sizeof(buildLog), buildLog, nullptr) == CL_SUCCESS)
                printf("%hs\n", buildLog);
            throw;
        }

        // Create the kernel
        cl_int CreateKernelResult;
        cl_kernel Kernel = clCreateKernel(Program, "saxpy", &CreateKernelResult);
        OCL::Check(CreateKernelResult);
        assert(Kernel);

        // Allocate host memory for input and output vectors
        std::vector<float> x, y, z;

        // Set values to something easy to verify
        for(int i = 0; i < dimension; ++i)
        {
            x.emplace_back(static_cast<float>(i));
            y.emplace_back(static_cast<float>(100 - i));
            z.emplace_back(static_cast<float>(-i));
        }

        {
            // Allocate memory on the device for vectors x, y (read-only to the kernel), z (write-only to the kernel)
            OCL::Buffer devXmem(Context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, x.data(), dimension * sizeof (cl_float));
            OCL::Buffer devYmem(Context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, y.data(), dimension * sizeof (cl_float));
            OCL::Buffer devZmem(Context, CL_MEM_WRITE_ONLY, dimension * sizeof (cl_float));

            // Set kernel parameters
            OCL::Check(clSetKernelArg(Kernel, 0, sizeof (cl_mem), &devXmem.get()));
            OCL::Check(clSetKernelArg(Kernel, 1, sizeof (cl_mem), &devYmem.get()));
            OCL::Check(clSetKernelArg(Kernel, 2, sizeof (cl_mem), &devZmem.get()));

            // The constant 'a'
            float constexpr a = 2.0f;
            OCL::Check(clSetKernelArg(Kernel, 3, sizeof (cl_float), &a));

            // Execute the kernel
            OCL::Check(clEnqueueNDRangeKernel(CommandQueue, Kernel, 1, 0, &dimension, 0, 0, 0, 0));

            // Copy the results back to host memory
            OCL::Check(clEnqueueReadBuffer(CommandQueue, devZmem.get(), CL_TRUE, 0, dimension * sizeof(cl_float), z.data(), 0, 0, 0));

            // Check that results are correct.  Note that the code below
            // depends on the computation being exact, which may not be the
            // case for more complicated computations.
            for(size_t i = 0; i < dimension; ++ i)
                if(x[i] * a + y[i] != z[i])
                {
                    WCHAR Message[256];
                    swprintf_s(Message, L"Unexpected result at position %zu: x[i]*a + y[i] = %f * %f + %f != z[i] = %f", i, x[i], a, y[i], z[i]);
                    throw winrt::hresult_error(E_FAIL, static_cast<winrt::hstring>(Message));
                }
            printf("Computation appears to have completed successfully.\n");
            // Free device memory
        }

        // Release kernel, program, command queue, and context.
        OCL::Check(clReleaseKernel(Kernel));
        OCL::Check(clReleaseProgram(Program));
        OCL::Check(clReleaseCommandQueue(CommandQueue));
        OCL::Check(clReleaseContext(Context));
    }
    catch(winrt::hresult_error Exception)
    {
        printf("Exception: %ls\n", Exception.message().c_str());
        return static_cast<int>(Exception.code());
    }
    return 0;
}

/*
 * SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: NVIDIA TensorRT Source Code License Agreement
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "tensorrt_llm/kernels/decoderMaskedMultiheadAttention/decoderXQAImplJIT/nvrtcWrapper/include/nvrtcWrapper.h"

#include <cstring>
#include <cuda.h>
#include <nvPTXCompiler.h>
#include <nvrtc.h>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// Would be generated by gen_cpp_header.py via CMake dependency.
#include "xqa_sources.h"

// We only read Data_type definition from this header file, should be safe because there is no C++ ABI stuff involved.
#include "tensorrt_llm/kernels/multiHeadAttentionCommon.h"

#define CHECK_NVRTC_ERROR(content)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        nvrtcResult status_ = content;                                                                                 \
        if (status_ != NVRTC_SUCCESS)                                                                                  \
        {                                                                                                              \
            setErrorString("NVRTC Internal Error");                                                                    \
            return TLLM_XQA_JIT_INTERNAL_ERROR;                                                                        \
        }                                                                                                              \
    } while (0)

#define CHECK_TLLM_XQA_JIT_ERROR(content)                                                                              \
    do                                                                                                                 \
    {                                                                                                                  \
        tllmXqaJitStatus status_ = content;                                                                            \
        if (status_ != TLLM_XQA_JIT_SUCCESS)                                                                           \
        {                                                                                                              \
            return status_;                                                                                            \
        }                                                                                                              \
    } while (0)

#define CHECK_NVPTX_ERROR(content)                                                                                     \
    do                                                                                                                 \
    {                                                                                                                  \
        nvPTXCompileResult status_ = content;                                                                          \
        if (status_ != NVPTXCOMPILE_SUCCESS)                                                                           \
        {                                                                                                              \
            setErrorString("nvPTXCompiler Internal Error");                                                            \
            return TLLM_XQA_JIT_INTERNAL_ERROR;                                                                        \
        }                                                                                                              \
    } while (0)

struct _tllmXqaJitProgram
{
    nvrtcProgram program;
    tllmXqaJitContext const* context;
    // For SM120 two-stage compilation: store cubin data from nvPTXCompiler
    std::vector<char> cubin_data;
    bool use_stored_cubin = false;
};

namespace
{

std::string gErrorString;

void setErrorString(std::string const& errorString)
{
    gErrorString = errorString;
}

std::string getMacroFlag(std::string const& name, std::string const& value)
{
    return "-D" + name + "=" + value;
}

std::string getSMFlag(int SM)
{
    std::string smStr = std::to_string(SM);
    if (SM == 90 || SM == 120 || SM == 121)
    {
        smStr += "a";
    }
    return "-arch=sm_" + smStr;
}

std::string getPTXSMFlag(int SM)
{
    // For SM120, we use compute_89 for PTX generation
    if (SM == 120 || SM == 121)
    {
        return "-arch=compute_89";
    }

    std::string smStr = std::to_string(SM);
    if (SM == 90)
    {
        smStr += "a";
    }
    return "-arch=compute_" + smStr;
}

tllmXqaJitStatus getMacroFlags(tllmXqaJitContext const* context, std::vector<std::string>* result)
{
    // Macro name -> Macro value.
    std::unordered_map<std::string, std::string> macros;

    unsigned int head_size = context->head_size;
    unsigned int num_q_heads = context->num_q_heads;
    unsigned int num_kv_heads = context->num_kv_heads;
    if (num_q_heads % num_kv_heads != 0)
    {
        std::ostringstream oss;
        oss << "num_q_heads (" << num_q_heads << ") must be multiples of num_kv_heads (" << num_kv_heads << ").";
        setErrorString(oss.str());
        return TLLM_XQA_JIT_INVALID_INPUT;
    }
    unsigned int num_q_heads_over_kv = num_q_heads / num_kv_heads;
    unsigned int beam_width = context->beam_width;
    if (context->multi_query_tokens)
    {
        macros["SPEC_DEC"] = "1";
    }
    // MultiQueryToken kernels can handle either 16/32 for M direction per CTA.
    uint32_t const m_tilesize = [&context, num_q_heads_over_kv]() -> uint32_t
    {
        if (!context->multi_query_tokens)
        {
            return num_q_heads_over_kv;
        }
        if (context->kernel_type == TLLM_XQA_JIT_QGMMA)
        {
            return 64;
        }
        uint32_t const m = context->q_seq_len * num_q_heads_over_kv;
        return m < 16 ? 16 : 32;
    }();
    if (context->data_type == tensorrt_llm::kernels::DATA_TYPE_FP16)
    {
        macros["INPUT_FP16"] = "1";
        macros["DTYPE"] = "__half";
    }
    else if (context->data_type == tensorrt_llm::kernels::DATA_TYPE_BF16)
    {
        macros["INPUT_FP16"] = "0";
        macros["DTYPE"] = "__nv_bfloat16";
    }
    else if (context->data_type == tensorrt_llm::kernels::DATA_TYPE_E4M3)
    {
        TLLM_CHECK(context->kernel_type == TLLM_XQA_JIT_MLA);
    }
    else
    {
        setErrorString(
            "data_type must be DATA_TYPE_FP16 or DATA_TYPE_BF16 for non-MLA kernels and DATA_TYPE_E4M3 for the MLA "
            "kernel");
        return TLLM_XQA_JIT_INVALID_INPUT;
    }

    macros["GENERATE_CUBIN"] = "1";
    macros["NDEBUG"] = "1";
    macros["HEAD_ELEMS"] = std::to_string(head_size);
    macros["BEAM_WIDTH"] = std::to_string(beam_width);

    if (context->kv_cache_data_type == tensorrt_llm::kernels::DATA_TYPE_INT8)
    {
        macros["CACHE_ELEM_ENUM"] = "1";
    }
    else if (context->kv_cache_data_type == tensorrt_llm::kernels::DATA_TYPE_E4M3)
    {
        macros["CACHE_ELEM_ENUM"] = "2";
    }
    else
    {
        if (context->data_type == tensorrt_llm::kernels::DATA_TYPE_FP16)
        {
            if (context->kv_cache_data_type != tensorrt_llm::kernels::DATA_TYPE_FP16)
            {
                setErrorString("kv_cache_data_type must be DATA_TYPE_FP16 when data_type is DATA_TYPE_FP16");
                return TLLM_XQA_JIT_INVALID_INPUT;
            }
        }
        else
        {
            if (context->kv_cache_data_type != tensorrt_llm::kernels::DATA_TYPE_BF16)
            {
                setErrorString("kv_cache_data_type must be DATA_TYPE_BF16 when data_type is not DATA_TYPE_FP16");
                return TLLM_XQA_JIT_INVALID_INPUT;
            }
        }
        macros["CACHE_ELEM_ENUM"] = "0";
    }

    macros["TOKENS_PER_PAGE"] = context->paged_kv_cache ? std::to_string(context->tokens_per_block) : "0";
    macros["HEAD_GRP_SIZE"] = std::to_string(num_q_heads_over_kv);
    macros["M_TILESIZE"] = std::to_string(m_tilesize);
    macros["USE_CUSTOM_BARRIER"] = "1";
    // Sliding window is not supported when spec dec is enabled.
    macros["SLIDING_WINDOW"] = context->multi_query_tokens ? "0" : "1";
    macros["LOW_PREC_OUTPUT"] = context->fp8_output ? "1" : "0";
    macros["USE_INPUT_KV"] = context->use_input_kv ? "1" : "0";
    macros["ROPE_STYLE"] = std::to_string(int(context->rope_style));

    // Without these macros, NVRTC uses precompiled headers for cuda_fp16.h etc.
    // Linking might fail due to ABI incompatibility.
    //
    // https://nvbugspro.nvidia.com/bug/4549708 this WAR is proposed to bypass the issue.
    macros["__FORCE_INCLUDE_CUDA_FP16_HPP_FROM_FP16_H__"] = "1";
    macros["__FORCE_INCLUDE_CUDA_BF16_HPP_FROM_BF16_H__"] = "1";

    for (auto const& macro : macros)
    {
        result->push_back(getMacroFlag(macro.first, macro.second));
    }

#ifndef NDEBUG
    std::stringstream ss;
    ss << "XQA Macros: ";
    for (auto const& [k, v] : macros)
    {
        ss << k << "=" << v << " ";
    }
    puts(ss.str().c_str());
#endif

    return TLLM_XQA_JIT_SUCCESS;
}

tllmXqaJitStatus getBuildOptions(_tllmXqaJitProgram const* prog, std::vector<std::string>* result)
{
    // Common flags
    result->push_back("-dw");
    result->push_back("--use_fast_math");
    result->push_back("-default-device");

    // Arch
    result->push_back(getSMFlag(prog->context->sm));

    std::vector<std::string> macros;
    CHECK_TLLM_XQA_JIT_ERROR(getMacroFlags(prog->context, &macros));
    // Macros
    for (auto const& flag : macros)
    {
        result->push_back(flag);
    }

    return TLLM_XQA_JIT_SUCCESS;
}

tllmXqaJitStatus getBuildOptionsPTX(_tllmXqaJitProgram const* prog, std::vector<std::string>* result)
{
    // Common flags
    result->push_back("-dw");
    result->push_back("--use_fast_math");
    result->push_back("-default-device");

    // Use PTX arch for two-stage compilation
    result->push_back(getPTXSMFlag(prog->context->sm));

    std::vector<std::string> macros;
    CHECK_TLLM_XQA_JIT_ERROR(getMacroFlags(prog->context, &macros));
    // Macros
    for (auto const& flag : macros)
    {
        result->push_back(flag);
    }

    return TLLM_XQA_JIT_SUCCESS;
}

tllmXqaJitStatus createProgram(tllmXqaJitProgram* prog, tllmXqaJitContext const* context)
{
    *prog = new _tllmXqaJitProgram;
    (*prog)->context = context;

    char const* src_content = context->kernel_type == TLLM_XQA_JIT_MLA
        ? tensorrt_llm::kernels::mla_sm120_cu_content
        : (context->kernel_type == TLLM_XQA_JIT_QGMMA ? tensorrt_llm::kernels::mha_sm90_cu_content
                                                      : tensorrt_llm::kernels::mha_cu_content);

    std::vector<char const*> headers_content, headers_name;
    for (auto x : tensorrt_llm::kernels::xqa_headers_content)
        headers_content.push_back(x);
    for (auto x : tensorrt_llm::kernels::xqa_headers_name)
        headers_name.push_back(x);

    CHECK_NVRTC_ERROR(nvrtcCreateProgram(&(*prog)->program, src_content, /*name=*/nullptr, headers_content.size(),
        headers_content.data(), headers_name.data()));

    return TLLM_XQA_JIT_SUCCESS;
}

tllmXqaJitStatus compileProgram(tllmXqaJitProgram prog)
{
    bool needsTwoStageCompilation
        = (prog->context->sm == 120 || prog->context->sm == 121) && (prog->context->kernel_type == TLLM_XQA_JIT_HMMA);

    if (needsTwoStageCompilation)
    {
#ifndef NDEBUG
        // Two-stage compilation avoids accuracy regressions and cubin compatibility issues on SM120
        // by using compute_89 for PTX generation then targeting sm_120 for final cubin
        printf(
            "Using two-stage compilation for SM120: NVRTC (C++ -> PTX compute_89) + nvPTXCompiler (PTX -> cubin "
            "sm_120)\n");
#endif
        // Stage 1: Compile C++ to PTX using compute_89
        std::vector<std::string> ptx_options;
        CHECK_TLLM_XQA_JIT_ERROR(getBuildOptionsPTX(prog, &ptx_options));
        std::vector<char const*> ptx_options_cstr;
        for (auto const& option : ptx_options)
        {
            ptx_options_cstr.push_back(option.c_str());
        }

#ifdef NDEBUG
        CHECK_NVRTC_ERROR(nvrtcCompileProgram(prog->program, ptx_options_cstr.size(), ptx_options_cstr.data()));
#else
        auto const err = nvrtcCompileProgram(prog->program, ptx_options_cstr.size(), ptx_options_cstr.data());
        if (err != NVRTC_SUCCESS)
        {
            size_t logSize;
            CHECK_NVRTC_ERROR(nvrtcGetProgramLogSize(prog->program, &logSize));
            std::string log;
            log.resize(logSize);
            CHECK_NVRTC_ERROR(nvrtcGetProgramLog(prog->program, log.data()));
            printf("nvrtc PTX compilation error log:\n%s\n", log.c_str());
            CHECK_NVRTC_ERROR(err);
        }
#endif

        size_t ptx_size;
        CHECK_NVRTC_ERROR(nvrtcGetPTXSize(prog->program, &ptx_size));
        std::vector<char> ptx_data(ptx_size);
        CHECK_NVRTC_ERROR(nvrtcGetPTX(prog->program, ptx_data.data()));

        // Stage 2: Compile PTX to cubin for sm_120 using nvPTXCompiler
        nvPTXCompilerHandle ptx_compiler;
        CHECK_NVPTX_ERROR(nvPTXCompilerCreate(&ptx_compiler, ptx_size, ptx_data.data()));

        std::vector<char const*> ptx_compile_options = {"--gpu-name=sm_120f"};
        CHECK_NVPTX_ERROR(nvPTXCompilerCompile(ptx_compiler, ptx_compile_options.size(), ptx_compile_options.data()));

        size_t cubin_size;
        CHECK_NVPTX_ERROR(nvPTXCompilerGetCompiledProgramSize(ptx_compiler, &cubin_size));

        prog->cubin_data.resize(cubin_size);
        CHECK_NVPTX_ERROR(nvPTXCompilerGetCompiledProgram(ptx_compiler, prog->cubin_data.data()));
        prog->use_stored_cubin = true;

        CHECK_NVPTX_ERROR(nvPTXCompilerDestroy(&ptx_compiler));

#ifndef NDEBUG
        printf("Two-stage compilation completed: PTX size=%zu, cubin size=%zu\n", ptx_size, cubin_size);
#endif
    }
    else
    {
        std::vector<std::string> options;
        CHECK_TLLM_XQA_JIT_ERROR(getBuildOptions(prog, &options));
        std::vector<char const*> options_cstr;
        for (auto const& option : options)
        {
            options_cstr.push_back(option.c_str());
        }
#ifdef NDEBUG
        CHECK_NVRTC_ERROR(nvrtcCompileProgram(prog->program, options_cstr.size(), options_cstr.data()));
#else
        auto const err = nvrtcCompileProgram(prog->program, options_cstr.size(), options_cstr.data());
        if (err != NVRTC_SUCCESS)
        {
            size_t logSize;
            CHECK_NVRTC_ERROR(nvrtcGetProgramLogSize(prog->program, &logSize));
            std::string log;
            log.resize(logSize);
            CHECK_NVRTC_ERROR(nvrtcGetProgramLog(prog->program, log.data()));
            printf("nvrtc error log:\n%s\n", log.c_str());
            CHECK_NVRTC_ERROR(err);
        }
#endif
    }

    return TLLM_XQA_JIT_SUCCESS;
}

} // anonymous namespace

tllmXqaJitStatus tllmXqaJitCreateAndCompileProgram(tllmXqaJitProgram* prog, tllmXqaJitContext const* context)
{
    CHECK_TLLM_XQA_JIT_ERROR(createProgram(prog, context));
    CHECK_TLLM_XQA_JIT_ERROR(compileProgram(*prog));
    return TLLM_XQA_JIT_SUCCESS;
}

tllmXqaJitStatus tllmXqaJitGetCUBINSize(tllmXqaJitProgram prog, size_t* cubinSizeRet)
{
    // For SM120 two-stage compilation, return stored cubin size
    if (prog->use_stored_cubin)
    {
        *cubinSizeRet = prog->cubin_data.size();
        return TLLM_XQA_JIT_SUCCESS;
    }
    else
    {
        CHECK_NVRTC_ERROR(nvrtcGetCUBINSize(prog->program, cubinSizeRet));
        return TLLM_XQA_JIT_SUCCESS;
    }
}

tllmXqaJitStatus tllmXqaJitGetCUBIN(tllmXqaJitProgram prog, char* cubin)
{
    // For SM120 two-stage compilation, copy stored cubin data
    if (prog->use_stored_cubin)
    {
        std::memcpy(cubin, prog->cubin_data.data(), prog->cubin_data.size());
        return TLLM_XQA_JIT_SUCCESS;
    }
    else
    {
        CHECK_NVRTC_ERROR(nvrtcGetCUBIN(prog->program, cubin));
        return TLLM_XQA_JIT_SUCCESS;
    }
}

tllmXqaJitStatus tllmXqaJitDestroyProgram(tllmXqaJitProgram* prog)
{
    CHECK_NVRTC_ERROR(nvrtcDestroyProgram(&(*prog)->program));

    delete *prog;
    *prog = nullptr;

    return TLLM_XQA_JIT_SUCCESS;
}

size_t tllmXqaJitGetLastErrorStringSize()
{
    return gErrorString.size() + 1;
}

void tllmXqaJitGetLastErrorString(char* output)
{
    if (gErrorString.empty())
    {
        return;
    }
    strcpy(output, gErrorString.c_str());
}

/*
 * Copyright (c) 2019-2024, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

#pragma once

#include "NrcUtils.h"

#include <string>
#include <mutex>
#include <codecvt>

using namespace std;

namespace NrcUtils
{
    void Validate(HRESULT hr, LPWSTR msg)
    {
        if (FAILED(hr))
        {
            MessageBox(NULL, msg, L"Error", MB_OK);
            PostQuitMessage(EXIT_FAILURE);
        }
    }

    std::wstring StringToWstring(const std::string& wstr)
    {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
        return converter.from_bytes(wstr);
    }
}

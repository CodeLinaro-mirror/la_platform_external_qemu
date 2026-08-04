#!/usr/bin/env python3
"""
Shader compilation script for Virtual Environment Renderer (VER).

Compiles GLSL shaders into SPIR-V bytecodes using glslc and generates/updates VulkanShaders.h.
"""

import os
import platform
import shutil
import subprocess
import sys

SHADERS = {
    "kTexturedVertSpirv": ("vertex", """#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(push_constant) uniform PushConstants {
    mat4 u_modelViewProj;
} push;
layout(location = 0) out vec2 uv;
void main() {
    uv = in_uv;
    gl_Position = push.u_modelViewProj * vec4(in_position, 1.0);
}"""),

    "kTexturedFragSpirv": ("fragment", """#version 450
layout(location = 0) in vec2 uv;
layout(binding = 0) uniform sampler2D tex_sampler;
layout(location = 0) out vec4 fragColor;
void main() {
    fragColor = texture(tex_sampler, uv);
}"""),

    "kCheckerboardFragSpirv": ("fragment", """#version 450
layout(location = 0) in vec2 uv;
layout(push_constant) uniform PushConstants {
    mat4 u_modelViewProj;
    float u_time;
} push;
layout(location = 0) out vec4 fragColor;

#define CHECKERBOARD_SQUARE_WIDTH 16.0
#define MOVING_SQUARE_SIZE 2.5
#define VISIBLE_HEIGHT 9.0
#define COLOR_BLACK vec3(0.0, 0.0, 0.0)
#define COLOR_GREY vec3(0.5, 0.5, 0.5)
#define COLOR_RED vec3(0.82422, 0.03125, 0.0)
#define COLOR_GREEN vec3(0.01172, 0.65625, 0.0)
#define MOVING_SQUARE_VELOCITY vec2(1.5, 1.5)
#define CHECKERBOARD_VELOCITY vec2(1.0, 1.0 / 3.0)
#define COLOR_CHANGE_TIME 5.0

void main() {
    float u_time = push.u_time;
    vec2 staticBoardSpacePosition = vec2(uv.x * CHECKERBOARD_SQUARE_WIDTH, uv.y * CHECKERBOARD_SQUARE_WIDTH);
    vec2 movingBoardSpacePosition = staticBoardSpacePosition + u_time * CHECKERBOARD_VELOCITY;
    vec3 checkerboardColor = COLOR_BLACK + (mod(floor(mod(movingBoardSpacePosition.x, 2.0)) + floor(mod(movingBoardSpacePosition.y, 2.0)), 2.0)) * (COLOR_GREY - COLOR_BLACK);
    vec2 totalSquareMovement = MOVING_SQUARE_VELOCITY * u_time;
    float squareXTravel = CHECKERBOARD_SQUARE_WIDTH - MOVING_SQUARE_SIZE;
    float squareYTravel = VISIBLE_HEIGHT - MOVING_SQUARE_SIZE;
    vec2 squarePosition = vec2(
        mod(totalSquareMovement.x, squareXTravel * 2.0) > squareXTravel ?
        squareXTravel - mod(totalSquareMovement.x, squareXTravel) :
        mod(totalSquareMovement.x, squareXTravel),
        ((CHECKERBOARD_SQUARE_WIDTH - VISIBLE_HEIGHT) / 2.0) +
        (mod(totalSquareMovement.y, squareYTravel * 2.0) > squareYTravel ?
        squareYTravel - mod(totalSquareMovement.y, squareYTravel) :
        mod(totalSquareMovement.y, squareYTravel)));
    vec3 squareColor = COLOR_GREEN + floor(mod(u_time, COLOR_CHANGE_TIME * 2.0) / COLOR_CHANGE_TIME) * (COLOR_RED - COLOR_GREEN);
    vec2 posFromSquareOrigin = staticBoardSpacePosition - squarePosition;

    if (posFromSquareOrigin.x >= 0.0 && posFromSquareOrigin.y >= 0.0 &&
        posFromSquareOrigin.x < MOVING_SQUARE_SIZE && posFromSquareOrigin.y < MOVING_SQUARE_SIZE) {
        fragColor = vec4(squareColor, 1.0);
    } else {
        fragColor = vec4(checkerboardColor, 1.0);
    }
}"""),

    "kScreenSpaceVertSpirv": ("vertex", """#version 450
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec2 uv;
void main() {
    uv = in_uv;
    gl_Position = vec4(in_position, 1.0);
}"""),

    "kBlitFragSpirv": ("fragment", """#version 450
layout(location = 0) in vec2 uv;
layout(binding = 0) uniform sampler2D tex_sampler;
layout(push_constant) uniform PushConstants {
    vec2 resolution;
} push;
layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = texture(tex_sampler, uv);
}"""),

    "kFxaaFragSpirv": ("fragment", """#version 450
layout(location = 0) in vec2 uv;
layout(binding = 0) uniform sampler2D tex_sampler;
layout(push_constant) uniform PushConstants {
    vec2 resolution;
} push;
layout(location = 0) out vec4 fragColor;

#define FXAA_MUL  (1.0/8.0)
#define FXAA_MIN  (1.0/32.0)
#define FXAA_SPAN 8.0

void main() {
    vec2 res = push.resolution;
    vec3 rgbNW = texture(tex_sampler, uv + vec2(-1.0, -1.0) * res).xyz;
    vec3 rgbNE = texture(tex_sampler, uv + vec2( 1.0, -1.0) * res).xyz;
    vec3 rgbM  = texture(tex_sampler, uv).xyz;
    vec3 rgbSW = texture(tex_sampler, uv + vec2(-1.0,  1.0) * res).xyz;
    vec3 rgbSE = texture(tex_sampler, uv + vec2( 1.0,  1.0) * res).xyz;

    vec3 luma = vec3(0.299, 0.587, 0.114);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaM  = dot(rgbM, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);

    vec2 dir = vec2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                    ((lumaNW + lumaSW) - (lumaNE + lumaSE)));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * FXAA_MUL), FXAA_MIN);
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = min(vec2(FXAA_SPAN, FXAA_SPAN), max(vec2(-FXAA_SPAN, -FXAA_SPAN), dir * rcpDirMin)) * res;

    vec4 rgbA = 0.5 * (texture(tex_sampler, uv + dir * (1.0 / 3.0 - 0.5)) +
                       texture(tex_sampler, uv + dir * (2.0 / 3.0 - 0.5)));
    vec4 rgbB = rgbA * 0.5 + 0.25 * (texture(tex_sampler, uv + dir * (0.0 / 3.0 - 0.5)) +
                                     texture(tex_sampler, uv + dir * (3.0 / 3.0 - 0.5)));
    float lumaB = dot(rgbB.xyz, luma);

    if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
        fragColor = rgbA;
    } else {
        fragColor = rgbB;
    }
}""")
}


def find_glslc():
    candidate_paths = []
    
    which_glslc = shutil.which("glslc")
    if which_glslc:
        candidate_paths.append(which_glslc)

    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        machine = platform.machine()
        binary_names = ["glslc.exe", "glslc"] if sys.platform.startswith("win") else ["glslc"]
        for bname in binary_names:
            if machine:
                candidate_paths.append(os.path.join(vulkan_sdk, machine, "bin", bname))
            candidate_paths.append(os.path.join(vulkan_sdk, "x86_64", "bin", bname))
            candidate_paths.append(os.path.join(vulkan_sdk, "macOS", "bin", bname))
            candidate_paths.append(os.path.join(vulkan_sdk, "bin", bname))

    for path in candidate_paths:
        if path and os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise RuntimeError("Could not find glslc compiler binary")


def compile_shaders():
    glslc = find_glslc()
    script_dir = os.path.dirname(os.path.abspath(__file__))
    output_header = os.path.join(script_dir, "..", "src", "VulkanShaders.h")

    header_content = """/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Auto-generated by compile_shaders.py - DO NOT EDIT MANUALLY */

#pragma once

#include <cstddef>
#include <cstdint>

namespace android {
namespace ver {
"""

    for var_name, (stage, glsl_source) in SHADERS.items():
        cmd = [glslc, f"-fshader-stage={stage}", "-", "-o", "-"]
        proc = subprocess.run(cmd, input=glsl_source.encode("utf-8"), capture_output=True, check=True)
        raw_bytes = proc.stdout
        words = [int.from_bytes(raw_bytes[i:i + 4], "little") for i in range(0, len(raw_bytes), 4)]

        header_content += f"\nalignas(4) inline constexpr uint32_t {var_name}[{len(words)}] = {{\n"
        for i in range(0, len(words), 8):
            chunk = words[i:i + 8]
            header_content += "    " + ", ".join(f"0x{w:08x}u" for w in chunk) + ",\n"
        header_content += "};\n"

    header_content += """
}  // namespace ver
}  // namespace android
"""

    with open(output_header, "w") as f:
        f.write(header_content)

    print(f"Successfully compiled {len(SHADERS)} shaders -> {os.path.abspath(output_header)}")


if __name__ == "__main__":
    compile_shaders()

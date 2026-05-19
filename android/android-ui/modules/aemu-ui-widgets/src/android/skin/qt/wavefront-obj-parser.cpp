// Copyright 2016 The Android Open Source Project
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "android/skin/qt/wavefront-obj-parser.h"

#include <qchar.h>                             // for operator==
#include <qloggingcategory.h>                  // for qCWarning
#include <qstring.h>                           // for QString::SkipEmptyParts
#include <qtextstream.h>                       // for QTextStream::Ok, QText...
#include <QList>                               // for QList
#include <QString>                             // for QString
#include <QStringList>                         // for QStringList
#include <QTextStream>                         // for QTextStream
#include <cstddef>                             // for size_t
#include <string>                              // for basic_string, string
#include <tuple>                               // for tuple, get, make_tuple
#include <unordered_map>                       // for unordered_map, operator==
#include <utility>                             // for hash, pair

#include "android/utils/debug.h"

namespace std {
template <>
struct hash<std::tuple<int, int, int>> {
    size_t operator()(const tuple<int, int, int>& t) const {
        return hash_combine(std::hash<int>()(get<0>(t)),
                            hash_combine(std::hash<int>()(get<1>(t)),
                                         std::hash<int>()(get<2>(t))));
    }

private:
    static int hash_combine(int seed, int v) {
        return std::hash<int>()(v) + 0x9e3779b9 + (seed<<6) + (seed>>2);
    }
};
}

// =============================================================================
// Wavefront OBJ Format Overview
// =============================================================================
// The Wavefront OBJ (.obj) format is a standard ASCII text format used for
// representing 3D geometry. A 3D mesh is defined by listing its component
// vertices, texture coordinates, and normals, followed by the faces (polygons)
// that connect them together.
//
// Example OBJ File (Simple Square Surface):
// # Define 4 vertices (corners)
// v -1.0 1.0 1.0
// v 1.0 1.0 1.0
// v -1.0 -1.0 1.0
// v 1.0 -1.0 1.0
// # Define 4 texture coordinates
// vt 0.0 1.0
// vt 1.0 1.0
// vt 0.0 0.0
// vt 1.0 0.0
// # Define 1 normal vector pointing along +Z
// vn 0.0 0.0 1.0
// # Define 2 triangular faces forming the square surface
// f 1/1/1 2/2/1 3/3/1
// f 2/2/1 4/4/1 3/3/1
//
// Structure & Token Definitions:
// - '#' : Comment lines. Ignored during parsing.
// - 'v' : Vertex position coordinates in 3D space (X, Y, Z).
// - 'vt': Texture coordinates in 2D space (U, V), used for texture mapping.
// - 'vn': Vertex normal vectors in 3D space (NX, NY, NZ), used for lighting.
// - 'f' : Polygonal faces connecting vertices.
//
// Indexing & Format Requirements:
// 1. 1-Based Indexing: In OBJ files, indices start at 1. The first 'v' defined
//    in the file is index 1. The parser converts these to 0-based indices.
// 2. Sequential Ordering: Vertex definitions (v, vt, vn) must appear in the file
//    prior to any face (f) that references them.
// 3. Face Specification Format: Each face corner is specified as a triplet of
//    indices separated by slashes: vp/vt/vn (position/texture/normal).
//    For example, 'f 1/1/1 2/2/1 3/3/1' defines a triangular face.
// 4. Triangular Faces Only: This parser specifically requires triangular faces
//    (exactly 3 vertices per face).
// =============================================================================
bool parseWavefrontOBJ(QTextStream& stream,
                       std::vector<float>& vtx_buf,
                       std::vector<GLuint>& idx_buf) {
    std::vector<float> pos, tex, norm;
    std::unordered_map<std::tuple<int, int, int>, size_t> idx_table;
    QString str;
    int vertices = 0;

    vtx_buf.clear();
    idx_buf.clear();

    for (stream >> str; !stream.atEnd(); stream >> str) {
        if (str.length() <= 0) {
            continue;
        }
        if (str[0] == '#') {
            // Comment, read till the end of line.
            stream.readLine();
        } else if (str == "v" || str == "vn") {
            // Vertex position or normal.
            // Both are specified with 3 floating point numbers
            // separated by whitespace.
            // Example lines in OBJ file:
            // v -1.000000 1.000000 0.000000
            // vn 0.000000 0.000000 1.000000
            float xyz[3];
            stream >> xyz[0] >> xyz[1] >> xyz[2];
            if (stream.status() != QTextStream::Ok) {
                dwarning("OBJ parser: invalid position or normal");
                return false;
            }
            auto& container = (str == "v" ? pos : norm);
            container.insert(container.end(), xyz, xyz + 3);
        } else if (str == "vt") {
            // UV coords.
            // Specified with 2 floating point numbers separated by whitespace.
            // Example line in OBJ file:
            // vt 0.000000 1.000000
            float uv[2];
            stream >> uv[0] >> uv[1];
            if (stream.status() != QTextStream::Ok) {
                dwarning("OBJ parser: invalid UV");
                return false;
            }
            tex.insert(tex.end(), uv, uv + 2);
        } else if (str == "f") {
            // Face.
            // A face is specified with 3 vertices.
            // A vertex is specified like "vp/vt/vn" (no spaces)
            // where vp, vt and vn are indices into the position,
            // UV and normal arrays.
            // Example line in OBJ file:
            // f 1/1/1 2/2/1 3/3/1
            QString v;
            size_t vp, vt, vn;
            for (size_t i = 0; i < 3; i++) {
                stream >> v;
                QStringList components = v.split('/', Qt::SkipEmptyParts);
                if (components.size() != 3) {
                    dwarning("OBJ parser: invalid face specification");
                    return false;
                }
                bool pos_result, tex_result, nrm_result;
                // Note that indices in OBJ are 1-based (must be >= 1).
                // First parse as signed integers to validate 1-based indexing,
                // preventing any possibility of unsigned underflow wraparound during conversion.
                int raw_vp = components[0].toInt(&pos_result);
                int raw_vt = components[1].toInt(&tex_result);
                int raw_vn = components[2].toInt(&nrm_result);

                if (!(pos_result && tex_result && nrm_result) ||
                    raw_vp < 1 || raw_vt < 1 || raw_vn < 1) {
                    dwarning("OBJ parser: invalid face specification (indices must be 1-based)");
                    return false;
                }

                // Convert to 0-based unsigned indices.
                vp = static_cast<size_t>(raw_vp - 1);
                vt = static_cast<size_t>(raw_vt - 1);
                vn = static_cast<size_t>(raw_vn - 1);

                // Validity check:
                // Verify that the referenced vertex position, texture coordinate,
                // and normal indices exist within the data read so far.
                // Multiplication is required because pos/norm store 3 floats per element,
                // and tex stores 2 floats per element.
                // Checking upper bounds (> size()) alone is mathematically sufficient
                // because indices are guaranteed non-negative from the 1-based check above,
                // and vectors are always populated in exact multiples of 3 or 2.
                if (vp * 3 + 3 > pos.size() ||
                    vt * 2 + 2 > tex.size() ||
                    vn * 3 + 3 > norm.size()) {
                    dwarning("OBJ parser: invalid face specification (index out of bounds)");
                    return false;
                }

                auto vertex_idx = std::make_tuple(vp, vt, vn);
                auto vertex_it = idx_table.find(vertex_idx);
                if (vertex_it == idx_table.end()) {
                    // First time encountering this vertex, write its attributes
                    // into vertex buffer.
                    size_t element_array_idx = vertices++;
                    idx_buf.push_back(element_array_idx);
                    vtx_buf.insert(vtx_buf.end(), pos.data() + vp * 3, pos.data() + vp * 3 + 3);
                    vtx_buf.insert(vtx_buf.end(), norm.data() + vn * 3, norm.data() + vn * 3 + 3);
                    vtx_buf.insert(vtx_buf.end(), tex.data() + vt * 2, tex.data() + vt * 2 + 2);
                    idx_table[vertex_idx] = element_array_idx;
                } else {
                    // We've already encountered this vertex, write its index into the
                    // index buffer.
                    idx_buf.push_back(vertex_it->second);
                }
            }
        } else {
            // Something's wrong, bail out.
            dwarning("OBJ parser: invalid input [%s]", str.toStdString().c_str());
            return false;
        }
    }

    return true;
}

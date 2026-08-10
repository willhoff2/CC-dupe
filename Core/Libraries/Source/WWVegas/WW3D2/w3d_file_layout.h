/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// TheSuperHackers @port Layout assertions for the .w3d on-disk structures.
//
// Every structure declared in w3d_file.h is read from, or written to, a .w3d file as a raw
// byte blob: chunkio hands the file bytes straight into the struct. Its size and the offset
// of every member are therefore part of the file format, not an implementation detail.
//
// The reference values below were taken from the layout the structures have when `uint32`
// and friends are genuinely 32 bits (clang -m32 on the pre-port tree, which reproduces the
// Windows/VC6 layout), and they are what a correct 64-bit build must also produce. They
// exist to make the LP64 widening bug - `typedef unsigned long uint32`, 8 bytes on
// macOS/Linux - a build failure rather than a silent asset-corruption bug.
//
// Included at the end of w3d_file.h; not intended to be included directly.

#pragma once

#include <stddef.h>
#include <Utility/CppMacros.h>

STATIC_ASSERT_ALWAYS(sizeof(W3dChunkHeader) == 8, "W3dChunkHeader must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dChunkHeader, ChunkType) == 0, "W3dChunkHeader::ChunkType must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dChunkHeader, ChunkSize) == 4, "W3dChunkHeader::ChunkSize must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dTexCoordStruct) == 8, "W3dTexCoordStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTexCoordStruct, U) == 0, "W3dTexCoordStruct::U must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTexCoordStruct, V) == 4, "W3dTexCoordStruct::V must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dRGBStruct) == 4, "W3dRGBStruct must be 4 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBStruct, R) == 0, "W3dRGBStruct::R must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBStruct, G) == 1, "W3dRGBStruct::G must be at offset 1");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBStruct, B) == 2, "W3dRGBStruct::B must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBStruct, pad) == 3, "W3dRGBStruct::pad must be at offset 3");

STATIC_ASSERT_ALWAYS(sizeof(W3dRGBAStruct) == 4, "W3dRGBAStruct must be 4 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBAStruct, R) == 0, "W3dRGBAStruct::R must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBAStruct, G) == 1, "W3dRGBAStruct::G must be at offset 1");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBAStruct, B) == 2, "W3dRGBAStruct::B must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dRGBAStruct, A) == 3, "W3dRGBAStruct::A must be at offset 3");

STATIC_ASSERT_ALWAYS(sizeof(W3dMaterialInfoStruct) == 16, "W3dMaterialInfoStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMaterialInfoStruct, PassCount) == 0, "W3dMaterialInfoStruct::PassCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMaterialInfoStruct, VertexMaterialCount) == 4, "W3dMaterialInfoStruct::VertexMaterialCount must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dMaterialInfoStruct, ShaderCount) == 8, "W3dMaterialInfoStruct::ShaderCount must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dMaterialInfoStruct, TextureCount) == 12, "W3dMaterialInfoStruct::TextureCount must be at offset 12");

STATIC_ASSERT_ALWAYS(sizeof(W3dVertexMaterialStruct) == 32, "W3dVertexMaterialStruct must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Attributes) == 0, "W3dVertexMaterialStruct::Attributes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Ambient) == 4, "W3dVertexMaterialStruct::Ambient must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Diffuse) == 8, "W3dVertexMaterialStruct::Diffuse must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Specular) == 12, "W3dVertexMaterialStruct::Specular must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Emissive) == 16, "W3dVertexMaterialStruct::Emissive must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Shininess) == 20, "W3dVertexMaterialStruct::Shininess must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Opacity) == 24, "W3dVertexMaterialStruct::Opacity must be at offset 24");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertexMaterialStruct, Translucency) == 28, "W3dVertexMaterialStruct::Translucency must be at offset 28");

STATIC_ASSERT_ALWAYS(sizeof(W3dShaderStruct) == 16, "W3dShaderStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, DepthCompare) == 0, "W3dShaderStruct::DepthCompare must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, DepthMask) == 1, "W3dShaderStruct::DepthMask must be at offset 1");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, ColorMask) == 2, "W3dShaderStruct::ColorMask must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, DestBlend) == 3, "W3dShaderStruct::DestBlend must be at offset 3");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, FogFunc) == 4, "W3dShaderStruct::FogFunc must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, PriGradient) == 5, "W3dShaderStruct::PriGradient must be at offset 5");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, SecGradient) == 6, "W3dShaderStruct::SecGradient must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, SrcBlend) == 7, "W3dShaderStruct::SrcBlend must be at offset 7");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, Texturing) == 8, "W3dShaderStruct::Texturing must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, DetailColorFunc) == 9, "W3dShaderStruct::DetailColorFunc must be at offset 9");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, DetailAlphaFunc) == 10, "W3dShaderStruct::DetailAlphaFunc must be at offset 10");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, ShaderPreset) == 11, "W3dShaderStruct::ShaderPreset must be at offset 11");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, AlphaTest) == 12, "W3dShaderStruct::AlphaTest must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, PostDetailColorFunc) == 13, "W3dShaderStruct::PostDetailColorFunc must be at offset 13");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, PostDetailAlphaFunc) == 14, "W3dShaderStruct::PostDetailAlphaFunc must be at offset 14");
STATIC_ASSERT_ALWAYS(offsetof(W3dShaderStruct, pad) == 15, "W3dShaderStruct::pad must be at offset 15");

STATIC_ASSERT_ALWAYS(sizeof(W3dPS2ShaderStruct) == 12, "W3dPS2ShaderStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, DepthCompare) == 0, "W3dPS2ShaderStruct::DepthCompare must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, DepthMask) == 1, "W3dPS2ShaderStruct::DepthMask must be at offset 1");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, PriGradient) == 2, "W3dPS2ShaderStruct::PriGradient must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, Texturing) == 3, "W3dPS2ShaderStruct::Texturing must be at offset 3");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, AlphaTest) == 4, "W3dPS2ShaderStruct::AlphaTest must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, AParam) == 5, "W3dPS2ShaderStruct::AParam must be at offset 5");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, BParam) == 6, "W3dPS2ShaderStruct::BParam must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, CParam) == 7, "W3dPS2ShaderStruct::CParam must be at offset 7");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, DParam) == 8, "W3dPS2ShaderStruct::DParam must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dPS2ShaderStruct, pad) == 9, "W3dPS2ShaderStruct::pad must be at offset 9");

STATIC_ASSERT_ALWAYS(sizeof(W3dFXShaderInfoStruct) == 36, "W3dFXShaderInfoStruct must be 36 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dFXShaderInfoStruct, ShaderName) == 0, "W3dFXShaderInfoStruct::ShaderName must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dFXShaderInfoStruct, Technique) == 32, "W3dFXShaderInfoStruct::Technique must be at offset 32");
STATIC_ASSERT_ALWAYS(offsetof(W3dFXShaderInfoStruct, Pad) == 33, "W3dFXShaderInfoStruct::Pad must be at offset 33");

STATIC_ASSERT_ALWAYS(sizeof(W3dTextureInfoStruct) == 12, "W3dTextureInfoStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureInfoStruct, Attributes) == 0, "W3dTextureInfoStruct::Attributes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureInfoStruct, AnimType) == 2, "W3dTextureInfoStruct::AnimType must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureInfoStruct, FrameCount) == 4, "W3dTextureInfoStruct::FrameCount must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureInfoStruct, FrameRate) == 8, "W3dTextureInfoStruct::FrameRate must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dTriStruct) == 32, "W3dTriStruct must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTriStruct, Vindex) == 0, "W3dTriStruct::Vindex must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTriStruct, Attributes) == 12, "W3dTriStruct::Attributes must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dTriStruct, Normal) == 16, "W3dTriStruct::Normal must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dTriStruct, Dist) == 28, "W3dTriStruct::Dist must be at offset 28");

STATIC_ASSERT_ALWAYS(sizeof(W3dMeshHeader3Struct) == 116, "W3dMeshHeader3Struct must be 116 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, Version) == 0, "W3dMeshHeader3Struct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, Attributes) == 4, "W3dMeshHeader3Struct::Attributes must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, MeshName) == 8, "W3dMeshHeader3Struct::MeshName must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, ContainerName) == 24, "W3dMeshHeader3Struct::ContainerName must be at offset 24");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, NumTris) == 40, "W3dMeshHeader3Struct::NumTris must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, NumVertices) == 44, "W3dMeshHeader3Struct::NumVertices must be at offset 44");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, NumMaterials) == 48, "W3dMeshHeader3Struct::NumMaterials must be at offset 48");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, NumDamageStages) == 52, "W3dMeshHeader3Struct::NumDamageStages must be at offset 52");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, SortLevel) == 56, "W3dMeshHeader3Struct::SortLevel must be at offset 56");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, PrelitVersion) == 60, "W3dMeshHeader3Struct::PrelitVersion must be at offset 60");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, FutureCounts) == 64, "W3dMeshHeader3Struct::FutureCounts must be at offset 64");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, VertexChannels) == 68, "W3dMeshHeader3Struct::VertexChannels must be at offset 68");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, FaceChannels) == 72, "W3dMeshHeader3Struct::FaceChannels must be at offset 72");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, Min) == 76, "W3dMeshHeader3Struct::Min must be at offset 76");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, Max) == 88, "W3dMeshHeader3Struct::Max must be at offset 88");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, SphCenter) == 100, "W3dMeshHeader3Struct::SphCenter must be at offset 100");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshHeader3Struct, SphRadius) == 112, "W3dMeshHeader3Struct::SphRadius must be at offset 112");

STATIC_ASSERT_ALWAYS(sizeof(W3dVertInfStruct) == 8, "W3dVertInfStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertInfStruct, BoneIdx) == 0, "W3dVertInfStruct::BoneIdx must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dVertInfStruct, Pad) == 2, "W3dVertInfStruct::Pad must be at offset 2");

STATIC_ASSERT_ALWAYS(sizeof(W3dMeshDeform) == 20, "W3dMeshDeform must be 20 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshDeform, SetCount) == 0, "W3dMeshDeform::SetCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshDeform, AlphaPasses) == 4, "W3dMeshDeform::AlphaPasses must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshDeform, reserved) == 8, "W3dMeshDeform::reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dDeformSetInfo) == 12, "W3dDeformSetInfo must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformSetInfo, KeyframeCount) == 0, "W3dDeformSetInfo::KeyframeCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformSetInfo, flags) == 4, "W3dDeformSetInfo::flags must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformSetInfo, reserved) == 8, "W3dDeformSetInfo::reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dDeformKeyframeInfo) == 16, "W3dDeformKeyframeInfo must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformKeyframeInfo, DeformPercent) == 0, "W3dDeformKeyframeInfo::DeformPercent must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformKeyframeInfo, DataCount) == 4, "W3dDeformKeyframeInfo::DataCount must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformKeyframeInfo, reserved) == 8, "W3dDeformKeyframeInfo::reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dDeformData) == 28, "W3dDeformData must be 28 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformData, VertexIndex) == 0, "W3dDeformData::VertexIndex must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformData, Position) == 4, "W3dDeformData::Position must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformData, Color) == 16, "W3dDeformData::Color must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dDeformData, reserved) == 20, "W3dDeformData::reserved must be at offset 20");

STATIC_ASSERT_ALWAYS(sizeof(W3dMeshAABTreeHeader) == 32, "W3dMeshAABTreeHeader must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeHeader, NodeCount) == 0, "W3dMeshAABTreeHeader::NodeCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeHeader, PolyCount) == 4, "W3dMeshAABTreeHeader::PolyCount must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeHeader, Padding) == 8, "W3dMeshAABTreeHeader::Padding must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dMeshAABTreeNode) == 32, "W3dMeshAABTreeNode must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeNode, Min) == 0, "W3dMeshAABTreeNode::Min must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeNode, Max) == 12, "W3dMeshAABTreeNode::Max must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeNode, FrontOrPoly0) == 24, "W3dMeshAABTreeNode::FrontOrPoly0 must be at offset 24");
STATIC_ASSERT_ALWAYS(offsetof(W3dMeshAABTreeNode, BackOrPolyCount) == 28, "W3dMeshAABTreeNode::BackOrPolyCount must be at offset 28");

STATIC_ASSERT_ALWAYS(sizeof(W3dHierarchyStruct) == 36, "W3dHierarchyStruct must be 36 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHierarchyStruct, Version) == 0, "W3dHierarchyStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHierarchyStruct, Name) == 4, "W3dHierarchyStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dHierarchyStruct, NumPivots) == 20, "W3dHierarchyStruct::NumPivots must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dHierarchyStruct, Center) == 24, "W3dHierarchyStruct::Center must be at offset 24");

STATIC_ASSERT_ALWAYS(sizeof(W3dPivotStruct) == 60, "W3dPivotStruct must be 60 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotStruct, Name) == 0, "W3dPivotStruct::Name must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotStruct, ParentIdx) == 16, "W3dPivotStruct::ParentIdx must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotStruct, Translation) == 20, "W3dPivotStruct::Translation must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotStruct, EulerAngles) == 32, "W3dPivotStruct::EulerAngles must be at offset 32");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotStruct, Rotation) == 44, "W3dPivotStruct::Rotation must be at offset 44");

STATIC_ASSERT_ALWAYS(sizeof(W3dPivotFixupStruct) == 48, "W3dPivotFixupStruct must be 48 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dPivotFixupStruct, TM) == 0, "W3dPivotFixupStruct::TM must be at offset 0");

STATIC_ASSERT_ALWAYS(sizeof(W3dAnimHeaderStruct) == 44, "W3dAnimHeaderStruct must be 44 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimHeaderStruct, Version) == 0, "W3dAnimHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimHeaderStruct, Name) == 4, "W3dAnimHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimHeaderStruct, HierarchyName) == 20, "W3dAnimHeaderStruct::HierarchyName must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimHeaderStruct, NumFrames) == 36, "W3dAnimHeaderStruct::NumFrames must be at offset 36");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimHeaderStruct, FrameRate) == 40, "W3dAnimHeaderStruct::FrameRate must be at offset 40");

STATIC_ASSERT_ALWAYS(sizeof(W3dCompressedAnimHeaderStruct) == 44, "W3dCompressedAnimHeaderStruct must be 44 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, Version) == 0, "W3dCompressedAnimHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, Name) == 4, "W3dCompressedAnimHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, HierarchyName) == 20, "W3dCompressedAnimHeaderStruct::HierarchyName must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, NumFrames) == 36, "W3dCompressedAnimHeaderStruct::NumFrames must be at offset 36");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, FrameRate) == 40, "W3dCompressedAnimHeaderStruct::FrameRate must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dCompressedAnimHeaderStruct, Flavor) == 42, "W3dCompressedAnimHeaderStruct::Flavor must be at offset 42");

STATIC_ASSERT_ALWAYS(sizeof(W3dAnimChannelStruct) == 16, "W3dAnimChannelStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, FirstFrame) == 0, "W3dAnimChannelStruct::FirstFrame must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, LastFrame) == 2, "W3dAnimChannelStruct::LastFrame must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, VectorLen) == 4, "W3dAnimChannelStruct::VectorLen must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, Flags) == 6, "W3dAnimChannelStruct::Flags must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, Pivot) == 8, "W3dAnimChannelStruct::Pivot must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, pad) == 10, "W3dAnimChannelStruct::pad must be at offset 10");
STATIC_ASSERT_ALWAYS(offsetof(W3dAnimChannelStruct, Data) == 12, "W3dAnimChannelStruct::Data must be at offset 12");

STATIC_ASSERT_ALWAYS(sizeof(W3dBitChannelStruct) == 10, "W3dBitChannelStruct must be 10 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, FirstFrame) == 0, "W3dBitChannelStruct::FirstFrame must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, LastFrame) == 2, "W3dBitChannelStruct::LastFrame must be at offset 2");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, Flags) == 4, "W3dBitChannelStruct::Flags must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, Pivot) == 6, "W3dBitChannelStruct::Pivot must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, DefaultVal) == 8, "W3dBitChannelStruct::DefaultVal must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dBitChannelStruct, Data) == 9, "W3dBitChannelStruct::Data must be at offset 9");

STATIC_ASSERT_ALWAYS(sizeof(W3dTimeCodedAnimChannelStruct) == 12, "W3dTimeCodedAnimChannelStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedAnimChannelStruct, NumTimeCodes) == 0, "W3dTimeCodedAnimChannelStruct::NumTimeCodes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedAnimChannelStruct, Pivot) == 4, "W3dTimeCodedAnimChannelStruct::Pivot must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedAnimChannelStruct, VectorLen) == 6, "W3dTimeCodedAnimChannelStruct::VectorLen must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedAnimChannelStruct, Flags) == 7, "W3dTimeCodedAnimChannelStruct::Flags must be at offset 7");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedAnimChannelStruct, Data) == 8, "W3dTimeCodedAnimChannelStruct::Data must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dTimeCodedBitChannelStruct) == 12, "W3dTimeCodedBitChannelStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedBitChannelStruct, NumTimeCodes) == 0, "W3dTimeCodedBitChannelStruct::NumTimeCodes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedBitChannelStruct, Pivot) == 4, "W3dTimeCodedBitChannelStruct::Pivot must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedBitChannelStruct, Flags) == 6, "W3dTimeCodedBitChannelStruct::Flags must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedBitChannelStruct, DefaultVal) == 7, "W3dTimeCodedBitChannelStruct::DefaultVal must be at offset 7");
STATIC_ASSERT_ALWAYS(offsetof(W3dTimeCodedBitChannelStruct, Data) == 8, "W3dTimeCodedBitChannelStruct::Data must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dAdaptiveDeltaAnimChannelStruct) == 16, "W3dAdaptiveDeltaAnimChannelStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, NumFrames) == 0, "W3dAdaptiveDeltaAnimChannelStruct::NumFrames must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, Pivot) == 4, "W3dAdaptiveDeltaAnimChannelStruct::Pivot must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, VectorLen) == 6, "W3dAdaptiveDeltaAnimChannelStruct::VectorLen must be at offset 6");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, Flags) == 7, "W3dAdaptiveDeltaAnimChannelStruct::Flags must be at offset 7");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, Scale) == 8, "W3dAdaptiveDeltaAnimChannelStruct::Scale must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dAdaptiveDeltaAnimChannelStruct, Data) == 12, "W3dAdaptiveDeltaAnimChannelStruct::Data must be at offset 12");

STATIC_ASSERT_ALWAYS(sizeof(W3dMorphAnimHeaderStruct) == 48, "W3dMorphAnimHeaderStruct must be 48 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, Version) == 0, "W3dMorphAnimHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, Name) == 4, "W3dMorphAnimHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, HierarchyName) == 20, "W3dMorphAnimHeaderStruct::HierarchyName must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, FrameCount) == 36, "W3dMorphAnimHeaderStruct::FrameCount must be at offset 36");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, FrameRate) == 40, "W3dMorphAnimHeaderStruct::FrameRate must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimHeaderStruct, ChannelCount) == 44, "W3dMorphAnimHeaderStruct::ChannelCount must be at offset 44");

STATIC_ASSERT_ALWAYS(sizeof(W3dMorphAnimKeyStruct) == 8, "W3dMorphAnimKeyStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimKeyStruct, MorphFrame) == 0, "W3dMorphAnimKeyStruct::MorphFrame must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dMorphAnimKeyStruct, PoseFrame) == 4, "W3dMorphAnimKeyStruct::PoseFrame must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dHModelHeaderStruct) == 40, "W3dHModelHeaderStruct must be 40 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelHeaderStruct, Version) == 0, "W3dHModelHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelHeaderStruct, Name) == 4, "W3dHModelHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelHeaderStruct, HierarchyName) == 20, "W3dHModelHeaderStruct::HierarchyName must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelHeaderStruct, NumConnections) == 36, "W3dHModelHeaderStruct::NumConnections must be at offset 36");

STATIC_ASSERT_ALWAYS(sizeof(W3dHModelNodeStruct) == 18, "W3dHModelNodeStruct must be 18 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelNodeStruct, RenderObjName) == 0, "W3dHModelNodeStruct::RenderObjName must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHModelNodeStruct, PivotIdx) == 16, "W3dHModelNodeStruct::PivotIdx must be at offset 16");

STATIC_ASSERT_ALWAYS(sizeof(W3dLODModelHeaderStruct) == 24, "W3dLODModelHeaderStruct must be 24 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODModelHeaderStruct, Version) == 0, "W3dLODModelHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODModelHeaderStruct, Name) == 4, "W3dLODModelHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODModelHeaderStruct, NumLODs) == 20, "W3dLODModelHeaderStruct::NumLODs must be at offset 20");

STATIC_ASSERT_ALWAYS(sizeof(W3dLODStruct) == 40, "W3dLODStruct must be 40 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODStruct, RenderObjName) == 0, "W3dLODStruct::RenderObjName must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODStruct, LODMin) == 32, "W3dLODStruct::LODMin must be at offset 32");
STATIC_ASSERT_ALWAYS(offsetof(W3dLODStruct, LODMax) == 36, "W3dLODStruct::LODMax must be at offset 36");

STATIC_ASSERT_ALWAYS(sizeof(W3dCollectionHeaderStruct) == 32, "W3dCollectionHeaderStruct must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dCollectionHeaderStruct, Version) == 0, "W3dCollectionHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dCollectionHeaderStruct, Name) == 4, "W3dCollectionHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dCollectionHeaderStruct, RenderObjectCount) == 20, "W3dCollectionHeaderStruct::RenderObjectCount must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dCollectionHeaderStruct, pad) == 24, "W3dCollectionHeaderStruct::pad must be at offset 24");

STATIC_ASSERT_ALWAYS(sizeof(W3dPlaceholderStruct) == 56, "W3dPlaceholderStruct must be 56 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dPlaceholderStruct, version) == 0, "W3dPlaceholderStruct::version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dPlaceholderStruct, transform) == 4, "W3dPlaceholderStruct::transform must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dPlaceholderStruct, name_len) == 52, "W3dPlaceholderStruct::name_len must be at offset 52");

STATIC_ASSERT_ALWAYS(sizeof(W3dTransformNodeStruct) == 56, "W3dTransformNodeStruct must be 56 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTransformNodeStruct, version) == 0, "W3dTransformNodeStruct::version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTransformNodeStruct, transform) == 4, "W3dTransformNodeStruct::transform must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dTransformNodeStruct, name_len) == 52, "W3dTransformNodeStruct::name_len must be at offset 52");

STATIC_ASSERT_ALWAYS(sizeof(W3dLightStruct) == 24, "W3dLightStruct must be 24 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Attributes) == 0, "W3dLightStruct::Attributes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Unused) == 4, "W3dLightStruct::Unused must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Ambient) == 8, "W3dLightStruct::Ambient must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Diffuse) == 12, "W3dLightStruct::Diffuse must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Specular) == 16, "W3dLightStruct::Specular must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightStruct, Intensity) == 20, "W3dLightStruct::Intensity must be at offset 20");

STATIC_ASSERT_ALWAYS(sizeof(W3dSpotLightStruct) == 20, "W3dSpotLightStruct must be 20 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dSpotLightStruct, SpotDirection) == 0, "W3dSpotLightStruct::SpotDirection must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dSpotLightStruct, SpotAngle) == 12, "W3dSpotLightStruct::SpotAngle must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dSpotLightStruct, SpotExponent) == 16, "W3dSpotLightStruct::SpotExponent must be at offset 16");

STATIC_ASSERT_ALWAYS(sizeof(W3dLightAttenuationStruct) == 8, "W3dLightAttenuationStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightAttenuationStruct, Start) == 0, "W3dLightAttenuationStruct::Start must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightAttenuationStruct, End) == 4, "W3dLightAttenuationStruct::End must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dLightTransformStruct) == 48, "W3dLightTransformStruct must be 48 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dLightTransformStruct, Transform) == 0, "W3dLightTransformStruct::Transform must be at offset 0");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterHeaderStruct) == 20, "W3dEmitterHeaderStruct must be 20 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterHeaderStruct, Version) == 0, "W3dEmitterHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterHeaderStruct, Name) == 4, "W3dEmitterHeaderStruct::Name must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterUserInfoStruct) == 12, "W3dEmitterUserInfoStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterUserInfoStruct, Type) == 0, "W3dEmitterUserInfoStruct::Type must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterUserInfoStruct, SizeofStringParam) == 4, "W3dEmitterUserInfoStruct::SizeofStringParam must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterUserInfoStruct, StringParam) == 8, "W3dEmitterUserInfoStruct::StringParam must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterInfoStruct) == 332, "W3dEmitterInfoStruct must be 332 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, TextureFilename) == 0, "W3dEmitterInfoStruct::TextureFilename must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, StartSize) == 260, "W3dEmitterInfoStruct::StartSize must be at offset 260");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, EndSize) == 264, "W3dEmitterInfoStruct::EndSize must be at offset 264");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, Lifetime) == 268, "W3dEmitterInfoStruct::Lifetime must be at offset 268");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, EmissionRate) == 272, "W3dEmitterInfoStruct::EmissionRate must be at offset 272");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, MaxEmissions) == 276, "W3dEmitterInfoStruct::MaxEmissions must be at offset 276");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, VelocityRandom) == 280, "W3dEmitterInfoStruct::VelocityRandom must be at offset 280");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, PositionRandom) == 284, "W3dEmitterInfoStruct::PositionRandom must be at offset 284");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, FadeTime) == 288, "W3dEmitterInfoStruct::FadeTime must be at offset 288");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, Gravity) == 292, "W3dEmitterInfoStruct::Gravity must be at offset 292");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, Elasticity) == 296, "W3dEmitterInfoStruct::Elasticity must be at offset 296");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, Velocity) == 300, "W3dEmitterInfoStruct::Velocity must be at offset 300");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, Acceleration) == 312, "W3dEmitterInfoStruct::Acceleration must be at offset 312");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, StartColor) == 324, "W3dEmitterInfoStruct::StartColor must be at offset 324");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStruct, EndColor) == 328, "W3dEmitterInfoStruct::EndColor must be at offset 328");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterExtraInfoStruct) == 40, "W3dEmitterExtraInfoStruct must be 40 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterExtraInfoStruct, FutureStartTime) == 0, "W3dEmitterExtraInfoStruct::FutureStartTime must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterExtraInfoStruct, Padding) == 4, "W3dEmitterExtraInfoStruct::Padding must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dVolumeRandomizerStruct) == 32, "W3dVolumeRandomizerStruct must be 32 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dVolumeRandomizerStruct, ClassID) == 0, "W3dVolumeRandomizerStruct::ClassID must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dVolumeRandomizerStruct, Value1) == 4, "W3dVolumeRandomizerStruct::Value1 must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dVolumeRandomizerStruct, Value2) == 8, "W3dVolumeRandomizerStruct::Value2 must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dVolumeRandomizerStruct, Value3) == 12, "W3dVolumeRandomizerStruct::Value3 must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dVolumeRandomizerStruct, reserved) == 16, "W3dVolumeRandomizerStruct::reserved must be at offset 16");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterInfoStructV2) == 124, "W3dEmitterInfoStructV2 must be 124 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, BurstSize) == 0, "W3dEmitterInfoStructV2::BurstSize must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, CreationVolume) == 4, "W3dEmitterInfoStructV2::CreationVolume must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, VelRandom) == 36, "W3dEmitterInfoStructV2::VelRandom must be at offset 36");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, OutwardVel) == 68, "W3dEmitterInfoStructV2::OutwardVel must be at offset 68");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, VelInherit) == 72, "W3dEmitterInfoStructV2::VelInherit must be at offset 72");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, Shader) == 76, "W3dEmitterInfoStructV2::Shader must be at offset 76");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, RenderMode) == 92, "W3dEmitterInfoStructV2::RenderMode must be at offset 92");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, FrameMode) == 96, "W3dEmitterInfoStructV2::FrameMode must be at offset 96");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterInfoStructV2, reserved) == 100, "W3dEmitterInfoStructV2::reserved must be at offset 100");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterPropertyStruct) == 40, "W3dEmitterPropertyStruct must be 40 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, ColorKeyframes) == 0, "W3dEmitterPropertyStruct::ColorKeyframes must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, OpacityKeyframes) == 4, "W3dEmitterPropertyStruct::OpacityKeyframes must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, SizeKeyframes) == 8, "W3dEmitterPropertyStruct::SizeKeyframes must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, ColorRandom) == 12, "W3dEmitterPropertyStruct::ColorRandom must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, OpacityRandom) == 16, "W3dEmitterPropertyStruct::OpacityRandom must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, SizeRandom) == 20, "W3dEmitterPropertyStruct::SizeRandom must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterPropertyStruct, reserved) == 24, "W3dEmitterPropertyStruct::reserved must be at offset 24");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterColorKeyframeStruct) == 8, "W3dEmitterColorKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterColorKeyframeStruct, Time) == 0, "W3dEmitterColorKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterColorKeyframeStruct, Color) == 4, "W3dEmitterColorKeyframeStruct::Color must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterOpacityKeyframeStruct) == 8, "W3dEmitterOpacityKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterOpacityKeyframeStruct, Time) == 0, "W3dEmitterOpacityKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterOpacityKeyframeStruct, Opacity) == 4, "W3dEmitterOpacityKeyframeStruct::Opacity must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterSizeKeyframeStruct) == 8, "W3dEmitterSizeKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterSizeKeyframeStruct, Time) == 0, "W3dEmitterSizeKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterSizeKeyframeStruct, Size) == 4, "W3dEmitterSizeKeyframeStruct::Size must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterRotationHeaderStruct) == 16, "W3dEmitterRotationHeaderStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationHeaderStruct, KeyframeCount) == 0, "W3dEmitterRotationHeaderStruct::KeyframeCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationHeaderStruct, Random) == 4, "W3dEmitterRotationHeaderStruct::Random must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationHeaderStruct, OrientationRandom) == 8, "W3dEmitterRotationHeaderStruct::OrientationRandom must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationHeaderStruct, Reserved) == 12, "W3dEmitterRotationHeaderStruct::Reserved must be at offset 12");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterRotationKeyframeStruct) == 8, "W3dEmitterRotationKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationKeyframeStruct, Time) == 0, "W3dEmitterRotationKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterRotationKeyframeStruct, Rotation) == 4, "W3dEmitterRotationKeyframeStruct::Rotation must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterFrameHeaderStruct) == 16, "W3dEmitterFrameHeaderStruct must be 16 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterFrameHeaderStruct, KeyframeCount) == 0, "W3dEmitterFrameHeaderStruct::KeyframeCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterFrameHeaderStruct, Random) == 4, "W3dEmitterFrameHeaderStruct::Random must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterFrameHeaderStruct, Reserved) == 8, "W3dEmitterFrameHeaderStruct::Reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterFrameKeyframeStruct) == 8, "W3dEmitterFrameKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterFrameKeyframeStruct, Time) == 0, "W3dEmitterFrameKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterFrameKeyframeStruct, Frame) == 4, "W3dEmitterFrameKeyframeStruct::Frame must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterBlurTimeHeaderStruct) == 12, "W3dEmitterBlurTimeHeaderStruct must be 12 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterBlurTimeHeaderStruct, KeyframeCount) == 0, "W3dEmitterBlurTimeHeaderStruct::KeyframeCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterBlurTimeHeaderStruct, Random) == 4, "W3dEmitterBlurTimeHeaderStruct::Random must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterBlurTimeHeaderStruct, Reserved) == 8, "W3dEmitterBlurTimeHeaderStruct::Reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterBlurTimeKeyframeStruct) == 8, "W3dEmitterBlurTimeKeyframeStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterBlurTimeKeyframeStruct, Time) == 0, "W3dEmitterBlurTimeKeyframeStruct::Time must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterBlurTimeKeyframeStruct, BlurTime) == 4, "W3dEmitterBlurTimeKeyframeStruct::BlurTime must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dEmitterLinePropertiesStruct) == 64, "W3dEmitterLinePropertiesStruct must be 64 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, Flags) == 0, "W3dEmitterLinePropertiesStruct::Flags must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, SubdivisionLevel) == 4, "W3dEmitterLinePropertiesStruct::SubdivisionLevel must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, NoiseAmplitude) == 8, "W3dEmitterLinePropertiesStruct::NoiseAmplitude must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, MergeAbortFactor) == 12, "W3dEmitterLinePropertiesStruct::MergeAbortFactor must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, TextureTileFactor) == 16, "W3dEmitterLinePropertiesStruct::TextureTileFactor must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, UPerSec) == 20, "W3dEmitterLinePropertiesStruct::UPerSec must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, VPerSec) == 24, "W3dEmitterLinePropertiesStruct::VPerSec must be at offset 24");
STATIC_ASSERT_ALWAYS(offsetof(W3dEmitterLinePropertiesStruct, Reserved) == 28, "W3dEmitterLinePropertiesStruct::Reserved must be at offset 28");

STATIC_ASSERT_ALWAYS(sizeof(W3dAggregateHeaderStruct) == 20, "W3dAggregateHeaderStruct must be 20 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateHeaderStruct, Version) == 0, "W3dAggregateHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateHeaderStruct, Name) == 4, "W3dAggregateHeaderStruct::Name must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dAggregateInfoStruct) == 36, "W3dAggregateInfoStruct must be 36 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateInfoStruct, BaseModelName) == 0, "W3dAggregateInfoStruct::BaseModelName must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateInfoStruct, SubobjectCount) == 32, "W3dAggregateInfoStruct::SubobjectCount must be at offset 32");

STATIC_ASSERT_ALWAYS(sizeof(W3dAggregateSubobjectStruct) == 64, "W3dAggregateSubobjectStruct must be 64 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateSubobjectStruct, SubobjectName) == 0, "W3dAggregateSubobjectStruct::SubobjectName must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateSubobjectStruct, BoneName) == 32, "W3dAggregateSubobjectStruct::BoneName must be at offset 32");

STATIC_ASSERT_ALWAYS(sizeof(W3dTextureReplacerHeaderStruct) == 4, "W3dTextureReplacerHeaderStruct must be 4 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerHeaderStruct, ReplacedTexturesCount) == 0, "W3dTextureReplacerHeaderStruct::ReplacedTexturesCount must be at offset 0");

STATIC_ASSERT_ALWAYS(sizeof(W3dTextureReplacerStruct) == 1492, "W3dTextureReplacerStruct must be 1492 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerStruct, MeshPath) == 0, "W3dTextureReplacerStruct::MeshPath must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerStruct, BonePath) == 480, "W3dTextureReplacerStruct::BonePath must be at offset 480");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerStruct, OldTextureName) == 960, "W3dTextureReplacerStruct::OldTextureName must be at offset 960");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerStruct, NewTextureName) == 1220, "W3dTextureReplacerStruct::NewTextureName must be at offset 1220");
STATIC_ASSERT_ALWAYS(offsetof(W3dTextureReplacerStruct, TextureParams) == 1480, "W3dTextureReplacerStruct::TextureParams must be at offset 1480");

STATIC_ASSERT_ALWAYS(sizeof(W3dAggregateMiscInfo) == 20, "W3dAggregateMiscInfo must be 20 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateMiscInfo, OriginalClassID) == 0, "W3dAggregateMiscInfo::OriginalClassID must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateMiscInfo, Flags) == 4, "W3dAggregateMiscInfo::Flags must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dAggregateMiscInfo, reserved) == 8, "W3dAggregateMiscInfo::reserved must be at offset 8");

STATIC_ASSERT_ALWAYS(sizeof(W3dHLodHeaderStruct) == 40, "W3dHLodHeaderStruct must be 40 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodHeaderStruct, Version) == 0, "W3dHLodHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodHeaderStruct, LodCount) == 4, "W3dHLodHeaderStruct::LodCount must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodHeaderStruct, Name) == 8, "W3dHLodHeaderStruct::Name must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodHeaderStruct, HierarchyName) == 24, "W3dHLodHeaderStruct::HierarchyName must be at offset 24");

STATIC_ASSERT_ALWAYS(sizeof(W3dHLodArrayHeaderStruct) == 8, "W3dHLodArrayHeaderStruct must be 8 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodArrayHeaderStruct, ModelCount) == 0, "W3dHLodArrayHeaderStruct::ModelCount must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodArrayHeaderStruct, MaxScreenSize) == 4, "W3dHLodArrayHeaderStruct::MaxScreenSize must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dHLodSubObjectStruct) == 36, "W3dHLodSubObjectStruct must be 36 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodSubObjectStruct, BoneIndex) == 0, "W3dHLodSubObjectStruct::BoneIndex must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dHLodSubObjectStruct, Name) == 4, "W3dHLodSubObjectStruct::Name must be at offset 4");

STATIC_ASSERT_ALWAYS(sizeof(W3dBoxStruct) == 68, "W3dBoxStruct must be 68 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Version) == 0, "W3dBoxStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Attributes) == 4, "W3dBoxStruct::Attributes must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Name) == 8, "W3dBoxStruct::Name must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Color) == 40, "W3dBoxStruct::Color must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Center) == 44, "W3dBoxStruct::Center must be at offset 44");
STATIC_ASSERT_ALWAYS(offsetof(W3dBoxStruct, Extent) == 56, "W3dBoxStruct::Extent must be at offset 56");

STATIC_ASSERT_ALWAYS(sizeof(W3dNullObjectStruct) == 48, "W3dNullObjectStruct must be 48 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dNullObjectStruct, Version) == 0, "W3dNullObjectStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dNullObjectStruct, Attributes) == 4, "W3dNullObjectStruct::Attributes must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dNullObjectStruct, pad) == 8, "W3dNullObjectStruct::pad must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dNullObjectStruct, Name) == 16, "W3dNullObjectStruct::Name must be at offset 16");

STATIC_ASSERT_ALWAYS(sizeof(W3dSoundRObjHeaderStruct) == 56, "W3dSoundRObjHeaderStruct must be 56 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dSoundRObjHeaderStruct, Version) == 0, "W3dSoundRObjHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dSoundRObjHeaderStruct, Name) == 4, "W3dSoundRObjHeaderStruct::Name must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dSoundRObjHeaderStruct, Flags) == 20, "W3dSoundRObjHeaderStruct::Flags must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dSoundRObjHeaderStruct, Padding) == 24, "W3dSoundRObjHeaderStruct::Padding must be at offset 24");

STATIC_ASSERT_ALWAYS(sizeof(W3dShdMeshHeaderStruct) == 80, "W3dShdMeshHeaderStruct must be 80 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, Version) == 0, "W3dShdMeshHeaderStruct::Version must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, Attributes) == 4, "W3dShdMeshHeaderStruct::Attributes must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, NumTris) == 8, "W3dShdMeshHeaderStruct::NumTris must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, NumVertices) == 12, "W3dShdMeshHeaderStruct::NumVertices must be at offset 12");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, NumSubMeshes) == 16, "W3dShdMeshHeaderStruct::NumSubMeshes must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, FutureCounts) == 20, "W3dShdMeshHeaderStruct::FutureCounts must be at offset 20");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, BoxMin) == 40, "W3dShdMeshHeaderStruct::BoxMin must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, BoxMax) == 52, "W3dShdMeshHeaderStruct::BoxMax must be at offset 52");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, SphCenter) == 64, "W3dShdMeshHeaderStruct::SphCenter must be at offset 64");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdMeshHeaderStruct, SphRadius) == 76, "W3dShdMeshHeaderStruct::SphRadius must be at offset 76");

STATIC_ASSERT_ALWAYS(sizeof(W3dShdSubMeshHeaderStruct) == 56, "W3dShdSubMeshHeaderStruct must be 56 bytes on disk");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, NumTris) == 0, "W3dShdSubMeshHeaderStruct::NumTris must be at offset 0");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, NumVertices) == 4, "W3dShdSubMeshHeaderStruct::NumVertices must be at offset 4");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, FutureCounts) == 8, "W3dShdSubMeshHeaderStruct::FutureCounts must be at offset 8");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, BoxMin) == 16, "W3dShdSubMeshHeaderStruct::BoxMin must be at offset 16");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, BoxMax) == 28, "W3dShdSubMeshHeaderStruct::BoxMax must be at offset 28");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, SphCenter) == 40, "W3dShdSubMeshHeaderStruct::SphCenter must be at offset 40");
STATIC_ASSERT_ALWAYS(offsetof(W3dShdSubMeshHeaderStruct, SphRadius) == 52, "W3dShdSubMeshHeaderStruct::SphRadius must be at offset 52");

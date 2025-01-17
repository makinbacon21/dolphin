// Copyright 2014 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/GeometryShaderGen.h"

#include <cmath>
#include <cstring>

#include "Common/CommonTypes.h"
#include "VideoCommon/DriverDetails.h"
#include "VideoCommon/LightingShaderGen.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/XFMemory.h"

constexpr std::array<const char*, 4> primitives_ogl = {
    {"points", "lines", "triangles", "triangles"}};
constexpr std::array<const char*, 4> primitives_d3d = {{"point", "line", "triangle", "triangle"}};

bool geometry_shader_uid_data::IsPassthrough() const
{
  const bool stereo = g_ActiveConfig.iStereoMode > 0;
  const bool wireframe = g_ActiveConfig.bWireFrame;
  return primitive_type >= static_cast<u32>(PrimitiveType::Triangles) && !stereo && !wireframe;
}

GeometryShaderUid GetGeometryShaderUid(PrimitiveType primitive_type)
{
  ShaderUid<geometry_shader_uid_data> out;
  geometry_shader_uid_data* uid_data = out.GetUidData<geometry_shader_uid_data>();
  memset(uid_data, 0, sizeof(geometry_shader_uid_data));

  uid_data->primitive_type = static_cast<u32>(primitive_type);
  uid_data->numTexGens = xfmem.numTexGen.numTexGens;

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType ApiType, bool wireframe, bool pixel_lighting,
                       bool first_vertex = false);
static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType ApiType, bool wireframe,
                         bool pixel_lighting);

ShaderCode GenerateGeometryShaderCode(APIType ApiType, const ShaderHostConfig& host_config,
                                      const geometry_shader_uid_data* uid_data)
{
  ShaderCode out;
  // Non-uid template parameters will write to the dummy data (=> gets optimized out)

  const bool wireframe = host_config.wireframe;
  const bool pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool msaa = host_config.msaa;
  const bool ssaa = host_config.ssaa;
  const bool stereo = host_config.stereo;
  const PrimitiveType primitive_type = static_cast<PrimitiveType>(uid_data->primitive_type);
  const unsigned primitive_type_index = static_cast<unsigned>(uid_data->primitive_type);
  const unsigned vertex_in = std::min(static_cast<unsigned>(primitive_type_index) + 1, 3u);
  unsigned vertex_out = primitive_type == PrimitiveType::TriangleStrip ? 3 : 4;

  const unsigned int layers = host_config.more_layers * 2 + (int)(stereo) + 1;

  if (wireframe)
    vertex_out++;

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
  {
    // Insert layout parameters
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout(%s, invocations = %d) in;\n", primitives_ogl[primitive_type_index],
                layers);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n", wireframe ? "line" : "triangle",
                vertex_out);
    }
    else
    {
      out.Write("layout(%s) in;\n", primitives_ogl[primitive_type_index]);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n", wireframe ? "line" : "triangle",
                vertex_out * layers);
    }
  }

  out.Write("%s", s_lighting_struct);

  // uniforms
  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    out.Write("UBO_BINDING(std140, 3) uniform GSBlock {\n");
  else
    out.Write("cbuffer GSBlock {\n");

  out.Write("\tfloat4 " I_STEREOPARAMS ";\n"
            "\tfloat4 " I_LINEPTPARAMS ";\n"
            "\tint4 " I_TEXOFFSET ";\n"
            "};\n");

  out.Write("struct VS_OUTPUT {\n");
  GenerateVSOutputMembers<ShaderCode>(out, ApiType, uid_data->numTexGens, pixel_lighting, "");
  out.Write("};\n");

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("VARYING_LOCATION(0) in VertexData {\n");
    GenerateVSOutputMembers<ShaderCode>(out, ApiType, uid_data->numTexGens, pixel_lighting,
                                        GetInterpolationQualifier(msaa, ssaa, true, true));
    out.Write("} vs[%d];\n", vertex_in);

    out.Write("VARYING_LOCATION(0) out VertexData {\n");
    GenerateVSOutputMembers<ShaderCode>(out, ApiType, uid_data->numTexGens, pixel_lighting,
                                        GetInterpolationQualifier(msaa, ssaa, true, false));

    if (stereo || host_config.more_layers)
      out.Write("\tflat int layer;\n");

    out.Write("} ps;\n");

    out.Write("void main()\n{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {\n");
    out.Write("\tVS_OUTPUT o;\n");

    if (stereo || host_config.more_layers)
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");

    out.Write("};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount(%d)]\n[instance(%d)]\n", vertex_out, layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output, in uint "
                "InstanceID : SV_GSInstanceID)\n{\n",
                primitives_d3d[primitive_type_index], vertex_in, wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount(%d)]\n", vertex_out * layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output)\n{\n",
                primitives_d3d[primitive_type_index], vertex_in, wireframe ? "Line" : "Triangle");
    }

    out.Write("\tVertexData ps;\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT start, end;\n");
      AssignVSOutputMembers(out, "start", "vs[0]", uid_data->numTexGens, pixel_lighting);
      AssignVSOutputMembers(out, "end", "vs[1]", uid_data->numTexGens, pixel_lighting);
    }
    else
    {
      out.Write("\tVS_OUTPUT start = o[0];\n");
      out.Write("\tVS_OUTPUT end = o[1];\n");
    }

    // GameCube/Wii's line drawing algorithm is a little quirky. It does not
    // use the correct line caps. Instead, the line caps are vertical or
    // horizontal depending the slope of the line.
    out.Write("\tfloat2 offset;\n"
              "\tfloat2 to = abs(end.pos.xy / end.pos.w - start.pos.xy / start.pos.w);\n"
              // FIXME: What does real hardware do when line is at a 45-degree angle?
              // FIXME: Lines aren't drawn at the correct width. See Twilight Princess map.
              "\tif (" I_LINEPTPARAMS ".y * to.y > " I_LINEPTPARAMS ".x * to.x) {\n"
              // Line is more tall. Extend geometry left and right.
              // Lerp LineWidth/2 from [0..VpWidth] to [-1..1]
              "\t\toffset = float2(" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".x, 0);\n"
              "\t} else {\n"
              // Line is more wide. Extend geometry up and down.
              // Lerp LineWidth/2 from [0..VpHeight] to [1..-1]
              "\t\toffset = float2(0, -" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".y);\n"
              "\t}\n");
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    {
      out.Write("\tVS_OUTPUT center;\n");
      AssignVSOutputMembers(out, "center", "vs[0]", uid_data->numTexGens, pixel_lighting);
    }
    else
    {
      out.Write("\tVS_OUTPUT center = o[0];\n");
    }

    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
    out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
              ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
  }

  if (stereo || host_config.more_layers)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < %d; ++eye) {\n", layers);
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first;\n");

  out.Write("\tfor (int i = 0; i < %d; ++i) {\n", vertex_in);

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
  {
    out.Write("\tVS_OUTPUT f;\n");
    AssignVSOutputMembers(out, "f", "vs[i]", uid_data->numTexGens, pixel_lighting);

    if (host_config.backend_depth_clamp &&
        DriverDetails::HasBug(DriverDetails::BUG_BROKEN_CLIP_DISTANCE))
    {
      // On certain GPUs we have to consume the clip distance from the vertex shader
      // or else the other vertex shader outputs will get corrupted.
      out.Write("\tf.clipDist0 = gl_in[i].gl_ClipDistance[0];\n");
      out.Write("\tf.clipDist1 = gl_in[i].gl_ClipDistance[1];\n");
    }
  }
  else
  {
    out.Write("\tVS_OUTPUT f = o[i];\n");
  }

  if (host_config.vr)
  {
    // Select the output layer
    out.Write("\tps.layer = eye;\n");
    if (ApiType == APIType::OpenGL)
      out.Write("\tgl_Layer = eye;\n");
    // StereoParams[eye] = camera shift in game units * projection[0][0]
    // StereoParams[eye+2] = offaxis shift from Oculus projection[0][2]
    out.Write("\tf.clipPos.x += " I_STEREOPARAMS "[eye] - " I_STEREOPARAMS
              "[eye+2] * f.clipPos.w;\n");
    out.Write("\tf.pos.x += " I_STEREOPARAMS "[eye] - " I_STEREOPARAMS "[eye+2] * f.pos.w;\n");
  }
  else if (stereo || host_config.more_layers)
  {
    // Select the output layer
    out.Write("\tps.layer = eye;\n");
    if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
      out.Write("\tgl_Layer = eye;\n");

    // For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
    // to the depth of the vertex. We retrieve the depth value from the w-component of the projected
    // vertex which contains the negated z-component of the original vertex.
    // For negative parallax (out-of-screen effects) we subtract a convergence value from
    // the depth value. This results in objects at a distance smaller than the convergence
    // distance to seemingly appear in front of the screen.
    // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
    out.Write("\tfloat hoffset = (eye == 0) ? " I_STEREOPARAMS ".x : " I_STEREOPARAMS ".y;\n");
    out.Write("\tf.pos.x += hoffset * (f.pos.w - " I_STEREOPARAMS ".z);\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Write("\tVS_OUTPUT l = f;\n"
              "\tVS_OUTPUT r = f;\n");

    out.Write("\tl.pos.xy -= offset * l.pos.w;\n"
              "\tr.pos.xy += offset * r.pos.w;\n");

    out.Write("\tif (" I_TEXOFFSET "[2] != 0) {\n");
    out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");

    for (unsigned int i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[0] >> %d) & 0x1) != 0)\n", i);
      out.Write("\t\tr.tex%d.x += texOffset;\n", i);
    }
    out.Write("\t}\n");

    EmitVertex(out, host_config, uid_data, "l", ApiType, wireframe, pixel_lighting, true);
    EmitVertex(out, host_config, uid_data, "r", ApiType, wireframe, pixel_lighting);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Write("\tVS_OUTPUT ll = f;\n"
              "\tVS_OUTPUT lr = f;\n"
              "\tVS_OUTPUT ul = f;\n"
              "\tVS_OUTPUT ur = f;\n");

    out.Write("\tll.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr.pos.xy += float2(1,-1) * offset;\n"
              "\tul.pos.xy += float2(-1,1) * offset;\n"
              "\tur.pos.xy += offset;\n");

    out.Write("\tif (" I_TEXOFFSET "[3] != 0) {\n");
    out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET
              "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n");

    for (unsigned int i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[1] >> %d) & 0x1) != 0) {\n", i);
      out.Write("\t\tul.tex%d.xy += float2(0,1) * texOffset;\n", i);
      out.Write("\t\tur.tex%d.xy += texOffset;\n", i);
      out.Write("\t\tlr.tex%d.xy += float2(1,0) * texOffset;\n", i);
      out.Write("\t}\n");
    }
    out.Write("\t}\n");

    EmitVertex(out, host_config, uid_data, "ll", ApiType, wireframe, pixel_lighting, true);
    EmitVertex(out, host_config, uid_data, "lr", ApiType, wireframe, pixel_lighting);
    EmitVertex(out, host_config, uid_data, "ul", ApiType, wireframe, pixel_lighting);
    EmitVertex(out, host_config, uid_data, "ur", ApiType, wireframe, pixel_lighting);
  }
  else
  {
    EmitVertex(out, host_config, uid_data, "f", ApiType, wireframe, pixel_lighting, true);
  }

  out.Write("\t}\n");

  EndPrimitive(out, host_config, uid_data, ApiType, wireframe, pixel_lighting);

  if ((stereo || host_config.more_layers) && !host_config.backend_gs_instancing)
    out.Write("\t}\n");

  out.Write("}\n");

  return out;
}

static void EmitVertex(ShaderCode& out, const ShaderHostConfig& host_config,
                       const geometry_shader_uid_data* uid_data, const char* vertex,
                       APIType ApiType, bool wireframe, bool pixel_lighting, bool first_vertex)
{
  if (wireframe && first_vertex)
    out.Write("\tif (i == 0) first = %s;\n", vertex);

  if (ApiType == APIType::OpenGL)
  {
    out.Write("\tgl_Position = %s.pos;\n", vertex);
    if (host_config.backend_depth_clamp)
    {
      out.Write("\tgl_ClipDistance[0] = %s.clipDist0;\n", vertex);
      out.Write("\tgl_ClipDistance[1] = %s.clipDist1;\n", vertex);
    }
    AssignVSOutputMembers(out, "ps", vertex, uid_data->numTexGens, pixel_lighting);
  }
  else if (ApiType == APIType::Vulkan)
  {
    // Vulkan NDC space has Y pointing down (right-handed NDC space).
    out.Write("\tgl_Position = %s.pos;\n", vertex);
    out.Write("\tgl_Position.y = -gl_Position.y;\n");
    AssignVSOutputMembers(out, "ps", vertex, uid_data->numTexGens, pixel_lighting);
  }
  else
  {
    out.Write("\tps.o = %s;\n", vertex);
  }

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    out.Write("\tEmitVertex();\n");
  else
    out.Write("\toutput.Append(ps);\n");
}

static void EndPrimitive(ShaderCode& out, const ShaderHostConfig& host_config,
                         const geometry_shader_uid_data* uid_data, APIType ApiType, bool wireframe,
                         bool pixel_lighting)
{
  if (wireframe)
    EmitVertex(out, host_config, uid_data, "first", ApiType, wireframe, pixel_lighting);

  if (ApiType == APIType::OpenGL || ApiType == APIType::Vulkan)
    out.Write("\tEndPrimitive();\n");
  else
    out.Write("\toutput.RestartStrip();\n");
}

void EnumerateGeometryShaderUids(const std::function<void(const GeometryShaderUid&)>& callback)
{
  GeometryShaderUid uid;
  std::memset(&uid, 0, sizeof(uid));

  const std::array<PrimitiveType, 3> primitive_lut = {
      {g_ActiveConfig.backend_info.bSupportsPrimitiveRestart ? PrimitiveType::TriangleStrip :
                                                               PrimitiveType::Triangles,
       PrimitiveType::Lines, PrimitiveType::Points}};
  for (PrimitiveType primitive : primitive_lut)
  {
    auto* guid = uid.GetUidData<geometry_shader_uid_data>();
    guid->primitive_type = static_cast<u32>(primitive);

    for (u32 texgens = 0; texgens <= 8; texgens++)
    {
      guid->numTexGens = texgens;
      callback(uid);
    }
  }
}

template <class T>
static T GenerateAvatarGeometryShader(PrimitiveType primitive_type, APIType ApiType, const ShaderHostConfig& host_config)
{
  T out;

  const bool wireframe = host_config.wireframe;
  const bool pixel_lighting = g_ActiveConfig.bEnablePixelLighting;
  const bool stereo = host_config.stereo;

  // Non-uid template parameters will write to the dummy data (=> gets optimized out)
  geometry_shader_uid_data dummy_data;
  geometry_shader_uid_data* uid_data = out.template GetUidData<geometry_shader_uid_data>();
  if (uid_data == nullptr)
    uid_data = &dummy_data;

  const unsigned primitive_type_index = static_cast<unsigned>(primitive_type);
  uid_data->primitive_type = primitive_type_index;
  const unsigned vertex_in = std::min(static_cast<unsigned>(primitive_type_index) + 1, 3u);
  unsigned vertex_out = primitive_type == PrimitiveType::TriangleStrip ? 3 : 4;

  const unsigned int layers = host_config.more_layers * 2 + host_config.stereo + 1;

  if (wireframe)
    vertex_out++;

  if (ApiType == APIType::OpenGL)
  {
    // Insert layout parameters
    if (host_config.backend_gs_instancing)
    {
      out.Write("layout(%s, invocations = %d) in;\n", primitives_ogl[primitive_type_index],
                layers);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n",
                wireframe ? "line" : "triangle", vertex_out);
    }
    else
    {
      out.Write("layout(%s) in;\n", primitives_ogl[primitive_type_index]);
      out.Write("layout(%s_strip, max_vertices = %d) out;\n",
                wireframe ? "line" : "triangle",
                vertex_out * layers);
    }
  }

  // uniforms
  if (ApiType == APIType::OpenGL)
    out.Write("layout(std140%s) uniform GSBlock {\n",
              g_ActiveConfig.backend_info.bSupportsBindingLayout ? ", binding = 3" : "");
  else
    out.Write("cbuffer GSBlock {\n");
  out.Write("\tfloat4 " I_STEREOPARAMS ";\n"
            "\tfloat4 " I_LINEPTPARAMS ";\n"
            "\tint4 " I_TEXOFFSET ";\n"
            "};\n");

  uid_data->numTexGens = 1;
  const char* qualifier = "";

  out.Write("struct VS_OUTPUT {\n");
  DefineOutputMember(out, ApiType, qualifier, "float4", "pos", -1, "POSITION");
  DefineOutputMember(out, ApiType, qualifier, "float4", "colors_", 0, "COLOR", 0);
  DefineOutputMember(out, ApiType, qualifier, "float3", "tex", 0, "TEXCOORD", 0);
  out.Write("};\n");

  if (ApiType == APIType::OpenGL)
  {
    if (host_config.backend_gs_instancing)
      out.Write("#define InstanceID gl_InvocationID\n");

    out.Write("in VertexData {\n");
    qualifier = g_ActiveConfig.backend_info.bSupportsBindingLayout ? "centroid" : "centroid in";
    DefineOutputMember(out, ApiType, qualifier, "float4", "pos", -1, "POSITION");
    DefineOutputMember(out, ApiType, qualifier, "float4", "colors_", 0, "COLOR", 0);
    DefineOutputMember(out, ApiType, qualifier, "float3", "tex", 0, "TEXCOORD", 0);
    out.Write("} vs[%d];\n", vertex_in);

    out.Write("out VertexData {\n");
    qualifier = g_ActiveConfig.backend_info.bSupportsBindingLayout ? "centroid" : "centroid out";
    DefineOutputMember(out, ApiType, qualifier, "float4", "pos", -1, "POSITION");
    DefineOutputMember(out, ApiType, qualifier, "float4", "colors_", 0, "COLOR", 0);
    DefineOutputMember(out, ApiType, qualifier, "float3", "tex", 0, "TEXCOORD", 0);

    if (stereo || host_config.more_layers)
      out.Write("\tflat int layer;\n");

    out.Write("} ps;\n");

    out.Write("void main()\n{\n");
  }
  else  // D3D
  {
    out.Write("struct VertexData {\n");
    out.Write("\tVS_OUTPUT o;\n");

    if (stereo || host_config.more_layers)
      out.Write("\tuint layer : SV_RenderTargetArrayIndex;\n");

    out.Write("};\n");

    if (host_config.backend_gs_instancing)
    {
      out.Write("[maxvertexcount(%d)]\n[instance(%d)]\n", vertex_out, layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output, in uint "
                "InstanceID : SV_GSInstanceID)\n{\n",
                primitives_d3d[primitive_type_index], vertex_in,
                wireframe ? "Line" : "Triangle");
    }
    else
    {
      out.Write("[maxvertexcount(%d)]\n", vertex_out * layers);
      out.Write("void main(%s VS_OUTPUT o[%d], inout %sStream<VertexData> output)\n{\n",
                primitives_d3d[primitive_type_index], vertex_in,
                wireframe ? "Line" : "Triangle");
    }

    out.Write("\tVertexData ps;\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    if (ApiType == APIType::OpenGL)
    {
      out.Write("\tVS_OUTPUT start, end;\n");
      const char* a = "start";
      const char* b = "vs[0]";
      out.Write("\t%s.pos = %s.pos;\n", a, b);
      out.Write("\t%s.colors_0 = %s.colors_0;\n", a, b);
      out.Write("\t%s.tex%d = %s.tex%d;\n", a, 0, b, 0);
      a = "end";
      b = "vs[1]";
      out.Write("\t%s.pos = %s.pos;\n", a, b);
      out.Write("\t%s.colors_0 = %s.colors_0;\n", a, b);
      out.Write("\t%s.tex%d = %s.tex%d;\n", a, 0, b, 0);
    }
    else
    {
      out.Write("\tVS_OUTPUT start = o[0];\n");
      out.Write("\tVS_OUTPUT end = o[1];\n");
    }

    // GameCube/Wii's line drawing algorithm is a little quirky. It does not
    // use the correct line caps. Instead, the line caps are vertical or
    // horizontal depending the slope of the line.
    out.Write("\tfloat2 offset;\n"
              "\tfloat2 to = abs(end.pos.xy - start.pos.xy);\n"
              // FIXME: What does real hardware do when line is at a 45-degree angle?
              // FIXME: Lines aren't drawn at the correct width. See Twilight Princess map.
              "\tif (" I_LINEPTPARAMS ".y * to.y > " I_LINEPTPARAMS ".x * to.x) {\n"
              // Line is more tall. Extend geometry left and right.
              // Lerp LineWidth/2 from [0..VpWidth] to [-1..1]
              "\t\toffset = float2(" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".x, 0);\n"
              "\t} else {\n"
              // Line is more wide. Extend geometry up and down.
              // Lerp LineWidth/2 from [0..VpHeight] to [1..-1]
              "\t\toffset = float2(0, -" I_LINEPTPARAMS ".z / " I_LINEPTPARAMS ".y);\n"
              "\t}\n");
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    if (ApiType == APIType::OpenGL)
    {
      const char* a = "center";
      const char* b = "vs[0]";
      out.Write("\tVS_OUTPUT center;\n");
      out.Write("\t%s.pos = %s.pos;\n", a, b);
      out.Write("\t%s.colors_0 = %s.colors_0;\n", a, b);
      out.Write("\t%s.tex%d = %s.tex%d;\n", a, 0, b, 0);
    }
    else
    {
      out.Write("\tVS_OUTPUT center = o[0];\n");
    }

    // Offset from center to upper right vertex
    // Lerp PointSize/2 from [0,0..VpWidth,VpHeight] to [-1,1..1,-1]
    out.Write("\tfloat2 offset = float2(" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS
              ".x, -" I_LINEPTPARAMS ".w / " I_LINEPTPARAMS ".y) * center.pos.w;\n");
  }

  if (stereo || host_config.more_layers)
  {
    // If the GPU supports invocation we don't need a for loop and can simply use the
    // invocation identifier to determine which layer we're rendering.
    if (host_config.backend_gs_instancing)
      out.Write("\tint eye = InstanceID;\n");
    else
      out.Write("\tfor (int eye = 0; eye < 2; ++eye) {\n");
  }

  if (wireframe)
    out.Write("\tVS_OUTPUT first;\n");

  out.Write("\tfor (int i = 0; i < %d; ++i) {\n", vertex_in);

  if (ApiType == APIType::OpenGL)
  {
    out.Write("\tVS_OUTPUT f;\n");
    const char* a = "f";
    const char* b = "vs[i]";
    out.Write("\t%s.pos = %s.pos;\n", a, b);
    out.Write("\t%s.colors_0 = %s.colors_0;\n", a, b);
    out.Write("\t%s.tex%d = %s.tex%d;\n", a, 0, b, 0);
  }
  else
  {
    out.Write("\tVS_OUTPUT f = o[i];\n");
  }

  if (host_config.vr)
  {
    // Select the output layer
    out.Write("\tps.layer = eye;\n");
    if (ApiType == APIType::OpenGL)
      out.Write("\tgl_Layer = eye;\n");
    // StereoParams[eye] = camera shift in game units * projection[0][0]
    // StereoParams[eye+2] = offaxis shift from Oculus projection[0][2]
    // out.Write("\tf.clipPos.x += " I_STEREOPARAMS"[eye] - " I_STEREOPARAMS"[eye+2] *
    // f.clipPos.w;\n");
    out.Write("\tf.pos.x += " I_STEREOPARAMS "[eye] - " I_STEREOPARAMS "[eye+2] * f.pos.w;\n");
  }
  else if (stereo || host_config.more_layers)
  {
    // Select the output layer
    out.Write("\tps.layer = eye;\n");
    if (ApiType == APIType::OpenGL)
      out.Write("\tgl_Layer = eye;\n");

    // For stereoscopy add a small horizontal offset in Normalized Device Coordinates proportional
    // to the depth of the vertex. We retrieve the depth value from the w-component of the projected
    // vertex which contains the negated z-component of the original vertex.
    // For negative parallax (out-of-screen effects) we subtract a convergence value from
    // the depth value. This results in objects at a distance smaller than the convergence
    // distance to seemingly appear in front of the screen.
    // This formula is based on page 13 of the "Nvidia 3D Vision Automatic, Best Practices Guide"
    // out.Write("\tf.clipPos.x += " I_STEREOPARAMS"[eye] * (f.clipPos.w - "
    // I_STEREOPARAMS"[2]);\n");
    out.Write("\tf.pos.x += " I_STEREOPARAMS "[eye] * (f.pos.w - " I_STEREOPARAMS "[2]);\n");
  }

  if (primitive_type == PrimitiveType::Lines)
  {
    out.Write("\tVS_OUTPUT l = f;\n"
              "\tVS_OUTPUT r = f;\n");

    out.Write("\tl.pos.xy -= offset * l.pos.w;\n"
              "\tr.pos.xy += offset * r.pos.w;\n");

    out.Write("\tif (" I_TEXOFFSET "[2] != 0) {\n");
    out.Write("\tfloat texOffset = 1.0 / float(" I_TEXOFFSET "[2]);\n");

    for (unsigned int i = 0; i < uid_data->numTexGens; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[0] >> %d) & 0x1) != 0)\n", i);
      out.Write("\t\tr.tex%d.x += texOffset;\n", i);
    }
    out.Write("\t}\n");

    EmitVertex(out, host_config, uid_data, "l", ApiType, wireframe, pixel_lighting, true);
    EmitVertex(out, host_config, uid_data, "r", ApiType, wireframe, pixel_lighting);
  }
  else if (primitive_type == PrimitiveType::Points)
  {
    out.Write("\tVS_OUTPUT ll = f;\n"
              "\tVS_OUTPUT lr = f;\n"
              "\tVS_OUTPUT ul = f;\n"
              "\tVS_OUTPUT ur = f;\n");

    out.Write("\tll.pos.xy += float2(-1,-1) * offset;\n"
              "\tlr.pos.xy += float2(1,-1) * offset;\n"
              "\tul.pos.xy += float2(-1,1) * offset;\n"
              "\tur.pos.xy += offset;\n");

    out.Write("\tif (" I_TEXOFFSET "[3] != 0) {\n");
    out.Write("\tfloat2 texOffset = float2(1.0 / float(" I_TEXOFFSET
              "[3]), 1.0 / float(" I_TEXOFFSET "[3]));\n");

    for (unsigned int i = 0; i < 1; ++i)
    {
      out.Write("\tif (((" I_TEXOFFSET "[1] >> %d) & 0x1) != 0) {\n", i);
      out.Write("\t\tll.tex%d.xy += float2(0,1) * texOffset;\n", i);
      out.Write("\t\tlr.tex%d.xy += texOffset;\n", i);
      out.Write("\t\tur.tex%d.xy += float2(1,0) * texOffset;\n", i);
      out.Write("\t}\n");
    }
    out.Write("\t}\n");

    EmitVertex(out, host_config, uid_data, "ll", ApiType, wireframe, pixel_lighting, true);
    EmitVertex(out, host_config, uid_data, "lr", ApiType, wireframe, pixel_lighting);
    EmitVertex(out, host_config, uid_data, "ul", ApiType, wireframe, pixel_lighting);
    EmitVertex(out, host_config, uid_data, "ur", ApiType, wireframe, pixel_lighting);
  }
  else
  {
    EmitVertex(out, host_config, uid_data, "f", ApiType, wireframe, pixel_lighting, true);
  }

  out.Write("\t}\n");

  EndPrimitive(out, host_config, uid_data, ApiType, wireframe, pixel_lighting);

  if ((stereo || host_config.more_layers) && !host_config.backend_gs_instancing)
    out.Write("\t}\n");

  out.Write("}\n");
  return out;
}

ShaderCode GenerateAvatarGeometryShaderCode(PrimitiveType primitive_type, APIType ApiType, const ShaderHostConfig& host_config)
{
  return GenerateAvatarGeometryShader<ShaderCode>(primitive_type, ApiType, host_config);
}

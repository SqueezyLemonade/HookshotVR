// (OoT VR, true stereo) A translated vertex, geometry or mesh stage made to draw both eyes in one multiview draw.
//
// With the game camera at the head's centre, an eye e metres along the camera's right sees each point at the same clip
// position less (e * P00, 0, 0, 0): view space v_e = v - (e, 0, 0), clip_e = P * v_e = clip - e * P * (1, 0, 0, 0), and the
// first column of the game's perspective projection is (P00, 0, 0, 0) (its jitter is in the third column). So each view only
// moves clip x by its constant: x -= kx[ViewIndex]. Nothing about the game's own uniforms is needed.
//
// The patch (SPIR-V words in, words out): OpCapability MultiView (and SPV_KHR_multiview before SPIR-V 1.3), an Input
// variable decorated BuiltIn ViewIndex added to the entry point's interface, and
//  - vertex and geometry stages: before each OpReturn of the entry point (vertex) and each OpEmitVertex/OpEmitStreamVertex
//    (geometry), the Position output loaded, its x less kx[ViewIndex], stored back. (After everything the stage itself did,
//    the translator's own depth conversion included.) A vertex whose w is exactly 1 is left where it is: that is a
//    full-screen triangle or a screen composite (no perspective), whose shift would move the whole picture and draw a
//    second outline (STE-06). A world vertex is practically never exactly 1 m away (and only loses 3 cm of eye shift there).
//  - mesh stages: each store into the Position output array, of a whole vec4 or of its x (a constant or dynamic component
//    index), gets the same subtraction on its value.
// Stages without a Position output, and anything not understood, are refused (false): the caller draws that pipeline as one
// view.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mkwvk::vr {

enum class SpirvEyesStage { Vertex, Geometry, Mesh };

/// The stage's SPIR-V with both eyes' shifts (kx[0] view 0's, kx[1] view 1's, clip-space x units); false when it cannot be
/// patched (why: a short reason for the log)
bool spirv_eyes_patch(const uint32_t* words, size_t count, SpirvEyesStage stage, const float kx[2], std::vector<uint32_t>& out,
                      const char** why);
/// (OoT VR, the VR tab's HUD size) A vertex stage of the game's 2D layer drawn smaller about the middle: before each return of
/// the entry point, the Position output's x and y each multiplied by scale, unless that coordinate is on the screen's edge
/// (|c| at least 0.998 w): a full-screen fade or dim keeps covering the whole view while the HUD and text boxes come inward
/// (every 2D draw is 30 vertices, its size in the shader's data: the vertex is the only place they differ). False: not patched
bool spirv_hud_patch(const uint32_t* words, size_t count, float scale, std::vector<uint32_t>& out, const char** why);
/// The execution model of a module's first entry point: 0 vertex, 3 geometry, 5365 mesh (EXT), ...; -1 not SPIR-V
int spirv_execution_model(const uint32_t* words, size_t count);

}  // namespace mkwvk::vr

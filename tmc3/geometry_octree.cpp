/* The copyright in this software is being made available under the BSD
 * Licence, included below.  This software may be subject to other third
 * party and contributor rights, including patent rights, and no such
 * rights are granted under this licence.
 *
 * Copyright (c) 2017-2018, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the ISO/IEC nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "geometry_octree.h"

#include <algorithm>
#include <iterator>

#include "PCCMisc.h"
#include "tables.h"

namespace pcc {

//-------------------------------------------------------------------------
// map the @occupancy pattern bits to take into account symmetries in the
// neighbour configuration @neighPattern.
//
uint8_t
mapGeometryOccupancy(uint8_t occupancy, uint8_t neighPattern, int planarMaskZ)
{
  if (planarMaskZ && !(planarMaskZ & 1)) {
    occupancy = kOccMapMirrorXY[occupancy];
  }

  switch (kOccMapRotateZIdFromPatternXY[neighPattern & 15]) {
  case 1: occupancy = kOccMapRotateZ090[occupancy]; break;
  case 2: occupancy = kOccMapRotateZ180[occupancy]; break;
  case 3: occupancy = kOccMapRotateZ270[occupancy]; break;
  }

  bool flag_ud = (neighPattern & 16) && !(neighPattern & 32);
  if (flag_ud) {
    occupancy = kOccMapMirrorXY[occupancy];
  }

  if (kOccMapRotateYIdFromPattern[neighPattern]) {
    occupancy = kOccMapRotateY270[occupancy];
  }

  switch (kOccMapRotateXIdFromPattern[neighPattern]) {
  case 1: occupancy = kOccMapRotateX090[occupancy]; break;
  case 2: occupancy = kOccMapRotateX270Y180[occupancy]; break;
  case 3: occupancy = kOccMapRotateX090Y180[occupancy]; break;
  }

  return occupancy;
}

//-------------------------------------------------------------------------
// map the @occupancy pattern bits to take into account symmetries in the
// neighbour configuration @neighPattern.
//
uint8_t
mapGeometryOccupancyInv(
  uint8_t occupancy, uint8_t neighPattern, int planarMaskZ)
{
  switch (kOccMapRotateXIdFromPattern[neighPattern]) {
  case 1: occupancy = kOccMapRotateX270[occupancy]; break;
  case 2: occupancy = kOccMapRotateX270Y180[occupancy]; break;
  case 3: occupancy = kOccMapRotateX090Y180[occupancy]; break;
  }

  if (kOccMapRotateYIdFromPattern[neighPattern]) {
    occupancy = kOccMapRotateY090[occupancy];
  }

  bool flag_ud = (neighPattern & 16) && !(neighPattern & 32);
  if (flag_ud) {
    occupancy = kOccMapMirrorXY[occupancy];
  }

  switch (kOccMapRotateZIdFromPatternXY[neighPattern & 15]) {
  case 1: occupancy = kOccMapRotateZ270[occupancy]; break;
  case 2: occupancy = kOccMapRotateZ180[occupancy]; break;
  case 3: occupancy = kOccMapRotateZ090[occupancy]; break;
  }

  if (planarMaskZ && !(planarMaskZ & 1)) {
    occupancy = kOccMapMirrorXY[occupancy];
  }

  return occupancy;
}

//============================================================================
// Update the neighbour pattern flags for a node and the 'left' neighbour on
// each axis.  This update should be applied to each newly inserted node.
//
// @param siblingRestriction limits neighbours to direct siblings of child

void
updateGeometryNeighState(
  bool siblingRestriction,
  const ringbuf<PCCOctree3Node>::iterator& bufEnd,
  int64_t numNodesNextLvl,
  PCCOctree3Node& child,
  int childIdx,
  uint8_t neighPattern,
  uint8_t parentOccupancy)
{
  int64_t midx;
  if (!siblingRestriction) {
    midx = child.mortonIdx = mortonAddr(child.pos);
  }

  static const struct {
    int childIdxBitPos;
    int axis;
    int patternFlagUs;
    int patternFlagThem;
  } neighParamMap[] = {
    {4, 2, 1 << 1, 1 << 0},  // x
    {2, 1, 1 << 2, 1 << 3},  // y
    {1, 0, 1 << 4, 1 << 5},  // z
  };

  for (const auto& param : neighParamMap) {
    // skip expensive check if parent's flags indicate adjacent neighbour
    // is not present.
    if ((childIdx & param.childIdxBitPos) == 0) {
      // $axis co-ordinate = 0
      if (parentOccupancy & (1 << (childIdx + param.childIdxBitPos)))
        child.neighPattern |= param.patternFlagThem;

      if (!(neighPattern & param.patternFlagUs))
        continue;
    } else {
      if (parentOccupancy & (1 << (childIdx - param.childIdxBitPos)))
        child.neighPattern |= param.patternFlagUs;

      // no external search is required for $axis co-ordinate = 1
      continue;
    }

    if (siblingRestriction)
      continue;

    // calculate the morton address of the 'left' neighbour,
    // the delta is then used as the starting position for a search
    int64_t mortonIdxNeigh =
      morton3dAxisDec(midx, param.axis) & ~0x8000000000000000ull;
    int64_t mortonDelta = midx - mortonIdxNeigh;

    if (mortonDelta < 0) {
      // no neighbour due to being in zero-th col/row/plane
      continue;
    }

    // NB: fifo already contains current node, no point searching it
    auto posEnd = bufEnd;
    std::advance(posEnd, -1);

    auto posStart = bufEnd;
    std::advance(posStart, -std::min(numNodesNextLvl, mortonDelta + 2));

    auto found = std::lower_bound(
      posStart, posEnd, mortonIdxNeigh,
      [](const PCCOctree3Node& node, int64_t mortonIdx) {
        return node.mortonIdx < mortonIdx;
      });

    // NB: found is always valid (see posEnd) => can skip check.
    if (found->mortonIdx != mortonIdxNeigh) {
      // neighbour isn't present => must have been empty
      continue;
    }

    // update both node's neighbour pattern
    // NB: neighours being present implies occupancy
    child.neighPattern |= param.patternFlagUs;
    found->neighPattern |= param.patternFlagThem;
  }
}

//============================================================================

CtxMapOctreeOccupancy::CtxMapOctreeOccupancy()
{
  map.reset(new CtxIdxMap);
  b[0] = map->b0;
  b[1] = map->b1;
  b[2] = map->b2;
  b[3] = map->b3;
  b[4] = map->b4;
  b[5] = map->b5;
  b[6] = map->b6;
  b[7] = map->b7;

  using namespace std;
  fill(begin(map->b0), end(map->b0), 127);
  fill(begin(map->b1), end(map->b1), 127);
  fill(begin(map->b2), end(map->b2), 127);
  fill(begin(map->b3), end(map->b3), 127);
  fill(begin(map->b4), end(map->b4), 127);
  fill(begin(map->b5), end(map->b5), 127);
  fill(begin(map->b6), end(map->b6), 127);
  fill(begin(map->b7), end(map->b7), 127);
}

//============================================================================
// determine if a 222 block is planar

void
isPlanarNode(
  PCCPointSet3& pointCloud,
  PCCOctree3Node& node0,
  const Vec3<int>& nodeSizeLog2Minus1,
  uint8_t& planarMode,
  uint8_t& planePosBits,
  bool planarEligible[3])
{
  planarMode = 0;
  planePosBits = 0;

  bool occupX[2] = {false, false};
  bool occupY[2] = {false, false};
  bool occupZ[2] = {false, false};

  bool notPlanar[3] = {!planarEligible[0], !planarEligible[1],
                       !planarEligible[2]};

  // find occupancy N xyz-planes
  for (int k = node0.start; k < node0.end; k++) {
    if (!notPlanar[0]) {
      uint32_t px = pointCloud[k][0];
      px = (px & (1 << nodeSizeLog2Minus1[0])) >> nodeSizeLog2Minus1[0];
      occupX[px] = true;
      if (occupX[1 - px])
        notPlanar[0] = true;
    }

    if (!notPlanar[1]) {
      uint32_t py = pointCloud[k][1];
      py = (py & (1 << nodeSizeLog2Minus1[1])) >> nodeSizeLog2Minus1[1];
      occupY[py] = true;
      if (occupY[1 - py])
        notPlanar[1] = true;
    }

    if (!notPlanar[2]) {
      uint32_t pz = pointCloud[k][2];
      pz = (pz & (1 << nodeSizeLog2Minus1[2])) >> nodeSizeLog2Minus1[2];
      occupZ[pz] = true;
      if (occupZ[1 - pz])
        notPlanar[2] = true;
    }
  }

  // determine planar
  if (!(occupX[0] && occupX[1])) {
    planarMode |= 1;
    planePosBits |= occupX[1] ? 1 : 0;
  }

  if (!(occupY[0] && occupY[1])) {
    planarMode |= 2;
    planePosBits |= occupY[1] ? 2 : 0;
  }

  if (!(occupZ[0] && occupZ[1])) {
    planarMode |= 4;
    planePosBits |= occupZ[1] ? 4 : 0;
  }
}

//============================================================================
// intitialize planes for planar pred

void
planarInitPlanes(
  const int kNumPlanarPlanes, int depth, int* planes3x3, int* planes[9])
{
  const int kPlanarInit[9] = {-2,    -1000, -1000, -1000, -2,
                              -1000, -1000, -1000, -2};

  int shift = 0;
  const int planeSize = kNumPlanarPlanes << depth;

  for (int idxP = 0; idxP < 9; idxP++) {
    int* plane = planes3x3 + shift;
    shift += planeSize;
    planes[idxP] = plane;
    int v = kPlanarInit[idxP];

    for (int p = 0; p < planeSize; p++) {
      plane[p] = v;
    }
  }
}

//============================================================================
// update the plane rate depending on the occupancy

void
updateplanarRate(
  int planarRate[3], int occupancy, int& localDensity, int numSiblings)
{
  bool isPlanarX = !((occupancy & 0xf0) && (occupancy & 0x0f));
  bool isPlanarY = !((occupancy & 0xcc) && (occupancy & 0x33));
  bool isPlanarZ = !((occupancy & 0x55) && (occupancy & 0xaa));

  planarRate[0] = (255 * planarRate[0] + (isPlanarX ? 256 * 8 : 0) + 128) >> 8;
  planarRate[1] = (255 * planarRate[1] + (isPlanarY ? 256 * 8 : 0) + 128) >> 8;
  planarRate[2] = (255 * planarRate[2] + (isPlanarZ ? 256 * 8 : 0) + 128) >> 8;

  localDensity = (255 * localDensity + 1024 * numSiblings) >> 8;
}

//============================================================================
// planar eligbility

void
eligilityPlanar(
  bool planarEligible[3],
  int planarRate[3],
  const int threshold[3],
  int localDensity)
{
  planarEligible[0] = false;
  planarEligible[1] = false;
  planarEligible[2] = false;
  if (localDensity >= 3 * 1024) {
    return;
  }

  if (planarRate[0] >= planarRate[1] && planarRate[0] >= planarRate[2]) {
    // planar x dominates
    planarEligible[0] = planarRate[0] >= threshold[0];
    if (planarRate[1] >= planarRate[2]) {
      planarEligible[1] = planarRate[1] >= threshold[1];
      planarEligible[2] = planarRate[2] >= threshold[2];
    } else {
      planarEligible[2] = planarRate[2] >= threshold[1];
      planarEligible[1] = planarRate[1] >= threshold[2];
    }
  } else if (
    planarRate[1] >= planarRate[0] && planarRate[1] >= planarRate[2]) {
    // planar y dominates
    planarEligible[1] = planarRate[1] >= threshold[0];
    if (planarRate[0] >= planarRate[2]) {
      planarEligible[0] = planarRate[0] >= threshold[1];
      planarEligible[2] = planarRate[2] >= threshold[2];
    } else {
      planarEligible[2] = planarRate[2] >= threshold[1];
      planarEligible[0] = planarRate[0] >= threshold[2];
    }
  } else if (
    planarRate[2] >= planarRate[0] && planarRate[2] >= planarRate[1]) {
    // planar z dominates
    planarEligible[2] = planarRate[2] >= threshold[0];
    if (planarRate[0] >= planarRate[1]) {
      planarEligible[0] = planarRate[0] >= threshold[1];
      planarEligible[1] = planarRate[1] >= threshold[2];
    } else {
      planarEligible[1] = planarRate[1] >= threshold[1];
      planarEligible[0] = planarRate[0] >= threshold[2];
    }
  }
}

//============================================================================
// directional mask depending on the planarity

int
maskPlanarX(PCCOctree3Node& node0, bool implicitSkip)
{
  if (implicitSkip)
    return 0xf0;

  if ((node0.planarMode & 1) == 0)
    return 0;

  return (node0.planePosBits & 1) ? 0x0f : 0xf0;
}

//----------------------------------------------------------------------------

int
maskPlanarY(PCCOctree3Node& node0, bool implicitSkip)
{
  if (implicitSkip)
    return 0xcc;

  if ((node0.planarMode & 2) == 0)
    return 0;

  return (node0.planePosBits & 2) ? 0x33 : 0xcc;
}

//----------------------------------------------------------------------------

int
maskPlanarZ(PCCOctree3Node& node0, bool implicitSkip)
{
  // QTBT does not split in this direction
  //   => infer the mask low for occupancy bit coding
  if (implicitSkip)
    return 0xaa;

  if ((node0.planarMode & 4) == 0)
    return 0;

  return (node0.planePosBits & 4) ? 0x55 : 0xaa;
}

//----------------------------------------------------------------------------

// three direction mask
void
maskPlanar(PCCOctree3Node& node0, int mask[3], const int occupancySkip)
{
  static const uint8_t kPossibleMask[3] = {6, 5, 3};
  for (int k = 0; k <= 2; k++)
    if (occupancySkip & (4 >> k)) {
      node0.planarPossible = node0.planarPossible | (1 << k);
      node0.planePosBits = node0.planePosBits & kPossibleMask[k];
      node0.planarMode = node0.planarMode | (1 << k);
    }

  mask[0] = maskPlanarX(node0, occupancySkip & 4);
  mask[1] = maskPlanarY(node0, occupancySkip & 2);
  mask[2] = maskPlanarZ(node0, occupancySkip & 1);
}

//============================================================================

}  // namespace pcc

/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_SUPPORT_SOURCEMAPGENERATOR_H
#define HERMES_SUPPORT_SOURCEMAPGENERATOR_H

#include "hermes/SourceMap/SourceMap.h"
#include "hermes/Support/StringSetVector.h"

#include "llvm/ADT/ArrayRef.h"

namespace hermes {

/// A class representing a JavaScript source map, version 3 only. It borrows
/// terminology from the SourceMap spec: the "represented" code is the original,
/// while the "generated" code is the output of the minifier/compiler/etc.
/// See https://sourcemaps.info/spec.html for the spec that this class
/// implements.
class SourceMapGenerator {
 public:
  /// Add a line \p line represented as a list of Segments to the 'mappings'
  /// section.
  /// \param cjsModuleOffset the offset of the module represented by the given
  /// line, used as the "line" when reporting stack traces from the VM,
  /// which doesn't have access to the segment IDs.
  void addMappingsLine(SourceMap::SegmentList line, uint32_t cjsModuleOffset) {
    if (lines_.size() <= cjsModuleOffset) {
      lines_.resize(cjsModuleOffset + 1);
    }
    lines_[cjsModuleOffset] = std::move(line);
  }

  /// \return the list of mappings lines.
  llvm::ArrayRef<SourceMap::SegmentList> getMappingsLines() const {
    return lines_;
  }

  /// Set the list of input source maps to \p maps.
  /// The order should match the indexes used in the sourceIndex field of
  /// Segment.
  void setInputSourceMaps(std::vector<std::unique_ptr<SourceMap>> maps) {
    inputSourceMaps_ = std::move(maps);
  }

  /// Adds the source filename to filenameTable_ if it doesn't already exist.
  /// \return the index of \p filename in filenameTable_.
  uint32_t addSource(llvm::StringRef filename) {
    return filenameTable_.insert(filename);
  }

  /// Output the given source map as JSON.
  void outputAsJSON(llvm::raw_ostream &OS) const;

  /// Get the source index given the filename.
  uint32_t getSourceIndex(llvm::StringRef filename) const {
    auto it = filenameTable_.find(filename);
    assert(it != filenameTable_.end() && "unable to find filenameId");
    return std::distance(filenameTable_.begin(), it);
  }

 private:
  /// Delta encoding state.
  struct State {
    int32_t generatedColumn = 0;
    int32_t sourceIndex = 0;
    int32_t representedLine = 0;
    int32_t representedColumn = 0;
    int32_t nameIndex = 0;
  };

  /// Implementation of outputAsJSON for a merged source map generator.
  void outputAsJSONImpl(llvm::raw_ostream &OS) const;

  /// \return the mappings encoded in VLQ format.
  std::string getVLQMappingsString() const;

  /// \return a list of sources, in order.
  /// This list refers to internals of the StringMap and is invalidated by
  /// addSource().
  std::vector<llvm::StringRef> getSources() const;

  /// Encode the list \p segments into \p OS using the SourceMap
  /// Base64-VLQ scheme, delta-encoded with \p lastState as the starting state.
  static SourceMapGenerator::State encodeSourceLocations(
      const SourceMapGenerator::State &lastState,
      llvm::ArrayRef<SourceMap::Segment> segments,
      llvm::raw_ostream &OS);

  /// Merge the input source maps with the state in this generator,
  /// and return a new generator which contains a merged representation.
  SourceMapGenerator mergedWithInputSourceMaps() const;

  /// \return the input source map location for \p seg if the input source map
  /// exists and has a valid location for \p seg, else return llvm::None.
  llvm::Optional<SourceMapTextLocation> getInputLocationForSegment(
      const SourceMap::Segment &seg) const {
    if (seg.representedLocation.hasValue()) {
      assert(
          seg.representedLocation->sourceIndex >= 0 && "Negative source index");
    }
    // True iff inputSourceMaps_ has a valid source map for
    // seg.representedLocation->sourceIndex.
    bool hasInput = seg.representedLocation.hasValue() &&
        (uint32_t)seg.representedLocation->sourceIndex <
            inputSourceMaps_.size() &&
        inputSourceMaps_[seg.representedLocation->sourceIndex] != nullptr;

    return hasInput ? inputSourceMaps_[seg.representedLocation->sourceIndex]
                          ->getLocationForAddress(
                              seg.representedLocation->lineIndex + 1,
                              seg.representedLocation->columnIndex + 1)
                    : llvm::None;
  }

  /// The list of symbol names, populating the names field.
  std::vector<std::string> symbolNames_;

  /// The list of segments in our VLQ scheme.
  std::vector<SourceMap::SegmentList> lines_;

  /// The list of input source maps, such that the input file i has the
  /// SourceMap at index i. If no map was provided for a file, this list
  /// contains nullptr.
  std::vector<std::unique_ptr<SourceMap>> inputSourceMaps_;

  /// Map from {filename => source index}.
  StringSetVector filenameTable_{};
};

} // namespace hermes

#endif // HERMES_SUPPORT_SOURCEMAPGENERATOR_H

/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===------------------ Sequence.cpp - ONNX Operations -------------------===//
//
// Copyright 2019-2022 The IBM Research Authors.
//
// =============================================================================
//
// This file provides definition of ONNX dialect Sequence operations.
//
//===----------------------------------------------------------------------===//

#include "src/Dialect/ONNX/ONNXOps/OpHelper.hpp"

using namespace mlir;
using namespace mlir::OpTrait::util;
using namespace onnx_mlir;

// Sequence related operations
// The general form for seq is seq<tensor<*xT>>
// Tensors will be add to or removed from a seq dynamically.
// The tensor type in a seq should be a summary of all the tensor type in
// the seq.
// It is possible seq<tensor<*xT>> can be refined into seq<RankedTensor>,
// or even seq<StaticShapedTensor> if all the tensors have common shape info
// It is important to refine the type for seq in onnx-mlir because static
// type is used. If seq of unranked tensor remains, onnx-mlir can not handle
// the unranked tensor retrieved from the seq.
// Here is the rules for shape inferences of seq-related ops:
// * A seq is started empty as the result of SequenceEmpty. We can track this
//   property with a tag in seq type or along dataflow.
// * When the an element is added, we can merge its shape with that in seq.
// * when an element is removed from seq, the seq becomes empty if it is the
//   last tenor in the seq (known statically).
// Since the seq is usually used as a parameter of a graph (e.g. for LoopOp),
// shape inference for region may need improvement.

//===----------------------------------------------------------------------===//
// Support
//===----------------------------------------------------------------------===//

namespace {

// Helper function used in Sequence ops shape inference
ShapedType sequenceAddType(
    ShapedType accumulatedType, ShapedType additionalType) {
  Type elementType = accumulatedType.getElementType();
  assert(elementType == additionalType.getElementType() &&
         "types to merge must have the same data type");
  // Pick the weaker attr: known dim > unknown dim > unranked
  if (!accumulatedType.hasRank())
    return accumulatedType;
  if (!additionalType.hasRank())
    return additionalType;
  int64_t rank = accumulatedType.getRank();
  if (rank != additionalType.getRank())
    return UnrankedTensorType::get(elementType);
  ArrayRef<int64_t> acc = accumulatedType.getShape();
  ArrayRef<int64_t> add = additionalType.getShape();
  SmallVector<int64_t, 4> dims;
  for (int64_t i = 0; i < rank; i++) {
    dims.push_back(acc[i] != add[i] ? -1 : add[i]);
  }
  return RankedTensorType::get(dims, elementType);
}

} // namespace

//===----------------------------------------------------------------------===//
// SequenceAtOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceAtOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  auto outputType = getResult().getType();
  auto inputElementType =
      input_sequence().getType().cast<SeqType>().getElementType();
  if (!inputElementType.isa<UnrankedTensorType>() &&
      outputType.isa<UnrankedTensorType>()) {
    getResult().setType(inputElementType);
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceConstructOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceConstructOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  auto types = inputs().getTypes();
  ShapedType seqTensorType = types[0].cast<ShapedType>();
  for (size_t i = 1; i < types.size(); ++i) {
    seqTensorType = sequenceAddType(seqTensorType, types[i].cast<ShapedType>());
  }
  getResult().setType(SeqType::get(seqTensorType, types.size()));
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceEmptyOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceEmptyOp::verify() {
  // For the Optional dtypeAttr, the default type is F32
  auto builder = OpBuilder(getContext());
  Type elementType;
  if (dtypeAttr()) {
    elementType = convertONNXTypeToMLIRType(builder,
        (onnx::TensorProto_DataType)dtypeAttr().getValue().getSExtValue());
  } else {
    elementType = builder.getF32Type();
  }

  // Get element type for seq from the output
  ShapedType outputSeqElementType =
      getResult().getType().cast<SeqType>().getElementType();
  if (outputSeqElementType.getElementType() != elementType)
    return emitError("SequenceEmpty dtype() does not match the output type");
  return success();
}

LogicalResult ONNXSequenceEmptyOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  auto originTy = getResult().getType().cast<SeqType>();
  auto elementTy = originTy.getElementType();
  auto returnTy = SeqType::get(elementTy, 0);
  getResult().setType(returnTy);
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceEraseOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceEraseOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  auto inputTy = input_sequence().getType().cast<SeqType>();
  int64_t length = inputTy.getLength();

  if (length == 0)
    return emitError("SequenceErase from an empty seq");
  getResult().setType(
      SeqType::get(inputTy.getElementType(), length == -1 ? -1 : length - 1));
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceInsertOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceInsertOp::verify() {
  ONNXSequenceInsertOpAdaptor operandAdaptor =
      ONNXSequenceInsertOpAdaptor(*this);

  // These cast should be guaranteed by default verifier
  Type seqElementType = operandAdaptor.input_sequence()
                            .getType()
                            .dyn_cast<SeqType>()
                            .getElementType();
  Type elementType1 = seqElementType.dyn_cast<ShapedType>().getElementType();
  ShapedType insertType =
      operandAdaptor.tensor().getType().dyn_cast<ShapedType>();
  Type elementType2 = insertType.getElementType();

  if (elementType1 != elementType2) {
    return emitError("Element types of the tensor in seqence and input "
                     "have to be the same");
  }
  return success();
}

LogicalResult ONNXSequenceInsertOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  // Merge the tensor type for the seq and the inserted tensor
  SeqType seqType = input_sequence().getType().cast<SeqType>();
  ShapedType tensorType = tensor().getType().cast<ShapedType>();
  int64_t length = seqType.getLength();
  if (length == 0) {
    // When the input seq is empty, inherit the tensor type
    getResult().setType(SeqType::get(tensorType, 1));
  } else {
    int64_t newLength = length == -1 ? -1 : length + 1;
    ShapedType seqTensorType = seqType.getElementType().cast<ShapedType>();
    seqTensorType = sequenceAddType(seqTensorType, tensorType);
    getResult().setType(SeqType::get(seqTensorType, newLength));
  }
  return success();
}

//===----------------------------------------------------------------------===//
// SequenceLengthOp
//===----------------------------------------------------------------------===//

LogicalResult ONNXSequenceLengthOp::inferShapes(
    std::function<void(Region &)> doShapeInference) {
  Type outputTy = getResult().getType();
  if (!outputTy.isa<RankedTensorType>() ||
      outputTy.cast<RankedTensorType>().getRank() != 0) {
    SmallVector<int64_t, 1> dims;
    auto builder = Builder(getContext());
    Type scalarTy = RankedTensorType::get(dims, builder.getIntegerType(64));
    getResult().setType(scalarTy);
  }
  // ElementType of I64 will be checked by verifier
  return success();
}

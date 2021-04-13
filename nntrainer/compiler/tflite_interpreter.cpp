// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (C) 2021 Jihoon Lee <jhoon.it.lee@samsung.com>
 *
 * @file tflite_interpreter.cpp
 * @date 12 April 2021
 * @brief NNTrainer *.tflite Interpreter
 * @see	https://github.com/nnstreamer/nntrainer
 * @author Jihoon Lee <jhoon.it.lee@samsung.com>
 * @bug No known bugs except for NYI items
 */
#include <tflite_interpreter.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <tuple>

#include <tf_schema_generated.h>

#include <fc_layer.h>
#include <nntrainer_error.h>
#include <node_exporter.h>
#include <tensor.h>
#include <var_grad.h>
#include <weight.h>

#define UNUSED(x) x __attribute__((unused))
#define WILL(x) ;

static constexpr const char *FUNC_TAG = "[TFLITE INTERPRETER] ";

namespace nntrainer {

namespace {
/**
 * @brief after finishing building, call this to safe to a file
 *
 * @param builder flatbuffer builder
 * @param out out
 */
void builder2file(const flatbuffers::FlatBufferBuilder &builder,
                  const std::string &out) {
  uint8_t *buf = builder.GetBufferPointer();
  size_t size = builder.GetSize();
  flatbuffers::Verifier v(buf, size);

  NNTR_THROW_IF(!tflite::VerifyModelBuffer(v), std::invalid_argument)
    << FUNC_TAG << "Verifying serialized model failed";

  std::ofstream os(out, std::ios_base::binary);
  NNTR_THROW_IF(!os.good(), std::invalid_argument)
    << FUNC_TAG << "failed to open, reason: " << strerror(errno);
  os.write((char *)builder.GetBufferPointer(), builder.GetSize());
  os.close();
}

/**
 * @brief tensorflow operational node representation. This class contains,
 * information to build operation flatbuffer
 *
 */
class TfOpNode {
public:
  using Variables = std::vector<const Var_Grad *>;

  TfOpNode() = default;

  /**
   * @brief Construct a new Tf Op Node object from layer
   * @note this is a shortcut to skip if layer does not need to be devided or
   * fused
   * @param layer layer that is converted to TfOpNode
   */
  TfOpNode(const Layer &layer) {
    setInputs(layer.getInputRef());
    setOutputs(layer.getOutputRef());
    setWeights(layer.getWeightsRef());
    setOpType(layer.getType());
  }

  /**
   * @brief Set the Inputs object from layer
   *
   * @param inputs_ input to be inserted
   */
  void setInputs(const std::vector<std::shared_ptr<Var_Grad>> &inputs_) {
    inputs.reserve(inputs_.size());
    std::transform(inputs_.begin(), inputs_.end(), std::back_inserter(inputs),
                   [](const auto &data) { return data.get(); });
  }

  /**
   * @brief Set the Outputs object
   *
   * @param outputs_ output to be inserted
   */
  void setOutputs(const std::vector<std::shared_ptr<Var_Grad>> &outputs_) {
    outputs.reserve(outputs_.size());
    std::transform(outputs_.begin(), outputs_.end(),
                   std::back_inserter(outputs),
                   [](const auto &data) { return data.get(); });
  }

  /**
   * @brief Set the Weights object
   *
   * @param weights_ set weights from the object
   */
  void setWeights(const std::vector<Weight> &weights_) {
    weights.reserve(weights_.size());
    std::transform(weights_.begin(), weights_.end(),
                   std::back_inserter(weights),
                   [](const auto &data) { return &data; });
  }

  /**
   * @brief Set the Op Type object
   * @todo Considering number of alternatives to optimize this, for now it is
   * just workable.
   * 1. add and maintain global unordered map
   * 2. Save information in the appcontext later we can retrieve
   * 3. let type be an immutable property and let exporter handle this instead
   * of this method (preferrable)
   * @param type type to convert
   */
  void setOpType(const std::string &type) {
    if (istrequal(type, FullyConnectedLayer::type)) {
      setOpType(tflite::BuiltinOperator_FULLY_CONNECTED);
      return;
    }

    throw std::invalid_argument("not supported type");
  }

  /**
   * @brief Set the Builtin Options object,
   * @note this can go private, export from a layer and fill this out
   *
   * @param builtin_option_type_ builtin option type
   * @param builtin_ops_ flatbuffer offset of builtin_ops
   */
  void setBuiltinOptions(tflite::BuiltinOptions builtin_option_type_,
                         flatbuffers::Offset<void> &builtin_ops_) {
    builtin_ops = builtin_ops_;
    builtin_option_type = builtin_option_type_;
  }

  /**
   * @brief Get the Inputs object
   *
   * @return Variables& inputs
   */
  Variables &getInputs() { return inputs; }
  const Variables &getInputs() const { return inputs; }

  /**
   * @brief Get the Outputs object
   *
   * @return Variables&
   */
  Variables &getOutputs() { return outputs; }
  const Variables &getOutputs() const { return outputs; }

  /**
   * @brief Get the Weights object
   *
   * @return Variables&
   */
  Variables &getWeights() { return weights; }
  const Variables &getWeights() const { return weights; }

  /**
   * @brief Get the Op Type object
   *
   * @return const tflite::BuiltinOperator
   */
  const tflite::BuiltinOperator getOpType() const { return op_type; }

private:
  /**
   * @brief Set the Op Type object
   *
   * @param op_type_ operation type
   */
  void setOpType(tflite::BuiltinOperator op_type_) { op_type = op_type_; }

  Variables inputs;  /**< input variables */
  Variables outputs; /**< output variables */
  Variables weights; /**< weight variables */

  tflite::BuiltinOperator op_type;

  /// retrieve this from export_to
  flatbuffers::Offset<void> builtin_ops;
  tflite::BuiltinOptions builtin_option_type;
};

using TfOpNodes = std::vector<TfOpNode>;

/**
 * @brief Bidirectional Index map
 *
 * @tparam T type of a underlying value, please note that T will be copied, so
 * please use this for pointers and primitive values that is okay to copy
 */
template <typename T> class BidirectionalIndexMap {
public:
  /**
   * @brief addDatapoint to the map
   *
   * @param data data to be added if there is no occurrence, data will be
   * copied.
   */
  void addDataWhenNotFound(T data) {
    auto search = data2index.find(data);

    if (search == data2index.end()) {
      data2index[data] = index2data.size();
      index2data.push_back(data);
    }
  }

  /**
   * @brief Get the Index of the data
   *
   * @param key data that will be the key
   * @return unsigned int index
   */
  unsigned int getIndex(const T &key) const {
    auto search = data2index.find(key);

    NNTR_THROW_IF(search == data2index.end(), std::invalid_argument)
      << FUNC_TAG << "Cannot find index for key: " << key;

    return *search;
  }

  /**
   * @brief Get the Data object
   *
   * @param idx index to be searched
   * @return T datapoint T
   */
  T getData(unsigned int index) const {
    NNTR_THROW_IF(index >= index2data.size(), std::invalid_argument)
      << FUNC_TAG << "Cannot find data for index: " << index;

    return index2data[index];
  }

private:
  std::unordered_map<T, unsigned int> data2index; /**< data -> index map */
  std::vector<T> index2data;                      /**< index -> data map */
};

/**
 * @brief tensorflow operation index map, this class manages operation index
 * mapping
 *
 */
class TfOpIdxMap {
public:
  TfOpIdxMap(const TfOpNodes &nodes) {
    auto &opcode_map = getIndexMap<tflite::BuiltinOperator>();
    auto update_opcode = [&opcode_map](tflite::BuiltinOperator opcode) {
      opcode_map.addDataWhenNotFound(opcode);
    };

    auto &buffer_map = getIndexMap<const float *>();
    buffer_map.addDataWhenNotFound(empty_buffer); /// put empty buffer to first

    auto update_buffers = [&buffer_map](const TfOpNode::Variables &variables) {
      for (auto &variable : variables) {
        const Tensor &t = variable->getVariableRef();
        if (!t.uninitialized() && t.isAllocated()) {
          buffer_map.addDataWhenNotFound(t.getData());
        }
      }
    };

    auto &variable_map = getIndexMap<const Var_Grad *>();
    auto update_variables =
      [&variable_map](const TfOpNode::Variables &variables) {
        for (auto &variable : variables) {
          variable_map.addDataWhenNotFound(variable);
        }
      };

    for (auto &op_node : nodes) {
      update_opcode(op_node.getOpType());
      update_variables(op_node.getInputs());
      update_variables(op_node.getOutputs());
      update_variables(op_node.getWeights());
      update_buffers(op_node.getWeights());
    }
  }

  template <typename T> BidirectionalIndexMap<T> &getIndexMap() {
    return std::get<BidirectionalIndexMap<T>>(maps);
  }

private:
  float empty_buffer[0]; /**< unintialized tensor points to this buffer */

  std::tuple<BidirectionalIndexMap<const float *>, /**< underlying buffer map */
             BidirectionalIndexMap<tflite::BuiltinOperator>, /**< opcode map */
             BidirectionalIndexMap<const Var_Grad *>>        /**< tensor map */
    maps;
};

TfOpNodes
buildOpNodes(std::shared_ptr<const GraphRepresentation> representation) {
  TfOpNodes nodes;
  /// @todo, look ahead of layers to get nodes that can be fused
  for (const auto &ln : representation->getSorted()) {
    nodes.emplace_back(*ln->getObject());
  }

  return nodes;
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<tflite::Buffer>>>
buildBuffers(const TfOpIdxMap &map, flatbuffers::FlatBufferBuilder &fbb) {
  /** NYI! */
  return flatbuffers::Offset<
    flatbuffers::Vector<flatbuffers::Offset<tflite::Buffer>>>();
}

flatbuffers::Offset<
  flatbuffers::Vector<flatbuffers::Offset<tflite::OperatorCode>>>
buildOperatorCodes(const TfOpIdxMap &map, flatbuffers::FlatBufferBuilder &fbb) {
  /** NYI! */
  return flatbuffers::Offset<
    flatbuffers::Vector<flatbuffers::Offset<tflite::OperatorCode>>>();
};

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<tflite::SubGraph>>>
buildSubGraph(const TfOpNodes &nodes, const TfOpIdxMap &map,
              flatbuffers::FlatBufferBuilder &fbb) {
  /** NYI! */
  return flatbuffers::Offset<
    flatbuffers::Vector<flatbuffers::Offset<tflite::SubGraph>>>();
}

} // namespace

void TfliteInterpreter::serialize(
  std::shared_ptr<const GraphRepresentation> representation,
  const std::string &out) {
  /// @todo check if graph is finalized
  flatbuffers::FlatBufferBuilder fbb;

  auto opNodes = buildOpNodes(representation);
  TfOpIdxMap map(opNodes); /// build TfOpIdxMap

  auto UNUSED(opcodes) = buildOperatorCodes(map, fbb);
  auto UNUSED(buffers) = buildBuffers(map, fbb);
  auto UNUSED(subgraph) = buildSubGraph(opNodes, map, fbb);
  auto desc = fbb.CreateString("This file is generated from NNTrainer");

  tflite::ModelBuilder model_builder(fbb);

  WILL(model_builder.add_operator_codes(opcode_offset));
  WILL(model_builder.add_buffers(buffers));
  WILL(model_builder.add_subgraphs(subgraph));
  model_builder.add_version(3);
  model_builder.add_description(desc);
  auto model = model_builder.Finish();

  fbb.Finish(model, tflite::ModelIdentifier());
  builder2file(fbb, out);
}

std::shared_ptr<GraphRepresentation>
TfliteInterpreter::deserialize(const std::string &in) { /** NYI! */
  return nullptr;
}

} // namespace nntrainer

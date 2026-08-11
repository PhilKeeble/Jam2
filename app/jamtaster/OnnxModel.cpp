#include "OnnxModel.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace jamtaster::native {
namespace {

std::size_t elementCount(const std::vector<std::int64_t>& shape)
{
    std::size_t result = 1;
    for (const auto dimension : shape) {
        if (dimension <= 0) throw std::runtime_error("runtime tensor shape is not concrete");
        const auto value = static_cast<std::size_t>(dimension);
        if (value > (std::numeric_limits<std::size_t>::max)() / result) {
            throw std::length_error("tensor shape is too large");
        }
        result *= value;
    }
    return result;
}

} // namespace

OnnxModel::OnnxModel(const std::filesystem::path& path, int threads)
    : environment_(ORT_LOGGING_LEVEL_WARNING, "jamtaster-native")
{
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("ONNX model is missing: " + path.string());
    }
    options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    options_.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    options_.SetInterOpNumThreads(1);
    if (threads > 0) options_.SetIntraOpNumThreads(threads);
    session_ = std::make_unique<Ort::Session>(environment_, path.c_str(), options_);
    inputs_ = describe(*session_, true);
    outputs_ = describe(*session_, false);
    if (inputs_.size() != 1) {
        throw std::runtime_error("native lab currently requires a one-input ONNX graph");
    }
}

const std::vector<TensorDescription>& OnnxModel::inputs() const noexcept
{
    return inputs_;
}

const std::vector<TensorDescription>& OnnxModel::outputs() const noexcept
{
    return outputs_;
}

std::vector<TensorResult> OnnxModel::run(
    const std::vector<float>& values,
    const std::vector<std::int64_t>& shape)
{
    if (elementCount(shape) != values.size()) {
        throw std::runtime_error("ONNX input shape does not match its values");
    }
    std::vector<float> mutableValues(values);
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto tensor = Ort::Value::CreateTensor<float>(
        memory, mutableValues.data(), mutableValues.size(), shape.data(), shape.size());
    const char* inputNames[] = {inputs_.front().name.c_str()};
    std::vector<const char*> outputNames;
    outputNames.reserve(outputs_.size());
    for (const auto& output : outputs_) outputNames.push_back(output.name.c_str());
    auto valuesOut = session_->Run(
        Ort::RunOptions{nullptr}, inputNames, &tensor, 1,
        outputNames.data(), outputNames.size());
    std::vector<TensorResult> result;
    result.reserve(valuesOut.size());
    for (std::size_t index = 0; index < valuesOut.size(); ++index) {
        const auto info = valuesOut[index].GetTensorTypeAndShapeInfo();
        if (info.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
            throw std::runtime_error("ONNX output is not float32: " + outputs_[index].name);
        }
        TensorResult output;
        output.name = outputs_[index].name;
        output.shape = info.GetShape();
        const std::size_t count = elementCount(output.shape);
        const float* data = valuesOut[index].GetTensorData<float>();
        output.values.assign(data, data + count);
        result.push_back(std::move(output));
    }
    return result;
}

std::string OnnxModel::runtimeVersion()
{
    return OrtGetApiBase()->GetVersionString();
}

std::vector<TensorDescription> OnnxModel::describe(Ort::Session& session, bool input)
{
    Ort::AllocatorWithDefaultOptions allocator;
    const std::size_t count = input ? session.GetInputCount() : session.GetOutputCount();
    std::vector<TensorDescription> result;
    result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto name = input
            ? session.GetInputNameAllocated(index, allocator)
            : session.GetOutputNameAllocated(index, allocator);
        const auto type = input
            ? session.GetInputTypeInfo(index)
            : session.GetOutputTypeInfo(index);
        const auto tensor = type.GetTensorTypeAndShapeInfo();
        result.push_back(TensorDescription{
            name.get(), tensor.GetShape(), tensor.GetElementType()});
    }
    return result;
}

} // namespace jamtaster::native

#pragma once

#include <onnxruntime_cxx_api.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace jamtaster::native {

struct TensorResult {
    std::string name;
    std::vector<std::int64_t> shape;
    std::vector<float> values;
};

struct TensorDescription {
    std::string name;
    std::vector<std::int64_t> shape;
    ONNXTensorElementDataType elementType = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
};

class OnnxModel {
public:
    OnnxModel(const std::filesystem::path& path, int threads);

    [[nodiscard]] const std::vector<TensorDescription>& inputs() const noexcept;
    [[nodiscard]] const std::vector<TensorDescription>& outputs() const noexcept;
    [[nodiscard]] std::vector<TensorResult> run(
        const std::vector<float>& values,
        const std::vector<std::int64_t>& shape);
    [[nodiscard]] static std::string runtimeVersion();

private:
    static std::vector<TensorDescription> describe(
        Ort::Session& session, bool input);

    Ort::Env environment_;
    Ort::SessionOptions options_;
    std::unique_ptr<Ort::Session> session_;
    std::vector<TensorDescription> inputs_;
    std::vector<TensorDescription> outputs_;
};

} // namespace jamtaster::native

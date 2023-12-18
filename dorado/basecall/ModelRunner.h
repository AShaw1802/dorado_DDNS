#pragma once

#include "CRFModel.h"
#include "CRFModelConfig.h"
#include "decode/Decoder.h"
#include "utils/stats.h"

#include <torch/nn.h>

#include <atomic>
#include <string>

namespace dorado::basecall {

class ModelRunnerBase {
public:
    virtual ~ModelRunnerBase() = default;
    virtual void accept_chunk(int chunk_idx, const at::Tensor &chunk) = 0;
    virtual std::vector<decode::DecodedChunk> call_chunks(int num_chunks) = 0;
    virtual const CRFModelConfig &config() const = 0;
    virtual size_t model_stride() const = 0;
    virtual size_t chunk_size() const = 0;
    virtual size_t batch_size() const = 0;
    virtual void terminate() = 0;
    virtual void restart() = 0;
    virtual std::string get_name() const = 0;
    virtual stats::NamedStats sample_stats() const = 0;
};

using RunnerPtr = std::unique_ptr<ModelRunnerBase>;

template <typename T>
class ModelRunner final : public ModelRunnerBase {
public:
    ModelRunner(const CRFModelConfig &model_config,
                const std::string &device,
                int chunk_size,
                int batch_size);
    void accept_chunk(int chunk_idx, const at::Tensor &chunk) final;
    std::vector<decode::DecodedChunk> call_chunks(int num_chunks) final;
    const CRFModelConfig &config() const final { return m_config; };
    size_t model_stride() const final { return m_config.stride; }
    size_t chunk_size() const final { return m_input.size(2); }
    size_t batch_size() const final { return m_input.size(0); }
    void terminate() final {}
    void restart() final {}
    std::string get_name() const final { return "ModelRunner"; }
    stats::NamedStats sample_stats() const final;

private:
    const CRFModelConfig m_config;
    at::Tensor m_input;
    at::TensorOptions m_options;
    std::unique_ptr<T> m_decoder;
    decode::DecoderOptions m_decoder_options;
    torch::nn::ModuleHolder<torch::nn::AnyModule> m_module{nullptr};

    // Performance monitoring stats.
    std::atomic<int64_t> m_num_batches_called = 0;
    std::atomic<int64_t> m_model_ms = 0;
    std::atomic<int64_t> m_decode_ms = 0;
};

template <typename T>
ModelRunner<T>::ModelRunner(const CRFModelConfig &model_config,
                            const std::string &device,
                            int chunk_size,
                            int batch_size)
        : m_config(model_config) {
    m_decoder_options = decode::DecoderOptions();
    m_decoder_options.q_shift = model_config.qbias;
    m_decoder_options.q_scale = model_config.qscale;
    m_decoder = std::make_unique<T>();

    m_options = at::TensorOptions().dtype(T::dtype).device(device);
    m_module = load_crf_model(model_config, m_options);

    // adjust chunk size to be a multiple of the stride
    chunk_size -= chunk_size % model_config.stride;

    m_input = at::zeros({batch_size, model_config.num_features, chunk_size},
                        at::TensorOptions().dtype(T::dtype).device(at::kCPU));
}

template <typename T>
std::vector<decode::DecodedChunk> ModelRunner<T>::call_chunks(int num_chunks) {
    at::InferenceMode guard;
    dorado::stats::Timer timer;
    auto scores = m_module->forward(m_input.to(m_options.device_opt().value()));
    const auto forward_ms = timer.GetElapsedMS();
    auto decoded_chunks = m_decoder->beam_search(scores, num_chunks, m_decoder_options);
    const auto forward_plus_decode_ms = timer.GetElapsedMS();
    ++m_num_batches_called;
    m_model_ms += forward_ms;
    m_decode_ms += forward_plus_decode_ms - forward_ms;
    return decoded_chunks;
}

template <typename T>
void ModelRunner<T>::accept_chunk(int chunk_idx, const at::Tensor &chunk) {
    m_input.index_put_({chunk_idx, at::indexing::Ellipsis}, chunk);
}

template <typename T>
stats::NamedStats ModelRunner<T>::sample_stats() const {
    stats::NamedStats stats;
    stats["batches_called"] = double(m_num_batches_called);
    stats["model_ms"] = double(m_model_ms);
    stats["decode_ms"] = double(m_decode_ms);
    return stats;
}

}  // namespace dorado::basecall
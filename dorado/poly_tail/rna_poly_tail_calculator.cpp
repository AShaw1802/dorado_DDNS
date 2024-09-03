#include "rna_poly_tail_calculator.h"

#include "read_pipeline/messages.h"
#include "utils/math_utils.h"
#include "utils/sequence_utils.h"

#include <edlib.h>
#include <spdlog/spdlog.h>

#include <algorithm>

namespace {
EdlibAlignConfig init_edlib_config_for_adapter() {
    EdlibAlignConfig placement_config = edlibDefaultAlignConfig();
    placement_config.mode = EDLIB_MODE_HW;
    placement_config.task = EDLIB_TASK_LOC;
    return placement_config;
}
}  // namespace

namespace dorado::poly_tail {

RNAPolyTailCalculator::RNAPolyTailCalculator(PolyTailConfig config, bool is_rna_adapter)
        : PolyTailCalculator(std::move(config)), m_rna_adapter(is_rna_adapter) {}

float RNAPolyTailCalculator::average_samples_per_base(const std::vector<float>& sizes) const {
    auto quantiles = dorado::utils::quantiles(sizes, {0.1f, 0.9f});
    float sum = 0.f;
    int count = 0;
    for (auto s : sizes) {
        if (s >= quantiles[0] && s <= quantiles[1]) {
            sum += s;
            count++;
        }
    }
    return (count > 0 ? (sum / count) : 0.f);
}

SignalAnchorInfo RNAPolyTailCalculator::determine_signal_anchor_and_strand(
        const SimplexRead& read) const {
    if (!m_rna_adapter) {
        return SignalAnchorInfo{false, read.read_common.rna_adapter_end_signal_pos, 0, false};
    }

    const std::string& rna_adapter = m_config.rna_adapter;
    const float threshold = m_config.flank_threshold;
    const int window = m_config.primer_window;
    int trailing_Ts = 0;

    std::string_view seq_view = std::string_view(read.read_common.seq);
    auto bottom_start = std::max(0, (int)seq_view.length() - window);
    std::string_view read_bottom = seq_view.substr(bottom_start, window);

    EdlibAlignConfig align_config = init_edlib_config_for_adapter();
    EdlibAlignResult align_result =
            edlibAlign(rna_adapter.data(), int(rna_adapter.length()), read_bottom.data(),
                       int(read_bottom.length()), align_config);

    spdlog::trace("polytail barcode mask edit dist {}", align_result.editDistance);

    const float adapter_score =
            1.f - static_cast<float>(align_result.editDistance) / rna_adapter.length();

    SignalAnchorInfo result = {false, -1, trailing_Ts, false};

    if (adapter_score >= threshold) {
        const auto stride = read.read_common.model_stride;
        const auto seq_to_sig_map = dorado::utils::moves_to_map(
                read.read_common.moves, stride, read.read_common.get_raw_data_samples(),
                read.read_common.seq.size() + 1);

        int base_anchor = bottom_start + align_result.startLocations[0] - m_config.rna_offset;
        // RNA sequence is reversed wrt the signal and move table
        int signal_anchor = int(seq_to_sig_map[seq_view.length() - base_anchor]);
        result = {false, signal_anchor, trailing_Ts, false};
    } else {
        spdlog::trace("{} adapter score too low {}", read.read_common.read_id, adapter_score);
    }

    edlibFreeAlignResult(align_result);
    return result;
}

// Create an offset for dRNA data. There is a tendency to overestimate the length of dRNA
// tails, especially shorter ones. This correction factor appears to fix the bias
// for most dRNA data. This exponential fit was done based on the standards data.
// TODO: In order to improve this, perhaps another pass over the tail interval is needed
// to get a more refined boundary estimation?
int RNAPolyTailCalculator::signal_length_adjustment(int signal_len) const {
    return int(std::round(
            std::min(100.f, std::exp(5.6838f - 0.0021f * static_cast<float>(signal_len)))));
}

std::pair<int, int> RNAPolyTailCalculator::signal_range(int signal_anchor,
                                                        int signal_len,
                                                        float samples_per_base) const {
    const int kSpread = int(std::round(samples_per_base * max_tail_length()));
    return {std::max(0, signal_anchor - 50), std::min(signal_len, signal_anchor + kSpread)};
}

}  // namespace dorado::poly_tail

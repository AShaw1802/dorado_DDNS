#include "ProgressTracker.h"

#include "utils/string_utils.h"
#include "utils/tty_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace dorado {

ProgressTracker::ProgressTracker(int total_reads, bool duplex, float post_processing_percentage)
        : m_num_reads_expected(total_reads),
          m_duplex(duplex),
          m_post_processing_percentage(post_processing_percentage) {
    m_initialization_time = std::chrono::system_clock::now();
}

ProgressTracker::~ProgressTracker() = default;

void ProgressTracker::summarize() const {
    // Don't output anything if stderr is not a tty.
    if (!utils::is_fd_tty(stderr)) {
        return;
    }

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(m_end_time -
                                                                          m_initialization_time)
                            .count();
    if (m_num_simplex_reads_written > 0) {
        spdlog::info("> Simplex reads basecalled: {}", m_num_simplex_reads_written);
    }
    if (m_num_simplex_reads_filtered > 0) {
        spdlog::info("> Simplex reads filtered: {}", m_num_simplex_reads_filtered);
    }
    if (m_duplex) {
        spdlog::info("> Duplex reads basecalled: {}", m_num_duplex_reads_written);
        if (m_num_duplex_reads_filtered > 0) {
            spdlog::info("> Duplex reads filtered: {}", m_num_duplex_reads_filtered);
        }
        spdlog::info(
                "> Duplex rate: {}%",
                ((static_cast<float>(m_num_duplex_bases_processed - m_num_duplex_bases_filtered) *
                  2) /
                 (m_num_simplex_bases_processed - m_num_simplex_bases_filtered)) *
                        100);
    }
    if (m_num_bases_processed > 0) {
        std::ostringstream samples_sec;
        if (m_duplex) {
            samples_sec << std::scientific << m_num_bases_processed / (duration / 1000.0);
            spdlog::info("> Basecalled @ Bases/s: {}", samples_sec.str());
        } else {
            samples_sec << std::scientific << m_num_samples_processed / (duration / 1000.0);
            spdlog::info("> Basecalled @ Samples/s: {}", samples_sec.str());
            spdlog::debug("> Including Padding @ Samples/s: {:.3e} ({:.2f}%)",
                          m_num_samples_incl_padding / (duration / 1000.0),
                          100.f * m_num_samples_processed / m_num_samples_incl_padding);
        }
    }

    if (m_num_barcodes_demuxed > 0) {
        std::ostringstream rate_str;
        rate_str << std::scientific << m_num_barcodes_demuxed / (duration / 1000.0);
        spdlog::info("> {} reads demuxed @ classifications/s: {}", m_num_barcodes_demuxed,
                     rate_str.str());
        // Report how many reads were classified into each
        // barcode.
        if (spdlog::get_level() <= spdlog::level::debug) {
            spdlog::debug("Barcode distribution :");
            size_t unclassified = 0;
            size_t total = 0;
            for (const auto& [bc_name, bc_count] : m_barcode_count) {
                spdlog::debug("{} : {}", bc_name, bc_count);
                total += bc_count;
                if (bc_name == "unclassified") {
                    unclassified += bc_count;
                }
            }
            spdlog::debug("Classified rate {}%", (1.f - float(unclassified) / total) * 100.f);
        }
    }

    if (m_num_poly_a_called + m_num_poly_a_not_called > 0) {
        // Visualize a distribution of the tail lengths called.
        if (!m_poly_a_tail_length_count.empty()) {
            spdlog::debug("PolyA tail length distribution :");
            auto max_val = std::max_element(
                    m_poly_a_tail_length_count.begin(), m_poly_a_tail_length_count.end(),
                    [](const auto& l, const auto& r) { return l.second < r.second; });
            int factor = std::max(1, 1 + max_val->second / 100);
            for (const auto& [len, count] : m_poly_a_tail_length_count) {
                spdlog::debug("{:03d} : {}", len, std::string(count / factor, '*'));
            }
        }

        spdlog::info("> PolyA tails called {}, not called {}, avg tail length {}",
                     m_num_poly_a_called, m_num_poly_a_not_called, m_avg_poly_a_tail_lengths);
    }
}

void ProgressTracker::update_progress_bar(const stats::NamedStats& stats) {
    // Instead of capturing end time when summarizer is called,
    // which suffers from delays due to sampler and pipeline termination
    // costs, store it whenever stats are updated.
    m_end_time = std::chrono::system_clock::now();

    auto fetch_stat = [&stats](const std::string& name) {
        auto res = stats.find(name);
        if (res != stats.end()) {
            return res->second;
        }
        return 0.;
    };

    m_num_simplex_reads_written = int(fetch_stat("HtsWriter.unique_simplex_reads_written") +
                                      fetch_stat("BarcodeDemuxerNode.demuxed_reads_written"));

    m_num_simplex_reads_filtered = int(fetch_stat("ReadFilterNode.simplex_reads_filtered"));
    m_num_simplex_bases_filtered = int(fetch_stat("ReadFilterNode.simplex_bases_filtered"));
    m_num_simplex_bases_processed = int64_t(fetch_stat("BasecallerNode.bases_processed"));
    m_num_bases_processed = m_num_simplex_bases_processed;
    m_num_samples_processed = int64_t(fetch_stat("BasecallerNode.samples_processed"));
    m_num_samples_incl_padding = int64_t(fetch_stat("BasecallerNode.samples_incl_padding"));
    if (m_duplex) {
        m_num_duplex_bases_processed = int64_t(fetch_stat("StereoBasecallerNode.bases_processed"));
        m_num_bases_processed += m_num_duplex_bases_processed;
    }
    m_num_duplex_reads_written = int(fetch_stat("HtsWriter.duplex_reads_written"));
    m_num_duplex_reads_filtered = int(fetch_stat("ReadFilterNode.duplex_reads_filtered"));
    m_num_duplex_bases_filtered = int(fetch_stat("ReadFilterNode.duplex_bases_filtered"));

    // Barcode demuxing stats.
    m_num_barcodes_demuxed = int(fetch_stat("BarcodeClassifierNode.num_barcodes_demuxed"));

    // PolyA tail stats.
    m_num_poly_a_called = int(fetch_stat("PolyACalculator.reads_estimated"));
    m_num_poly_a_not_called = int(fetch_stat("PolyACalculator.reads_not_estimated"));
    m_avg_poly_a_tail_lengths = int(fetch_stat("PolyACalculator.average_tail_length"));

    // don't output progress bar if stderr is not a tty
    if (!utils::is_fd_tty(stderr)) {
        return;
    }

    if (m_num_reads_expected != 0) {
        // TODO: Add the ceiling because in duplex, reads written can exceed reads expected
        // because of the read splitting. That needs to be handled properly.
        float progress = std::min(100.f, 100.f *
                                                 static_cast<float>(m_num_simplex_reads_written +
                                                                    m_num_simplex_reads_filtered) /
                                                 m_num_reads_expected);
        if (progress > 0 && progress > m_last_progress_written) {
            m_last_progress_written = progress;
            internal_set_progress(progress, false);
        }
    } else {
        std::cerr << "\r> Output records written: " << m_num_simplex_reads_written;
        std::cerr << "\r";
    }

    // Collect per barcode stats.
    if (m_num_barcodes_demuxed > 0 && (spdlog::get_level() <= spdlog::level::debug)) {
        for (const auto& [stat, val] : stats) {
            const std::string prefix = "BarcodeClassifierNode.bc.";
            if (utils::starts_with(stat, prefix)) {
                auto bc_name = stat.substr(prefix.length());
                m_barcode_count[bc_name] = static_cast<int>(val);
            }
        }
    }

    if (m_num_poly_a_called + m_num_poly_a_not_called > 0 &&
        (spdlog::get_level() <= spdlog::level::debug)) {
        for (const auto& [stat, val] : stats) {
            const std::string prefix = "PolyACalculator.pt.";
            if (utils::starts_with(stat, prefix)) {
                auto len = std::stoi(stat.substr(prefix.length()));
                m_poly_a_tail_length_count[len] = static_cast<int>(val);
            }
        }
    }
}

void ProgressTracker::update_post_processing_progress(float progress) {
    if (progress > m_last_post_processing_progress) {
        m_last_post_processing_progress = progress;
        internal_set_progress(progress, true);
    }
}

void ProgressTracker::internal_set_progress(float progress, bool post_processing) {
    // Sanity clamp.
    progress = std::min(progress, 100.f);

    // Map progress to total progress.
    float total_progress;
    if (post_processing) {
        total_progress =
                100 * (1 - m_post_processing_percentage) + progress * m_post_processing_percentage;
    } else {
        total_progress = progress * (1 - m_post_processing_percentage);
    }

    // Don't output the progress bar if stderr is not a tty.
    if (!utils::is_fd_tty(stderr)) {
        return;
    }

    // Draw it.
#ifdef _WIN32
    m_progress_bar.set_progress(static_cast<size_t>(total_progress));
#else
    m_progress_bar.set_progress(total_progress);
#endif
#ifdef __linux__
    std::cerr << "\033[K";
#endif  // WIN32
    std::cerr << "\r";
}

}  // namespace dorado

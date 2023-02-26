#include "WriterNode.h"

#include "Version.h"
#include "indicators/progress_bar.hpp"
#include "utils/sequence_utils.h"

#include <indicators/cursor_control.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace dorado {

void WriterNode::print_header() {
    if (!m_emit_fastq) {
        std::cout << "@HD\tVN:1.6\tSO:unknown\n"
                  << "@PG\tID:basecaller\tPN:dorado\tVN:" << DORADO_VERSION << "\tCL:dorado";
        for (const auto& arg : m_args) {
            std::cout << " " << arg;
        }
        std::cout << "\n";

        // Add read groups
        for (auto const& x : m_read_groups) {
            std::cout << "@RG\t";
            std::cout << "ID:" << x.first << "\t";
            std::cout << "PU:" << x.second.flowcell_id << "\t";
            std::cout << "PM:" << x.second.device_id << "\t";
            std::cout << "DT:" << x.second.exp_start_time << "\t";
            std::cout << "PL:"
                      << "ONT"
                      << "\t";
            std::cout << "DS:"
                      << "basecall_model=" << x.second.basecalling_model
                      << " runid=" << x.second.run_id << "\t";
            std::cout << "LB:" << x.second.sample_id << "\t";
            std::cout << "SM:" << x.second.sample_id;
            std::cout << std::endl;
        }
    }
}

void WriterNode::worker_thread() {
    std::shared_ptr<Read> read;
    while (m_work_queue.try_pop(read)) {
        m_num_bases_processed += read->seq.length();
        m_num_samples_processed += read->raw_data.size(0);
        ++m_num_reads_processed;

        if (m_rna) {
            std::reverse(read->seq.begin(), read->seq.end());
            std::reverse(read->qstring.begin(), read->qstring.end());
        }

        if (((m_num_reads_processed % progress_bar_increment) == 0) && m_isatty &&
            ((m_num_reads_processed / progress_bar_increment) < 100)) {
            if (m_num_reads_expected != 0) {
                m_progress_bar.tick();
            } else {
                std::scoped_lock<std::mutex> lock(m_cerr_mutex);
                std::cerr << "\r> Reads processed: " << m_num_reads_processed;
            }
        }

        if (utils::mean_qscore_from_qstring(read->qstring) < m_min_qscore) {
            m_num_reads_failed += 1;
            continue;
        }

        if (m_emit_fastq) {
            std::scoped_lock<std::mutex> lock(m_cout_mutex);
            std::cout << "@" << read->read_id << "\n"
                      << read->seq << "\n"
                      << "+\n"
                      << read->qstring << "\n";
        } else {
            try {
                for (const auto& sam_line : read->extract_sam_lines(m_emit_moves, m_duplex)) {
                    std::scoped_lock<std::mutex> lock(m_cout_mutex);
                    std::cout << sam_line << "\n";
                }
            } catch (const std::exception& ex) {
                std::scoped_lock<std::mutex> lock(m_cerr_mutex);
                spdlog::error("{}", ex.what());
            }
        }
    }
}

WriterNode::WriterNode(std::vector<std::string> args,
                       bool emit_fastq,
                       bool emit_moves,
                       bool rna,
                       bool duplex,
                       size_t min_qscore,
                       size_t num_worker_threads,
                       std::unordered_map<std::string, ReadGroup> read_groups,
                       int num_reads,
                       size_t max_reads)
        : ReadSink(max_reads),
          m_args(std::move(args)),
          m_emit_fastq(emit_fastq),
          m_emit_moves(emit_moves),
          m_rna(rna),
          m_duplex(duplex),
          m_min_qscore(min_qscore),
          m_read_groups(std::move(read_groups)),
          m_num_bases_processed(0),
          m_num_samples_processed(0),
          m_num_reads_processed(0),
          m_num_reads_failed(0),
          m_initialization_time(std::chrono::system_clock::now()),
          m_num_reads_expected(num_reads) {
#ifdef _WIN32
    m_isatty = true;
#else
    m_isatty = isatty(fileno(stderr));
#endif

    if (m_num_reads_expected <= 100) {
        progress_bar_increment = 100;
    } else {
        progress_bar_increment = m_num_reads_expected / 100;
    }

    print_header();
    for (size_t i = 0; i < num_worker_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&WriterNode::worker_thread, this)));
    }
}

WriterNode::~WriterNode() {
    terminate();
    for (auto& m : m_workers) {
        m->join();
    }
    auto end_time = std::chrono::system_clock::now();

    auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - m_initialization_time)
                    .count();
    if (m_isatty) {
        std::cerr << "\r";
    }
    spdlog::info("> Reads basecalled: {}", m_num_reads_processed);
    if (m_min_qscore > 0) {
        spdlog::info("> Reads skipped (qscore < {}): {}", m_min_qscore, m_num_reads_failed);
    }
    std::ostringstream samples_sec;
    if (m_duplex) {
        samples_sec << std::scientific << m_num_bases_processed / (duration / 1000.0);
        spdlog::info("> Bases/s: {}", samples_sec.str());
    } else {
        samples_sec << std::scientific << m_num_samples_processed / (duration / 1000.0);
        spdlog::info("> Samples/s: {}", samples_sec.str());
    }
}

}  // namespace dorado
